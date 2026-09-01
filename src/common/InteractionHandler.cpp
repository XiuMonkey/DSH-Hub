#include "InteractionHandler.h"

#include "DshApiClient.h"
#include "ThemeManager.h"

#include <QAbstractButton>
#include <QButtonGroup>
#include <QCheckBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QObject>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>
#include <QWidget>

namespace
{
	struct UiQuestion
	{
		QString id;
		bool multiSelect = false;
		QList<QAbstractButton*> optionButtons;
		QButtonGroup* singleGroup = nullptr;
		QLineEdit* customEdit = nullptr;
	};

	QJsonArray stringListToJsonArray(const QStringList& values)
	{
		QJsonArray array;
		for (const QString& value : values)
			array.append(value);
		return array;
	}

	QWidget* createInteractionPanel(QVBoxLayout* layout)
	{
		auto* panel = new QWidget;
		panel->setObjectName(QStringLiteral("interactionPanel"));
		panel->setAttribute(Qt::WA_StyledBackground, true);
		panel->setStyleSheet(
			QStringLiteral("QWidget#interactionPanel {")
			+ QStringLiteral("  background: ") + Theme::color(QStringLiteral("panelBg")) + QStringLiteral(";")
			+ QStringLiteral("  border: 1px solid ") + Theme::color(QStringLiteral("border")) + QStringLiteral(";")
			+ QStringLiteral("  border-radius: 12px;")
			+ QStringLiteral("}")
			+ QStringLiteral("QWidget#interactionPanel QLabel {")
			+ QStringLiteral("  background: transparent;")
			+ QStringLiteral("  color: ") + Theme::color(QStringLiteral("textPrimary")) + QStringLiteral(";")
			+ QStringLiteral("}")
			+ QStringLiteral("QWidget#interactionPanel QFrame#questionCard {")
			+ QStringLiteral("  background: ") + Theme::color(QStringLiteral("hoverBg")) + QStringLiteral(";")
			+ QStringLiteral("  border-radius: 10px;")
			+ QStringLiteral("}")
			+ QStringLiteral("QWidget#interactionPanel QRadioButton,")
			+ QStringLiteral("QWidget#interactionPanel QCheckBox {")
			+ QStringLiteral("  background: transparent;")
			+ QStringLiteral("  spacing: 8px;")
			+ QStringLiteral("  font-size: 13px;")
			+ QStringLiteral("  color: ") + Theme::color(QStringLiteral("textPrimary")) + QStringLiteral(";")
			+ QStringLiteral("}")
			+ QStringLiteral("QWidget#interactionPanel QRadioButton::indicator {")
			+ QStringLiteral("  width: 18px;")
			+ QStringLiteral("  height: 18px;")
			+ QStringLiteral("  border-radius: 9px;")
			+ QStringLiteral("  border: 2px solid ") + Theme::color(QStringLiteral("border")) + QStringLiteral(";")
			+ QStringLiteral("  background: transparent;")
			+ QStringLiteral("}")
			+ QStringLiteral("QWidget#interactionPanel QRadioButton::indicator:hover {")
			+ QStringLiteral("  border-color: ") + Theme::color(QStringLiteral("accent")) + QStringLiteral(";")
			+ QStringLiteral("}")
			+ QStringLiteral("QWidget#interactionPanel QRadioButton::indicator:checked {")
			+ QStringLiteral("  border-color: ") + Theme::color(QStringLiteral("accent")) + QStringLiteral(";")
			+ QStringLiteral("  background: ") + Theme::color(QStringLiteral("accent")) + QStringLiteral(";")
			+ QStringLiteral("}")
			+ QStringLiteral("QWidget#interactionPanel QCheckBox::indicator {")
			+ QStringLiteral("  width: 18px;")
			+ QStringLiteral("  height: 18px;")
			+ QStringLiteral("  border-radius: 5px;")
			+ QStringLiteral("  border: 2px solid ") + Theme::color(QStringLiteral("border")) + QStringLiteral(";")
			+ QStringLiteral("  background: transparent;")
			+ QStringLiteral("}")
			+ QStringLiteral("QWidget#interactionPanel QCheckBox::indicator:hover {")
			+ QStringLiteral("  border-color: ") + Theme::color(QStringLiteral("accent")) + QStringLiteral(";")
			+ QStringLiteral("}")
			+ QStringLiteral("QWidget#interactionPanel QCheckBox::indicator:checked {")
			+ QStringLiteral("  border-color: ") + Theme::color(QStringLiteral("accent")) + QStringLiteral(";")
			+ QStringLiteral("  background: ") + Theme::color(QStringLiteral("accent")) + QStringLiteral(";")
			+ QStringLiteral("}")
			+ QStringLiteral("QWidget#interactionPanel QLineEdit {")
			+ QStringLiteral("  background: ") + Theme::color(QStringLiteral("inputBg")) + QStringLiteral(";")
			+ QStringLiteral("  border: 1px solid ") + Theme::color(QStringLiteral("border")) + QStringLiteral(";")
			+ QStringLiteral("  border-radius: 8px;")
			+ QStringLiteral("  padding: 6px 10px;")
			+ QStringLiteral("}")
			+ QStringLiteral("QWidget#interactionPanel QPushButton {")
			+ QStringLiteral("  background: ") + Theme::color(QStringLiteral("accent")) + QStringLiteral(";")
			+ QStringLiteral("  color: ") + Theme::color(QStringLiteral("textOnAccent")) + QStringLiteral(";")
			+ QStringLiteral("  border: none;")
			+ QStringLiteral("  border-radius: 8px;")
			+ QStringLiteral("  padding: 6px 16px;")
			+ QStringLiteral("}")
			+ QStringLiteral("QWidget#interactionPanel QPushButton:hover {")
			+ QStringLiteral("  background: ") + Theme::color(QStringLiteral("accentHover")) + QStringLiteral(";")
			+ QStringLiteral("}")
			+ QStringLiteral("QWidget#interactionPanel QPushButton#approvalRejectButton {")
			+ QStringLiteral("  background: transparent;")
			+ QStringLiteral("  color: ") + Theme::color(QStringLiteral("danger")) + QStringLiteral(";")
			+ QStringLiteral("  border: 1px solid ") + Theme::color(QStringLiteral("danger")) + QStringLiteral(";")
			+ QStringLiteral("}")
			+ QStringLiteral("QWidget#interactionPanel QPushButton#approvalRejectButton:hover {")
			+ QStringLiteral("  background: ") + Theme::color(QStringLiteral("dangerBg")) + QStringLiteral(";")
			+ QStringLiteral("}"));

		auto* panelLayout = new QVBoxLayout(panel);
		panelLayout->setContentsMargins(14, 12, 14, 12);
		panelLayout->setSpacing(8);

		layout->addWidget(panel);
		return panel;
	}

	QFrame* createQuestionCard(QWidget* panel, QVBoxLayout* panelLayout)
	{
		auto* card = new QFrame(panel);
		card->setObjectName(QStringLiteral("questionCard"));
		card->setAttribute(Qt::WA_StyledBackground, true);

		auto* cardLayout = new QVBoxLayout(card);
		cardLayout->setContentsMargins(12, 10, 12, 10);
		cardLayout->setSpacing(6);

		panelLayout->addWidget(card);
		return card;
	}
} // namespace

QWidget* InteractionHandler::handleQuestion(const QJsonObject& frame, DshApiClient* api, QVBoxLayout* layout)
{
	if (!api || !layout)
		return nullptr;

	const QString rpcId = frame.value(QStringLiteral("rpcId")).toString();
	const QJsonObject payload = frame.value(QStringLiteral("payload")).toObject();
	const QString sessionId = payload.value(QStringLiteral("sessionId")).toString();
	const QJsonArray questions = payload.value(QStringLiteral("questions")).toArray();

	if (rpcId.isEmpty() || sessionId.isEmpty() || questions.isEmpty())
		return nullptr;

	QWidget* panel = createInteractionPanel(layout);
	auto* panelLayout = static_cast<QVBoxLayout*>(panel->layout());

	auto* hint = new QLabel(QStringLiteral("需要你确认以下问题"), panel);
	hint->setStyleSheet(QStringLiteral("font-size: 14px; font-weight: 600;"));
	panelLayout->addWidget(hint);

	QList<UiQuestion> uiQuestions;

	for (const auto& questionValue : questions) {
		const QJsonObject question = questionValue.toObject();

		UiQuestion ui;
		ui.id = question.value(QStringLiteral("id")).toString();
		ui.multiSelect = question.value(QStringLiteral("multiSelect")).toBool();

		QFrame* card = createQuestionCard(panel, panelLayout);
		auto* cardLayout = static_cast<QVBoxLayout*>(card->layout());

		const QString header = question.value(QStringLiteral("header")).toString();
		if (!header.isEmpty()) {
			auto* headerLabel = new QLabel(
				QStringLiteral("<b>%1</b>").arg(header.toHtmlEscaped()), card);
			headerLabel->setWordWrap(true);
			cardLayout->addWidget(headerLabel);
		}

		auto* questionLabel = new QLabel(
			question.value(QStringLiteral("question")).toString(), card);
		questionLabel->setWordWrap(true);
		cardLayout->addWidget(questionLabel);

		const QJsonArray options = question.value(QStringLiteral("options")).toArray();
		if (!options.isEmpty()) {
			if (!ui.multiSelect) {
				ui.singleGroup = new QButtonGroup(card);
				ui.singleGroup->setExclusive(true);
			}

			for (const auto& optionValue : options) {
				const QJsonObject option = optionValue.toObject();
				const QString label = option.value(QStringLiteral("label")).toString();
				if (label.isEmpty())
					continue;

				QAbstractButton* button = nullptr;
				if (ui.multiSelect)
					button = new QCheckBox(label, card);
				else {
					button = new QRadioButton(label, card);
					ui.singleGroup->addButton(button);
				}

				const QString description = option.value(QStringLiteral("description")).toString();
				if (!description.isEmpty())
					button->setToolTip(description);

				cardLayout->addWidget(button);
				ui.optionButtons.append(button);
			}

			// 单选且只有一个选项时默认选中，减少用户操作
			if (!ui.multiSelect && ui.optionButtons.size() == 1)
				ui.optionButtons.first()->setChecked(true);
		}
		else {
			ui.customEdit = new QLineEdit(card);
			ui.customEdit->setPlaceholderText(QStringLiteral("请输入你的回答"));
			cardLayout->addWidget(ui.customEdit);
		}

		uiQuestions.append(ui);
	}

	auto* submitButton = new QPushButton(QStringLiteral("提交答案"), panel);
	panelLayout->addWidget(submitButton, 0, Qt::AlignRight);

	QObject::connect(submitButton, &QPushButton::clicked, panel, [panel, api, rpcId, sessionId, uiQuestions]() {
		QJsonObject answerPayload;
		answerPayload.insert(QStringLiteral("sessionId"), sessionId);

		QJsonArray answers;
		for (const UiQuestion& ui : uiQuestions) {
			QStringList selected;
			for (QAbstractButton* button : ui.optionButtons) {
				if (button->isChecked())
					selected.append(button->text());
			}

			QJsonObject answer;
			answer.insert(QStringLiteral("id"), ui.id);
			answer.insert(QStringLiteral("selected"), stringListToJsonArray(selected));

			if (ui.customEdit) {
				const QString custom = ui.customEdit->text().trimmed();
				if (!custom.isEmpty())
					answer.insert(QStringLiteral("custom"), custom);
			}

			answers.append(answer);
		}

		QJsonObject answer;
		answer.insert(QStringLiteral("answers"), answers);
		answerPayload.insert(QStringLiteral("answer"), answer);

		api->respond(rpcId, answerPayload);
		panel->deleteLater();
		});

	return panel;
}

QWidget* InteractionHandler::handleApproval(const QJsonObject& frame, DshApiClient* api, QVBoxLayout* layout)
{
	if (!api || !layout)
		return nullptr;

	const QString rpcId = frame.value(QStringLiteral("rpcId")).toString();
	const QJsonObject payload = frame.value(QStringLiteral("payload")).toObject();
	const QString sessionId = payload.value(QStringLiteral("sessionId")).toString();
	const QString approvalId = payload.value(QStringLiteral("approvalId")).toString();
	const QString toolName = payload.value(QStringLiteral("toolName")).toString();
	const QString reason = payload.value(QStringLiteral("reason")).toString();

	if (rpcId.isEmpty() || sessionId.isEmpty() || approvalId.isEmpty())
		return nullptr;

	QWidget* panel = createInteractionPanel(layout);
	auto* panelLayout = static_cast<QVBoxLayout*>(panel->layout());

	auto* title = new QLabel(QStringLiteral("工具调用请求审批"), panel);
	title->setStyleSheet(QStringLiteral("font-size: 14px; font-weight: 600;"));
	panelLayout->addWidget(title);

	auto* toolLabel = new QLabel(
		QStringLiteral("工具：%1").arg(toolName.toHtmlEscaped()), panel);
	toolLabel->setWordWrap(true);
	panelLayout->addWidget(toolLabel);

	if (!reason.isEmpty()) {
		auto* reasonLabel = new QLabel(
			QStringLiteral("原因：%1").arg(reason.toHtmlEscaped()), panel);
		reasonLabel->setWordWrap(true);
		panelLayout->addWidget(reasonLabel);
	}

	auto* buttonRow = new QHBoxLayout;
	buttonRow->setSpacing(8);

	auto* rejectButton = new QPushButton(QStringLiteral("拒绝"), panel);
	rejectButton->setObjectName(QStringLiteral("approvalRejectButton"));
	auto* allowButton = new QPushButton(QStringLiteral("允许一次"), panel);

	buttonRow->addStretch();
	buttonRow->addWidget(rejectButton);
	buttonRow->addWidget(allowButton);
	panelLayout->addLayout(buttonRow);

	QObject::connect(allowButton, &QPushButton::clicked, panel, [panel, api, rpcId, sessionId, approvalId]() {
		QJsonObject answer;
		answer.insert(QStringLiteral("sessionId"), sessionId);
		answer.insert(QStringLiteral("approvalId"), approvalId);
		answer.insert(QStringLiteral("outcome"), QStringLiteral("allowed-once"));

		api->respond(rpcId, answer);
		panel->deleteLater();
		});

	QObject::connect(rejectButton, &QPushButton::clicked, panel, [panel, api, rpcId, sessionId, approvalId]() {
		QJsonObject answer;
		answer.insert(QStringLiteral("sessionId"), sessionId);
		answer.insert(QStringLiteral("approvalId"), approvalId);
		answer.insert(QStringLiteral("outcome"), QStringLiteral("rejected"));

		api->respond(rpcId, answer);
		panel->deleteLater();
		});

	return panel;
}