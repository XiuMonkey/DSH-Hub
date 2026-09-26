#include "common/appearance/TranslationManager.h"

// “就地换文案”：构造时写死文案的界面也能免重启切语言，不必逐个手写 retranslateUi()。
// apply() 两步顺序不可换：换 translator 前用当前译文建「此刻显示文案 -> id」映射（换完认不出旧文案），
// 换之后遍历控件按 id 重译，幂等。主键用 id 而非原文 —— Qt 查不到的 id 会原样返回 id，缺条时就显示
// topbar_settings 这类代号，刻意保留便于调试；发布包条目齐全，正常不露出。
// 已按标准做法接 changeEvent 的控件（TitleBar / TopBar / PopupWindow / LoadMoreButton / ChatInputWidget）
// 会先自刷新，因而不会命中旧映射。不覆盖 arg() 拼出的动态文案、已渲染进 HTML 的历史消息、自绘控件。

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QDebug>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QTabWidget>
#include <QWidget>
#include <functional>
#include <utility>

namespace
{
	// 一个控件的文案槽位：取出来、写回去
	struct TextSlot
	{
		QString text;
		std::function<void(const QString&)> apply;
	};

	// 把某个控件的所有“固定文案”枚举出来（只列本项目实际用到的几类）
	void collectSlots(QWidget* widget, QVector<TextSlot>& targets)
	{
		const auto add = [&targets](const QString& text, std::function<void(const QString&)> apply) {
			if (!text.isEmpty())
				targets.append(TextSlot{ text, std::move(apply) });
			};

		add(widget->windowTitle(), [widget](const QString& t) { widget->setWindowTitle(t); });
		add(widget->toolTip(), [widget](const QString& t) { widget->setToolTip(t); });

		if (auto* label = qobject_cast<QLabel*>(widget)) {
			add(label->text(), [label](const QString& t) { label->setText(t); });
		}
		if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
			// QPushButton / QCheckBox / QRadioButton 等都走这一支
			add(button->text(), [button](const QString& t) { button->setText(t); });
		}
		if (auto* edit = qobject_cast<QLineEdit*>(widget)) {
			add(edit->placeholderText(), [edit](const QString& t) { edit->setPlaceholderText(t); });
		}
		if (auto* group = qobject_cast<QGroupBox*>(widget)) {
			add(group->title(), [group](const QString& t) { group->setTitle(t); });
		}
		if (auto* combo = qobject_cast<QComboBox*>(widget)) {
			for (int i = 0; i < combo->count(); ++i) {
				const QString itemText = combo->itemText(i);
				if (itemText.isEmpty())
					continue;
				targets.append(TextSlot{ itemText, [combo, i](const QString& t) {
					if (i < combo->count())
						combo->setItemText(i, t);
					} });
			}
			add(combo->placeholderText(), [combo](const QString& t) { combo->setPlaceholderText(t); });
		}
		if (auto* tabs = qobject_cast<QTabWidget*>(widget)) {
			for (int i = 0; i < tabs->count(); ++i) {
				const QString tabText = tabs->tabText(i);
				if (tabText.isEmpty())
					continue;
				targets.append(TextSlot{ tabText, [tabs, i](const QString& t) {
					if (i < tabs->count())
						tabs->setTabText(i, t);
					} });
			}
		}
	}

	// qtTrId 即 translate(nullptr, id)，与界面里 qtTrId("...") 走同一条查找路径
	QString translateBy(const TranslationSource& entry)
	{
		const QByteArray id = entry.id.toUtf8();
		return qtTrId(id.constData());
	}
}

namespace Translation
{
	WidgetTextSnapshot snapshotWidgetTexts()
	{
		WidgetTextSnapshot snapshot;
		const QVector<TranslationSource> sources = translationSources();
		if (sources.isEmpty()) {
			// 没有 .ts 清单（例如只分发了 .qm）：降级为不做，不影响其它功能
			qInfo().noquote() << QStringLiteral("[Translation] no .ts manifest under")
				<< translationsDir()
				<< QStringLiteral("-> in-place retranslate disabled");
			return snapshot;
		}

		// 用**当前** translator 把每个 id 翻成“此刻的样子”作为识别键。缺条的 id 译文等于 id 本身、
		// 控件此刻显示的也正是那串 id，这种条目**照样入表**：换成补齐了这条的语言时才认得出来。
		for (const TranslationSource& entry : sources) {
			const QString currentText = translateBy(entry);
			// 空译文会把控件清空，比显示代号更糟，这一条仍然不入表
			if (currentText.isEmpty())
				continue;
			if (!snapshot.contains(currentText))
				snapshot.insert(currentText, entry);
		}
		return snapshot;
	}

	void applyWidgetTextSnapshot(const WidgetTextSnapshot& snapshot)
	{
		if (snapshot.isEmpty())
			return;

		int replaced = 0;

		// allWidgets() 含隐藏控件，因此未打开的弹窗也会被换到（下次打开即新语言）
		const QWidgetList widgets = QApplication::allWidgets();
		for (QWidget* widget : widgets) {
			if (!widget)
				continue;

			QVector<TextSlot> targets;
			collectSlots(widget, targets);

			for (const TextSlot& slot : targets) {
				const auto it = snapshot.constFind(slot.text);
				if (it == snapshot.constEnd())
					continue;

				// 现在是新语言了，按 id 查新译法。新语言缺这条时 qtTrId 原样返回 id，界面直接显示代号 ——
				// 刻意不拦。只挡两种有害情况：空译文（会清空控件）、查出来与当前显示相同（重复 set，保证幂等）。
				const QString updated = translateBy(it.value());
				if (updated.isEmpty() || updated == slot.text)
					continue;

				slot.apply(updated);
				++replaced;
			}
		}

		qInfo().noquote() << QStringLiteral("[Translation] in-place retranslate: %1 text(s)")
			.arg(replaced);
	}
}
