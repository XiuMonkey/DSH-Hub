#include "CardShadow.h"

#include <QHash>
#include <QPainter>
#include <QPixmap>
#include <QRegularExpression>
#include <QSize>
#include <QString>

#include <cmath>

namespace CardShadow
{
	namespace
	{
		// 缓存预算（字节，按**设备像素**计，见 paint() 里的占用量法）。
		// 消息气泡的尺寸逐条不同，缓存会不断换入换出，所以必须有上限。
		//
		// 注意预算是"字节"而不是"像素"：同一个界面在 200% 缩放下每条位图是 4 倍大，
		// 于是**能放下的条数变少** —— 这是内存上限该有的行为，不是 bug。
		// 真要在高缩放下也保持同样的命中率，就把这个值按 dpr² 往上调。
		constexpr qint64 kCacheBudget = 24 * 1024 * 1024;
		// 单张超过这个大小就不进缓存了：一张巨图会把整个预算顶掉，
		// 反而让常用的小阴影全被淘汰。
		//
		// 8MB 这个值是按实测定的：一条长回复的气泡（约 880×1250 逻辑像素）
		// 位图约 4.3MB —— 卡在旧的 4MB 上限外，于是**每帧重新生成**，
		// 实测每次 3.08ms，是命中时（0.175ms）的 17 倍。放到 8MB 后它能进缓存。
		constexpr qint64 kMaxEntry = 8 * 1024 * 1024;
		// 环数：每像素一圈。少于 6 圈渐变会有台阶感，多于 48 圈纯属浪费。
		constexpr int kMinRings = 6;
		constexpr int kMaxRings = 48;

		struct Entry
		{
			QPixmap pixmap;
			qint64 bytes = 0;
			quint64 stamp = 0;
		};

		QHash<QString, Entry>& cache()
		{
			static QHash<QString, Entry> instance;
			return instance;
		}

		qint64& cachedBytes()
		{
			static qint64 bytes = 0;
			return bytes;
		}

		quint64& clock()
		{
			static quint64 tick = 0;
			return tick;
		}

		// 按 LRU 淘汰到预算之内
		void evictToBudget()
		{
			QHash<QString, Entry>& c = cache();
			qint64& bytes = cachedBytes();
			if (bytes <= kCacheBudget)
				return;

			// 反复取出最旧的一条，直到回到预算内
			while (bytes > kCacheBudget && !c.isEmpty()) {
				QString oldestKey;
				quint64 oldestStamp = ~quint64(0);
				for (auto it = c.constBegin(); it != c.constEnd(); ++it) {
					if (it.value().stamp < oldestStamp) {
						oldestStamp = it.value().stamp;
						oldestKey = it.key();
					}
				}
				if (oldestKey.isEmpty())
					break;
				bytes -= c.value(oldestKey).bytes;
				c.remove(oldestKey);
			}
		}

		// 高斯尾函数 Q(x)=0.5*erfc(x/√2)：真实"模糊"阴影离形状边界 d 处的强度。
		// 它在 d=0 处正好是 0.5 —— 这正是模糊阴影的半高位置，也是本模块
		// 之前那版最致命的地方：那时环的透明度按"边界处累积到整个峰值"来配，
		// 于是贴着卡片是一条实边，观感是"又细又重的一条描边"。
		qreal gaussTail(qreal x)
		{
			return 0.5 * std::erfc(x / std::sqrt(2.0));
		}

		QPixmap buildShadow(const QSize& imageSize, const QRect& cardRect, const Spec& spec,
			const QColor& color, qreal dpr)
		{
			const int rings = qBound(kMinRings, spec.blur, kMaxRings);

			// 目标强度曲线：与原生 CSS 同构的两层阴影 ——
			//   一层贴着卡片（半径小、稍实），一层铺得开（半径大、很淡）。
			// 原版的 --dsw-shadow-lv2 就是 `0 4px 12px 2%, 0 2px 8px 4%` 这种两层写法；
			// 单层高斯看着"薄"，两层叠起来才有"东西压在上面"的厚度。
			// 两层的总 alpha 仍然等于色板给的那一个值，色值依旧只来自 JSON。
			const qreal peak = qBound(0.0, color.alphaF(), 1.0);
			const qreal sigma = qMax(0.5, spec.blur / 2.5);       // 宽层
			const qreal sigmaTight = qMax(0.5, sigma * 0.35);     // 紧层
			constexpr qreal kTightShare = 0.45;                   // 紧层占的份额

			auto target = [&](int d) {
				if (d >= rings)
					return 0.0;
				return peak * (kTightShare * gaussTail(d / sigmaTight)
					+ (1.0 - kTightShare) * gaussTail(d / sigma));
				};

			// 由目标曲线反解每一环该用的透明度。
			// 距离 d 的像素会被 grow>d 的所有环覆盖，即 i=d..rings-1，
			// 累积后 alpha(d) = 1 - Π(1-a_i)。要让它等于 target(d)：
			//     a_d = 1 - (1-target(d)) / (1-target(d+1))
			QPixmap pixmap(imageSize * dpr);
			pixmap.setDevicePixelRatio(dpr);
			pixmap.fill(Qt::transparent);

			QPainter painter(&pixmap);
			painter.setRenderHint(QPainter::Antialiasing, true);
			painter.setPen(Qt::NoPen);

			// 整块阴影先按 dy 平移，再由外向内一圈圈叠上去
			const QRect base = cardRect.translated(0, spec.dy);
			for (int d = rings - 1; d >= 0; --d) {
				const qreal ringAlpha = 1.0 - (1.0 - target(d)) / (1.0 - target(d + 1));
				if (ringAlpha <= 0.0)
					continue;

				QColor ringColor = color;
				ringColor.setAlphaF(qBound(0.0, ringAlpha, 1.0));

				const int grow = d + 1;
				const QRect ring = base.adjusted(-grow, -grow, grow, grow);
				// 圆角随外扩同步变大，否则外圈会出现"方角"
				painter.setBrush(ringColor);
				painter.drawRoundedRect(ring, spec.radius + grow, spec.radius + grow);
			}

			return pixmap;
		}

		QString cacheKey(const QSize& imageSize, const Spec& spec, const QColor& color, qreal dpr)
		{
			return QStringLiteral("%1x%2|r%3|b%4|d%5|c%6|p%7")
				.arg(imageSize.width())
				.arg(imageSize.height())
				.arg(spec.radius)
				.arg(spec.blur)
				.arg(spec.dy)
				.arg(color.rgba())
				.arg(qRound(dpr * 100.0));
		}
	} // namespace

	QMargins padding(const Spec& spec)
	{
		const int blur = qMax(0, spec.blur);
		// 阴影整体下移 dy：下侧要多留 dy，上侧相应少留（dy 超过 blur 时不留）
		return QMargins(blur, qMax(0, blur - spec.dy), blur, blur + qMax(0, spec.dy));
	}

	QColor parseColor(const QString& text)
	{
		const QString trimmed = text.trimmed();
		if (trimmed.isEmpty())
			return QColor();

		// rgb() / rgba()：QColor 自己不会解析，这里补上
		static const QRegularExpression fn(QStringLiteral(
			"^rgba?\\(\\s*(\\d{1,3})\\s*,\\s*(\\d{1,3})\\s*,\\s*(\\d{1,3})\\s*"
			"(?:,\\s*([0-9]*\\.?[0-9]+)\\s*(%)?\\s*)?\\)$"));
		const QRegularExpressionMatch m = fn.match(trimmed);
		if (m.hasMatch()) {
			QColor color(m.captured(1).toInt(), m.captured(2).toInt(), m.captured(3).toInt());
			const QString alphaText = m.captured(4);
			if (!alphaText.isEmpty()) {
				const double value = alphaText.toDouble();
				int alpha = 255;
				if (!m.captured(5).isEmpty())
					alpha = qRound(value * 255.0 / 100.0);   // 百分比
				else if (value <= 1.0)
					alpha = qRound(value * 255.0);           // 0-1 的小数（CSS 写法）
				else
					alpha = qRound(value);                   // 0-255 的整数
				color.setAlpha(qBound(0, alpha, 255));
			}
			return color;
		}

		// 其余交给 QColor：#RRGGBB / #AARRGGBB / SVG 颜色名
		return QColor(trimmed);
	}

	void paint(QPainter& painter, const QRect& cardRect, const Spec& spec,
		const QColor& color, qreal devicePixelRatio, const QMargins& padOverride)
	{
		if (cardRect.isEmpty() || !color.isValid() || color.alpha() == 0)
			return;

		// 四边都给了才算覆盖；只要有一边是负的，就回到"按 spec 推导"
		const bool hasOverride = padOverride.left() >= 0 && padOverride.top() >= 0
			&& padOverride.right() >= 0 && padOverride.bottom() >= 0;
		const QMargins pad = hasOverride ? padOverride : padding(spec);

		const QSize imageSize(cardRect.width() + pad.left() + pad.right(),
			cardRect.height() + pad.top() + pad.bottom());
		if (imageSize.isEmpty())
			return;

		const qreal dpr = devicePixelRatio > 0.0 ? devicePixelRatio : 1.0;

		// 卡片在阴影位图坐标系里的位置
		const QRect cardInImage(QPoint(pad.left(), pad.top()), cardRect.size());
		const QString key = cacheKey(imageSize, spec, color, dpr);

		QHash<QString, Entry>& c = cache();
		auto it = c.find(key);
		QPixmap shadow;
		if (it == c.end()) {
			shadow = buildShadow(imageSize, cardInImage, spec, color, dpr);

			// 占用要按**设备像素**算：位图是 imageSize*dpr（见 buildShadow），
			// 只按逻辑尺寸算会漏掉 dpr² —— 在 150%/200% 缩放的屏幕上，
			// 记账值只有真实内存的 1/2.25 或 1/4，于是"24MB 预算"实际能吃到 ~96MB，
			// 而且单张上限也会误判（逻辑 1.5MB 的气泡真实有 5.8MB）。
			const qint64 bytes = qint64(qRound(imageSize.width() * dpr))
				* qint64(qRound(imageSize.height() * dpr)) * 4;
			if (bytes <= kMaxEntry) {
				Entry entry;
				entry.pixmap = shadow;
				entry.bytes = bytes;
				entry.stamp = ++clock();
				cachedBytes() += bytes;
				c.insert(key, entry);
				evictToBudget();
			}
		}
		else {
			// 命中也算一次使用，刷新 LRU 时间戳
			it.value().stamp = ++clock();
			shadow = it.value().pixmap;
		}

		// 位图左上角相对卡片左上角的偏移
		painter.drawPixmap(cardRect.topLeft() - QPoint(pad.left(), pad.top()), shadow);
	}
}