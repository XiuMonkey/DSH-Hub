#pragma once

// DLL/COM 工具调用的分发：命名管道请求 → 线程池执行 → 回投 GUI 线程发响应。纯 header + inline，无链接改动。

#include "ExtensionSystem/DllCaller.h"
#include "ExtensionSystem/DshNamedPipeBridge.h"

#include <QJsonObject>
#include <QLocalSocket>
#include <QMetaObject>
#include <QPointer>
#include <QRunnable>
#include <QString>
#include <QThreadPool>

namespace ToolRequestDispatcher
{
	namespace detail
	{
		struct PipeJob
		{
			int id = 0;
			QPointer<QLocalSocket> socket;
			QString tool;
			QJsonObject args;
		};

		class ToolTask : public QRunnable
		{
		public:
			ToolTask(DllCaller* dllCaller, DshNamedPipeBridge* bridge,
				int id, const QString& tool, const QJsonObject& args, QLocalSocket* socket)
				: m_dllCaller(dllCaller)
				, m_bridge(bridge)
			{
				auto* job = new PipeJob;
				job->id = id;
				job->socket = socket;
				job->tool = tool;
				job->args = args;
				m_job = job;
			}

			void run() override
			{
				QJsonObject result;
				QString error;
				const bool ok = m_dllCaller->callTool(m_job->tool, m_job->args, result, &error);

				const int jobId = m_job->id;
				const QPointer<QLocalSocket> client = m_job->socket;
				const bool success = ok;
				const QJsonObject payload = result;
				const QString errText = error;
				delete m_job;
				m_job = nullptr;

				// QLocalSocket 只在 GUI 线程读写：响应经 queued 连接回投
				QMetaObject::invokeMethod(m_bridge,
					[bridge = QPointer<DshNamedPipeBridge>(m_bridge),
					jobId, client, success, payload, errText]() {
						if (bridge.isNull() || client.isNull())
							return; // 客户端已断开/桥已销毁，丢弃响应
						if (success)
							bridge->sendResponse(client.data(), jobId, true, payload);
						else
							bridge->sendResponse(client.data(), jobId, false, QJsonObject(), errText);
					},
					Qt::QueuedConnection);
			}

		private:
			DllCaller* m_dllCaller = nullptr;
			DshNamedPipeBridge* m_bridge = nullptr;
			PipeJob* m_job = nullptr;
		};
	} // namespace detail

	// DLL/COM 调用挪到 Worker 线程执行（GUI 不卡、不同 DLL 并行）；同一连接上响应可能乱序，Node 端按请求 id 配对。
	inline void dispatch(DllCaller* dllCaller, DshNamedPipeBridge* bridge, QThreadPool* pool,
		int id, const QString& tool, const QJsonObject& args, QLocalSocket* socket)
	{
		if (!dllCaller || !bridge || !pool)
			return;
		auto* task = new detail::ToolTask(dllCaller, bridge, id, tool, args, socket);
		pool->start(task);
	}
}
