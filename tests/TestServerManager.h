#pragma once

// ------------------------------------------------------------------
// TestServerManager.h
// ------------------------------------------------------------------
// ServerManager（DSH 服务管理）的单元测试。
// ------------------------------------------------------------------

#include <QObject>

class TestServerManager : public QObject
{
	Q_OBJECT

private slots:
	void initTestCase();
	void cleanupTestCase();

	void defaultState();
	void isRestartingDefault();
	void dshHomeDefault();
	// 出厂 settings.yaml 的契约：写出"不随附模型"的声明、幂等、且绝不覆盖用户配置
	void factorySettingsSuppressBundledModels();
};
