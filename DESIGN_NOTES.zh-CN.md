# DSH Hub 设计笔记

## 这份文件是什么

2026-09 对源码做了一轮**注释瘦身**：源码里那些"为什么这么写 / 为什么不那样写"的长篇论证被压成一句话，只留结论。

**这份文件收纳被压掉的论证。** 排查问题时该翻这里，而不是去 git 历史里考古。

两条约定：

- 源码里留「是什么 + 一句结论」；完整推理、实测叙述、被否决的方案放这里。
- 动某块代码前先搜一下这里有没有相关条目 —— 很多看着"多余"的写法，理由都在这。
- 各处标了「坑」的条目尤其别忽略：它们都是踩过的。

---

## `include/VirtualClass/VirtualCommon.h`

宿主 ↔ 客户端扩展（进程外 QPlugin DLL）的接口契约。

- **IID 是两边唯一的约定。** 插件侧 `obj->qt_metacast(IID)` 用的就是这个字符串串，双方 include 同一份头文件即自洽。**改 IID = 改接口 ABI**，旧插件会**静默转换失败** —— 不报错，只是拿到 nullptr，然后插件的功能无声消失。
- **vtable 槽位 = 声明的先后顺序。** 插件 DLL 是独立编译的：**末尾追加**虚方法时老插件照旧可用（它只碰得到前面的槽位）；**插到中间**会让所有已发布的插件调错位置。
- **接口必须全内联、不派生 QObject、没有 out-of-line 成员。** 任何 out-of-line 成员（哪怕只是个虚析构）都会成为真外部符号，插件在链接期报 **LNK2019**。
- **参数只按指针传递**，所以前向声明就够 —— 宿主与插件各编一份，谁都不依赖对方 exe 里的符号。
- `VirtualTheme::ExternalReloadStyles()` 的返回值语义是"**重载后样式表非空**"：`false` 基本等于文件没读到。插件覆盖完 `<exe>/styles/*.qss` 与 `theme-*.json` 后可以用它自检。
- **遮罩是"一个宿主窗口只留一层 + 按 owner 记名 + 引用计数"**（见 `WindowFrame`）：只有**最后一个** release 才真正隐藏。所以 `ExternalShowOverlay` / `ExternalHideOverlay` **必须成对**，否则遮罩永久残留、界面就点不动了。
- `ExternalShowOverlay` 的居中是宿主按**自己的中心**算的 —— 调用前 popup 的**尺寸必须已经定好**，否则会按错误几何居中。
- 它走的就是宿主自己弹窗（设置 / 插件市场 / 扩展管理）那条 `WindowFrame::showOverlayWithPopup`：两步背靠背，因此不会出现"遮罩先到、弹窗后到"。
- **`VirtualShell`（架空）的要点**：抢台后原生控件树不可见、不接事件但**不销毁**（还台立即恢复，消息 / 滚动位置 / 输入内容都在，无需重新加载）；`owner` 必须等于扩展的安装目录名；同一时刻只允许一个 owner 且**绝不静默顶掉**别人的台；抢台必须写在 `attachHost()` 里（切主题会重建主窗口并对同一插件实例再调一次）；不调 `ExternalSetCaptionBand` 就等于整个客户区都是客户区，**窗口只能靠 Alt+Space 移动**；带动态属性 `dshWindowControl=true` 的控件会被先排掉，所以扩展自己画的窗口按钮照旧可点。

## `include/common/util/CommonRegistry.h`

程序级唯一对象的字符串索引表。

- **值类型必须是 `QPointer<QObject>`**：对象销毁后自动置空，查询永不返回野指针；即使某个类漏写注销，最坏也只是表里留一条空记录。
- **登记是「覆盖」语义（后来者胜），不是「拒绝重复」。** 这是必需的而非随意的：切主题时 `ThemeManager` 会**先建新窗口**，而旧窗口要到下一轮事件循环才析构。若改成"拒绝登记"，新对象永远进不了表，而索引会在旧对象析构后**彻底变空**。
- **覆盖语义之所以安全，靠 `Destroy` 的身份校验**：先比对"注销请求者"与"表里现有指针"，不同就拒绝。被顶掉的旧对象随后析构时不会误删新记录。**这条校验不是可选项。**
- **单例刻意不析构**（`.cpp` 里定义为不析构的堆对象）：注册表必须活得比所有被登记对象更久，否则对象的析构函数里调用注销就会访问已析构的单例。
- 允许同一个 QObject 登记在多个 index 下（一个对象承担多种角色），注销其中一个不影响其它登记项。
- **表里只存对象、不存类型标签。** 类型由调用点用 `qobject_cast` / `Find<T>()` 还原。2026-09-19 曾移除过一个 `RegType` 枚举方案（宿主按标签代转）—— 那需要运行期把标签映射到具体签名的函数指针，而插件侧签名是编译期固定的，两者对不上，最后只会招来一层 thunk。
- 使用纪律：**只登记"整个程序生命周期内应当唯一"的界面 / 服务对象**；临时对象、一次性控件不要往里放，否则注册表会退化成什么都装的全局变量表。
- 用法：

  ```cpp
  CommonRegistry::instance().AddToRegistry("sidebar", this);
  if (auto* sb = CommonRegistry::instance().Find<Sidebar>("sidebar")) ...
  CommonRegistry::instance().Destroy("sidebar", this);   // 析构时
  ```

## `include/common/util/CodeHighlighter.h`

- 规则来自 `highlight_rules.json`，**初始化时一次性加载**；高亮结果进缓存（键 = 语言 + 代码），避免重复处理。
- `highlight()` 返回的是**可以直接放进 `<pre>` 的 HTML** —— 调用点不要再做二次转义。
- 高亮会被**渲染 worker 线程**并发调用，所以规则与缓存的读写统一加锁。
- **锁必须是递归锁**（`QRecursiveMutex`）：`loadFromFile` 持锁期间会调用同样加锁的 `clearCache()`，换成普通 `QMutex` 会自死锁。
- `loadFromFile` 重复调用会先清空旧规则；规则变化后需要 `clearCache()` 清掉旧缓存（否则旧规则的结果还在缓存里）。

## `include/common/util/Logger.h`

- Qt 消息按**模块目录** `logs/<module>/` 与**级别** debug/info/warn/error/fatal 分开落盘。
- `TimingLogger` 从第一次调用起计时，每行同时给**距上次打点**的耗时与**进程启动以来**的累计耗时两项。
- 输出走 `qInfo`（INFO 级别，随 Logger 写进 `logs/timing/info.log`）。
- 格式：`[Timing] server baseUrl ready  +2805ms (since start 4039ms)`。
- **相邻两个 mark 的差值 = 该阶段耗时**；只要按时间顺序打点，就能从日志还原整条初始化链路的分段耗时。
- 定位：只关心主线程初始化与关键链路的相对耗时，不追求覆盖全部路径。
- 计时器是**进程级单一实例**（私有 `timer()`），首次 `mark()` 时才创建并启动；`s_lastMs` 记上次打点时刻。

## `include/common/util/MarkdownPreprocess.h`

**这条是踩过的真坑，别撤掉这个函数。**

- Qt 6 的 Markdown 导入器（`QTextMarkdownImporter`）**不支持 `<br>` 标签**：内容里一旦出现 `<br>`，它会**丢弃其后同一行 / 同一单元格的所有内容**。
- 破坏的具体形态：表格单元格被截断、该行之后的表格行、以及**表格之后的整段内容全部丢失**。
- 取巧点：`QTextDocument` 会把 **U+2028（LINE SEPARATOR）** 渲染成真正的换行，所以替换之后既保留换行语义，又不破坏表格解析。
- 匹配覆盖 `<br>` / `<br/>` / `<br />` 三种写法，**大小写不敏感**。
- 使用时机：必须在 `QTextDocument::setMarkdown()` **之前**对 Markdown 源文本做这一步。

---

## `include/common/appearance/WindowFrame.h`

无边框窗口的**窗口级判定**（不含任何绘制）。

- 绘制不在这里：标题栏控件与窗口按钮 → `src/ui/TitleBar.cpp`；圆角 + 1px 描边 → `resources/styles/main-window.qss` 的 `#dshhubCentral`。本文件只做判定，一行都不画。
- 标题栏控件的 `objectName` **必须是** `windowTitleBar`：拖动区判定与遮罩覆盖范围都按它算。
- 窗口按钮**必须**带动态属性 `dshWindowControl=true`：命中测试要排除它们，否则鼠标落在按钮上会被系统当成"按住标题栏"，按钮永远收不到点击。
- 关掉 Windows 11 系统圆角的原因：系统圆角半径 8px，**会把自绘圆角的四个角裁掉**；关掉后仅保留系统投影、贴边吸附、右键系统菜单等原生行为。
- `applyBorderState` 必须**顺带 repolish**：动态属性变化不会自动重新匹配选择器，所以设完属性要手动触发重匹配（规则本体在 qss 的 `#dshhubCentral[maximized="true"]`）。
- `applyFrameStyle` 把动作顺序固定在本文件，是为了省得每个调用方各写一遍；它返回"是否贴屏幕边缘"，调用方据此刷新标题栏按钮的最大化/还原图标（按钮是控件，本文件不碰控件类型）。
- **遮罩只覆盖标题栏以下的内容区**：铺满客户区会把自绘标题栏一起盖住，窗口按钮就点不动了。
- 遮罩**一个宿主只留一层**，设置 / 插件市场 / 扩展管理 / 工具过滤共用；`owner` 按调用方记名，最后一个 `release` 的才真正隐藏 —— 所以 show/hide **必须成对**。隐藏时末尾补一次宿主同步重绘，让遮罩与弹窗落在同一帧。
- `showOverlayWithPopup` 为什么必须合成一个函数：遮罩是宿主的**子控件**（同步重绘后立刻上屏），弹窗是**独立顶层窗口** —— 只有两步在同一个事件循环轮次里完成，合成器才会放进同一帧。中间夹进耗时工作（建控件、拉数据）就会出现"遮罩先出、弹窗后到"。**调用方的准备工作必须在调本函数之前做完。**

## `include/common/appearance/ThemeManager.h`

- 为什么是**类**而不是原来的 `namespace` + 自由函数：客户端扩展（QPlugin DLL）只能通过 `CommonRegistry` 按 index 取**对象**再做接口转换，命名空间与自由函数没有 QObject 身份、拿不到 index，插件侧无从触达。做成 QObject 单例后插件只需 `DshHost::findObject("themeManager")` 即得 `VirtualTheme` 接口。
- `instance()` 刻意不析构：它是 QObject，而注册表必须活得比所有被登记对象更久，静态析构顺序无从保证，索性一起长存。
- **为什么外部接口不叫 `applyToWindow`**：插件 DLL 与宿主各自编译，接口一旦发布就只能增不能改。把"给外部用的名字"与宿主内部 API 分开，内部改动（改签名、拆重载）才不会波及已编译的插件。
- 跨边界符号规则：接口转换走 `obj->qt_metacast(IID)`（跨边界只传**字符串**，插件侧零宿主符号）；`ExternalXxx()` 是接口虚函数、走 vtable，同样不产生外部符号。
- ⚠️ **别改成 `dynamic_cast`**（Itanium ABI 下跨模块静默返回 `nullptr`），也别 `qobject_cast<ThemeManager*>`（需要 `ThemeManager::staticMetaObject`，那是宿主 exe 的外部符号且零导出 ⇒ 插件 DLL 链接期 **LNK2019**）。
- `applyToWindow` 在未 init（qss 为空）时直接返回的理由：`setStyleSheet("")` 会把窗口上**已有的样式清掉**，比什么都不做糟得多；而插件拿到注册表对象后可能在任何时刻调进来。
- `ExternalReloadStyles` 未 init 时一律拒绝并告警：那时没有可信的样式目录，重载只会把缓存刷成"仅 qrc 兜底"的那一份。返回 `false` 基本只有一种成因（那些文件根本没读到：写失败 / 路径不对 / 权限不足），所以它同时充当事后自检。⚠️ **它不重建窗口**：主题相关但构造期已固化的东西（如按 `isDark()` 选的 logo 资源）不会跟着变 —— 要那种一致得走 `switchTheme()` 那条重建窗口的路。
- 常量做成"返回引用的函数"而不是 static 数据成员：类/命名空间作用域的 `QString`、`QStringList` 会在**静态初始化期**构造（main 之前就分配堆内存）。`installPaletteFor` 有副作用（换掉整个应用的 `QPalette`），只允许被 `setMode()` / `reload()` 间接调用；放在类里只是为了"拼写归到类命名空间"，一律 private、插件拿不到。
- ⚠️ **`repolishScrollArea` 的 Qt 缓存坑**：`QAbstractScrollArea` 的滚动条是在**基类构造**里创建并首次解析规则的，那时子类构造函数体里的 `setObjectName("...")` 还没执行，`QStyleSheetStyle` 就把"匹配不到 `#objectName QScrollBar`"缓存了下来；之后再设 objectName **不会**触发重新匹配，滚动条就一直按基础（原生/老式）样式绘制。凡"先建控件、后设 objectName"的滚动区都要在设完名字后调一次。
- `applyToWindow` 替代全局 `qApp->setStyleSheet()` 的理由：各顶层窗口自行挂载，切主题时旧窗口不再被全局 re-polish。

## `include/common/appearance/CardShadow.h`

卡片阴影的绘制原语（纯绘制，只依赖 QtGui）。

- **不用 `QGraphicsDropShadowEffect` 的第一条理由**：它每次源控件重绘都要把整棵子树离屏渲染一遍再做模糊 —— 而本项目最热的两条路径（流式输出的消息气泡、每敲一个键都要重排的输入卡片）正好都压在这个代价上。
- **第二条理由**：它的阴影画在控件矩形**之外**，滚动区与相邻兄弟控件会把边缘裁掉，局部重绘后还容易留残影（本项目已在滚动条解析上踩过同类的 Qt 缓存坑）。
- **算法全貌**：把 N 个同心圆角矩形由外向内叠上去，距离 d 处的像素被 `grow>d` 的那些环覆盖，累积透光率 = `Π(1-a_i)`；反解每一环的 `a_i`，就能让结果**精确等于**给定目标曲线 `a_d = 1 - (1-T(d)) / (1-T(d+1))`。
- **目标曲线**取"两层高斯尾"叠加：`T(d) = A · [ 0.45·Q(d/σ紧) + 0.55·Q(d/σ宽) ]`；两层与原生 CSS 写法同构（`--dsw-shadow-lv2` 就是 `0 4px 12px 2%, 0 2px 8px 4%`）。
- **为什么取这条曲线**：`d=0` 处正好是峰值的一半 —— 真实模糊阴影在形状边界正是半高位置，所以贴着卡片不会出现"一条又细又重的实边"。全程不需要做模糊，生成成本很低；结果按（尺寸, 半径, 扩散, 偏移, 颜色, DPR）缓存复用。
- ⚠️ **`parseColor` 为什么必须存在（静默失效链）**：`QColor` 的字符串构造只认 `#RRGGBB` / `#AARRGGBB` 与 SVG 颜色名，**不认 CSS 的 `rgba(...)`**；而色板里的阴影色恰恰写成 `rgba(0,0,0,0.45)` —— 直接构造会得到无效 `QColor`，`paint()` 因色无效而**直接返回**，现象是"阴影整块消失"且**不报任何错**。
- `padding()` 为什么不对称：下留白是 `blur + qMax(0, dy)`，所以 `dy > 0`（阴影向下）时下侧留得多、上侧留得少。
- `padOverride` 的动机："卡片在外壳里摆哪儿"和"阴影长什么样"是两件独立的事 —— 光靠 `Spec::dy` 无法把卡片往下挪，因为下留白被 `qMax(0, dy)` 钳住、dy 一转为负就不再收缩；有了覆盖值才能只挪卡片位置而阴影方向保持不变。
- `level3()` 为什么是唯一"共用的档位"：浮层是独立顶层窗口，留白不占主窗口布局，可以给足；而贴在窗口里的面（侧栏 / 输入卡片 / 消息气泡）的留白是从布局里切出来的，要按各自的位置成本单独权衡 —— 用一个"通用档位"反而会把不同约束的面绑在一起。
- 颜色不硬编码：调用方从色板取（`ThemeManager::instance().color("shadow")` 等），与项目"色值只允许出现在 `theme-*.json`"的约定一致。

## `include/common/appearance/TranslationManager.h`

- ⚠️ **源码里没有任何自然语言文案**：每条文案用全局唯一 key 引用（如 `topbar_settings`），文案本身只在翻译文件里。因此**所有语言（含出厂默认的中文）都需要语言包**，没有"某个语言免翻译"的例外 —— 这条约定是刻意的，目的是彻底消除源码对特定语言的偏好。
- `.ts` 的形状契约：只写 `<message id="key">` + `<translation>`；id 是查表主键，`<source>` **不参与查表**、也不在运行期使用（写了也会被 `tools/release-translations.ps1` 删掉），原文由 `zh_CN` 的 `<translation>` 承担。
- **兜底是 key 本身**（`qtTrId` 对未知 id 原样返回），所以语言包缺条会让界面显示 `topbar_settings` 这样的代号。这是**刻意保留**的：缺哪条一眼可见、便于调试，运行时不做任何粉饰式拦截（唯一例外见 `TranslationUi.cpp`，空译文会被挡下）。
- 改文案时必须同步更新语言包，工具侧的 id 一致性校验会挡住漏条的包。
- 语言包查找顺序与释放策略：从 exe 同目录 `translations/` 读，装不上再退回 qrc 内置；内置包启动时释放到外部目录（**存在则不覆盖**），便于就地修改而不必重新编译。
- 免重启切换的机制：`apply()` 换掉 translator 之后 Qt 会给所有控件发 `QEvent::LanguageChange`，各界面在自己的 `changeEvent` 里重新设置文案即可；**不是控件、收不到该事件的对象**（例如持有展示文案的逻辑类）接 `TranslationNotifier::languageChanged`。
- **"就地换文案"的完整机制与不覆盖边界**：换 translator 前用**当前** translator 把 `.ts` 里所有 id 翻一遍，建立"界面此刻显示的文案 → id"映射；换好后遍历控件替换命中项。已按标准做法接 `changeEvent` 的控件会先自己刷新、不再命中旧映射，因而不会重复翻译。**明确不覆盖**：①用 `arg()` 拼出来的动态文案（如"共 3 个模型，1 个提供方。"）不是整串文案、匹配不上，随数据刷新重建；②已渲染进 HTML 的历史消息（如思考块标题）—— 这两处要随语言变得各自走重建 / 重渲染路径。
- 历史背景：key 化之前中文是"身份翻译"（译文 == 源码原文），"未翻译"判断会把所有条目都跳过、机制实际在空转；改读 id 之后译文与原文不再相同，这条路径才真正生效。
- `isDefaultLanguage` 只用于语言列表去重（默认语言单独占一个槽位），**不代表"无需翻译"**；`defaultLanguageCode()` 返回的出厂默认语言同样需要语言包。

## `include/common/appearance/InteractionHandler.h`

- `question/requested` → 在对话气泡下方创建提问面板并回传答案；`approval/requested` → 创建审批面板。
- 用户选择/填写（审批为「允许一次」/「拒绝」）后一律经 `DshApiClient::respond()` 发回服务端。
- 帧无效时返回 `nullptr`，调用方据此决定是否插入面板。
- 这些 UI 与应答逻辑集中在本文件/源文件，是为了避免散落进主窗口逻辑。

## `include/common/appearance/TopBarTools.h`

顶栏工具过滤的**功能半**（不含任何控件）。

- 它读写的是 DSH 里 `ToolsFilterPlugin` 暴露的 `/api/tools-filter`：`GET /api/tools-filter?session=<会话 id>` → 该会话可见的工具目录（名字 + 描述 + 参数格式）+ 当前过滤状态；`POST /api/tools-filter { sessionId, config: { FilterList, DropGuidance } }` → 整份写回该会话的 `ToolsFilterConfig.json`。
- `ensureSession()`：先拉目录；该会话还没有配置文件时，用**工具名**建一份初始版 —— 插件工具按插件名分目录，原版自带工具按功能分目录；描述与参数只留在内存里给界面用、**不写进 json**；没有 `directory` 字段时才回退到 `Default`。
- 配置文件形状（与 `resources/ToolsFilterPlugin/example.ToolsFilterConfig.json` 同族）：

  ```json
  { "FilterList": [
      { "Directory": { "IsExpanded": "True", "DirectoryName": "Default",
                       "ToolsList": [ "read", "glob",
                                      { "ToolName": "pwsh", "IsVisible": "False" } ] } },
      { "Directory": { "IsExpanded": "True", "DirectoryName": "built-in",
                       "Description": "…", "ToolsList": [ … ] } } ] }
  ```

- **可见的工具就是名字**（不写参数/描述）；被隐藏的那一项才写成 `{ToolName, IsVisible:"False"}` —— 插件的判定是按 `IsVisible`，"从列表里删掉"表达不了隐藏。
- `FilterList` 里可以有任意多个目录；初始文档由插件的 `directory` 元数据生成（插件工具按插件名，原版工具按功能），之后界面按存储文档把工具分回各自目录，**没被任何目录认领的工具全部进 `Default`**。
- **过滤语义**（与插件侧一致，见 `resources/ToolsFilterPlugin/index.js`）：隐藏只影响"发给模型的工具清单"（省 token），工具仍然**可以被调用**。
- `IsExpanded` 是**筛选语义**（`"False"` = 整组隐藏，见插件的 `compileFilterList`），与界面上的展开/收起**无关**：界面上点目录名只是把该目录下的工具行显示出来，所以这个值平时只做原样带回。唯一例外是每个目录条目工具行底部的「折叠/可见」开关会改写它所属目录的这个值（见 `TopBar.cpp` 的 `groupHiddenChanged`）。
- **异步约定**：全部走 `QNetworkAccessManager` 的完成回调，主线程只做收发、从不等待（没有 `waitForFinished`、没有阻塞循环），因此不会卡界面。
- **为什么这个头文件里没有 `Q_OBJECT`**：它在 vcxproj 里登记为普通 `ClInclude`（不跑 moc），所以异步结果一律用 `std::function` 回调返回；UI 半在 `include/TopBar.h`（QtMoc）。

## `include/common/appearance/UiStage.h`

"架空原 UI"的让渡机制（宿主把整个客户区让给客户端扩展自绘）。

- **为什么在 `ExtensionSystem/`**：它是客户端扩展机制的**宿主侧那一半**（抢台 / 还台 / 强制收台），与 `ClientExtension` 的装载、卸载同属一套。判定方是 DSHHub（`core/`，谁在架空）与 WindowFrame（`common/appearance/`，遮罩范围与窗口条命中测试）；**后者构成 `common/appearance → ExtensionSystem` 的唯一一条依赖边**，属轻微的层次倒置，动这两层时要留意。让渡动作与"宿主窗口具体是哪个类"无关（只要是个 QMainWindow）。
- 宿主侧改动因此能压到"**接口 + if**"：DSHHub 只转发（见 `DSHHub.h` 里那 4 个内联 override），WindowFrame 只问一句 `isTakenOver()` / `captionBand()`，真正的让渡动作全在本文件。
- 线程规则：全部只允许 GUI 线程调用（与 `CommonRegistry`、UI 一致），非 GUI 线程一律拒绝，并对每个入口只告警一次。
- **抢台的所有权契约**：`owner` 必须等于扩展的安装目录名（宿主的 `ClientExtension` 会在 `attachHost()` 之前用可选槽 `setHostIdentity` 把它推给插件），宿主靠它做所有权校验与"卸载时强制收台"。
- `acquire()` 的失败与幂等规则：已被别人占着 → 拒绝（同一时刻只允许一个 owner，**绝不静默顶掉**）；`owner` 为空 / 非 GUI 线程 → 拒绝；同一 owner 重复调用**幂等**（返回同一个舞台）—— 因为切主题会重建窗口并对同一插件实例再调一次 `attachHost()`，而"重挂"正是那次该做的事。
- `release()` 里 `owner` 为空 = 不问身份（宿主的兜底路径用）；非空且与当前不符 → 拒绝并返回 `false`。
- ⚠️ **`release()` 为什么不删舞台里的控件**：那些控件归扩展，其 vtable 可能已经不在本模块里（卸载扩展后 dll 已解映射），删一次就是崩；扩展必须在还台**同一时刻**自己删干净 —— 这是 `DshHostPlugin.h` 里 `detachHost()` 的既有契约。
- **`releaseForOwner()` 这条兜底为什么必须有**：不能指望插件在自己的 `detachHost()` 里调 `ExternalReleaseStage` —— 插件崩了或忘了的话，宿主控件树里会留着一个 vtable 指向已解映射内存的控件，`unload()` 之后碰一下就崩。
- 自绘窗口条：扩展自绘标题栏时**必须**调一次 `setCaptionBand()`，否则窗口只能靠 Alt+Space 拖动 / 贴边吸附；`captionBand()` 是供 `WindowFrame::hitTest` 回落用的查询入口，`height > 0` 才算有效。

---

## `include/core/DSHHub.h`

- **遮罩与居中**走的就是宿主自己那条弹窗路径（`WindowFrame::showOverlayWithPopup`），所以"遮罩先到、弹窗后到"的缝不存在。插件必须**先把窗口建好**（控件搭完、尺寸定下）再调 `ExternalShowOverlay`。
- 插件侧取接口的完整示例：`DshHost::findObject(DshHostIndex::kMainWindow)` → `qobject_cast<VirtualWindow*>` → `window->ExternalShowOverlay(myWindow)`。
- **会话投影为什么必须按块合并、不能整包覆盖**：同一份数据有两个来源 —— `session/follow` 快照是**全量折叠**（`sessionStats` + `tokenUsage` 两块都有），`session/list` 行只是**投影缓存检查点**（可能只有 `tokenUsage`）。早先整包覆盖时，后到的列表行会把快照带来的"轮/步 + LLM/工具/首 token"那段擦掉，表现是闪一下然后只剩"缓存命中 / 输入输出"。
- `asOfSeq` 用服务端那套 **higher-seq-wins**，旧值直接丢，避免用停得很旧的检查点盖掉新值。
- 小灰字的数据源**曾用过 `session/list` 行，已弃用**（那是缓存检查点，冷会话会很旧）；现在只认 `session/follow` 的 projections 与 `session/control` 的 baseline + 实时帧。这块暂时留在 god class 里，等接口稳定再分离（候选：一个 `SessionStatsService` 取数 + 输入区的控制器推给控件）。
- **新建会话的 `workspaceId` 只能来自 `workspace/follow` 基线**：服务端 `session/create` 的响应里没有 `workspaceId`，而 0.1.5 已删掉 `workspace list` 这个 RPC。
- 架空让渡动作（摘原生客户区、建舞台、收宿主浮层、窗口条登记）**刻意全在 `ExtensionSystem/UiStage.cpp`**，`DSHHub` 里一行逻辑都不写；那 4 个 VirtualShell 转发定义在头文件里而不是另开 .cpp（都是单行转发，另开编译单元只多一份样板，也省得往 `.vcxproj` 再登一个源文件）。

## `include/core/MessageHost.h`

- **为什么这些不放进 `MessageQuery` 自己**：`MessageQuery` 是"每个会话一份、切会话就整体替换或被缓存接管"的**短命值对象**，而这里管的是**跨会话存活的单例**（滚动区、按钮、队列、缓存、当前会话身份）—— 单例的所有权放不进多实例对象，所以单独一层，`MessageQuery` 一行不改。
- mux 帧路由（`handleMuxFrame`）原先在 `DSHHub` 里，拆出来后帧的第一个消费者就是这里，`DSHHub` 只把 `DshApiClient::muxFrameReceived` 接过来转发；拆出来时明确**不再碰**会话标题与工作区清单（都归 `DSHHub` 的侧栏）。
- `m_sessionLastSeq` 原先住在 `DSHHub`，随 mux 帧路由一起搬来 —— 它是 **mux 观测**，不该由窗口持有。恢复缓存时用它判断缓存内容是否已被后来的事件超越：超越就得重建，没超越就秒开。
- **载入中提示层**是懒建、常驻复用，父控件是聊天区 viewport（只盖聊天区、鼠标穿透）。
- **流式渲染态用定时器节流 50ms 批量重渲染**，避免每个 chunk 都全量重排；`fitContentThenLayout` 是"消息列先撑够高度再铺布局"，避免流式期间气泡被挤扁。
- **`m_followBottom` 是用户"意图"而不是滚动位置**：光看滚动位置不行 —— 流式输出时内容一直在长高，底部会从脚下溜走。
- `SmoothWheelScroller` 由滚动区持有（父对象是滚动区），这里只借用：钉滚动位置前先 `stop()`，并借 `isAnimating()` 判断"用户正在滚"。
- **`showSession` 的决策顺序就是"控件缓存 → 预取页 → 网络拉取"**，中间那些（缓存命中/预取/拉页/分批构建）都在这一类里拍板，`DSHHub` 只说"切到谁"。
- 预构建控件树的**队列配额有限、每轮事件循环只建一个**（每个控件树占内存）。

## `include/core/ServerManager.h`

- **为什么要显式写空数组 `llm-deepseek: models: []`**：适配器自带的默认目录（deepseek-flash 等 4 条）**只在该路由的 `models` 缺席时才生效** —— 写了空数组才等于"这条路由一条也不公布"。
- 只在文件缺失时写入：用户自己配置过的 harness **一个字都不动**。之所以落在客户端：数据根是客户端建的；缺这一步时"清空 harness"会让随附模型复活。
- 内置插件台账（`.dsh-hub-builtin.json`）记录已装内容的 **revision** ⇒ 语义是"首次装一次、源码变了才重装"，**不是每次启动都覆盖**。
- `takeProcess()` 的用途：主题切换等"重建窗口但复用同一服务端"的场景移交服务进程，接管方通过 `start()` 的 `initialServerProcess` 参数拿回。

## `include/core/HostExports.h`

- **为什么需要这层 C 导出**：宿主是 Application、无 `.def`、无 `dllexport`，导出表本来是空的 —— 插件直接调 `CommonRegistry::instance()` 会 **LNK2019**。而"插件也编一份 `CommonRegistry.cpp`"更糟：会得到**第二个单例**，插件的登记与查询和宿主静默分家。
- ABI 规则：一律 `extern "C"`（导出表里是未修饰的名字），参数与返回值只用 POD；返回的 `void*` 实际是 `QObject*`，插件自己转成**全内联接口**再调，接口一旦有 out-of-line 成员就撞回 LNK2019。**动签名 = 动 ABI**，必须同步升 `DSHHUB_HOST_ABI_VERSION`。
- 查询只在主线程可用：注册表未加锁，且表里是 UI 对象；非主线程返回 NULL 并（仅首次）打一条 `qWarning`。插件在 Worker 线程要用对象就得自己投到主线程。
- **刻意不导出 `AddToRegistry`**：让插件往宿主全局表里塞对象，会把"谁负责析构"变成跨 DLL 的所有权问题。
- `index` 是纯字符串、**没有编译期检查**，拼错只会静默拿到空对象（和"没登记""不在主线程"长得一模一样，无从区分），所以两侧都必须从 `DshHostIndex` 取，别手写字面量。
- `resolveHostSymbol` 用 `QCoreApplication::applicationFilePath()` 而不是 `GetModuleHandle(NULL)`，是为了不必引 `<windows.h>`；用 `QLibrary::resolve` 的**静态**重载，是因为成员版本会被局部 `QLibrary` 对象的析构把库卸载掉。

## `include/core/ToolRequestDispatcher.h`

- 抽出动机：把"命名管道请求 → 线程池执行 → 回投 GUI 线程发送响应"从 `DSHHub` 里拿出来，控制器只保留一句 `dispatch()`。纯 header + inline ⇒ 无需链接改动。
- `QLocalSocket` **只在 GUI 线程读写**，所以响应必须经 queued 连接回投；投递前要判 bridge/client 是否已空（客户端已断开或桥已销毁时直接丢弃响应）。
- DLL/COM 调用挪到 Worker 线程执行：GUI 不被长任务卡住，不同 DLL 可并行。同一连接上多个请求的响应可能乱序返回，**Node 端按请求 id 配对**。

---

## `include/ExtensionSystem/ClientExtension.h`

- **客户端扩展与工具扩展是两套东西**，只是都用 `.ext` 交付：工具扩展 → 服务端扩展目录、Worker 线程跑 JSON 工具调用、登记在 `extensions.json`；客户端扩展 → `<exe>/clientExtensions/`、GUI 线程用 `QPluginLoader` 装载。分流按 regulation 的 `Type` 字段。
- **`installedNames()` 与 `loadedNames()` 的语义差别**：前者扫 `<dir>/<Name>/regulation.json5`、反映**磁盘状态**（重启后仍在、装载失败也在），安装与移除都以它为判据；后者是进程内已装载集合，同时是防重复装载的依据。
- **`remove()` 的两条路径**：① 插件声明了 `detachHost()` 槽 → 先让它拆掉挂在宿主里的东西，再 `QPluginLoader::unload()`（会顺手删掉插件根组件、dll 随之解映射 ⇒ **目录当场删干净**；前提是装载时清掉了 `PreventUnloadHint`）；② 没声明 → 删 `regulation.json5`（判据，必定成功），再尽力删目录，被映射着的 dll 删不掉就写 `.pending-removal`，由**下次启动**清扫。
- **`remove()` 返回 false 的完整含义**：判据已删（不再算已安装、下次启动也不会装载），但还有文件被占用删不掉。
- **为什么不做"改名 / 移动目录"的花招**：Windows 上躲不开文件锁（实测：目录里有被占用的文件时，连**父目录改名都会被拒**）。

## `include/ExtensionSystem/ComCaller.h`

- **为什么名字里没有 "Json"**：参数虽以 JSON 传输，但执行的是 COM 自动化（`IDispatch`），与 `DllCaller` 的 json/native 风格是**并列关系**。
- **组件白名单设计**：`ProgId` 只在 `regulation.json5` 的 `"Com"` 段声明，运行时 `args` **不能任意指定对象**。
- `args` 字段契约：`member`（方法/属性名，必填）、`kind`（`method` 默认 | `get` | `put`）、`path`（可选，先沿属性链下行）、`params`（`kind=method` 的参数）、`value`（`kind=put` 要写入的值）。
- 结果契约：成功 → `{ "value": <JSON 标量> }`，结果为对象时附带 `"object": true`（仅摘要）。
- **能力边界**：当前只支持无状态的"一次一调"（每次新建组件实例、**无对象句柄保活**），对象继续下钻的会话能力留待后续。

## `include/ExtensionSystem/DllCaller.h`

- **为什么名字里没有 "Json"**：同一份描述里既有 `"json"` 风格，也有 `"native"` 风格（经 Thunk 按真实签名调用），本类两者都负责。
- json 风格的三个桥接签名：`int Func(const char* argsJson, char** resultJson)` / `void Func(...)` / `string Func(const char* argsJson)`。
- **结果内存契约**：json 风格（`ReturnType=string`）的工具若其 DLL 导出了 `ClearMem`，则返回值是 DLL `malloc` 的堆内存，本类读取后**立即调用 `ClearMem` 归还**；老扩展无 `ClearMem` 时保持"拷走即用、不释放"的兼容行为。
- **`resolvedDllPath` 为什么必须有**：多扩展可能各有同名 `main.dll`，调用期一律按规范化**绝对路径**查找 / 加载，避免按短文件名缓存导致的**串库**。
- **线程模型**：`callTool` 在 Worker 线程执行；**同一 DLL 的调用经 `runMutex` 串行**（旧扩展可能带 static 缓冲 / 非重入代码），不同 DLL 之间并行。
- `inFlight` / `drained` 的用途：卸载 / 移除时等待在途调用结束，防止卸载中的 `QLibrary` 被使用。

## `include/ExtensionSystem/ExtensionLoader.h`

- **两条分流路线**：无 `Type` / 其它值 → 工具扩展（包内有 `Function[]` 与 `AttachedPlugin/`）；`ClientExtension[Debug]` → 客户端扩展（包里只需 regulation + 一个 dll）。**只共用 `.ext` 这层壳 —— 载荷、宿主、线程、登记位置都不同**。
- `isClientExtension` 的调用方契约：true ⇒ 应在 GUI 线程调 `ClientExtension::loadOne(...)`，且**不要碰** `extensions.json` / `cordis.patch.yml` / 服务端重启。
- 字段的安装期语义：`jsonPath` / `dllPath` 指的是"**已落到扩展目录的那份**"（不是包里临时解出来的那份）；`pluginName` 对客户端扩展取自 regulation 的 `Name`。
- `regulation.json5` 是 **JSON5**（允许注释与尾随逗号）。

## `include/ExtensionSystem/Thunk.h`

- **Windows x64 专用**：运行时根据参数 / 返回值描述生成一段**可执行机器码**，把统一参数数组转换成目标 DLL 函数的**真实调用约定** —— 只有 native 风格需要它。
- 统一参数槽的内存布局：每个参数固定占 **8 字节**；Bool/Int/String 用 int64/pointer，Double 用 double 的位模式（`Arg::as` 是 union，这也是 `Pointer32` 字段存在的原因）。
- thunk 入口 ABI 统一为 `void (*)(const Arg* args, void* result)`；`build()` 失败原因经 `errorString()` 取（不抛异常）。

---

## `include/chat/`（6 个头文件）

### 量高与渲染的坑**不在头文件里**，在 .cpp —— 以下是它们的准确地址

这一层瘦身时最重要的一条发现：`documentSize()` 历史 bug、`QPlainTextDocumentLayout` 的单位、"按行数×行高"、延后到事件循环重算、`m_proseViews` 的 `deleteLater` 时序，**全都不在 `include/chat`**。要改这些行为，去看：

- **不用 `documentSize()`、改按"行数 × 行高"** —— `src/chat/CodeBlockView.cpp:55-62`（`lines * lineSpacing + 20`；20 = viewport 6+6 + 文档 margin 约 8；注释写明历史 bug 是"会算成单行"）。
- **`fitProseView` 的宽度陷阱 = 流式抖动的根因** —— `src/chat/AgentMessageUnit.cpp:160-178`：新建视图未入布局前宽度还是 Qt 默认 100px，而 `QTextBrowser` 的文档宽度跟随视口；此时读 `documentSize()` 得到的是"按 100px 窄宽换行"的高度（**实测长回复可达 7000+ px**），一旦 `setFixedHeight()` 提交，气泡瞬间被撑极高、下一轮拟合才恢复。**必须先 `setFixedWidth(contentWidth)` + `setTextWidth` 再量。**
- **控件加入布局后要延后到事件循环再重算** —— `src/chat/AgentMessageUnit.cpp:203-205`（用 `QTimer::singleShot(0, ...)` 等布局真正跑完）。
- **`clearParts` 的删除时序** —— `src/chat/AgentMessageUnit.cpp:262-273`：`takeAt` 逐个摘下，**先 `hide()` 再 `deleteLater()`**（可能正处在自身 `anchorClicked` 信号处理中），最后 `m_proseViews.clear()`；不能直接 `delete`。
- `documentSize()` 已包含 `QTextDocument` 自身边距 —— `src/chat/UserMessageUnit.cpp:75`。
- `QPlainTextDocumentLayout` 的高度单位是**行数不是像素** —— `src/ui/ChatInputWidget.cpp:599`。

### `AgentMessageUnit.h`

- 架构：旧实现是**单篇 QTextBrowser 文档**；现为 QWidget + 垂直布局的"部件流"。**动机**：同一气泡内普通文本与代码块必须严格按出现顺序排布。每"一段普通文本"一个 QTextBrowser（`objectName=agentProse`），Markdown 围栏切成独立 `CodeBlockView`（上方带语言小标签 `objectName=codeBlockLang`）。
- **bulk 模式**：`append*` 期间不逐次拟合高度，整批结束后统一 `updateHeightToContent()`（对应 `MessageQuery::setBulkFitting`）。
- **流式去重**：`m_lastFlushedFingerprint` = 最近一次已渲染内容的指纹（类型 + 内容哈希），无变化就跳过整段重建。
- **流式增量渲染**：只重画"正在增长的最后一段"，7 个 `m_live*` / `m_streamSealedCount` 成员跟踪尾部 live 区域。
- `proseHost()` 的宿主选择：只有"末尾部件本身就是 ProseView"才复用它；否则在布局末尾新建一个空 ProseView —— **保证锚点/分隔符总落在消息真实尾部之后**。
- 行内代码**先替换成占位符**再交给 Qt，避免 Qt 解析丢失样式。

### `MessageQuery.h`

- 上翻走一元 `session/page`，**必须带 `throughSeq`**（缺了服务端回 `gateway/input-invalid`，所以必须有回落游标）；`beforeSeq` 是当前内容里最早一条事件的 seq（**排他上界**）；`maxMessages` 数的是**消息**不是事件。
- `session/page` 返回的记录**全部**比 `beforeSeq` 更早 ⇒ 整页插到顶部即可，不需要旧版"比条数取差集"。
- 游标可能来得晚：`load()` 在拿到游标前挂起并起看门狗；超时后若 `DSHHub` 喂过回落游标就直接发请求，否则**明确报错**，不让 UI 一直转圈。
- **`firstHistoryArrived` 的时序约束**：只有首屏真正上屏（分批构建完成、控件已挂进实时布局）才发 —— **不能在"开始构建"时就收遮罩，否则会先露出空白聊天区**。
- **`m_reachedEnd` 不能用 `m_history->hasMore()` 做门控**：部分流程里该值与服务端实际不符，会导致"明明还有更多却一直提示没有更多"。
- **`seedFromPrefetched` 与 `seedFromSnapshot` 的区别**：前者不做新鲜度判定，直接走分批构建（每轮事件循环 5 条）。**原因：整树一次性冷布局会造成单帧阻塞（实测 116 条 ≈ 307ms）**。
- 老页插入后按插入前记录的锚点校正滚动，且要**分多次直到几何稳定**。
- 只播种最近 `kSeedEventCap = 200` 条（`src/chat/MessageQuery.cpp:605`）：快照一页可能几十条消息，全量构建会明显拖慢切会话。

### `CacheHistoryManager.h`

- ⚠️ 原文件头声称预取缓存"已删除"是**错的**（代码是活的）—— 该句已删除。
- **判"缓存还能不能用"必须靠游标，不能靠内容条数**：0.1.5 起首屏来自 `session/follow` 快照，不再是"重拉同一个 maxMessages 尾窗口"，条数不再可比。
- 三个游标：`throughSeq` = 缓存建立时的 follow 游标；`oldestSeq` = 缓存内容里最早一条事件的 seq（"加载更多"的 `beforeSeq`）；`lastSeq` = 最新一条事件的 seq（对比新快照 cursor 判过期）。
- 三者齐全时恢复缓存可**完全跳过重拉与二次渲染**（秒开），且"加载更多"依旧可用。

---

## `include/ui/`（16 个头文件）

### `ChatInputWidget.h`

- **为什么 `ChatInputWidget` 不是卡片本身**而是"卡片 + 小灰字"的竖排容器：小灰字要落在卡片**外面**（原生 composer 就这么排），而它由本控件创建 —— 所以卡片本体下沉成一层内层控件（`m_capsule`）；样式规则仍认 `#inputCapsule`，后代选择器不受影响。
- **阴影走绘制不走 QSS**：QSS 没有 `box-shadow`，而原版这张卡片带 `--dsw-shadow-lv2`，所以外面套一层 `ShadowPanel`（`m_capsuleShadow`）画阴影，**卡片自己的 QSS 规则一条都不用改**。
- 阴影规格放在本类的原因：`Main.cpp` 要拿它的四周留白反推输入区边距（**边距 = 原边距 − 留白**，卡片宽度才不会被阴影挤窄），两处共用一个数字以免漂移。
- **`SessionStatsLine` 与官方的有意差异**：官方在没有可显示内容时整行不渲染，这里改成「轮/步」与「输入/输出」两组**无条件出现** —— 新会话看到的是 `0 轮 · 0 步 | 输入 0 tok · 输出 0 tok` 而不是一片空白；其余组无有意义 0 表示，仍为 0 就不出现。
- 字段语义与官方 `StatsLine.d.ts` / `turn-metrics.d.ts` **完全一致**（`sessionStats` @dsh-session-stats、`tokenUsage` @dsh-token-meter），所以"服务端给什么就画什么"两边同一套规则。
- 那行小灰字固定 14px 行高、一行居中、超宽用省略号并把完整内容挂 tooltip；**有/无统计时行高不变，输入区不会上下跳**。

### `Sidebar.h`

- 数据与 RPC 逻辑在 common（`SessionCatalog` / `SessionService`），本文件**只保留控件与绘制**。
- 会话列表的滚动容器：列表内容再长也只滚动，**不参与撑高侧栏**。

### `TopBar.h`

- **`m_expanded` 不用 `m_body->isVisible()` 反推**：窗口还没显示时（重建发生在打开之前那种情况）子控件的 `isVisible()` 一律 `false`，反推会让"第一次点击"被吞掉。
- **判断"新会话"必须用 `m_collapsedSeedSession`，不能用 `m_sessionId`**：`setContext()` 会先把它覆盖成新会话，比较永远相等。
- **目录默认全折叠**，展开状态是"这一次翻看"的状态；配置里的 `IsExpanded`（`"False"` = 整组隐藏）是另一回事 —— 后者写的是筛选语义。
- **`m_layout` 必须是类级成员**：原先是构造里的局部变量，但外部要通过 `GetLayout()` 拿到它，局部变量在构造结束后就够不着了。布局顺序：标题 | stretch | 已挂的外部控件 | 工具按钮。
- 跨 DLL 契约：插件侧 `qobject_cast<VirtualTopBar*>(host)` → `GetLayout()->addWidget(...)`；⚠️ 别改成 `dynamic_cast`（Itanium ABI 下跨模块静默返回 `nullptr`），也别 `qobject_cast<TopBar*>`（要 `TopBar::staticMetaObject` ⇒ **LNK2019**）。

### `SmoothWheelScroller.h`

- **为什么需要这一层**：Qt Widgets 默认的滚轮处理是一次同步 `setValue` —— 本机实测（Qt 6.11.2）一格（`angleDelta 120`）= `wheelScrollLines(3)` × `singleStep(20)` = **60px，中间没有任何过渡帧**，观感就是"跳格"。
- **宿主还需要知道"用户正在滚"**：流式输出时宿主会"跟随底部"，原判定是"离底 80px 内"—— **比一格滚轮(60px)还大**，于是用户往上滚一格会被下一帧立刻拽回底部。
- **光看位置不够**：滚轮事件是**同步**启动补间的，而那一帧可能赶在补间第一个步进之前（此时滚动条还没离开底部）—— 所以宿主必须能拿到"用户要离开底部"的同步信号（`userScrolledAway`）与"正在滚"状态（`isAnimating`）。
- 装在任意滚动区上即可，不需要换控件类型；键盘 / 拖动滚动条 / 程序 `setValue` 都不受影响；**滚动区自己没得滚时不接管，事件照旧往上层的滚动区传（嵌套滚动链不变）**。
- `stop()` 必须在宿主自己要把滚动位置钉到某处**之前**调用，否则两者打架。
- 目标滚动位置用 `double`：高精度设备（触控板）给的是小增量，取整会把它磨没。

### `Tooltip.h`

- **为什么值得单独做一层**：`QToolTip` 是 Qt 内部自己建的顶层 `QLabel`，`QToolTip { ... }` 这套选择器只能改底色/文字，**圆角、描边、阴影、内外边距一概不生效**；在自绘圆角窗口里很突兀。
- **零改动接管**：文案的唯一来源仍是 `QWidget::toolTip()`，所以既有的 20 余处 `setToolTip(...)` 一行都不用改；`TranslationUi` 的"按快照就地换文案"读的也正是它。
- **首次悬浮的延时由 Qt 给**（实测 `QEvent::ToolTip` 到达时距鼠标移动约 700ms，且从一个有提示的控件移到另一个时 Qt 会自己缩短延时）—— 所以**不叠加任何自定义延时**，加了就是双重等待、手感发黏。
- 气泡窗口是**常驻单例**，而 `setMode()` 只换全局调色板、不重挂已存在的顶层窗口 ⇒ 显示前要检查深浅色是否变过，变了就重挂一次样式表。

### `TitleBar.h`

- 按钮只发"意图"信号，动作由 `common/WindowFrame` 落地（接线在 `DSHHub` 构造函数）；外观全走 QSS，这里只换字形与发信号。
- 拖动 / 双击最大化 / 贴边吸附 / 右键系统菜单**都不在这里实现** —— 命中测试把本控件覆盖的区域（按钮除外）当成系统标题栏交回系统。
- **两条对外约定**：`objectName` 固定 `windowTitleBar`（拖动区/遮罩范围按它算）；三个窗口按钮带动态属性 `dshWindowControl=true`（命中测试排除，否则点按钮会变成拖窗口）。
- `kHeight = 42`，`Main.cpp` 据此换算窗口高度。

### `ShadowPanel.h`

- 外壳透明，只在四周留白里画一圈阴影（`CardShadow`）；**被包的卡片保持原样** —— QSS 里的背景、圆角、边框一条都不用改。
- `spec` **必须显式给**：各面的留白成本不一样（浮层不占布局、贴边的面板要从布局里切），用默认值容易悄悄用错档。
- `setPadding` 的硬约束：要让外壳外面的布局不受影响，**覆盖值四边之和必须与 `padding(spec)` 保持一致**（总高不变）；传 `QMargins(-1,-1,-1,-1)` 恢复"按 spec 推导"。
- 卡片圆角要跟卡片自己的 QSS 一致，阴影形状才对得上。

### `StatusPopupWindow.h`

- **为什么提取成基类**：插件市场弹窗与扩展管理弹窗原本各自复制了一份状态文本排版代码。排版逻辑是纯绘制辅助（`QFontMetrics`），所以留在 ui 层而不是 common。
- `setStatus` 的完整文本**同时作为 tooltip**，并按当前标签宽度重排。

### `ModelSelector.h`

- 历史沿革：**原来是只管档位的 `ThinkingDepthSelector`**，改名并扩写后模型选择也归它，两者共用同一份目录数据。
- 菜单在 chip **正上方**弹出（上拉），高度按内容适配、装不下才滚动。
- **换模型时把档位清空**（交给新模型的默认档位），换档位时沿用当前 `provider/model`；两次都走 `session.selectModel`，**以服务端回显为准**。
- **会话自己的选择在 `session/list` 行的 `projections.values.modelSelection` 里**（`modelCatalog` 只给部署默认值 `default`），由 `DSHHub` 从 `SessionCatalog` 取出来喂给 `overrideCurrentSelection`；`provider/model` 为空表示"服务端还没记录"，此时保持目录给的默认值不动。
- 用 `QPushButton` 而非裸 `QWidget`：与原生 composer 的 `<button>` 语义一致，同时天然获得键盘焦点与无障碍/自动化可调用性。要自己覆写 `sizeHint`/`minimumSizeHint`（自身无文本，默认值偏小）。
- `submitSelection` 是**乐观更新**：先改 chip → 发 RPC → 以服务端回显为准。

### `ModelListPanel.h`

- 表单放在滚动区**内部**（而不是窗口底部）：这样面板高度不随表单开合变化，"添加模型"按钮就不会跟着上下跳。
- 数据与写入都在服务端：本面板**不保存任何模型清单**（刷新即重新向服务端要）。
- 「获取模型」按当前路由问一次 `llm/discoverModels`，候选在输入框下方展开成下拉、点一条回填模型 ID（**只读，不写配置**）。

### `Settings.h` / `PluginsManager.h`

- 两者都是**随主窗口创建后一直存在的常驻对象**，不是一次性窗口实例；职责都是"界面搭建与交互 + 窗口本身的开关管理（遮罩、居中、判重、关闭清理）"。
- 开关统一走 `openSettings()/closeSettings()`、`openPlugins()/closePlugins()`：主窗口只保留少量调用，**不再在 `DSHHub` 里管理遮罩成员**。
- 遮罩是**窗口级**的（设置/插件/扩展管理/工具过滤共用同一层，由 `WindowFrame::showOverlay/hideOverlay` 持有）；宿主 resize 时通过 `syncOverlayToHost()` 保持铺满。
- **每次打开都 `refreshOnOpen()` 重新拉数据**，避免常驻对象在服务端未就绪时就联网请求。
- 模型与凭据**一律只与"当前所连服务端"打交道**：客户端不做任何本地配置读写，也不针对某个具体提供方写死任何东西（引用名由服务端 profile 给出）。
- `agentPresetChanged` 的**生效范围由服务端定：只影响此后新建的会话，已有会话不受影响**。

### `PopupWindow.h` / `ExtensionManagerPopup.h` / `LoadMoreButton.h` / `SpinnerWidget.h`

- `PopupWindow`：无系统边框（无原生边框/圆角/白底/灰细边框/右上角关闭按钮）；空标题显示**可翻译的默认名**；语言切换后标题与关闭提示要跟着换。
- `ExtensionManagerPopup` 继承 `StatusPopupWindow` 是为了与插件弹窗样式一致；逻辑分工：`extensions.json`/扩展目录/`cordis.patch.yml` → `ExtensionRegistry`，后台解压与安装 → `ExtensionInstallTask`。交互上**不直接弹文件选择框**，而是先打开管理窗口在其中安装。

---

## `include/network/DshApiClient.h`（原 36 行说明书，最重要的一份协议知识）

- **RPC 协议形状**：endpoint 是 `<namespace>/<method>`（0.1.5 起用**斜杠**，不再是点号）；body 固定 `{type:"client-request", rpcId, method, payload:{args:{…}}}`，其中 `payload` **必须恰好只有一个 `args` 对象**，键名是描述符里的 wire 名（`session/list`→`_request`、`session/create`→`request`、`agentPresets/list`→无参）；应答统一 `{type:"server-response", rpcId, result:{ok, value|error}}` —— **HTTP 成功也可能 `ok == false`**。返回裸数组的端点（如 `llm/listConfigurableProviders`）要用 `callMethodValue`。
- **认证围栏全链路**：启动令牌只出现在服务端打印的认证 URL（`http://127.0.0.1:<port>/?token=…`）；`GET` 它返回 **303 + `Set-Cookie: dsh-auth-<authority 哈希>=<签名值>`**；此后**所有 HTTP 请求与 WebSocket 握手都必须带该 cookie**，否则一律 401 ⇒ 所以顺序固定为 `setBaseUrl()` 取令牌 → `startAuthHandshake()` 换 cookie → `openStreams()`。
- **mux 拓扑**：单条 `/api/remote.mux` 上跑三条逻辑流 —— `$events`（首帧 `{type:"ready", clientId, host}`；waterfall 审批/提问帧**必须回执**：`respond()` 发 `POST /api/$events/result`，`args={clientId, eventId, outcome:{kind:"result", value}}`）、`workspace/follow`（baseline/upsert/remove/order/archived）、`session/follow`（首帧 snapshot = cursor + records + hasMore 播种首屏，之后 event 帧才是实时日志）。
- **`muxFrameReceived` 为什么存在**：0.1.5 线上帧形状变了，这里把新帧**翻译成旧形状**再发，好让 `DSHHub::handleMuxFrame` 与 `InteractionHandler` 保持不动。翻译表：`session/follow` 的 event/snapshot → `{payload:{type:"session/event", sessionId, event}}`；`$events` 的 waterfall `approval/request` → `{rpcId:<eventId>, payload:{type:"approval/requested", …}}`；`user-questions/request` → `{type:"question/requested", …}`。
- ⚠️ **逻辑流 id 必须唯一**：服务端遇到重复 id 会 `close(1008)` 关掉**整条 mux**（不只是那条流），所以每次 open 都走 `nextStreamId()`；断线由 `scheduleReconnect()` 自愈并重开三条流。
- ⚠️ `muxFrameReceived` 里的 rpcId 用的是 0.1.5 的 `eventId`，`respond()` 会把它原样发回 `/api/$events/result` —— 即"**旧形状里塞的其实是新 id**"。
- **三条异步/竞态设计的理由**：① `QueuedCall` —— 服务端刚重启（扩展安装、插件市场装完、手动重启都会触发）那一两秒里"没有 cookie 就直发"必然 401，用户点发送就撞上，故先挂起、握手完成后按序补发；② `m_authAttempt`（认证代次）—— 服务端重启后旧地址那次握手会**晚一步失败**，靠它把过期回包丢掉，不让它清掉新一轮的在途标记、也不让它覆盖 cookie；③ `JsonParseRunnable` —— 大 JSON 回包不在主线程 `fromJson`，解析完经 queued `invokeMethod` 回投主线程。
- **`sessionProjectionsReady` 值得单独一个信号**：快照这条路上服务端用 `projectionMode:"all"` 做**全量折叠、直接从日志算**，所以"没被 Agent 附着"的会话（只是打开看看）也能立刻拿到整条日志的累计值；而 `session/list` 行给的是**投影缓存里的检查点，可能为空或很旧**（缓存按 200 事件 / 5s 落盘）。
- 沿革：旧的 `/api/events.mux`、`/api/events.host`、`POST /api/respond` 在 0.1.5 已不存在；本客户端**不支持 0.1.1 及更早**的服务端。

## `include/network/SessionPrefetcher.h`

- **为什么必须走一元 `session/page` 而不能开流**：`session/follow` 会在快照后 **promote** 该会话（激活 Agent + 常驻 follower），给 N 个会话开流等于激活 N 个会话。
- 为什么复用 `DshApiClient` 而不是自建 `QNAM`：要在认证栅栏下自带认证 cookie 与 `args` 信封，否则 401。
- 结果去向：`historyFetched` 交给 `DSHHub` 入库（`CacheManager`），并可选地预构建控件树。

## `include/common/appearance/ThemeManager.h`

- **为何必须是 QObject 单例**：扩展（QPlugin DLL）只能经 `CommonRegistry` 按 index 取对象再转接口 —— 命名空间与自由函数**没有 QObject 身份、够不到**。
- **`init()` 的调用窗口**：**QApplication 创建之后、主窗口创建之前**；释放默认模板 → 按 mode 读调色板 → 合成 QSS → 登记进全局注册表。mode 优先用 `AppearanceSetting.json` 的显式选择，未设置（System）才跟随系统。
- **`ExternalApplyToWindow` 为何要额外的"外部名字"**：转换走 `obj->qt_metacast(IID)`，跨边界传的是**字符串**、插件侧零宿主符号；接口虚函数走 vtable 同样不产生外部符号 ⇒ **接口一旦发布只能增不能改**；`External*` 与宿主内部 API 刻意分开。
- **`ExternalReloadStyles()` 的边界**：它**不重建窗口**，构造期就已固化的东西（如按 `isDark()` 选的 logo）不会跟着变 —— 要那种一致得走 `switchTheme()` 那条重建窗口的路。返回值 `false` 基本只有一种成因（文件根本没读到），故也充当事后自检。
- **常量为何做成"返回引用的函数"而不是 static 数据成员**：类/命名空间作用域的 `QString`/`QStringList` 会在**静态初始化期构造**，`main` 之前就分配堆内存；而 `installPaletteFor` 有副作用（换掉整个应用的 `QPalette`），只允许被 `setMode()`/`reload()` 间接调用。
- **`repolishScrollArea` 的完整成因**：`QAbstractScrollArea` 的滚动条在**基类构造**里创建并首次解析规则，那时子类构造函数体的 `setObjectName(...)` 还没执行，`QStyleSheetStyle` 就把"匹配不到 `#objectName QScrollBar`"**缓存**了下来；之后再设 objectName 不会触发重新匹配 ⇒ 凡是"先建控件、后设 objectName"的滚动区都要在设完名字后补调一次。
- `switchTheme` 是"用户显式选定主题"的**唯一入口**，会把新主题写进 `AppearanceSetting.json` —— 之后启动**不再跟随系统**。

## `include/common/appearance/TranslationManager.h`

- `.ts` 的正式文件**不维护 `<source>`**，写了也会被 `tools/release-translations.ps1` 删掉；原文由 `zh_CN` 的 `<translation>` 承担。
- **"查不到就显示代号"是刻意保留的**：缺哪条一眼可见；唯一例外是 `TranslationUi.cpp`（空译文被挡下）。改文案必须同步更新语言包。
- **"就地换文案"有两块明确不覆盖**：用 `arg()` 拼出来的**动态文案**（如"共 3 个模型，1 个提供方。"）不是整串文案、匹配不上；已渲染进 HTML 的**历史消息**（如思考块标题）。这两处要随语言变就得各自的重建/重渲染路径。
- `apply()` 在没找到对应 `.qm` 时会**退回源语言并返回 false**（不只是"失败"）。

## `include/common/util/`

- `CodeHighlighter.h`：高亮会被**渲染 worker 线程并发调用**，规则/缓存读写统一加锁；**用递归锁**是因为 `loadFromFile` 持锁期间会调用同样加锁的 `clearCache()`。
- `MarkdownPreprocess.h`：Qt 6 的 Markdown 导入器遇到 **`<br>` 会丢弃其后同一行/单元格的所有内容**导致表格截断，而 **U+2028 会被渲染成真正换行且不破坏表格解析**。
- `CommonRegistry.h`：本文件的方法刻意**不析构**（必须活得比所有被登记对象更久）；`Find<T>` 已移除（`RegType` 方案随之废弃），取对象走 `FindFromRegistry`。

## `include/common/extension/`

- `ExtensionRegistry.h`：`ensurePatchEntry` 的 **id 与 name 分开传**是因为两者不总相等（如 `session-stats` 的 id 是短名、name 是包名）；**判重看 name 行**；`comment` 非空时写成一行 **ASCII** 注释 —— 该文件常被别的工具读，无 BOM 的中文注释容易显示成乱码。
- `PluginMarketClient.h`：`registryLoaded` 的 `source` 可能是 `snapshot` / `cache` / 其它（实时数据）；`fetchDiagnosticLogs` 在 **5xx / 传输错误时自动触发**；`endpointUrl` **只借 baseUrl 的 scheme/host/port** 拼接口地址。
- `PluginMarketInstaller.h`：PATH 里没有 pnpm 时**生成 pnpm shim** 用本地 node 跑 `pnpm.cjs`；以 `DSH_HOME`/`PATH` 环境变量启动 `pnpm add dshmarket`；装完把 dshmarket 写进 profile 的 `dependencies` + `dsh.profile.bundles`；`installFinished(true)` 表示**退出码 0 且 profile 清单已更新**。
- `ExtensionInstallTask.h`：`tryFinish()` **只成功返回一次**；`waitForFinished()` 存在的理由是**弹窗关闭时等待，避免任务写到已销毁的对象**。

## `include/common/settings/`

- `ClientSettings.h`：明文文件 vs 注册表的取舍、可被 `DSHHUB_CLIENT_SETTING_DIR` 覆盖、`QSaveFile` 原子替换 —— **手改坏一个字符只会丢设置，不会让客户端起不来**。
- `SettingsStore.h`：键名**只允许出现在这里**；界面语言与主题**不在** QSettings；默认 Agent 预设归服务端设置、客户端刻意不留副本；`main.cpp` 未设 org/appName 所以 `server/url` 也存不住。

## `include/core/`（残留）

- `MessageHost.h`：`cancelBuild()` 在**切会话、丢弃会话**时都要调；`onPrefetched()` 的三分支恰好对应 `PrefetchOutcome` 的三个枚举值；`syncLoadingGeometry()` 只在**窗口尺寸变化**时用；`swapInBuilt()` 把离屏构建好的列表换成当前列表，`handOffToCache()` 把当前实例**与分页元数据**一起交给缓存。
- `HostExports.h`：`findObject()` 的完整返回语义 —— **符号取不到 / index 为空 / 不在主线程 → 空**；返回 `QPointer`，故长期持有也不会成野指针。
- `DSHHub.h`：VirtualWindow 的三条口头约定（**窗口要先建好**、**show/hide 必须成对**、**遮罩只留一层、按 owner 记名**）；`workspace/follow` 的 order 与 archived 帧都是**整体替换**（不是增量打点）；小灰字**每次变化整包重算一行文本**、**换会话时连同 `asOfSeq` 一起清空**、**无当前会话时擦掉文字但高度照旧占着**（防布局跳动）。
