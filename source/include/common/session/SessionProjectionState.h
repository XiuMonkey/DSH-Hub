#pragma once

// 小灰字（会话统计）的本地投影合并态（纯 header + inline，无链接改动）：两块投影分开记 —— 哪个来源
// 带了哪块就更新哪块，没带的那块保持原样（早先整包覆盖时，只带 tokenUsage 的列表行会把快照带来的
// "轮/步 + 耗时"那段擦掉）。两个来源都走这里：session/follow 快照里的全量折叠、session/control 的
// baseline 与实时帧。
//
// 合并规则只有一条「服务端 higher-seq-wins」—— 比已经并进来的旧就整帧丢弃（列表行带的是很旧的检查点，
// 所以这个判断是必需的），最容易写错，所以封在单点。
// ⚠️ 这里只负责合并 JSON，不负责拼行文本。SessionUsageStats / parseSessionUsage 定义在
//   ui/ChatInputWidget.h，本类在 common/session/ 下不能反向依赖 UI 层，所以对外只吐 QJsonObject。

#include <QJsonObject>
#include <QJsonValue>
#include <QString>

class SessionProjectionState
{
public:
	// 小灰字只显示这两块投影键（@deepseek-ai/dsh-session-stats / dsh-token-meter）。判断必须留在合并
	// 之前：别的 key 的帧如果放进来，会把 asOfSeq 顶高，接着真正要用的那块就被 higher-seq-wins 丢掉。
	static bool handlesKey(const QString& key)
	{
		return key == QStringLiteral("sessionStats") || key == QStringLiteral("tokenUsage");
	}

	// 并入一帧投影。返回 false = 这一帧比已并进来的旧（higher-seq-wins），调用方整帧丢弃。
	bool merge(int asOfSeq, const QJsonObject& values)
	{
		if (asOfSeq < m_asOfSeq)
			return false;
		m_asOfSeq = asOfSeq;

		const QJsonValue sessionStats = values.value(QStringLiteral("sessionStats"));
		if (sessionStats.isObject())
			m_sessionStatsBlock = sessionStats.toObject();

		const QJsonValue tokenUsage = values.value(QStringLiteral("tokenUsage"));
		if (tokenUsage.isObject())
			m_tokenUsageBlock = tokenUsage.toObject();

		return true;
	}

	// 换会话时必须调：新会话的投影序号要从头算，否则旧会话的数字会留在新会话上。
	void reset()
	{
		m_sessionStatsBlock = QJsonObject();
		m_tokenUsageBlock = QJsonObject();
		m_asOfSeq = -1;
	}

	// 两块合并后的整包，交给 parseSessionUsage 拼小灰字那一行。
	// 注意：两块都空时也照样返回空对象 —— 全 0 也要交给控件（那条小灰字是"始终显示"的）。
	QJsonObject merged() const
	{
		QJsonObject merged;
		if (!m_sessionStatsBlock.isEmpty())
			merged.insert(QStringLiteral("sessionStats"), m_sessionStatsBlock);
		if (!m_tokenUsageBlock.isEmpty())
			merged.insert(QStringLiteral("tokenUsage"), m_tokenUsageBlock);
		return merged;
	}

	// 已并进来的投影反映到哪个 seq
	int asOfSeq() const { return m_asOfSeq; }

private:
	QJsonObject m_sessionStatsBlock;  // values.sessionStats
	QJsonObject m_tokenUsageBlock;  // values.tokenUsage
	int m_asOfSeq = -1;
};
