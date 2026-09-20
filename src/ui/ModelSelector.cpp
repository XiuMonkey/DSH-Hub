#include "ui/ModelSelector.h"

#include <algorithm>

#include "network/DshApiClient.h"
#include "ui/ShadowPanel.h"
#include "common/appearance/ThemeManager.h"

#include <QDebug>
#include <QDialog>
#include <QFrame>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QScreen>
#include <QScrollArea>
#include <QVBoxLayout>

namespace
{
	// 上拉菜单：chip 上方留 8px 间距（与原生 composer 的 bottom: calc(100% + 8px) 一致）
	constexpr int kMenuGap = 8;
	constexpr int kMenuWidth = 300;
	// 菜单主体（圆角面板）自身的上下内边距：算可用高度时要扣掉
	constexpr int kMenuBodyChrome = 12;
	// 菜单与屏幕边缘之间至少留的余量
	constexpr int kMenuScreenMargin = 12;
	// 清单再矮也得有这么高（否则一两项时菜单会缩成一条）
	constexpr int kMenuMinListHeight = 120;
	constexpr int kChipHeight = 28;
	// 菜单本体的圆角（= #modelSelectorMenu 的 QSS 圆角），阴影形状要跟着它
	constexpr int kMenuRadius = 12;
	// 菜单的阴影档位：浮层用最高一档
	inline CardShadow::Spec menuShadowSpec()
	{
		CardShadow::Spec spec = CardShadow::level3();
		spec.radius = kMenuRadius;
		return spec;
	}
}

// ------------------------------------------------------------------
// ModelSelectorRow
// ------------------------------------------------------------------

ModelSelectorRow::ModelSelectorRow(
	Kind kind,
	const QString& provider,
	const QString& id,
	const QString& title,
	const QString& subtitle,
	bool selected,
	QWidget* parent)
	: QPushButton(parent)
	, m_kind(kind)
	, m_provider(provider)
	, m_id(id)
{
	setObjectName(QStringLiteral("modelSelectorOption"));
	setFlat(true);
	// 单选组：同一父控件下 autoExclusive 保证“同时只能选中一个”，
	// 也不会像普通 checkable 按钮那样把已选中项再点一次就取消勾选
	setCheckable(true);
	setAutoExclusive(true);
	setChecked(selected);
	setCursor(Qt::PointingHandCursor);
	setMinimumHeight(38);
	// 菜单行不参与焦点链：弹出层一旦出现可聚焦子控件，Windows 上会因激活
	// 变化被系统立刻关掉；键盘导航由弹窗自己负责（当前只做点击选择）
	setFocusPolicy(Qt::NoFocus);
	setAccessibleName(subtitle.isEmpty()
		? title
		: qtTrId("common_accessible_pair_fmt").arg(title, subtitle));

	auto* layout = new QHBoxLayout(this);
	layout->setContentsMargins(8, 6, 8, 6);
	layout->setSpacing(8);

	auto* copy = new QVBoxLayout;
	copy->setContentsMargins(0, 0, 0, 0);
	copy->setSpacing(0);

	auto* nameLabel = new QLabel(title, this);
	nameLabel->setObjectName(QStringLiteral("modelSelectorOptionName"));

	copy->addWidget(nameLabel);

	if (!subtitle.isEmpty()) {
		auto* descLabel = new QLabel(subtitle, this);
		descLabel->setObjectName(QStringLiteral("modelSelectorOptionDesc"));
		descLabel->setWordWrap(true);
		copy->addWidget(descLabel);
	}

	layout->addLayout(copy, 1);

	m_check = new QLabel(QStringLiteral("✓"), this);
	m_check->setObjectName(QStringLiteral("modelSelectorOptionCheck"));
	m_check->setFixedWidth(18);
	m_check->setAlignment(Qt::AlignCenter);
	layout->addWidget(m_check, 0, Qt::AlignVCenter);

	// 勾选标记跟随按钮状态：单选组把别的行取消勾选时，这里也要同步
	updateCheckVisibility();
	connect(this, &QPushButton::toggled, this, [this]() { updateCheckVisibility(); });

	connect(this, &QPushButton::clicked, this, [this]() {
		if (m_kind == LevelKind)
			emit levelChosen(m_id);
		else
			emit modelChosen(m_provider, m_id);
		});
}

void ModelSelectorRow::updateCheckVisibility()
{
	if (m_check)
		m_check->setVisible(isChecked());
}

QSize ModelSelectorRow::sizeHint() const
{
	const QLayout* own = layout();
	return own ? own->sizeHint() : QPushButton::sizeHint();
}

QSize ModelSelectorRow::minimumSizeHint() const
{
	return sizeHint();
}

// ------------------------------------------------------------------
// ModelSelector::MenuDialog
// ------------------------------------------------------------------
// 无边框透明弹窗 + 内层圆角主体（与 PopupWindow 同样的做法，
// 保证圆角外真正透明），内容为若干小节（每节一个标题 + 若干行）。
// ------------------------------------------------------------------

class ModelSelector::MenuDialog : public QDialog
{
public:
	explicit MenuDialog(QWidget* parent)
		: QDialog(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
	{
		setAttribute(Qt::WA_TranslucentBackground);
		// 上拉菜单只做短暂浮层：不抢激活，避免被系统在激活变化时立即关闭
		setAttribute(Qt::WA_ShowWithoutActivating, true);

		auto* outer = new QVBoxLayout(this);
		outer->setContentsMargins(0, 0, 0, 0);
		outer->setSpacing(0);

		auto* body = new QWidget(this);
		body->setObjectName(QStringLiteral("modelSelectorMenu"));
		body->setAttribute(Qt::WA_StyledBackground, true);
		body->setFixedWidth(kMenuWidth);

		// 上拉菜单是浮层：套一层阴影外壳（lv3）。留白会让整个弹窗比菜单本体大一圈，
		// 菜单本体的宽度不变（body 仍是 setFixedWidth(kMenuWidth)）。
		auto* bodyPanel = new ShadowPanel(QStringLiteral("shadowFloat"),
			menuShadowSpec(), this);
		bodyPanel->setRadius(kMenuRadius); // 与 #modelSelectorMenu 的 QSS 圆角一致
		bodyPanel->setCard(body);
		outer->addWidget(bodyPanel);

		auto* layout = new QVBoxLayout(body);
		layout->setContentsMargins(6, 6, 6, 6);
		layout->setSpacing(2);

		// 行多时滚动。滚动区不参与焦点链，理由同行本身。
		m_scroll = new QScrollArea(body);
		m_scroll->setObjectName(QStringLiteral("modelSelectorScroll"));
		m_scroll->setFrameShape(QFrame::NoFrame);
		m_scroll->setWidgetResizable(true);
		m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		m_scroll->setFocusPolicy(Qt::NoFocus);
		// 滚动条早于 objectName 存在（基类构造时创建），设完名字重新解析一次
		Theme::repolishScrollArea(m_scroll);

		m_content = new QWidget(m_scroll);
		m_content->setObjectName(QStringLiteral("modelSelectorMenuContent"));

		m_options = new QVBoxLayout(m_content);
		m_options->setContentsMargins(0, 0, 0, 0);
		m_options->setSpacing(2);
		m_scroll->setWidget(m_content);

		// 两节各自包一层容器。行的 autoExclusive 是按「同一父控件」分组的，
		// 如果把模型行与档位行都挂在 m_content 下，两部分会互相取消勾选——
		// 那样菜单里只能剩一个 ✓。分容器后，每节内部各自单选。
		m_modelSection = makeSection(qtTrId("model_label"));
		m_levelSection = makeSection(qtTrId("model_think_depth"));

		layout->addWidget(m_scroll);
	}

	// 模型行/档位行的父控件与所属布局
	QWidget* modelSection() const { return m_modelSection; }
	QWidget* levelSection() const { return m_levelSection; }
	QVBoxLayout* modelRowsLayout() const { return m_modelRows; }
	QVBoxLayout* levelRowsLayout() const { return m_levelRows; }

	// 模型节里的提供方分组标签
	void addGroupLabel(const QString& text)
	{
		auto* label = new QLabel(text, m_modelSection);
		label->setObjectName(QStringLiteral("modelSelectorMenuGroup"));
		label->setContentsMargins(8, 6, 8, 2);
		m_modelRows->addWidget(label);
	}

	// 只有说明、没有可点项的占位行（放进档位节）
	void addLevelHint(const QString& text)
	{
		auto* hint = new QLabel(text, m_levelSection);
		hint->setObjectName(QStringLiteral("modelSelectorOptionHint"));
		hint->setContentsMargins(8, 4, 8, 4);
		hint->setWordWrap(true);
		m_levelRows->addWidget(hint);
	}

	// 菜单内容真正需要的高度：把各小节与标题的 sizeHint 直接累加。
	//
	// 不能用布局的 sizeHint()：弹窗还没 show()，QWidgetItem 会把子控件当成空项跳过，
	// 量出来只有标题那几十像素（菜单被压成一条、平白多出滚动条）。
	// totalSizeHint() 虽然算了 minimumSize，仍比真实内容矮一行，一样会多出滚动条。
	// 所以这里逐个控件自己加。
	int contentHeight() const
	{
		int total = 0;
		int counted = 0;
		for (int i = 0; i < m_options->count(); ++i) {
			QLayoutItem* item = m_options->itemAt(i);
			QWidget* widget = item ? item->widget() : nullptr;
			if (!widget)
				continue;

			if (counted > 0)
				total += m_options->spacing();
			total += widget->sizeHint().height();
			++counted;
		}
		return total;
	}

	// 按内容算滚动区高度：够高就把内容整段展开（不出滚动条），
	// 只有内容真的超过可用高度时才滚动。
	// maxHeight 由调用方按“chip 上下能腾出多少地方”算出来。
	void fitContent(int maxHeight)
	{
		const int content = contentHeight();
		const int ceiling = qMax(kMenuMinListHeight, maxHeight);
		m_listHeight = qBound(kMenuMinListHeight, content, ceiling);
		m_scroll->setFixedHeight(m_listHeight);
	}

	// 菜单应有的整高/整宽。滚动区高度刚用 setFixedHeight 改过，
	// 这时 adjustSize()/height() 可能还拿着旧的 sizeHint，
	// 所以要按我们算出来的值直接给定尺寸，否则定位会用错高度
	// （表现为菜单压住 chip、甚至跑出屏幕）。
	int menuHeight() const { return m_listHeight + kMenuBodyChrome; }
	int menuWidth() const { return kMenuWidth; }

	void clearOptions()
	{
		// 关键：只 deleteLater() 的话，旧行在被真正销毁前仍是子控件，
		// 弹窗再次 show() 时会被一并显示出来——同一份菜单里就会出现多行勾选。
		// 这里立刻从控件树上摘下来（setParent(nullptr)）并隐藏，确保不会再被显示。
		for (QVBoxLayout* rows : { m_modelRows, m_levelRows }) {
			while (QLayoutItem* item = rows->takeAt(0)) {
				if (QWidget* widget = item->widget()) {
					widget->hide();
					widget->setParent(nullptr);
					widget->deleteLater();
				}
				delete item;
			}
		}

		// 清空后把高度约束放开，避免上一次的固定高度残留到这一份内容上
		// （buildMenu 结束前会再按新内容 fitContent() 一次）
		m_scroll->setMinimumHeight(0);
		m_scroll->setMaximumHeight(QWIDGETSIZE_MAX);
	}

private:
	// 建一节：标题 + 装行的容器，返回容器给调用方作行的父控件
	QWidget* makeSection(const QString& title)
	{
		auto* titleLabel = new QLabel(title, m_content);
		titleLabel->setObjectName(QStringLiteral("modelSelectorMenuTitle"));
		titleLabel->setContentsMargins(8, 3, 8, 5);
		m_options->addWidget(titleLabel);

		auto* section = new QWidget(m_content);
		section->setObjectName(QStringLiteral("modelSelectorMenuSection"));

		auto* rows = new QVBoxLayout(section);
		rows->setContentsMargins(0, 0, 0, 0);
		rows->setSpacing(2);
		m_options->addWidget(section);

		if (!m_modelSection) {
			m_modelRows = rows;
			return section;
		}

		m_levelRows = rows;
		return section;
	}

	QScrollArea* m_scroll = nullptr;
	QWidget* m_content = nullptr;
	QVBoxLayout* m_options = nullptr;
	// 清单区当前高度（fitContent 设定），menuHeight() 用它算整高
	int m_listHeight = kMenuMinListHeight;
	QWidget* m_modelSection = nullptr;
	QWidget* m_levelSection = nullptr;
	QVBoxLayout* m_modelRows = nullptr;
	QVBoxLayout* m_levelRows = nullptr;
};

// ------------------------------------------------------------------
// ModelSelector
// ------------------------------------------------------------------

ModelSelector::ModelSelector(QWidget* parent)
	: QPushButton(parent)
{
	setObjectName(QStringLiteral("modelSelectorChip"));
	// 样式表按 #inputCapsule QPushButton#modelSelectorChip 给出，压过 defaults.qss 的通用按钮规则
	setCursor(Qt::PointingHandCursor);
	setFixedHeight(kChipHeight);
	setToolTip(qtTrId("model_select_title"));
	setFlat(true);
	// 不参与焦点链：这是个鼠标/无障碍驱动的 chip，按钮一旦拿到焦点就会一直显示
	// 焦点态样式，而点击消息区、侧边栏等“不接收焦点”的地方并不会把焦点带走，
	// 于是灰色会一直挂着（用户反馈的“失去焦点后不会取消变灰”）。
	// 键盘用户仍可通过无障碍接口（UIA/MSAA 的 Invoke）操作它。
	setFocusPolicy(Qt::NoFocus);

	auto* layout = new QHBoxLayout(this);
	layout->setContentsMargins(8, 0, 6, 0);
	layout->setSpacing(4);

	m_label = new QLabel(this);
	m_label->setObjectName(QStringLiteral("modelSelectorChipLabel"));

	m_value = new QLabel(this);
	m_value->setObjectName(QStringLiteral("modelSelectorChipValue"));

	m_chevron = new QLabel(QStringLiteral("▾"), this);
	m_chevron->setObjectName(QStringLiteral("modelSelectorChipChevron"));

	layout->addWidget(m_label);
	layout->addWidget(m_value);
	layout->addWidget(m_chevron);

	m_menu = new MenuDialog(this);

	connect(this, &QPushButton::clicked, this, &ModelSelector::openMenu);

	hide();
}

QSize ModelSelector::sizeHint() const
{
	// 内容由子标签布局决定，QPushButton 自身没有文本
	const QLayout* own = layout();
	return own ? own->sizeHint() : QPushButton::sizeHint();
}

QSize ModelSelector::minimumSizeHint() const
{
	return sizeHint();
}

bool ModelSelector::hasModels() const
{
	return m_hasDirectory && !m_directory.groups.isEmpty();
}

QString ModelSelector::currentModelName() const
{
	return m_directory.currentModelName();
}

QString ModelSelector::currentLevelName() const
{
	return m_directory.currentLevelName();
}

QString ModelSelector::currentLevelId() const
{
	if (!m_directory.current.reasoningEffort.isEmpty())
		return m_directory.current.reasoningEffort;
	const ReasoningLevel* level = m_directory.currentLevel();
	return level ? level->id : QString();
}

void ModelSelector::setSession(DshApiClient* api, const QString& sessionId)
{
	// 同一会话且已有数据时不重复拉取（会话切换、连接建立都会调到）
	if (api == m_api && sessionId == m_sessionId && m_hasDirectory)
		return;

	m_api = api;
	m_sessionId = sessionId;

	// 会话换了：上一个会话的"会话级选择"作废，新会话的选择随后由 DSHHub 推入
	m_hasSessionSelection = false;
	m_sessionSelection = ModelSelection();

	clearDirectory();

	if (!m_api || m_sessionId.isEmpty())
		return;

	refresh();
}

void ModelSelector::refresh()
{
	if (!m_api || m_sessionId.isEmpty())
		return;

	// 会话可能在请求返回前被销毁（例如切主题会重建主窗口），用 QPointer 兜住
	QPointer<ModelSelector> self(this);
	const QString sessionId = m_sessionId;

	ModelSelectionService::fetch(m_api, sessionId,
		[self, sessionId](const SessionModelDirectory& directory) {
			if (!self || self->m_sessionId != sessionId)
				return; // 已经切到别的会话，丢弃过期结果
			self->applyDirectory(directory);
		},
		[self, sessionId](const DshApiClient::RpcError& error) {
			if (!self || self->m_sessionId != sessionId)
				return;
			qWarning().noquote() << QStringLiteral("[ModelSelector] session/modelCatalog failed:")
				<< error.code << error.message;
			self->clearDirectory();
		});
}

void ModelSelector::applyDirectory(const SessionModelDirectory& directory)
{
	m_directory = directory;

	// 0.1.5：目录只给部署默认值，会话自己的选择优先（session/list 投影里的那条）。
	// 只要该模型仍在目录里，就用它覆盖 current；刷新后仍然如此。
	if (m_hasSessionSelection && !m_sessionSelection.provider.isEmpty()) {
		const bool known = std::any_of(m_directory.groups.cbegin(), m_directory.groups.cend(),
			[this](const ModelProviderGroup& group) {
				if (group.id != m_sessionSelection.provider)
					return false;
				return std::any_of(group.models.cbegin(), group.models.cend(),
					[this](const ModelOption& option) { return option.id == m_sessionSelection.model; });
			});
		if (known)
			m_directory.current = m_sessionSelection;
		else
			qInfo().noquote() << QStringLiteral("[ModelSelector] session selection not in catalog, keeping default:")
			<< m_sessionSelection.provider << m_sessionSelection.model;
	}

	m_hasDirectory = true;
	updateChip();
}

void ModelSelector::overrideCurrentSelection(const QString& provider, const QString& model,
	const QString& reasoningEffort)
{
	if (provider.isEmpty() || model.isEmpty()) {
		// 服务端还没记录过该会话的选择：保持目录给的默认值
		m_hasSessionSelection = false;
		return;
	}

	m_sessionSelection.provider = provider;
	m_sessionSelection.model = model;
	m_sessionSelection.reasoningEffort = reasoningEffort;
	m_hasSessionSelection = true;

	if (m_hasDirectory) {
		m_directory.current = m_sessionSelection;
		updateChip();
	}

	qInfo().noquote() << QStringLiteral("[ModelSelector] session selection applied:")
		<< provider << model << reasoningEffort;
}

void ModelSelector::clearDirectory()
{
	m_directory = SessionModelDirectory();
	m_hasDirectory = false;
	updateChip();
}

void ModelSelector::updateChip()
{
	const bool available = hasModels();

	// chip 左侧显示当前模型：换模型是这个控件的主要用途，
	// 档位作为次要信息跟在后面（该模型没公布档位时整段省略）。
	const QString model = m_directory.currentModelName();
	const QString level = m_directory.currentLevelName();

	m_label->setText(model);
	m_label->setVisible(!model.isEmpty());

	m_value->setText(level);
	m_value->setVisible(!level.isEmpty());

	setAccessibleName(level.isEmpty()
		? qtTrId("model_name_fmt").arg(model)
		: qtTrId("model_name_think_fmt").arg(model, level));

	setVisible(available);
	if (available)
		updateGeometry();
}

void ModelSelector::buildMenu(int maxListHeight)
{
	m_menu->clearOptions();

	const QString currentProvider = m_directory.current.provider;
	const QString currentModel = m_directory.current.model;

	// ---------------- 模型 ----------------
	for (const ModelProviderGroup& group : m_directory.groups) {
		// 只有一个提供方时分组标题是噪音，直接省略（多数部署就是这么回事）
		if (m_directory.groups.size() > 1)
			m_menu->addGroupLabel(group.name);

		for (const ModelOption& option : group.models) {
			const bool selected = group.id == currentProvider && option.id == currentModel;

			// 第二行给出“模型 id”，同名模型跨提供方时能分清是哪一个
			QString subtitle = option.description;
			if (subtitle.isEmpty())
				subtitle = option.id;

			// 父控件用「模型」那一节的容器：autoExclusive 的单选组因此限定在这一节内
			auto* row = new ModelSelectorRow(
				ModelSelectorRow::ModelKind, group.id, option.id,
				option.name, subtitle, selected, m_menu->modelSection());

			connect(row, &ModelSelectorRow::modelChosen, m_menu,
				[this](const QString& provider, const QString& model) {
					m_menu->accept();
					chooseModel(provider, model);
				});
			m_menu->modelRowsLayout()->addWidget(row);
		}
	}

	// 目录里没有任何分组时也要给一句说明，否则弹出来是空框
	if (m_directory.groups.isEmpty())
		m_menu->addGroupLabel(qtTrId("model_none_published"));

	// ---------------- 思考深度 ----------------
	// 适配器没公布档位时也用通用四档兜底，所以这里总能给出可选项；
	// 只是要注明它是通用档位，别让用户以为那是适配器公布的能力。
	const QVector<ReasoningLevel> levels = m_directory.selectableLevels();
	if (m_directory.usesFallbackLevels())
		m_menu->addLevelHint(qtTrId("model_think_generic_note"));

	// 当前档位：显式选择的，否则回退到适配器默认档位
	const QString selectedId = currentLevelId();

	for (const ReasoningLevel& level : levels) {
		auto* row = new ModelSelectorRow(
			ModelSelectorRow::LevelKind, QString(), level.id,
			level.name, level.description, level.id == selectedId, m_menu->levelSection());

		connect(row, &ModelSelectorRow::levelChosen, m_menu,
			[this](const QString& levelId) {
				m_menu->accept();
				chooseLevel(levelId);
			});
		m_menu->levelRowsLayout()->addWidget(row);
	}

	m_menu->fitContent(maxListHeight);
}

void ModelSelector::openMenu()
{
	if (!hasModels())
		return;
	// 防重入：菜单已经开着时不再重建（重复重建会让旧行残留成“多行勾选”）
	if (m_menu->isVisible())
		return;

	// 先问清楚 chip 上下各能腾出多少地方：菜单按内容展开，但不超过能放下的高度
	// ——内容装得下就整段显示（不出滚动条），装不下才滚动。
	const QScreen* screen = QGuiApplication::screenAt(mapToGlobal(rect().center()));
	if (!screen)
		screen = QGuiApplication::primaryScreen();

	int maxListHeight = kMenuMinListHeight;
	if (screen) {
		const QRect available = screen->availableGeometry();
		const int above = mapToGlobal(QPoint(0, 0)).y() - available.top()
			- kMenuGap - kMenuScreenMargin;
		const int below = available.bottom() - mapToGlobal(QPoint(0, height())).y()
			- kMenuGap - kMenuScreenMargin;
		// 取更宽敞的一侧：菜单最终会落到那一侧
		maxListHeight = qMax(above, below) - kMenuBodyChrome;
	}
	// 阴影外壳也占地方，可用高度里先扣掉它，否则菜单本体虽放得下、整窗却顶到屏幕外
	const QMargins menuShadowPad = CardShadow::padding(menuShadowSpec());
	maxListHeight -= menuShadowPad.top() + menuShadowPad.bottom();

	buildMenu(maxListHeight);
	// 弹窗尺寸按算出来的高度直接设定：滚动区高度刚改过，
	// 靠 adjustSize() 可能还拿着旧的 sizeHint，那样下面定位会用错高度。
	// 注意 = 菜单本体尺寸 + 阴影留白（menuWidth()/menuHeight() 给的是本体）。
	m_menu->setFixedSize(m_menu->menuWidth() + menuShadowPad.left() + menuShadowPad.right(),
		m_menu->menuHeight() + menuShadowPad.top() + menuShadowPad.bottom());

	// 上拉：菜单**本体**的底边贴在 chip 上方 kMenuGap 处。
	// 整窗比本体大一圈（那圈是阴影留白），所以定位时要把留白补回来，
	// 否则菜单会离 chip 远出留白那么多，看起来"浮得没道理"。
	const QPoint above = mapToGlobal(QPoint(-menuShadowPad.left(),
		-m_menu->height() - kMenuGap + menuShadowPad.bottom()));
	QPoint pos = above;

	if (screen) {
		const QRect available = screen->availableGeometry();
		// 水平越界就右对齐 chip，仍越界则夹到屏幕内
		if (pos.x() + m_menu->width() > available.right())
			pos.setX(mapToGlobal(QPoint(width(), 0)).x() - m_menu->width() + menuShadowPad.right());
		pos.setX(qBound(available.left(), pos.x(), qMax(available.left(), available.right() - m_menu->width())));
		// 上方放不下时改到下方展开（同样要把阴影上留白补回来）
		if (pos.y() < available.top())
			pos.setY(mapToGlobal(QPoint(0, height() + kMenuGap)).y() - menuShadowPad.top());
	}

	m_menu->move(pos);

	m_chevron->setText(QStringLiteral("▴"));
	m_menu->exec();
	m_chevron->setText(QStringLiteral("▾"));
}

// 换模型 / 换档位是同一套动作：乐观更新 chip -> 发 session/selectModel ->
// 以服务端回显为准；失败就回去拉一次目录（回到服务端事实）。
// 差别只有两点，用参数区分：换模型后要重新拉目录（新模型可能带来别的档位集合），
// 以及成功后发哪个信号。
void ModelSelector::submitSelection(const ModelSelection& selection, bool reloadDirectory,
	const std::function<void(const ModelSelection& selected)>& onAccepted)
{
	// 先乐观更新 chip，避免等待往返期间界面停在旧值
	m_directory.current = selection;
	// 用户的选择就是该会话的选择：记下来，后续目录刷新不会被部署默认值覆盖
	m_sessionSelection = selection;
	m_hasSessionSelection = true;
	updateChip();

	QPointer<ModelSelector> self(this);
	const QString sessionId = m_sessionId;

	ModelSelectionService::select(m_api, sessionId, selection,
		[self, sessionId, reloadDirectory, onAccepted](const ModelSelection& selected) {
			if (!self || self->m_sessionId != sessionId)
				return; // 已经切到别的会话，丢弃过期结果

			self->m_directory.current = selected;
			self->updateChip();
			if (reloadDirectory)
				self->refresh();
			if (onAccepted)
				onAccepted(selected);
		},
		[self, sessionId](const DshApiClient::RpcError& error) {
			if (!self || self->m_sessionId != sessionId)
				return;
			qWarning().noquote() << QStringLiteral("[ModelSelector] session/selectModel failed:")
				<< error.code << error.message;
			// 失败：回到服务端事实
			self->refresh();
		});
}

void ModelSelector::chooseModel(const QString& provider, const QString& modelId)
{
	if (provider.isEmpty() || modelId.isEmpty() || !m_api || m_sessionId.isEmpty())
		return;

	// 点的是当前模型：什么都不用做（避免白跑一次 RPC）
	if (provider == m_directory.current.provider && modelId == m_directory.current.model)
		return;

	ModelSelection selection;
	selection.provider = provider;
	selection.model = modelId;
	// 档位留空 = 由新模型的适配器默认档位决定。
	// 沿用旧档位是错的：同一个档位 id 在新模型上未必存在，服务端会拒绝或静默改写。
	selection.reasoningEffort.clear();

	submitSelection(selection, /*reloadDirectory=*/true, [this](const ModelSelection& selected) {
		qInfo().noquote() << QStringLiteral("[ModelSelector] -> %1/%2")
			.arg(selected.provider, selected.model);
		emit modelChanged(selected.provider, selected.model);
		});
}

void ModelSelector::chooseLevel(const QString& levelId)
{
	if (levelId.isEmpty() || !m_api || m_sessionId.isEmpty())
		return;

	ModelSelection selection = m_directory.current;
	selection.reasoningEffort = levelId;

	submitSelection(selection, /*reloadDirectory=*/false, [this](const ModelSelection& selected) {
		qInfo().noquote() << QStringLiteral("[ModelSelector] %1/%2 -> %3")
			.arg(selected.provider, selected.model,
				selected.reasoningEffort.isEmpty() ? qtTrId("common_default_suffix") : selected.reasoningEffort);
		emit levelChanged(selected.reasoningEffort);
		});
}