#pragma once

// ------------------------------------------------------------------
// TimingLogger.h
// ------------------------------------------------------------------
// 耗时打点工具：从第一次调用起开始计时，记录“距上次打点”的耗时和
// “进程启动以来”的累计耗时，输出到 qInfo（随 Logger 一并写入
// exe 目录下的 log.txt，见 Logger.cpp）。
//
// 用法（只关心主线程初始化/关键链路的相对耗时即可）：
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

#include <QDebug>
#include <QElapsedTimer>
#include <QString>
#include <QtGlobal>

class TimingLogger
{
public:
	// 打点：打印相对上一个打点的耗时 + 相对进程首次打点的总耗时
	static void mark(const QString& phase)
	{
		const qint64 totalMs = timer().nsecsElapsed() / 1000000;
		const qint64 deltaMs = totalMs - s_lastMs;
		qInfo().noquote() << QStringLiteral("[Timing] %1  +%2ms  (since start %3ms)")
			.arg(phase)
			.arg(deltaMs)
			.arg(totalMs);
		s_lastMs = totalMs;
	}

private:
	// 进程级单一计时器：首次调用 mark 时才创建并启动
	static QElapsedTimer& timer()
	{
		static QElapsedTimer t = []() {
			QElapsedTimer tm;
			tm.start();
			return tm;
		}();
		return t;
	}

	inline static qint64 s_lastMs = 0;
};
