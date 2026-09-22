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
