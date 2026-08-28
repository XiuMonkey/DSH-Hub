#include "ThemeManager.h"

#include "DSHHub.h"
#include "SpinnerWidget.h"

#include <QGuiApplication>
#include <QHash>
#include <QLabel>
#include <QObject>
#include <QUrl>
#include <QScreen>
#include <QVBoxLayout>
#include <QDebug>


namespace Theme
{

Mode g_mode = Mode::Light;

void setMode(Mode mode)
{
    g_mode = mode;
    qInfo().noquote() << "[Theme] set mode:" << (mode == Mode::Dark ? "Dark" : "Light");
}

bool isDark()
{
    return g_mode == Mode::Dark;
}

QString color(const QString &key)
{
    static const QHash<QString, QString> light = {
        {QStringLiteral("windowBg"), QStringLiteral("#F8F9FB")},
        {QStringLiteral("panelBg"), QStringLiteral("#FFFFFF")},
        {QStringLiteral("cardBg"), QStringLiteral("#FFFFFF")},
        {QStringLiteral("hoverBg"), QStringLiteral("#F3F4F6")},
        {QStringLiteral("activeBg"), QStringLiteral("#EEF0F2")},
        {QStringLiteral("border"), QStringLiteral("#E5E7EB")},
        {QStringLiteral("textPrimary"), QStringLiteral("#1F2328")},
        {QStringLiteral("textSecondary"), QStringLiteral("#6B7280")},
        {QStringLiteral("accent"), QStringLiteral("#4C8BF5")},
        {QStringLiteral("accentHover"), QStringLiteral("#3A7AE0")},
        {QStringLiteral("danger"), QStringLiteral("#DC2626")},
        {QStringLiteral("dangerBg"), QStringLiteral("#FEF2F2")},
        {QStringLiteral("inputBg"), QStringLiteral("#F9FAFB")},
        {QStringLiteral("userBubbleBg"), QStringLiteral("#E0F2FE")},
        {QStringLiteral("userBubbleText"), QStringLiteral("#0C4A6E")},
        {QStringLiteral("scrollbar"), QStringLiteral("#D1D5DB")},
        {QStringLiteral("scrollbarHover"), QStringLiteral("#9CA3AF")},
    };

    static const QHash<QString, QString> dark = {
        {QStringLiteral("windowBg"), QStringLiteral("#111827")},
        {QStringLiteral("panelBg"), QStringLiteral("#1F2937")},
        {QStringLiteral("cardBg"), QStringLiteral("#1F2937")},
        {QStringLiteral("hoverBg"), QStringLiteral("#374151")},
        {QStringLiteral("activeBg"), QStringLiteral("#374151")},
        {QStringLiteral("border"), QStringLiteral("#374151")},
        {QStringLiteral("textPrimary"), QStringLiteral("#F9FAFB")},
        {QStringLiteral("textSecondary"), QStringLiteral("#9CA3AF")},
        {QStringLiteral("accent"), QStringLiteral("#4C8BF5")},
        {QStringLiteral("accentHover"), QStringLiteral("#3A7AE0")},
        {QStringLiteral("danger"), QStringLiteral("#F87171")},
        {QStringLiteral("dangerBg"), QStringLiteral("#7F1D1D")},
        {QStringLiteral("inputBg"), QStringLiteral("#111827")},
        {QStringLiteral("userBubbleBg"), QStringLiteral("#0C4A6E")},
        {QStringLiteral("userBubbleText"), QStringLiteral("#BAE6FD")},
        {QStringLiteral("scrollbar"), QStringLiteral("#4B5563")},
        {QStringLiteral("scrollbarHover"), QStringLiteral("#6B7280")},
    };

    // 原先硬编码在样式表里的颜色，暂时不区分亮/暗主题，统一保持原值
    static const QHash<QString, QString> shared = {
        {QStringLiteral("inputBorder"), QStringLiteral("#D0D7DE")},
        {QStringLiteral("buttonBg"), QStringLiteral("#E8EAED")},
        {QStringLiteral("buttonHover"), QStringLiteral("#DDE0E4")},
        {QStringLiteral("buttonPressed"), QStringLiteral("#CDD0D5")},
        {QStringLiteral("scrollbarHandle"), QStringLiteral("#C1C7CF")},
        {QStringLiteral("scrollbarHandleHover"), QStringLiteral("#A8B0B9")},
        {QStringLiteral("sendButtonHover"), QStringLiteral("#E0E7FF")},
        {QStringLiteral("sendButtonPressed"), QStringLiteral("#C7D2FE")},
        {QStringLiteral("toastBg"), QStringLiteral("rgba(31,35,40,0.85)")},
        {QStringLiteral("overlayBg"), QStringLiteral("rgba(128,128,128,0.65)")},
        {QStringLiteral("tagBg"), QStringLiteral("#EEF2FF")},
        {QStringLiteral("dangerButtonBg"), QStringLiteral("#E5484D")},
        {QStringLiteral("selectionBg"), QStringLiteral("#BFDBFE")},
        {QStringLiteral("iconButtonText"), QStringLiteral("#666666")},
        {QStringLiteral("iconButtonTextHover"), QStringLiteral("#222222")},
        {QStringLiteral("iconButtonHoverBg"), QStringLiteral("rgba(0,0,0,0.08)")},
        {QStringLiteral("iconButtonPressedBg"), QStringLiteral("rgba(0,0,0,0.12)")},
        {QStringLiteral("loadingText"), QStringLiteral("#57606A")},
        {QStringLiteral("loadingBorder"), QStringLiteral("#E1E4E8")},
        {QStringLiteral("systemMessageText"), QStringLiteral("#999999")},
        {QStringLiteral("shadow"), QStringLiteral("rgba(0,0,0,0.06)")},
        {QStringLiteral("shadowSubtle"), QStringLiteral("rgba(0,0,0,0.05)")},
        {QStringLiteral("textOnAccent"), QStringLiteral("white")},
    };

    const auto sharedIt = shared.constFind(key);
    if (sharedIt != shared.constEnd())
        return sharedIt.value();

    const QHash<QString, QString> &palette = isDark() ? dark : light;
    return palette.value(key, QStringLiteral("#000000"));
}

void switchTheme(QWidget *currentWindow)
{
    // 先切换到目标主题，弹窗会使用新主题的颜色
    setMode(isDark() ? Mode::Light : Mode::Dark);
    qInfo().noquote() << "[Theme] switch theme ->" << (isDark() ? "Dark" : "Light");

    if (currentWindow)
        currentWindow->hide();

    // 临时过渡弹窗：和初始化标签风格一致，使用即将生效的新主题
    auto *popup = new QWidget(nullptr, Qt::FramelessWindowHint | Qt::Dialog);
    popup->setAttribute(Qt::WA_TranslucentBackground);
    popup->setFixedSize(360, 200);

    auto *outerLayout = new QVBoxLayout(popup);
    outerLayout->setContentsMargins(1, 1, 1, 1);

    auto *body = new QWidget(popup);
    body->setObjectName(QStringLiteral("themeSwitchBody"));
    body->setAttribute(Qt::WA_StyledBackground, true);
    body->setStyleSheet(QStringLiteral("QWidget#themeSwitchBody {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("panelBg")) + QStringLiteral(";") + QStringLiteral("  border: 1px solid ") + Theme::color(QStringLiteral("border")) + QStringLiteral(";") + QStringLiteral("  border-radius: 20px;") + QStringLiteral("}"));
    outerLayout->addWidget(body);

    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(24, 20, 24, 20);
    layout->setSpacing(12);

    auto *spinner = new SpinnerWidget(body);
    spinner->setFixedSize(40, 40);
    spinner->start();
    layout->addWidget(spinner, 0, Qt::AlignHCenter);

    auto *label = new QLabel(QStringLiteral("正在切换主题..."), body);
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet(QStringLiteral("QLabel {") + QStringLiteral("  background: transparent;") + QStringLiteral("  color: ") + Theme::color(QStringLiteral("textPrimary")) + QStringLiteral(";") + QStringLiteral("  font-size: 16px;") + QStringLiteral("}"));
    layout->addWidget(label);

    layout->addStretch(1);

    popup->move(QGuiApplication::primaryScreen()->geometry().center() - popup->rect().center());
    popup->show();
    popup->raise();

    // 切换主题时复用当前 DSH server，不创建新 server
    QUrl oldBaseUrl;
    QProcess *oldServerProcess = nullptr;
    if (auto *oldHub = qobject_cast<DSHHub *>(currentWindow)) {
        oldBaseUrl = oldHub->baseUrl();
        oldServerProcess = oldHub->takeServerProcess();
    }

    auto *newWindow = new DSHHub(nullptr, oldBaseUrl, oldServerProcess);
    newWindow->hide();

    const auto showNewWindow = [currentWindow, newWindow, popup]() {
        popup->close();
        popup->deleteLater();

        if (currentWindow)
            currentWindow->deleteLater();

        newWindow->show();
        qInfo().noquote() << "[Theme] switch theme completed";
    };

    if (newWindow->isInitializationComplete()) {
        showNewWindow();
    } else {
        QObject::connect(newWindow, &DSHHub::initializationComplete,
                         newWindow, showNewWindow);
    }
    }
}

