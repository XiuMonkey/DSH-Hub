#include "PopupWindow.h"
#include "ThemeManager.h"

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QPushButton>
#include <QVBoxLayout>

PopupWindow::PopupWindow(QWidget *parent)
    : QWidget(parent)
{
    setWindowFlags(Qt::FramelessWindowHint | Qt::Dialog);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_StyledBackground, true);

    // 白色圆角主体
    auto *body = new QWidget(this);
    body->setObjectName(QStringLiteral("popupBody"));
    body->setAttribute(Qt::WA_StyledBackground, true);
    body->setStyleSheet(QStringLiteral("QWidget#popupBody {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("panelBg")) + QStringLiteral(";") + QStringLiteral("  border: 1px solid ") + Theme::color(QStringLiteral("border")) + QStringLiteral(";") + QStringLiteral("  border-radius: 16px;") + QStringLiteral("}"));

    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(1, 1, 1, 1);
    outerLayout->addWidget(body);

    m_mainLayout = new QVBoxLayout(body);
    m_mainLayout->setContentsMargins(20, 16, 20, 20);
    m_mainLayout->setSpacing(12);

    // 标题栏 + 关闭按钮
    auto *headerLayout = new QHBoxLayout;
    headerLayout->setSpacing(8);
    headerLayout->setContentsMargins(0, -4, -4, 0); // 关闭按钮向上、向右各靠近 4px

    m_titleLabel = new QLabel(QStringLiteral("弹窗"), body);
    m_titleLabel->setStyleSheet(QStringLiteral("QLabel {") + QStringLiteral("  background: transparent;") + QStringLiteral("  color: ") + Theme::color(QStringLiteral("textPrimary")) + QStringLiteral(";") + QStringLiteral("  font-size: 16px;") + QStringLiteral("}"));

    m_closeButton = new QPushButton(QStringLiteral("✕"), body);
    m_closeButton->setFixedSize(28, 28);
    m_closeButton->setCursor(Qt::PointingHandCursor);
    m_closeButton->setStyleSheet(QStringLiteral("QPushButton {") + QStringLiteral("  background: transparent;") + QStringLiteral("  color: ") + Theme::color(QStringLiteral("textSecondary")) + QStringLiteral(";") + QStringLiteral("  border: none;") + QStringLiteral("  border-radius: 6px;") + QStringLiteral("  font-size: 14px;") + QStringLiteral("}") + QStringLiteral("QPushButton:hover {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("hoverBg")) + QStringLiteral(";") + QStringLiteral("  color: ") + Theme::color(QStringLiteral("textPrimary")) + QStringLiteral(";") + QStringLiteral("}"));
    connect(m_closeButton, &QPushButton::clicked, this, &QWidget::close);

    headerLayout->addWidget(m_titleLabel, 1);
    headerLayout->addWidget(m_closeButton, 0, Qt::AlignTop);

    m_mainLayout->addLayout(headerLayout);

    // 内容区
    m_contentLayout = new QVBoxLayout;
    m_contentLayout->setContentsMargins(0, 0, 0, 0);
    m_contentLayout->setSpacing(0);
    m_mainLayout->addLayout(m_contentLayout, 1);
}

void PopupWindow::setTitle(const QString &title)
{
    if (m_titleLabel)
        m_titleLabel->setText(title);
}

void PopupWindow::setContent(QWidget *content)
{
    if (!content)
        return;

    // 移除旧内容
    while (QLayoutItem *item = m_contentLayout->takeAt(0)) {
        if (QWidget *w = item->widget())
            w->deleteLater();
        delete item;
    }

    m_contentLayout->addWidget(content);
}

void PopupWindow::closeEvent(QCloseEvent *event)
{
    emit closed();
    QWidget::closeEvent(event);
}
