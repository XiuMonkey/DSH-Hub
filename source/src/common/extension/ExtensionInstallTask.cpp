#include "common/extension/ExtensionInstallTask.h"

#include <chrono>
#include <utility>

ExtensionInstallTask::~ExtensionInstallTask()
{
	// std::async 的 future 析构会等待任务结束，这里显式写出来表明意图：绝不能让后台线程继续写
	// 已经消失的 this。
	waitForFinished();
}

bool ExtensionInstallTask::start(const QString& extFilePath, const QString& serverProfilePath)
{
	if (isRunning())
		return false;

	m_running = true;
	m_succeeded = false;
	m_error.clear();
	m_extension = ExtensionLoader::LoadedExtension();

	m_future = std::async(std::launch::async, [this, extFilePath, serverProfilePath]() {
		ExtensionLoader loader;
		return loader.loadAndInstall(extFilePath, serverProfilePath, &m_extension, &m_error);
		});

	return true;
}

bool ExtensionInstallTask::isRunning() const
{
	return m_running;
}

bool ExtensionInstallTask::tryFinish()
{
	if (!m_future.valid())
		return false;

	if (m_future.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
		return false;

	m_succeeded = m_future.get();
	m_running = false;
	return true;
}

bool ExtensionInstallTask::succeeded() const
{
	return m_succeeded;
}

QString ExtensionInstallTask::errorString() const
{
	return m_error;
}

const ExtensionLoader::LoadedExtension& ExtensionInstallTask::extension() const
{
	return m_extension;
}

void ExtensionInstallTask::waitForFinished()
{
	if (m_future.valid())
		m_future.wait();
	m_running = false;
}
