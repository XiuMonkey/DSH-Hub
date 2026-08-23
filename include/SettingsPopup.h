#pragma once

#include "PopupWindow.h"

class SettingsPopup : public PopupWindow
{
    Q_OBJECT

public:
    explicit SettingsPopup(QWidget *parent = nullptr);
};
