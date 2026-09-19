#include "ShadowPanel.h"

#include "ThemeManager.h"

#include <QMargins>
#include <QPainter>
#include <QPaintEvent>
#include <QVBoxLayout>

ShadowPanel::ShadowPanel(const QString& shadowKey, const CardShadow::Spec& spec, QWidget* parent)
	: QWidget(parent)
	, m_spec(spec)
	, m_shadowKey(shadowKey)
{
	// 外壳自己不画背景（QSS 里也没有匹配它的规则），只画阴影，
	// 让下层的窗口/面板底色透出来。
	m_layout = new QVBoxLayout(this);
	m_layout->setSpacing(0);
	applyMargins();
}

QMargins ShadowPanel::effectivePadding() const
{
	// 四边都为负 = 没给覆盖值
	if (m_padOverride.left() < 0 || m_padOverride.top() < 0
		|| m_padOverride.right() < 0 || m_padOverride.bottom() < 0)
		return CardShadow::padding(m_spec);
	return m_padOverride;
}

void ShadowPanel::applyMargins()
{
	m_layout->setContentsMargins(effectivePadding());
}

void ShadowPanel::setPadding(const QMargins& pad)
{
	m_padOverride = pad;
	applyMargins();
	update();
}

void ShadowPanel::setCard(QWidget* card)
{
	if (!card || card == m_card)
		return;

	if (m_card)
		m_layout->removeWidget(m_card);

	m_card = card;
	m_card->setParent(this);
	m_layout->addWidget(m_card);
	update();
}

void ShadowPanel::setRadius(int radius)
{
	if (m_spec.radius == radius)
		return;
	m_spec.radius = radius;
	update();
}

void ShadowPanel::paintEvent(QPaintEvent* event)
{
	Q_UNUSED(event);

	if (!m_card)
		return;

	// 卡片就摆在内部布局留出的空白里，直接用它的实际几何，
	// 这样即使布局给了额外的拉伸（AlignLeft 之类的）也能对上。
	// 卡片尺寸变化必然带动外壳尺寸变化，而 Qt 在 resize 后本来就会重绘整块，
	// 所以这里不需要额外的 resizeEvent。
	const QRect cardRect = m_card->geometry();
	if (cardRect.isEmpty())
		return;

	QPainter painter(this);
	// 走 CardShadow::parseColor：色板里的阴影色是 rgba(...) 写法，
	// QColor 的字符串构造不认它（会得到无效色 → 阴影整块不画）
	CardShadow::paint(painter, cardRect, m_spec,
		CardShadow::parseColor(Theme::color(m_shadowKey)), devicePixelRatioF(),
		m_padOverride);
}