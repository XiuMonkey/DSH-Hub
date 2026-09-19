#pragma once

// ------------------------------------------------------------------
// Logger / TimingLogger（统一日志头，实现见 Logger.cpp）
// ------------------------------------------------------------------
// Logger：Qt 消息处理，按模块（logs/<module>/）与级别
// （debug/info/warn/error/fatal）分开落盘。
//
// TimingLogger：耗时打点工具，从第一次调用起开始计时，记录“距上次打点”的
// 耗时和“进程启动以来”的累计耗时，输出到 qInfo（ INFO 级别，随 Logger
// 写入 logs/timing/info.log）。
//
// 用法（TimingLogger 只关心主线程初始化/关键链路的相对耗时即可）：
//   TimingLogger::mark("server spawned");
//   TimingLogger::mark("server baseUrl ready");
//
// 输出示例：
//   [Timing] server spawned              +412ms  (since start 1234ms)
//   [Timing] server baseUrl ready        +2805ms (since start 4039ms)
//
// 相邻两个 mark 之间的差值 = 该阶段耗时，因此只要按时间顺序打点，
// 即可从日志还原整条初始化链路的分段耗时。
// ------------------------------------------------------------------

#include <QString>
#include <QtGlobal>

class QElapsedTimer;

class Logger
{
public:
	static void init();
};

class TimingLogger
{
public:
	// 打点：打印相对上一个打点的耗时 + 相对进程首次打点的总耗时
	static void mark(const QString& phase);

private:
	// 进程级单一计时器：首次调用 mark 时才创建并启动
	static QElapsedTimer& timer();

	inline static qint64 s_lastMs = 0;
};