#include "TranslationManager.h"

// ------------------------------------------------------------------
// TranslationUi.cpp
// ------------------------------------------------------------------
// “就地换文案”的界面侧实现：让**构造时写死文案**的界面也能免重启切语言，
// 不必给每个界面手写 retranslateUi()。
//
// 原理（两步，由 Translation::apply() 串起来）：
//   1) snapshotWidgetTexts()：换 translator **之前**调用。拿 .ts 清单里的全部 id，
//      用当前 translator 翻一遍，得到每个 id“此刻显示的样子”，
//      建立「此刻显示文案 -> id」映射。
//      必须在换之前做：换完之后就认不出旧文案了。
//   2) applyWidgetTextSnapshot()：换 translator **之后**调用。遍历所有控件，
//      把命中映射的文案按新语言重译。
//
// 主键是 id 而不是原文。Qt 对查不到的 id 会**原样返回 id 本身**（不返回空串），
// 所以语言包缺条时界面上会直接显示 topbar_settings 这样的代号。
// 这是**刻意保留**的行为，不做拦截：缺哪一条一眼就能看见，比"静默留着旧语言文字"
// 更适合调试。正式发布的语言包条目齐全（工具侧的 id 一致性校验会挡住漏条的包），
// 正常不会露出来。
//
// 为什么不用“每个类 changeEvent + retranslateUi()”：
//   本项目 200+ 条文案散在十几个界面里，逐个手写既费工又极易漏站点；这套机制
//   对所有控件一视同仁，重复执行也是幂等的。已按标准做法接了 changeEvent 的控件
//   （TitleBar / TopBar / PopupWindow / LoadMoreButton / ChatInputWidget）会先自己
//   刷新成新语言，因而不会再命中旧文案映射，不会重复翻译。
//
// 明确不覆盖（不假装做到）：
//   * 用 arg() 拼出来的动态文案（如“共 3 个模型，1 个提供方。”）不是整串文案，
//     匹配不上——它们随数据刷新/重开面板重建；
//   * 已渲染进 HTML 的历史消息（如思考块标题）不是控件文案；
//   * 自绘控件（如侧边栏的会话按钮）用 paintEvent 画字，其“文案”多为用户数据，
//     不属于固定文案。
// ------------------------------------------------------------------

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
			add(edit->placeholderText(),
				[edit](const QString& t) { edit->setPlaceholderText(t); });
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
			add(combo->placeholderText(),
				[combo](const QString& t) { combo->setPlaceholderText(t); });
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

	// 按 id 现算译文。qtTrId 正是 translate(nullptr, id) 的入口，
	// 与界面里 qtTrId("...") 走完全相同的查找路径。
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

		// 用**当前** translator 把每个 id 翻成“此刻的样子”，作为识别键。
		// 当前语言缺这条时译文就等于 id，控件此刻显示的也正是那串 id —— 这种条目
		// **照样入表**：换成一份补齐了这条的语言时才认得出来、能把它救回去，
		// 否则那句会永远卡在代号上。
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

				// 现在已经是新语言了：按 id 查出新译法
				const QString updated = translateBy(it.value());
				// 新语言缺这条时 qtTrId 原样返回 id，界面就直接显示那串代号 —— 刻意不拦。
				// 只挡两种没意义/有害的情况：空译文（会清空控件，比代号更糟），
				// 以及"查出来跟现在显示的一模一样"（重复 set；顺带保证整体幂等）。
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