#include "ui/ShadowPanel.h"

#include "common/appearance/ThemeManager.h"

#include <QMargins>
#include <QPainter>
#include <QPaintEvent>
#include <QVBoxLayout>

ShadowPanel::ShadowPanel(const QString& shadowKey, const CardShadow::Spec& spec, QWidget* parent)
	: QWidget(parent)
	, m_spec(spec)
	, m_shadowKey(shadowKey)
{
	// 外壳自己不画背景，只画阴影
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

	// 用卡片实际几何，布局给额外拉伸也对得上
	const QRect cardRect = m_card->geometry();
	if (cardRect.isEmpty())
		return;

	QPainter painter(this);
	// 必须走 parseColor：色板是 rgba(...)，QColor 直接构造得无效色、阴影不画
	CardShadow::paint(painter, cardRect, m_spec, CardShadow::parseColor(ThemeManager::instance().color(m_shadowKey)),
		devicePixelRatioF(), m_padOverride);
}
