#include "Settings.h"
#include "ThemeManager.h"

#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStringList>
#include <QVBoxLayout>

SettingsButton::SettingsButton(QWidget *parent)
    : QPushButton(QStringLiteral("模型设置"), parent)
{
    setObjectName(QStringLiteral("modelSettingsButton"));
    setCursor(Qt::PointingHandCursor);
    setFixedWidth(120);
    setFixedHeight(36);
    setCheckable(true);
    setChecked(true);
    setStyleSheet(QStringLiteral("QPushButton#modelSettingsButton {") + QStringLiteral("  background: transparent;") + QStringLiteral("  border: none;") + QStringLiteral("  border-radius: 8px;") + QStringLiteral("  padding: 8px 16px;") + QStringLiteral("  color: ") + Theme::color(QStringLiteral("textPrimary")) + QStringLiteral(";") + QStringLiteral("  font-size: 14px;") + QStringLiteral("  text-align: left;") + QStringLiteral("}") + QStringLiteral("QPushButton#modelSettingsButton:hover,") + QStringLiteral("QPushButton#modelSettingsButton:checked {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("hoverBg")) + QStringLiteral(";") + QStringLiteral("}") + QStringLiteral("QPushButton#modelSettingsButton:pressed {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("border")) + QStringLiteral(";") + QStringLiteral("}"));
}

Settings::Settings(const QString &dshHome, QWidget *parent)
    : PopupWindow(parent)
    , m_dshHome(dshHome)
    , m_credentialsFile(dshHome.isEmpty() ? QString() : dshHome + QStringLiteral("/.credentials.yaml"))
{
    setTitle(QStringLiteral("设置"));

    auto *content = new QWidget(this);
    auto *layout = new QHBoxLayout(content);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    auto *modelButton = new SettingsButton(content);
    layout->addWidget(modelButton, 0, Qt::AlignTop);

    auto *rightPanel = new QWidget(content);
    auto *rightLayout = new QVBoxLayout(rightPanel);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(6);

    auto *apiLabel = new QLabel(QStringLiteral("API Key:"), rightPanel);
    apiLabel->setStyleSheet(QStringLiteral("QLabel {") + QStringLiteral("  background: transparent;") + QStringLiteral("  color: ") + Theme::color(QStringLiteral("textSecondary")) + QStringLiteral(";") + QStringLiteral("  font-size: 12px;") + QStringLiteral("}"));

    auto *modelEdit = new QLineEdit(rightPanel);
    modelEdit->setObjectName(QStringLiteral("modelSettingsEdit"));
    modelEdit->setPlaceholderText(QStringLiteral("输入模型设置"));
    modelEdit->setText(readApiKeyFromCredentialsFile());
    modelEdit->setStyleSheet(QStringLiteral("QLineEdit#modelSettingsEdit {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("inputBg")) + QStringLiteral(";") + QStringLiteral("  border: 1px solid ") + Theme::color(QStringLiteral("scrollbar")) + QStringLiteral(";") + QStringLiteral("  border-radius: 8px;") + QStringLiteral("  padding: 8px 12px;") + QStringLiteral("  font-size: 14px;") + QStringLiteral("  color: ") + Theme::color(QStringLiteral("textPrimary")) + QStringLiteral(";") + QStringLiteral("  selection-background-color: ") + QStringLiteral("#BFDBFE") + QStringLiteral(";") + QStringLiteral("}") + QStringLiteral("QLineEdit#modelSettingsEdit:focus {") + QStringLiteral("  background: ") + Theme::color(QStringLiteral("panelBg")) + QStringLiteral(";") + QStringLiteral("  border-color: ") + Theme::color(QStringLiteral("accent")) + QStringLiteral(";") + QStringLiteral("}"));

    rightLayout->addWidget(apiLabel);
    rightLayout->addWidget(modelEdit);
    rightLayout->addStretch(1);

    layout->addWidget(rightPanel, 1);

    connect(modelButton, &QPushButton::toggled, rightPanel, &QWidget::setVisible);
    // 点击后保持选中状态，避免再次点击取消选中
    connect(modelButton, &QPushButton::clicked, modelButton, [modelButton]() {
        modelButton->setChecked(true);
    });

    connect(modelEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        writeApiKeyToCredentialsFile(text);
    });

    setContent(content);

    resize(640, 480);
}

QString Settings::readApiKeyFromCredentialsFile() const
{
    QFile file(m_credentialsFile);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();

    QRegularExpression re(QStringLiteral("^\\s*DEEPSEEK_API_KEY\\s*:\\s*(.*)$"));
    while (!file.atEnd()) {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        const auto match = re.match(line);
        if (match.hasMatch()) {
            QString value = match.captured(1).trimmed();
            if ((value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"')))
                || (value.startsWith(QLatin1Char('\'')) && value.endsWith(QLatin1Char('\'')))) {
                value = value.mid(1, value.size() - 2);
            }
            return value;
        }
    }

    return QString();
}

void Settings::writeApiKeyToCredentialsFile(const QString &apiKey)
{
    if (m_credentialsFile.isEmpty())
        return;

    QDir().mkpath(m_dshHome);

    QStringList lines;
    QFile file(m_credentialsFile);
    if (file.exists()) {
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            while (!file.atEnd())
                lines.append(QString::fromUtf8(file.readLine()));
            file.close();
        }
    }

    QRegularExpression re(QStringLiteral("^\\s*DEEPSEEK_API_KEY\\s*:.*$"));
    bool replaced = false;
    for (QString &line : lines) {
        if (re.match(line).hasMatch()) {
            line = QStringLiteral("DEEPSEEK_API_KEY: %1\n").arg(apiKey);
            replaced = true;
            break;
        }
    }
    if (!replaced)
        lines.append(QStringLiteral("DEEPSEEK_API_KEY: %1\n").arg(apiKey));

    QSaveFile out(m_credentialsFile);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Text))
        return;

    for (const QString &line : lines)
        out.write(line.toUtf8());

    out.commit();
}
