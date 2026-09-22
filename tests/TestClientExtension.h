#pragma once

#include <QObject>
#include <QString>

// ------------------------------------------------------------------
// TestClientExtension
// ------------------------------------------------------------------
// 盯的是"移除客户端扩展"这条路径 —— 用户报过一个 bug：移除之后目录里还剩一个
// main.dll（Windows 上被本进程映射着的 dll 删不掉），看上去像没卸干净。
//
// 修好之后的行为（也是这里固定的契约）：
//   · 没被占用   → remove() 把目录整个删掉，返回 true；
//   · 被占用     → 判据 regulation.json5 照样删掉（**已卸载**：不再出现在
//                  installedNames，下次启动也不会装载），目录留下并写
//                  .pending-removal 标记，remove() 返回 false 且 error 里带说明；
//   · 下次启动   → loadAll() 开头的清扫把带标记（或"没判据却还有 dll"）的目录删掉。
//
// 测试全程只碰自己的临时目录（DSHHUB_CLIENT_EXTENSION_DIR 被指过去），
// 绝不碰真的 <exe>/clientExtensions。
// ------------------------------------------------------------------

class TestClientExtension : public QObject
{
	Q_OBJECT

private slots:
	void initTestCase();
	void cleanupTestCase();

	// 已安装的判据 = 目录里有 regulation.json5
	void installedNamesReflectsRegulation();

	// 没被占用：一次删干净
	void removeDeletesEverythingWhenUnlocked();

	// 被占用：判据已删、目录留标记、如实返回 false
	void removeMarksLeftoverWhenDllLocked();

	// 下次启动的清扫收尾（故意接在上一条之后 —— 复刻"移除 → 重启"的真实顺序）
	void sweepOnNextStartClearsLeftover();

private:
	// 造一个假扩展目录，返回目录绝对路径
	QString makeExtensionDir(const QString& name, bool withRegulation) const;

	// 用 Win32 句柄占住 main.dll（share 里不给 FILE_SHARE_DELETE =
	// 等价于"dll 被本进程映射着"）。非 Windows 上返回 false，相关用例 QSKIP。
	bool lockMainDll(const QString& dir);
	void unlockMainDll();

	QString m_root;
	void* m_lockHandle = nullptr;   // HANDLE；类型留在 .cpp，头文件不引 windows.h
};
