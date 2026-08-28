#pragma once

// ------------------------------------------------------------------
// InteractionHandler.h
// ------------------------------------------------------------------
// 处理 DSH 的交互式请求：
//   - question/requested：以对话框形式向用户提问并回传答案
//   - approval/requested：以对话框形式让用户批准或拒绝工具调用
//
// 相关 UI 与应答逻辑集中在本文件/源文件，避免散落在主窗口逻辑中。
// ------------------------------------------------------------------

#include <QJsonObject>

class DshApiClient;
class QWidget;

class InteractionHandler
{
public:
    /**
     * 显示 DSH 提问对话框。
     * 用户选择/填写后，通过 DshApiClient::respond() 把答案发送回服务端。
     *
     * @return true 表示成功识别并处理了 question/requested 帧
     */
    static bool handleQuestion(const QJsonObject &frame, DshApiClient *api, QWidget *parent);

    /**
     * 显示工具审批对话框。
     * 用户选择“允许一次”或“拒绝”后，通过 DshApiClient::respond() 发送审批结果。
     *
     * @return true 表示成功识别并处理了 approval/requested 帧
     */
    static bool handleApproval(const QJsonObject &frame, DshApiClient *api, QWidget *parent);
};