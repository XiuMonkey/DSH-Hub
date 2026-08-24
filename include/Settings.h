#pragma once

#include "PopupWindow.h"

#include <QPushButton>
#include <QString>

class SettingsButton : public QPushButton
{
    Q_OBJECT

public:
    explicit SettingsButton(QWidget *parent = nullptr);
};

class Settings : public PopupWindow
{
    Q_OBJECT

public:
    explicit Settings(const QString &dshHome, QWidget *parent = nullptr);

private:
    QString readApiKeyFromCredentialsFile() const;
    void writeApiKeyToCredentialsFile(const QString &apiKey);

    QString m_dshHome;
    QString m_credentialsFile;
};
