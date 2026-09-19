#pragma once

// ------------------------------------------------------------------
// InteractionHandler.h
// ------------------------------------------------------------------
// 处理 DSH 的交互式请求：
//   - question/requested：在对话气泡下方创建提问面板并回传答案
//   - approval/requested：在对话气泡下方创建审批面板
//
// 相关 UI 与应答逻辑集中在本文件/源文件，避免散落在主窗口逻辑中。
// ------------------------------------------------------------------

#include <QJsonObject>

class DshApiClient;
class QVBoxLayout;
class QWidget;

class InteractionHandler
{
public:
	/**
	 * 在对话气泡下方创建提问面板。
	 * 用户选择/填写后，通过 DshApiClient::respond() 把答案发送回服务端。
	 *
	 * @return 返回创建的面板；如果帧无效则返回 nullptr
	 */
	static QWidget* handleQuestion(const QJsonObject& frame, DshApiClient* api, QVBoxLayout* layout);

	/**
	 * 在对话气泡下方创建工具审批面板。
	 * 用户选择“允许一次”或“拒绝”后，通过 DshApiClient::respond() 发送审批结果。
	 *
	 * @return 返回创建的面板；如果帧无效则返回 nullptr
	 */
	static QWidget* handleApproval(const QJsonObject& frame, DshApiClient* api, QVBoxLayout* layout);
};