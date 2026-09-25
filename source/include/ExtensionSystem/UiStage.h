#pragma once

// "架空原 UI"：把宿主整个客户区让给客户端扩展自绘（抢台 / 还台 / 自绘窗口条）。
// ⚠️ 只允许 GUI 线程调用，非 GUI 线程一律拒绝并只告警一次。

#include <QString>

class QMainWindow;
class QWidget;

namespace UiStage
{
	// 抢台。owner 必须是扩展安装目录名；失败返回 nullptr（已被占 / owner 空 / 非 GUI 线程），同 owner 幂等
	QWidget* acquire(QMainWindow* host, const QString& owner);

	// 还台。owner 为空 = 不问身份，不符则拒绝。⚠️ 舞台里的控件归扩展，本函数不删（vtable 可能已随 dll 解映射）
	bool release(QMainWindow* host, const QString& owner);

	// 兜底：收掉 owner 在所有顶层窗口占的台；插件崩了或忘了 release 会留下 vtable 已解映射的控件
	int releaseForOwner(const QString& owner);

	// 当前 owner（空 = 没人在架空）
	QString ownerOf(const QWidget* host);

	bool isTakenOver(const QWidget* host);

	// 顶部哪一条算窗口拖动区；自绘标题栏时必须调，否则只能靠 Alt+Space
	void setCaptionBand(QWidget* host, int top, int height);

	bool captionBand(const QWidget* host, int* top, int* height);
}
