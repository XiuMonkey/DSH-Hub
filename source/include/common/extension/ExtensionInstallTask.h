#pragma once

// 把“后台解压 + 安装 .ext 扩展”封装成任务（无控件）：start() 在 worker 线程跑 ExtensionLoader::loadAndInstall，结果由 tryFinish() 在 UI 定时器里非阻塞取（只成功返回一次）。
// waitForFinished() 供弹窗关闭时等待，避免任务写到已销毁的对象。

#include "ExtensionSystem/ExtensionLoader.h"

#include <QString>

#include <future>

class ExtensionInstallTask
{
public:
	ExtensionInstallTask() = default;
	~ExtensionInstallTask();

	ExtensionInstallTask(const ExtensionInstallTask&) = delete;
	ExtensionInstallTask& operator=(const ExtensionInstallTask&) = delete;

	// 启动后台安装；已有任务在跑时返回 false
	bool start(const QString& extFilePath, const QString& serverProfilePath);

	bool isRunning() const;

	// 任务已结束则回收结果并返回 true（第二次调用返回 false）
	bool tryFinish();

	// tryFinish() 返回 true 之后才有意义
	bool succeeded() const;
	QString errorString() const;
	const ExtensionLoader::LoadedExtension& extension() const;

	// 阻塞等待后台任务结束（弹窗关闭时使用）
	void waitForFinished();

private:
	std::future<bool> m_future;
	ExtensionLoader::LoadedExtension m_extension;
	QString m_error;
	bool m_running = false;
	bool m_succeeded = false;
};
