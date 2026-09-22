#include "common/appearance/CardShadow.h"

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
		// 缓存预算（字节，按设备像素计）：高 DPI 下每条位图大 4 倍、能放下的条数就变少，这是内存上限该有的行为（要保住命中率就按 dpr² 调大）。
		constexpr qint64 kCacheBudget = 24 * 1024 * 1024;
		// 单张超过此大小不进缓存：一张巨图会顶掉整个预算，把常用的小阴影全挤出去。8MB 为实测值 —— 长回复气泡约 880×1250 逻辑像素、位图约 4.3MB，旧 4MB 上限下每帧重建需 3.08ms，是命中（0.175ms）的 17 倍。
		constexpr qint64 kMaxEntry = 8 * 1024 * 1024;
		// 每像素一圈：少于 6 圈渐变会有台阶感，多于 48 圈纯属浪费。
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

		void evictToBudget()
		{
			QHash<QString, Entry>& c = cache();
			qint64& bytes = cachedBytes();
			if (bytes <= kCacheBudget)
				return;

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

		// 高斯尾 Q(x)=0.5*erfc(x/√2)：真实"模糊"阴影离形状边界 d 处的强度，d=0 处正好 0.5（模糊阴影的半高位置）；旧版把边界处配成累积满峰值，于是贴着卡片是一条实边、观感像"又细又重的一条描边"。
		qreal gaussTail(qreal x)
		{
			return 0.5 * std::erfc(x / std::sqrt(2.0));
		}

		QPixmap buildShadow(const QSize& imageSize, const QRect& cardRect, const Spec& spec,
			const QColor& color, qreal dpr)
		{
			const int rings = qBound(kMinRings, spec.blur, kMaxRings);

			// 与原生 CSS 同构的两层阴影（紧层贴卡片、宽层铺开，见原版 --dsw-shadow-lv2 的 `0 4px 12px 2%, 0 2px 8px 4%`）：单层高斯看着"薄"，两层叠起来才有"东西压在上面"的厚度；两层总 alpha 仍等于色板那一个值，色值依旧只来自 JSON。
			const qreal peak = qBound(0.0, color.alphaF(), 1.0);
			const qreal sigma = qMax(0.5, spec.blur / 2.5);
			const qreal sigmaTight = qMax(0.5, sigma * 0.35);
			constexpr qreal kTightShare = 0.45;

			auto target = [&](int d) {
				if (d >= rings)
					return 0.0;
				return peak * (kTightShare * gaussTail(d / sigmaTight)
					+ (1.0 - kTightShare) * gaussTail(d / sigma));
				};

			// 由目标曲线反解每环透明度：距离 d 的像素被所有 grow>d 的环覆盖，累积 alpha(d) = 1 - Π(1-a_i)，令其等于 target(d) 得 a_d = 1 - (1-target(d)) / (1-target(d+1))。
			QPixmap pixmap(imageSize * dpr);
			pixmap.setDevicePixelRatio(dpr);
			pixmap.fill(Qt::transparent);

			QPainter painter(&pixmap);
			painter.setRenderHint(QPainter::Antialiasing, true);
			painter.setPen(Qt::NoPen);

			// 必须由外向内叠加（累积公式假设外环先画）；整块阴影先按 dy 平移
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
					alpha = qRound(value * 255.0 / 100.0);
				else if (value <= 1.0)
					alpha = qRound(value * 255.0);
				else
					alpha = qRound(value);
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

		const QRect cardInImage(QPoint(pad.left(), pad.top()), cardRect.size());
		const QString key = cacheKey(imageSize, spec, color, dpr);

		QHash<QString, Entry>& c = cache();
		auto it = c.find(key);
		QPixmap shadow;
		if (it == c.end()) {
			shadow = buildShadow(imageSize, cardInImage, spec, color, dpr);

			// 必须按设备像素记账：位图是 imageSize*dpr，只算逻辑尺寸会漏掉 dpr² —— 150%/200% 屏上记账值只有真实内存的 1/2.25 或 1/4，"24MB 预算"实际吃到 ~96MB，单张上限也会误判（逻辑 1.5MB 的气泡真实 5.8MB）。
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

		painter.drawPixmap(cardRect.topLeft() - QPoint(pad.left(), pad.top()), shadow);
	}
}