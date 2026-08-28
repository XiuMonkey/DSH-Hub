#include "InteractionHandler.h"

#include "DshApiClient.h"

#include <QAbstractButton>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QJsonArray>
#include <QLabel>
#include <QObject>
#include <QLineEdit>
#include <QPushButton>
#include <QRadioButton>
#include <QVBoxLayout>

namespace
{

struct UiQuestion
{
    QString id;
    bool multiSelect = false;
    QList<QAbstractButton *> optionButtons;
    QLineEdit *customEdit = nullptr;
};

QJsonArray stringListToJsonArray(const QStringList &values)
{
    QJsonArray array;
    for (const QString &value : values)
        array.append(value);
    return array;
}

} // namespace

bool InteractionHandler::handleQuestion(const QJsonObject &frame, DshApiClient *api, QWidget *parent)
{
    if (!api)
        return false;

    const QString rpcId = frame.value(QStringLiteral("rpcId")).toString();
    const QJsonObject payload = frame.value(QStringLiteral("payload")).toObject();
    const QString sessionId = payload.value(QStringLiteral("sessionId")).toString();
    const QJsonArray questions = payload.value(QStringLiteral("questions")).toArray();

    if (rpcId.isEmpty() || sessionId.isEmpty() || questions.isEmpty())
        return false;

    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("DSH 提问"));
    dialog.setMinimumWidth(480);

    auto *layout = new QVBoxLayout(&dialog);

    auto *hint = new QLabel(QStringLiteral("DSH 需要你确认以下问题："), &dialog);
    layout->addWidget(hint);

    QList<UiQuestion> uiQuestions;

    for (const auto &questionValue : questions) {
        const QJsonObject question = questionValue.toObject();

        UiQuestion ui;
        ui.id = question.value(QStringLiteral("id")).toString();
        ui.multiSelect = question.value(QStringLiteral("multiSelect")).toBool();

        const QString header = question.value(QStringLiteral("header")).toString();
        if (!header.isEmpty()) {
            auto *headerLabel = new QLabel(
                QStringLiteral("<b>%1</b>").arg(header.toHtmlEscaped()), &dialog);
            headerLabel->setWordWrap(true);
            layout->addWidget(headerLabel);
        }

        auto *questionLabel = new QLabel(question.value(QStringLiteral("question")).toString(), &dialog);
        questionLabel->setWordWrap(true);
        layout->addWidget(questionLabel);

        const QJsonArray options = question.value(QStringLiteral("options")).toArray();
        if (!options.isEmpty()) {
            for (const auto &optionValue : options) {
                const QJsonObject option = optionValue.toObject();
                const QString label = option.value(QStringLiteral("label")).toString();
                if (label.isEmpty())
                    continue;

                QAbstractButton *button = nullptr;
                if (ui.multiSelect) {
                    button = new QCheckBox(label, &dialog);
                } else {
                    button = new QRadioButton(label, &dialog);
                }

                const QString description = option.value(QStringLiteral("description")).toString();
                if (!description.isEmpty())
                    button->setToolTip(description);

                layout->addWidget(button);
                ui.optionButtons.append(button);
            }

            // 单选且只有一个选项时默认选中，减少用户操作。
            if (!ui.multiSelect && ui.optionButtons.size() == 1)
                ui.optionButtons.first()->setChecked(true);
        } else {
            ui.customEdit = new QLineEdit(&dialog);
            ui.customEdit->setPlaceholderText(QStringLiteral("请输入你的回答"));
            layout->addWidget(ui.customEdit);
        }

        uiQuestions.append(ui);
    }

    auto *buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
QObject::connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    QObject::connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    layout->addWidget(buttonBox);

    // 无论用户点击“确定”还是“取消”，都把当前选择回传，避免服务端一直等待。
    (void)dialog.exec();

    QJsonObject answerPayload;
    answerPayload.insert(QStringLiteral("sessionId"), sessionId);

    QJsonArray answers;
    for (const UiQuestion &ui : uiQuestions) {
        QStringList selected;
        for (QAbstractButton *button : ui.optionButtons) {
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
    return true;
}

bool InteractionHandler::handleApproval(const QJsonObject &frame, DshApiClient *api, QWidget *parent)
{
    if (!api)
        return false;

    const QString rpcId = frame.value(QStringLiteral("rpcId")).toString();
    const QJsonObject payload = frame.value(QStringLiteral("payload")).toObject();
    const QString sessionId = payload.value(QStringLiteral("sessionId")).toString();
    const QString approvalId = payload.value(QStringLiteral("approvalId")).toString();
    const QString toolName = payload.value(QStringLiteral("toolName")).toString();
    const QString reason = payload.value(QStringLiteral("reason")).toString();

    if (rpcId.isEmpty() || sessionId.isEmpty() || approvalId.isEmpty())
        return false;

    QDialog dialog(parent);
    dialog.setWindowTitle(QStringLiteral("DSH 审批"));
    dialog.setMinimumWidth(420);

    auto *layout = new QVBoxLayout(&dialog);

    auto *title = new QLabel(QStringLiteral("工具调用请求审批"), &dialog);
    title->setStyleSheet(QStringLiteral("font-size: 16px; font-weight: bold;"));
    layout->addWidget(title);

    auto *toolLabel = new QLabel(
        QStringLiteral("工具：%1").arg(toolName.toHtmlEscaped()), &dialog);
    toolLabel->setWordWrap(true);
    layout->addWidget(toolLabel);

    if (!reason.isEmpty()) {
        auto *reasonLabel = new QLabel(
            QStringLiteral("原因：%1").arg(reason.toHtmlEscaped()), &dialog);
        reasonLabel->setWordWrap(true);
        layout->addWidget(reasonLabel);
    }

    auto *buttonBox = new QDialogButtonBox(&dialog);
    auto *allowButton = buttonBox->addButton(
        QStringLiteral("允许一次"), QDialogButtonBox::AcceptRole);
    auto *rejectButton = buttonBox->addButton(
        QStringLiteral("拒绝"), QDialogButtonBox::RejectRole);

QObject::connect(allowButton, &QPushButton::clicked, &dialog, &QDialog::accept);
    QObject::connect(rejectButton, &QPushButton::clicked, &dialog, &QDialog::reject);

    layout->addWidget(buttonBox);

    const QString outcome = dialog.exec() == QDialog::Accepted
        ? QStringLiteral("allowed-once")
        : QStringLiteral("rejected");

    QJsonObject answer;
    answer.insert(QStringLiteral("sessionId"), sessionId);
    answer.insert(QStringLiteral("approvalId"), approvalId);
    answer.insert(QStringLiteral("outcome"), outcome);

    api->respond(rpcId, answer);
    return true;
}