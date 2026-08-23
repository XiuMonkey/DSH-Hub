#include "PopupWindow.h"

#include <QCloseEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QPainter>
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
    body->setStyleSheet(QStringLiteral(
        "QWidget#popupBody {"
        "  background: #FFFFFF;"
        "  border: 1px solid #E5E7EB;"
        "  border-radius: 16px;"
        "}"
    ));

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
    m_titleLabel->setStyleSheet(QStringLiteral(
        "QLabel {"
        "  background: transparent;"
        "  color: #1F2328;"
        "  font-size: 16px;"
        "}"
    ));

    m_closeButton = new QPushButton(QStringLiteral("✕"), body);
    m_closeButton->setFixedSize(28, 28);
    m_closeButton->setCursor(Qt::PointingHandCursor);
    m_closeButton->setStyleSheet(QStringLiteral(
        "QPushButton {"
        "  background: transparent;"
        "  color: #6B7280;"
        "  border: none;"
        "  border-radius: 6px;"
        "  font-size: 14px;"
        "}"
        "QPushButton:hover {"
        "  background: #F3F4F6;"
        "  color: #111827;"
        "}"
    ));
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

void PopupWindow::paintEvent(QPaintEvent *event)
{
    Q_UNUSED(event)

    // 可选：这里不再额外绘制，透明背景由 body 的圆角样式负责
    QWidget::paintEvent(event);
}
