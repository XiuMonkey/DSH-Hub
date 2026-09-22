#pragma once

// DSH 交互式请求的 UI 与应答：question/requested 在对话气泡下方创建提问面板并回传答案、approval/requested 创建审批面板；相关 UI 与应答逻辑集中在本文件/源文件，避免散落进主窗口逻辑。

#include <QJsonObject>

class DshApiClient;
class QVBoxLayout;
class QWidget;

class InteractionHandler
{
public:
	// 在对话气泡下方创建提问面板；用户选择/填写后通过 DshApiClient::respond() 把答案发送回服务端。返回创建的面板，帧无效则返回 nullptr。
	static QWidget* handleQuestion(const QJsonObject& frame, DshApiClient* api, QVBoxLayout* layout);

	// 在对话气泡下方创建工具审批面板（用户选择“允许一次”或“拒绝”）；结果通过 DshApiClient::respond() 发送。返回创建的面板，帧无效则返回 nullptr。
	static QWidget* handleApproval(const QJsonObject& frame, DshApiClient* api, QVBoxLayout* layout);
};
