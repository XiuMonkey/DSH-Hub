#pragma once

// Logger（实现见 Logger.cpp）：Qt 消息处理，按模块 logs/<module>/ 与级别 debug/info/warn/error/fatal 分开落盘。
// TimingLogger：耗时打点工具，相邻两个 mark 的差值 = 该阶段耗时，按时间顺序打点即可从日志还原整条初始化链路的分段耗时（输出 qInfo，随 Logger 写入 logs/timing/info.log）。

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