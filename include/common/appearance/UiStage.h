#pragma once

// ------------------------------------------------------------------
// UiStage.h —— "架空原 UI"：把宿主的整个客户区让给客户端扩展自绘
// ------------------------------------------------------------------
// 为什么独立成一个模块、而不是塞进 DSHHub：
//   1) 判定方有两处，且分属不同层 ——
//        DSHHub（谁在架空、抢台 / 还台）在 core/
//        WindowFrame（遮罩覆盖范围、窗口条命中测试）在 common/appearance/
//      共同状态只能放在两者都能依赖的下层，所以放这儿。
//   2) 让渡动作与"宿主窗口具体是哪个类"无关（只要是个 QMainWindow），
//      放这里的复用门槛最低。
//   3) 宿主侧改动因此能压到"接口 + if"：DSHHub 只转发（见 DSHHub.h 里那 4 个
//      内联 override），WindowFrame 只问一句 isTakenOver() / captionBand()，
//      真正的让渡动作全在本文件。
//
// 线程：全部只允许 GUI 线程调用（与 CommonRegistry、UI 一致）。
//       非 GUI 线程一律拒绝，并对每个入口只告警一次。
// ------------------------------------------------------------------

#include <QString>

class QMainWindow;
class QWidget;

namespace UiStage
{
	// 抢台。owner 必须等于扩展的安装目录名（宿主的 ClientExtension 会在
	// attachHost() 之前用可选槽 setHostIdentity 把它推给插件），宿主靠它做
	// 所有权校验与"卸载时强制收台"。
	//
	// 成功返回空舞台（已挂进窗口、已是当前可见面）；失败返回 nullptr：
	//   · 已被别人占着        → 拒绝（同一时刻只允许一个 owner，绝不静默顶掉）
	//   · owner 为空 / 非 GUI 线程 → 拒绝
	//   · 同一 owner 重复调用 → **幂等**，返回同一个舞台
	//     （切主题会重建窗口并对同一插件实例再调一次 attachHost()，
	//       而"重挂"正是那次调用该做的事）
	QWidget* acquire(QMainWindow* host, const QString& owner);

	// 还台。owner 为空 = 不问身份（宿主的兜底路径用）。
	// owner 非空且与当前不符 → 拒绝并返回 false。
	//
	// ⚠️ 舞台里的控件归扩展，本函数**不删**它们：那个 vtable 可能已经不在本模块
	//    里了（卸载扩展后 dll 已解映射），删一次就是崩。扩展必须在还台同一时刻
	//    自己删干净 —— 这是 DshHostPlugin.h 里 detachHost() 的既有契约。
	bool release(QMainWindow* host, const QString& owner);

	// 在所有顶层窗口上，把 owner 占的台收掉（扩展被移除时的兜底，宿主调用）。
	// 返回收掉的数量。
	//
	// 为什么必须有这条兜底：不能指望插件在自己的 detachHost() 里调
	// ExternalReleaseStage —— 插件崩了或忘了的话，宿主控件树里会留着一个 vtable
	// 指向已解映射内存的控件，unload() 之后碰一下就崩。
	int releaseForOwner(const QString& owner);

	// 当前 owner（空 = 没人在架空）
	QString ownerOf(const QWidget* host);

	// host 上是否有人在架空
	bool isTakenOver(const QWidget* host);

	// 自绘窗口条：顶部哪一条算窗口拖动区（逻辑像素）。
	// 扩展自绘标题栏时必须调一次，否则窗口只能靠 Alt+Space 拖动 / 贴边吸附。
	void setCaptionBand(QWidget* host, int top, int height);

	// 查询窗口条（供 WindowFrame::hitTest 回落用）。height > 0 才算有效。
	bool captionBand(const QWidget* host, int* top, int* height);
}
