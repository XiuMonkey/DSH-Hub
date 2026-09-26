#pragma once

// DSH 交互式请求的 UI 与应答：相关 UI 与应答逻辑集中在本文件/源文件，避免散落进主窗口逻辑。

#include <QJsonObject>

class DshApiClient;
class QVBoxLayout;
class QWidget;

class InteractionHandler
{
public:
	// 在对话气泡下方创建提问面板，用户选择/填写后经 DshApiClient::respond() 把答案发回服务端；帧无效则返回 nullptr。
	static QWidget* handleQuestion(const QJsonObject& frame, DshApiClient* api, QVBoxLayout* layout);

	// 在对话气泡下方创建工具审批面板（允许一次 / 拒绝），结果经 DshApiClient::respond() 发送；帧无效则返回 nullptr。
	static QWidget* handleApproval(const QJsonObject& frame, DshApiClient* api, QVBoxLayout* layout);
};
