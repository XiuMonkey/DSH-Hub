# 交接提示词：让 DSH Hub 使用其它服务端（例如 Codex）作为后端

> **这份文件是一份提示词**，用于把工作交接给新的会话 / 新的 agent / 另一位开发者。
> 接手者看不到之前的讨论，所以本文自成一体；完整方案、决策记录与事实依据在同目录的 **`misc/API_TAKEOVER_PLAN.zh-CN.md`**（下称"方案文档"），本文只给"任务、入口、边界、验证"。
>
> 仓库根：`C:\Users\Playe\Documents\DSH hub\DSH Hub`（下述路径都相对它）

---

## 0. 任务目的（写清楚，别跑偏）

**目的：让 DSH Hub 客户端能用其它服务端（例如 Codex）作为后端，而不是内置的 DSH 服务端。**

⚠️ **范围划分**：这项工作有**两半** —— ① **宿主侧的接管机制**（本交接的范围：接口 + `DshApiClient` 分流 + 停掉内置服务端 + 禁用 DSH 专属旁路）；② **插件侧的适配**（写客户端扩展：接出站请求、调 Codex、把数据按宿主帧格式喂回 UI）。**先做①**，②可以在机制跑通后另开一轮。

实现路径（已定，不要再重新论证）：

1. 给内置的 `DshApiClient` 加一个**"被接管"开关**；
2. 加**两个公共纯虚接口**：一个宿主实现（插件调用）、一个插件实现（宿主调用）；
3. 一个**进程内客户端扩展**（QPlugin DLL，`.ext` 里 `Type: ClientExtension`）接管：
   - **所有出站请求**（宿主不再走 HTTP，交给扩展）；
   - **入站数据注入**（扩展调宿主槽，把数据喂回现有 UI 管线）；
4. 内置 DSH 服务端**不再启动**（它原本是这条路线上唯一的后端）。

**关键设计判断（已定）**：不接管 UI 的意图信号、不重写宿主上层、不改 B 类回执信号。
只需在 `DshApiClient` 内部按开关分流 —— 因为**全部出站都经过这一个类**（8 个方法、18 个一元端点、4 条 mux 流、认证握手）。宿主上层（`DSHHub` / `MessageHost` / 服务层 / `MessageQuery` / `HistoryLoader`）**一行不改**。

---

## 1. 先读什么

| 顺序 | 文件 | 读什么 |
|---|---|---|
| 1 | `misc/API_TAKEOVER_PLAN.zh-CN.md` | 全文。重点：**§2.6 接口与通讯机制（定案）**、**§3 必写点**、**§5 已排除结论**、**§6 事实速查** |
| 2 | `source/include/network/DshApiClient.h` + `source/src/network/DshApiClient.cpp` | 要改的主体。看 `callMethod`(:389) / `post`(:484) / `onReplyFinished`(:535) / `handleParsedResponse`(:580) |
| 3 | `source/include/core/DshHostPlugin.h`、`source/include/core/HostExports.h` | 插件契约、注册表取址方式、全内联接口的 ABI 规则 |
| 4 | `source/include/VirtualClass/VirtualCommon.h`、`source/include/core/ConnectionManager.h` | **接口形态的现成先例**（`class ConnectionManager : public QObject, public VirtualConnectionManager`，`ConnectionManager.h:17`），照它写 |
| 5 | `misc/DESIGN_NOTES.zh-CN.md` | `:27-30` 的接口 ABI 四条规则；`ExtensionSystem` 与 `ClientExtension` 各节 |

**客户端扩展的参考样例在仓库外**（不在 git 里）：`..\CustomStylesExtension\`、`..\ShellDemoExtension\`、`..\test extension\`（各自带 README）。

---

## 2. 已定方案摘要（够你开始写代码）

### 2.1 两个接口

```cpp
// 接口一：宿主实现（挂在 DshApiClient 上），插件调用 —— 插件 → 宿主
class VirtualApiHost          // ← 名字待定
{
public:
    virtual ~VirtualApiHost() = default;                                  // 全内联
    virtual void Takenover(bool on) = 0;                                  // D1=A，返回 void（D0=A）
    virtual void CompleteCall(const char* rpcId, const char* resultJson) = 0;   // D3 回填成功
    virtual void FailCall(const char* rpcId, const char* code,
                          const char* message) = 0;                       // D3 回填失败
    // ⚠️ 以后只许在末尾追加
};
Q_DECLARE_INTERFACE(VirtualApiHost, "com.DSH_HUB.VirtualApiHost/1.0")

// 接口二：插件实现（挂在插件根对象上），宿主调用 —— 宿主 → 插件
class VirtualApiSink          // ← 名字待定
{
public:
    virtual ~VirtualApiSink() = default;
    virtual void OnOutboundRequest(const char* rpcId, const char* method,
                                   const char* argsJson) = 0;
};
Q_DECLARE_INTERFACE(VirtualApiSink, "com.DSH_HUB.VirtualApiSink/1.0")
```

- 两个接口都放**独立的全内联头文件**（建议 `source/include/VirtualClass/`）。
  **不要**把虚方法加进 `DshApiClient.h`：那个类的 ctor/dtor/方法全是 out-of-line，插件一碰就 `LNK2019`。
- 宿主的 `DshApiClient` 多继承接口一（照 `ConnectionManager.h:17`）。
- 插件根对象同时实现 `DshHostPlugin` 与接口二；宿主照 `ClientExtension.cpp:127` 的 `qobject_cast<DshHostPlugin*>(root)` 再加一个 cast —— **拿不到就在装载期拒绝**。

### 2.2 出站分流（`DshApiClient` 内部）

- 状态：一个 `bool`（`D1=A`）。
- 8 个出站方法全部要分支：`setBaseUrl`(:44)、`openStreams`(:53)、`closeStreams`(:56)、`followSession`(:59)、`unfollowSession`(:62)、`callMethod`(:68)、`callMethodValue`(:75)、`respond`(:82)。
- 接管时**不走 HTTP**：生成 `rpcId` → **把回调存进现成的 `m_pending`** → 调 `interface2->OnOutboundRequest(rpcId, method, argsJson)`。
- 扩展回填时调 `CompleteCall(rpcId, resultJson)` / `FailCall(rpcId, code, message)` → 宿主**调原来那个回调**。
  （两个回调是 `std::function`，**不是 metatype，不能随接口/信号传出去** —— 所以必须"传标识、宿主持有回调"。）

### 2.3 接口指针怎么给插件

把 `DshApiClient` **注册进宿主注册表**（新增 index，如 `"apiClient"`，加在 `HostExports.h` 的 `DshHostIndex`）。插件 `DshHost::findObject("apiClient")` → `qobject_cast<VirtualApiHost*>`。

- 【已验证】时序没问题：`m_api` 在 `DSHHub.cpp:57` 创建 → 登记在 `:451` → `ClientExtension::loadAll()` 在 `:458`。
- 登记/注销照抄成对模式：`DSHHub.cpp:451`/`:611`、`Sidebar.cpp:592`/`:599`。
- 切主题会重建窗口、换掉 `m_api` ⇒ **插件在每次 `attachHost()` 里重新取**（`DshHostPlugin.h:20-22` 已保证 `attachHost()` 会被多次调用）。`findObject` 返回 `QPointer`，旧指针只会变空、不会变野。

### 2.4 入站注入：复用宿主槽（D5=B）

插件用字符串 `invokeMethod` 调 `DSHHub` 的槽（当前都在 `private slots:`，**字符串 invokeMethod 不受访问级别限制**）：

```cpp
QObject* hub = DshHost::findObject(DshHostIndex::kMainWindow).data();   // = DSHHub
QMetaObject::invokeMethod(hub, "forwardMuxFrame", Q_ARG(QJsonObject, frame));
```

可注入的 11 个槽（签名见方案文档 §2.6）：`handleConnected`、`forwardMuxFrame`（主力）、`handleSessionSnapshot`、`handleSessionProjections`、`handleSessionControlBaseline`、`handleSessionProjectionChanged`、`handleWorkspaceSnapshot`、`handleWorkspaceUpserted`、`handleWorkspaceRemoved`、`handleWorkspaceReordered`、`handleWorkspaceArchiveChanged`。

📌 已说明：**后续会把其中部分槽公开**（提升为 `public slots:` 或整理成正式清单）。在那之前按上表调用；注意槽名是私有契约，宿主改名会静默失效。

### 2.5 类外问题的处置（已定）

| 项 | 处置 |
|---|---|
| `TopBarTools`（`/api/tools-filter`，自带 NAM） | **禁用**（关入口/不实例化） |
| 插件市场（`PluginMarketClient` 7 个 `/dsh-market/*` + `PluginMarketInstaller` 的 pnpm/dsh CLI） | **只关市场入口**（侧栏 `pluginsRequested` → `PluginsManager`）。⚠️ **不要动"扩展管理"**（`extensionsRequested` → `ExtensionManagerPopup`）—— 客户端扩展正是从那里装的（`ExtensionManagerPopup.cpp:208`） |
| `ServerManager` 启动 DSH 进程 | **D8 = C：不阻止启动，接管时停掉它。** 背景：装配顺序是 `installServer()`(`DSHHub.cpp:69` → `:181` 调 `m_serverManager->start()`) **早于** `registerHostObjects()`(`:84` → `:458` 调 `ClientExtension::loadAll()`)，且 `:65` 的注释说明这是**故意**的（Node 启动与后续步骤并行）⇒ 接管调用阻止不了启动。<br>**怎么停**：⚠️ 不用 `takeProcess()`（那是"移交给下一个窗口"用的），照 `ServerManager::restart()` 里现成的 kill 写法（`ServerManager.cpp:232-237`：`kill()` + `waitForFinished(2000)` + `delete` + 置空），建议加一个明确入口如 `stopForTakeover()`。<br>**必须一起处理的连带项**（详见方案文档 D8=C）：① 进程可能已 `ensureBuiltinPlugins()` / 写过 `settings.yaml` / **已发过 `baseUrlReady`**（`DSHHub.001` 已经调过 `m_api->setBaseUrl()`）；② `m_api` 可能已开始认证握手与开流 ⇒ 必须与 §3.1 的出站分支配套；③ 到来时进程可能是 nullptr / 正在启动 / 已退出 ⇒ 判空；④ `attachHost()` 会被多次调用 ⇒ **停止动作必须幂等**；⑤ 接管态下让 `ServerManager::restart()` 这类路径变 no-op |
| 卸载时复位接管状态 | **由插件的 `detachHost()` 负责**；宿主**不加**兜底。需在 `DshHostPlugin.h` 的 `detachHost()` 说明里补一条契约 |

---

## 3. 你要改/新增的文件（建议顺序）

1. **新增接口头**（两个纯虚类 + IID），放 `source/include/VirtualClass/`。
   ⚠️ `Q_DECLARE_INTERFACE(...)` 写在**全局作用域**（不要在 namespace 里）——照 `VirtualCommon.h:105-109` 的写法。
2. `source/include/core/HostExports.h`：加 `kApiClient` index 常量。
3. `source/include/network/DshApiClient.h`：多继承接口一；**并在类里加 `Q_INTERFACES(<接口一>)`**；加 `bool m_takenover`；声明三个 `override`。
   ⚠️ **`Q_INTERFACES` 不能漏**：`qobject_cast<接口一*>` 走的是 moc 生成的 `qt_metacast`，而它只认类里 `Q_INTERFACES` 列出的 IID —— 漏了就**永远 cast 出 nullptr**（而且不报错）。照 `ConnectionManager.h:19-20` 的 `Q_OBJECT` + `Q_INTERFACES(...)` 成对写法。
4. `source/src/network/DshApiClient.cpp`：**8 个出站方法的分支** + 接管路径（存回调 + 调接口二）+ `CompleteCall`/`FailCall` 的实现。
5. `source/src/core/DSHHub.cpp`：`m_api` 创建处登记 `kApiClient`；销毁处注销；接管时停掉已启动的 DSH 进程（D8=C）。
6. `source/src/ExtensionSystem/ClientExtension.cpp`：装载时 `qobject_cast<VirtualApiSink*>(root)`，拿不到就拒绝（并记日志）。
   ⚠️ 插件侧也要在自己的根对象上写 `Q_INTERFACES(DshHostPlugin VirtualApiSink)`（多个 IID 用空格分隔），否则这个 cast 同样是 nullptr。
7. 禁用三条旁路（见 2.5）。
8. `source/include/core/DshHostPlugin.h`：`detachHost()` 契约补一条。

**必写点清单（最容易漏的）**：方案文档 **§3.1 + §3.2** 共 10 条。其中三条尤其关键：

- 回填入口**不要直接复用 `handleParsedResponse`** —— 它第一步查的是 `m_parsing`，而接管路径的条目只在 `m_pending` 里，会命中 "stale parsed response" 分支**静默丢弃回调**（`DshApiClient.cpp:586-590`）。
- `respond` 有一个前置门槛 `m_clientId.isEmpty()` 会直接返回（`:447-456`），接管后不连 mux ⇒ `m_clientId` 恒空 ⇒ **审批应答根本不会被转发**，必须在接管态绕过。
- 三种结果语义要分开：`callMethod` 成功=object（`:410-413` 包了 `toObject()`）、`callMethodValue` 成功=裸值（`:437`）、失败=`RpcError`。

---

## 4. 还剩两项，边写边定（不阻塞动笔）

1. **D6 · 接管态下读接口的返回值**：`baseUrl()`（5 处调用，例 `Settings.cpp:361` 回显）、`launchToken()`（1 处）、`isConnected()`（1 处）该返回什么。
2. **D7 · 扩展侧的错误码词汇表**。【已验证】上层**没有任何一处比较 `error.code`**，全部拼进用户可见文本 → 自定即可。

---

## 5. 明确不做的事（避免走回头路）

| 不做 | 原因 |
|---|---|
| **接管 A 类意图信号**（`TakeoverConnection` 接管 `newWorkspaceRequested` / `sendRequested` 等） | 只关某个入口；而类内分流下任何入口都必须经过出站方法，覆盖更完整，且不动宿主上层 |
| **改宿主上层 / B 类回执信号** | 上层一行不改，回执照常 emit（`DSHHub`/`MessageHost`/服务层/`MessageQuery`/`HistoryLoader`）——**B 类不需要处理** |
| **用信号做两个方向的通讯** | 已定走公共虚函数接口；信号路线会引入"插件连信号只能用字符串版、PMF 会 `LNK2019`"和"共享头里放 `Q_OBJECT` 类会 moc 两份、`qt_metacast` 误判"两个坑 |
| **把虚方法加进 `DshApiClient.h`** | 那个类 out-of-line，插件一碰就 `LNK2019` |
| **修 `TakeoverConnection` 的裸签名缺陷（P1）** | 本方案不用它。那是独立缺陷（登记表存的是裸签名、字符串版 `connect` 需要 `"2"/"1"` 前缀），另有记录 |
| **宿主侧卸载兜底 / owner 记名 / 独占校验** | 实验阶段不做；复位由插件的 `detachHost()` 负责。留档思路见方案文档 §4.2 |
| **不要连带关掉"扩展管理"** | 那是客户端扩展的装载通道 |

---

## 6. 环境 / 构建 / 验证

**构建测试目标**（Windows / MSVC + Qt 6.11.2 + Ninja，工程已配置好）：

```bat
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
"D:\Qt\Tools\CMake_64\bin\cmake.exe" --build "<仓库>\build\windows-ninja" --target dshhub_tests
```

**跑测试**（两个坑）：

1. 程序是 **WIN32 子系统，抓不到 stdout** —— 用 `DSHHUB_TEST_REPORT_DIR` 取逐类报告：
   `set DSHHUB_TEST_REPORT_DIR=%TEMP%\reports`，然后运行 `build\windows-ninja\DSH Hub.Tests.exe`，看 `report-<类名>.txt`。
2. **必须从仓库根运行**（`TestThunk` 读的是相对路径 `tests/test_signature.json`），否则会出现"1 failed"的假失败。

**基线**：**209 passed / 0 failed / 3 skipped**。3 个 skip 是依赖仓库外 `test extension/NativeTunkExt` 的 `DllCaller` 用例（正常现象）。

> ⚠️ **测试只覆盖未接管路径**（含 `TestDshApiClient` 的 11 个）。新分支**没有任何回归保护**，改动要靠手工验证。

---

## 7. 关键事实速查

| 事实 | 位置 |
|---|---|
| 出站单一咽喉：8 个方法、18 个一元端点、4 条 mux 流、认证握手，**零 `virtual`** | `source/include/network/DshApiClient.h:25,44-82` |
| 出站调用链（`callMethod` → `post` → `m_pending` → `handleParsedResponse`） | `DshApiClient.cpp:389 / 484 / 509 / 580` |
| `m_pending`（等响应）/ `m_parsing`（正在解析）是**两张表** | `DshApiClient.h:205-206` |
| 接管路径的回填不能复用 `handleParsedResponse`（它读 `m_parsing`） | `DshApiClient.cpp:586-590` |
| 入站：12 个信号、15 处 `emit`；唯一的 `QWebSocket` | `DshApiClient.h:88-118`、`DshApiClient.cpp:49` |
| 宿主注册表实际只登记 5 项，`DshApiClient` 不在其中 | `DSHHub.cpp:451`、`Sidebar.cpp:592`、`TopBar.cpp:702`、`ThemeManager.cpp:369`、`main.cpp:24` |
| 可注入的槽（`private slots:`，invokeMethod 可调） | `source/include/core/DSHHub.h:93-133` |
| 插件装载 / `detachHost` 调用 / 不实现时不 unload | `ClientExtension.cpp:196-220` |
| 客户端扩展目录：`<exe>/clientExtensions/<Name>/`（`DSHHUB_CLIENT_EXTENSION_DIR` 可覆盖） | `source/src/ExtensionSystem/ClientExtension.cpp:149-156` |
| **装配顺序：服务端先起、插件后装** ⇒ 接管阻止不了启动，只能接管时停掉（D8=C） | `source/src/core/DSHHub.cpp:68-84`（`:181` 起服务端 / `:458` 装插件） |
| 停止 DSH 进程的现成写法（`kill()` + `waitForFinished` + `delete` + 置空） | `source/src/core/ServerManager.cpp:232-237`（`restart()` 内）。⚠️ `takeProcess()`(`ServerManager.h:24`) 是"移交给下一个窗口"，不是"停止" |
| 接口 ABI 四条规则（全内联 / vtable 末尾追加 / 改 IID = 改 ABI / 参数可用 Qt 类型） | `misc/DESIGN_NOTES.zh-CN.md:27-30` |

---

## 8. 交接时的工作树状态

**本项工作还没有写过任何实现代码** —— 只有方案文档。

**本轮讨论新增的两个文档**（未跟踪，`git status` 会显示）：

- `misc/API_TAKEOVER_PLAN.zh-CN.md` —— 方案 + 决策记录 + 事实依据
- `misc/API_TAKEOVER_HANDOFF.zh-CN.md` —— 本文

**同一次会话里改过的代码**（与本方案无关，是两组独立修复，已构建并跑通 209 基线）：

- `source/src/ExtensionSystem/Thunk.cpp`、`source/include/ExtensionSystem/Thunk.h` —— `sub/add rsp` 由 imm8 改 imm32（15/16 参数时 128 无法用 imm8 表达，符号扩展会把"分配栈帧"变成"抬高栈顶"）
- `tests/TestThunk.cpp`、`tests/TestThunk.h` —— 新增 `testMaximumStackArguments`（断言 prologue 编码，已验证在修复前的代码上会失败）
- `source/src/ExtensionSystem/DllCaller.cpp` —— `runMutex` 先释放再递减 `inFlight`；`removeExtension` 改成"先等在途归零再摘表"；8 处 `libraryForPath()` 裸解引用补判空
- `source/CMake/CMakeLists.txt`、`tests/DSH Hub.Tests.vcxproj` —— 测试源清单补 `ConnectionManager.cpp` + moc 头（**原有的构建缺陷**：两份清单都缺它，导致测试目标链接失败）

⚠️ **工作树里另有约 25 个与本方案无关的既有未提交改动**（`ui/*.cpp`、`DSHHub.cpp`、`PluginMarketInstaller.cpp` 等）以及 4 个未跟踪新文件（`ConnectionManager.{h,cpp}`、`ExtensionDllLoader.h`、`SessionProjectionState.h`）。动工前先 `git status` 看清哪些不是自己的，**不要误当成自己的改动**。

---

## 9. 给接手者的第一条行动建议

先把 **§2.6 的两个接口头文件**写出来（纯虚、全内联、两个 IID），编译通过即可 —— 它不依赖任何其他改动，且能立刻验证"接口形态 + 多继承"这套在本项目的构建里没问题。之后再动 `DshApiClient` 的分支。

（若接口命名要改，两个 IID 字符串一并改；**IID 一旦对外发布就不能随意改** —— 改 IID 会让旧插件静默 `nullptr`。）
