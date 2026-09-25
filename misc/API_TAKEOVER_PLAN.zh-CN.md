# DSH Hub 后端接管方案

> 让客户端扩展（进程内 QPlugin DLL）接管后端：关掉内置 DSH 服务端，服务端逻辑与内容处理由扩展负责，宿主 UI 继续用现有渲染管线。
>
> **先看第 1 节**（需要你决定的事项）。第 2 节之后是已定方案、清单与事实依据。
>
> 标注：【决定】= 已确定 ｜【事实】= 已在源码核实（附 `文件:行号`）｜【暂缓】= 本期不做

---

# 1. 需要你决定的事项

> **D0–D8 全部已定案**，没有待定项了：
> 通讯机制见 **2.6（定案）**、三条旁路与进程处置见 **2.5-A**、
> 实现期定下的 D6/D7 见 **2.8**（按本节原约定回填，条目已清空）。

---

# 2. 已定的方案

## 2.1 目标

客户端扩展（`.ext` 里 `Type: ClientExtension` 那条路线）接管后端：内部 DSH 服务端不再启动，服务端逻辑与内容处理由扩展负责，宿主 UI 沿用现有渲染管线。

## 2.2 接口形态【决定】

- 接口是**独立的、全内联的公共纯虚类**，扩展与宿主都 `#include`；宿主继承并实现，扩展 `qobject_cast` 后调用。
- 先例：`class ConnectionManager : public QObject, public VirtualConnectionManager` + `Q_OBJECT` + `Q_INTERFACES(...)`（`source/include/core/ConnectionManager.h:17-20`），接口写在 `source/include/VirtualClass/VirtualCommon.h`，宿主在 `source/src/core/main.cpp:24` 登记。
- 约束（照 `misc/DESIGN_NOTES.zh-CN.md:27-30`）：全内联、不派生 `QObject`、无 out-of-line 成员；`Q_DECLARE_INTERFACE` + IID；宿主侧 `Q_OBJECT` + `Q_INTERFACES`；参数可用 Qt 类型（两边共用同一份 Qt）。
- **不能**把虚方法直接加在 `DshApiClient` 上：它的 ctor/dtor/方法全在 `.cpp`（out-of-line），插件一碰这个类型就 `LNK2019`（`source/include/core/HostExports.h:4`、`source/include/core/DshHostPlugin.h` 开头）。

## 2.3 出站分流【决定】

- 在 `DshApiClient` 的出站方法里按接管状态分流：**被接管时不走 HTTP，把请求交给扩展**。
- 只交**可序列化**的部分（`rpcId` / `method` / `args`）；**回调不交给扩展**，仍留在宿主的 `m_pending` 表里（`source/include/network/DshApiClient.h:205`），扩展回传结果后**由宿主触发原回调**。
- 依据【事实】：两个回调是 `std::function`（`DshApiClient.h:68-82`），不是 metatype，不能作信号参数、不能跨 queued 连接 ⇒ 唯一可行形态就是"传标识、宿主持有回调"。
- 分支允许**完全独立**：接管态下可以写一整套新的处理语义（错误语义、超时、完成路径），不必复用未接管路径的实现。

## 2.4 入站【决定】

- 入站数据仍走现有 12 个信号（`connected` / `muxFrameReceived` / 各 snapshot / 各 workspace / `transportError`，`DshApiClient.h:88-118`），宿主上层不动。
- 扩展向宿主注入数据的通道见 D5。

## 2.5 类外问题的处置【决定】

### A · 三条出站旁路 —— 全部禁用

理由：它们服务的都是 DSH 服务端；本方案要支持的是其它服务端。

| 旁路 | 证据【事实】 | 处置 |
|---|---|---|
| `TopBarTools` → `/api/tools-filter`（自带 `QNetworkAccessManager`） | `source/src/common/appearance/TopBarTools.cpp:16`、`:141`、`:189`、`:296` | 关掉入口/不实例化 → 顶栏工具过滤面板消失 |
| 插件市场（`PluginMarketClient` 的 7 个 `/dsh-market/*` + `PluginMarketInstaller` 的 `pnpm add dshmarket`、`dsh plugin add`） | `source/include/common/extension/PluginMarketClient.h:24-29`、`PluginMarketInstaller.cpp` | **只关市场**（侧栏 `pluginsRequested` → `PluginsManager`） |
| `ServerManager`：启动/重启 DSH 进程、写 `settings.yaml`、装内置插件 | `source/src/core/ServerManager.cpp:211-225`、`:239-245` | **D8 = C：允许 DSH 先启动，接管时由宿主停掉它** —— 见下 |

#### D8 = C · "接管时停掉已启动的 DSH 进程"【决定】

**为什么不阻止启动**：【事实】`DSHHub::DSHHub` 的装配顺序（`source/src/core/DSHHub.cpp:68-84`）是

```
installWindowShell();
installServer(...);        // :69 → 内部 :181 调 m_serverManager->start()   ← 拉起 DSH 进程
...
registerHostObjects();     // :84 → 内部 :458 调 ClientExtension::loadAll()  ← 插件 attachHost()
```

而且 `:65` 的注释写明这是**故意**的（Node 启动约 1s，与后续步骤并行）⇒ 插件在 `attachHost()` 里调 `Takenover(true)` 时，进程**已经起来了**，接管调用阻止不了它。**决定**：不动这个顺序，改成接管时停掉。

**怎么停**：⚠️ 不是 `takeProcess()` —— 那个是"把进程移交给下一个窗口"用的（`ServerManager.h:24`，会把 parent 置空并交出去）。要的是**照 `ServerManager::restart()` 里那段现有的 kill 写法**（`ServerManager.cpp:232-237`）：

```cpp
if (m_serverProcess && m_serverProcess->state() != QProcess::NotRunning) {
    m_serverProcess->kill();
    m_serverProcess->waitForFinished(2000);
    delete m_serverProcess;
    m_serverProcess = nullptr;
}
```

建议在 `ServerManager` 上加一个明确的停止入口（例如 `stopForTakeover()`），内部复用上面那段，并判空。

**必须一起处理的连带项（否则会留下不一致状态）**：

1. **进程可能已经开始/做过事**：`ServerManager` 启动时会 `ensureBuiltinPlugins()`（装内置 cordis 插件）、`ensureFactorySettings()`（写 `settings.yaml`）、解析 stdout 拿 baseUrl 并发 `baseUrlReady`。也就是说**接管到来时 `baseUrlReady` 可能已经发过**，`DSHHub.001` 的 lambda 已经把 `m_api->setBaseUrl(url)` 和 `m_pluginsManager->setBaseUrl(...)` 调过了。
2. **`m_api` 可能已经开始连**：`setBaseUrl` 内部会启动认证握手并开流。所以"停进程"必须与**出站分支**（§3.1 那 8 个方法）配套 —— 尤其是 `setBaseUrl` / `openStreams` / `followSession` 在接管态要能正确地"不连/放弃"。
3. **时机不确定**：`Takenover(true)` 到来时进程可能**还没 spawn**（`m_serverProcess == nullptr`）、**正在启动**（stdout 还没输出 baseUrl）、或**已经退出**。判空与"进程不存在时也算成功"是必须的。
4. **`attachHost()` 会被多次调用**（切主题，`DshHostPlugin.h:20-22`）⇒ `Takenover(true)` 会重复到来 ⇒ **停止动作必须幂等**（已经停了就是 no-op）。
5. **"重启服务端"这类流程**：市场入口已禁用（见上表），但 `ServerManager::restart()` 本身仍可能在别处被触达（如 `DSHHub.004` 的 finished 分支、主题切换的 `takeProcess()` 移交）⇒ 接管态下要让这些路径变成 no-op 或明确拒绝。

⚠️ **不要连带关掉"扩展管理"**。它是**另一个 UI**：

| UI | 侧栏信号 | 类 | 作用 |
|---|---|---|---|
| 插件市场 | `pluginsRequested` | `PluginsManager` | DSH 服务端侧的 cordis 插件 —— **要关** |
| 扩展管理 | `extensionsRequested` | `ExtensionManagerPopup` | `.ext` 安装/移除（`ExtensionInstallTask` → `ExtensionLoader::loadAndInstall`）—— **必须保留**，客户端扩展正是从这条路进来的（`ExtensionManagerPopup.cpp:208` 调 `ClientExtension::loadOne`） |

### B · 接口指针的获取与生命周期 —— 用注册表

- **获取**：把 `DshApiClient` 注册进宿主注册表（新增 index，例如 `"apiClient"`），扩展用 `DshHost::findObject("apiClient")` 取 `QObject*` 再 `qobject_cast` 出接口。**不用**"公共虚函数返回自己"。
- 【事实】时序现成可用、无需调整：`m_api` 在 `source/src/core/DSHHub.cpp:57` 创建 → 注册表登记在 `:451` → `ClientExtension::loadAll()` 在 **`:458`**（登记之后）。登记/注销照抄成对模式：`DSHHub.cpp:451`/`:611`、`Sidebar.cpp:592`/`:599`、`TopBar.cpp:702`/`:707`。
- **失效**：切主题会重建窗口，`attachHost()` 会被同一插件实例**再次调用**（`source/include/core/DshHostPlugin.h:20-22` 明文契约），`m_api` 随之换对象（`DSHHub.h:225`）⇒ **插件在每次 `attachHost()` 里重新 `findObject` + 重新 cast**。
- 辅助条件【事实】：`findObject` 返回 `QPointer<QObject>`（`HostExports.h:62-68`），旧指针不会变野、只会变空；但"不空"不等于"还是当前那个 client"，所以必须重取。

### C · 接管状态的归属与卸载兜底 —— 由插件负责，不加宿主兜底

- **复位由插件的 `detachHost()` 负责【决定】**。这是契约上更正确的位置：它在 `unload()` **之前**被调用（`ClientExtension.cpp:200` 在 `:203` 前），插件还活着、还能通过接口做收尾。
- ⚠️ 前提：`detachHost()` 是**可选**的元对象字符串契约。宿主只在插件实现了它时才调用（`ClientExtension.cpp:198-200`）；没实现时**既不调 detach 也不 unload**（`:216-219`），插件继续留在内存里。
- 📌 需要在 `source/include/core/DshHostPlugin.h` 的 `detachHost()` 说明里**补一条契约**：「实现了 `detachHost()` 的插件必须在这里复位接管状态」。
- **不加宿主兜底【决定】**：实验阶段首要任务是跑通；扩展不实现、或实现了却没复位，都算扩展自己的问题。保护机制以后再做。

**风险地图（现状下会有哪些表现）：**

| 情形 | 会发生什么 | 现状处置 |
|---|---|---|
| 插件实现 `detachHost()` 且在其中复位 | 正常 | — |
| 插件实现 `detachHost()` 但**忘了**复位 | `unload()` 成功（`ClientExtension.cpp:203`）→ 插件已解映射 → 标志仍为 true → **所有 RPC 无响应、UI 永久停在加载中、无任何报错** | **接受**（实验阶段：重启客户端） |
| 插件**未实现** `detachHost()` | 不 unload → 插件仍映射、**仍能正常服务** → 标志保持 true 无害；只是"界面已移除却仍在服务"直到重启（既有幽灵行为，`UiStage` 同样） | 接受 |
| 插件内部崩溃 | 进程内 DLL 崩溃 = 整个客户端消失 → 标志随之消失 | 无需处理 |
| 上次卸载残留目录被下次启动清扫（`sweepPendingRemovals()`，`ClientExtension.cpp:60-80`） | 全新进程，标志不存在 | 无需处理 |

**暂缓（放下）**：owner 记名、独占校验（"同一时刻只允许一个 owner，绝不静默顶掉"）、`Takenover` 的返回值语义（拒绝时怎么告知调用方）。
- 这些是**多扩展并发接管**才需要的；单扩展原型阶段用不上，也是"以后做保护机制"时的内容。
- 对照既有实现：`VirtualShell::ExternalAcquireStage(const char* owner)` 用 owner 记名；`UiStage::acquire` 用返回 `nullptr` 表示拒绝。
- ⚠️ **签名有时间敏感性**：若将来要做 owner 记名，`Takenover` 的参数**现在就要预留**（见 D1）。

### ⚠️ 不要照搬 `UiStage` 的兜底位置

`ClientExtension.cpp:224` 的 `UiStage::releaseForOwner(name)` 位于 `:213` 那次 `return true` **之后** —— **最顺利的成功路径根本不会执行到它**。那对 `UiStage` 是有意的（它有一份"插件必须在 `detachHost()` 里还台"的契约，顺利路径由插件负责，宿主那行只是不顺路径的兜底，见 `UiStage.h:19`、`ClientExtension.cpp:222`）；而接管标志**没有**这份契约的等价物，照搬会漏掉最常见的情形。`UiStage` 在这里只作为**失效模式的先例**引用（宿主不指望插件自觉），两套机制在架构上无关。

## 2.6 接口与通讯机制（定案）【决定】

**D1 = A**（`Takenover(bool)`）、**D0 = A**（返回 `void`）、**D2/D3/D4 全部走公共虚函数接口**、**D5 = B**（入站注入复用宿主槽）。

### 接口一：宿主实现，插件调用（插件 → 宿主）

| 方法 | 签名 | 作用 |
|---|---|---|
| 接管开关 | `void Takenover(bool)` | D1=A + D0=A |
| 回填成功 | `void CompleteCall(const char* rpcId, const char* resultJson)` | D3 |
| 回填失败 | `void FailCall(const char* rpcId, const char* code, const char* message)` | D3 |

- **只有三个方法** —— 因为 D5 选了 B，入站注入**不占接口**（见下）。
- 宿主侧实现：`DshApiClient : public QObject, public <接口一>`（照 `ConnectionManager.h:17` 的多继承先例）。
  ⚠️ **类里必须同时写 `Q_INTERFACES(<接口一>)`** —— `qobject_cast` 走 moc 的 `qt_metacast`，它只认 `Q_INTERFACES` 里列出的 IID；漏了就是**永远 nullptr 且不报错**（`ConnectionManager.h:19-20` 是 `Q_OBJECT` + `Q_INTERFACES` 的成对样板）。`Q_DECLARE_INTERFACE` 要写在**全局作用域**（照 `VirtualCommon.h:105-109`）。
- 插件取得它：注册表 → `DshHost::findObject("apiClient")` → `qobject_cast<接口一*>`（见 2.5-B）。
- ⚠️ `Takenover` 返回 `void` ⇒ 插件无法得知接管成没成。补偿：见"接口二"那条（宿主装载时就能判定插件有没有实现接收端，从而当场拒绝/报错）。

### 接口二：插件实现，宿主调用（宿主 → 插件）

| 方法 | 签名 | 作用 |
|---|---|---|
| 出站请求 | `void OnOutboundRequest(const char* rpcId, const char* method, const char* argsJson)` | D4 |

- 插件**根对象**同时实现 `DshHostPlugin` 与接口二（类里写 `Q_INTERFACES(DshHostPlugin <接口二>)`，多个 IID 空格分隔）；宿主照 `ClientExtension::loadOneImpl` 里 `qobject_cast<DshHostPlugin*>(root)`（`ClientExtension.cpp:127`）的先例再加一个 cast。
- ✅ **装载时即可判定插件有没有实现接收端**：cast 得 nullptr → 当场拒绝接管 / 报错。

### 入站注入：复用宿主槽（D5 = B）

插件**不通过接口**注入数据，而是用字符串 `QMetaObject::invokeMethod` 调 `DSHHub` 的槽。这些槽当前都在 `private slots:`（`source/include/core/DSHHub.h:93-133`）——**字符串 invokeMethod 不受访问级别限制**。

可注入的 11 个槽：

| 槽 | 签名 | 喂什么 |
|---|---|---|
| `handleConnected` | `()` | 让 UI 认为已连接（开流之后） |
| `forwardMuxFrame` | `(QJsonObject)` | 一帧 mux 消息（会话事件 / 审批 / 提问）—— **主力入口** |
| `handleSessionSnapshot` | `(QString sessionId, int cursor, QJsonArray records, bool hasMore)` | session/follow 快照（首屏历史 + 游标） |
| `handleSessionProjections` | `(QString, int asOfSeq, QJsonObject)` | 快照里的会话投影（小灰字） |
| `handleSessionControlBaseline` | `(QJsonObject projectionsBySession)` | session/control baseline |
| `handleSessionProjectionChanged` | `(QString sessionId, QString key, QJsonValue, int seq)` | 实时投影帧 |
| `handleWorkspaceSnapshot` | `(QJsonArray items, QJsonArray archivedSessionIds)` | 工作区清单 + 归档集合 |
| `handleWorkspaceUpserted` | `(QJsonObject)` | 工作区增量 |
| `handleWorkspaceRemoved` | `(QString workspaceId)` | 工作区增量 |
| `handleWorkspaceReordered` | `(QStringList)` | 工作区增量（整体替换） |
| `handleWorkspaceArchiveChanged` | `(QJsonArray)` | 工作区增量（整体替换） |

调用形态：

```cpp
QObject* hub = DshHost::findObject(DshHostIndex::kMainWindow).data();   // = DSHHub
QMetaObject::invokeMethod(hub, "forwardMuxFrame", Q_ARG(QJsonObject, frame));
```

（`hub` 由 `kMainWindow` 取得；`QJsonObject` / `QJsonValue` / `QStringList` 都是已注册 metatype。）

⚠️ **代价（已知并接受）**：这些槽名是宿主私有实现，不在任何对外 ABI 里 —— 宿主改名或改签名会**静默失效**（`invokeMethod` 返回 false，但没有报错）。
📌 **已说明：后续会公开其中部分槽**（提升为 `public slots:`，或整理成一份正式清单）。在那之前按上表调用。

### 这个组合消掉的东西

两个方向都不走信号（出站两侧走接口，入站走字符串 `invokeMethod`），所以 D2 讨论里的两个坑**都不再适用**：

- ❌ "插件连信号只能用字符串版、PMF 会 `LNK2019`" —— 不涉及信号。
- ❌ "不要把 `Q_OBJECT` 类放进共享头（同名类各 moc 一份、`qt_metacast` 误判）" —— 共享头里只有纯虚、非 QObject 的接口。
- 插件需要的 QObject 交互只有两种：**注册表取对象 + 按 IID cast**，以及 D5 的**字符串 `invokeMethod`** —— 都是既有 sanctioned 路径（与 `DshHostPlugin` / `setHostIdentity` / `detachHost` 同款）。

## 2.7 不采用的做法【决定】

**不采用**"接管 A 类语义意图信号"（用 `TakeoverConnection` 接管 `newWorkspaceRequested` / `sendRequested` 等）作为主要手段。
原因：接管只关掉某一个入口；而**类内分流**下任何入口最终都要调 `DshApiClient` 的出站方法，必然进分支——覆盖更完整，且不必动宿主上层。

## 2.8 D6 / D7 定案（实现期定下，已落地）

### D6 = B · 接管态下读接口的返回值

| 读接口 | 接管态返回 | 为什么这么定 |
|---|---|---|
| `baseUrl()` | 空 `QUrl()` | 唯一"靠它干活"的路径是"切主题把地址带给新窗口"，而接管态下那个地址指向的 DSH 已经不在了（而且 `setBaseUrl` 在接管态本来就忽略新地址）。空值**不会**导致重新拉起内置服务端：`ServerManager` 有**进程级**接管标记，接管态下 `start()`/`restart()` 直接 no-op。回显那条（`Settings.cpp:361`）拿到空值也正好表达"没有内置服务端"。 |
| `launchToken()` | 空 `QString()` | 接管后没有内置服务端，也就没有令牌可带；`authenticatedBaseUrl()` 因此退化成空地址（同上，被标记挡住）。 |
| `isConnected()` | 恒 `true` | 传输归扩展所有，宿主这边没有任何"没连上"的判据。顺带压掉唯一那处调用（`DSHHub.cpp:169` 的"服务端已退出"提示）—— 接管时那个进程正是宿主主动杀掉的，不该弹给用户。 |

三个都是 const 读接口、只改返回值 ⇒ **未接管路径逐字不变**，既有 11 个 `TestDshApiClient` 用例照旧全过。

### D7 = A · 错误码词汇表

宿主自己产生的码统一带 `takenover-` 前缀（上层不比较 code，只拼进文本）：

| code | 场景 |
|---|---|
| `takenover-no-sink` | 接管态下取不到接收端（未装载 / 已被卸载） |
| `takenover-timeout` | 扩展在时限内没回填（`kTakeoverCallTimeoutMs = 120000`，每 5s 扫一次） |
| `takenover-released` | 扩展调 `Takenover(false)` 交还后端时，仍挂着的请求 |
| `takenover-bad-result` | `CompleteCall` 的 `resultJson` 不是合法 JSON |
| `takenover-error` | `FailCall` 的 code 为空时的兜底 |

完整约定（含扩展侧建议的前缀）写在 `include/VirtualClass/VirtualApiTakeover.h` 的文件头。

---

# 3. 必须写到、但不构成决策的点

这些**写就能解决**，列出来免得漏。漏掉各自的后果见右列。

## 3.1 分支必须覆盖全部 8 个出站方法

`setBaseUrl`(`DshApiClient.h:44`)、`openStreams`(`:53`)、`closeStreams`(`:56`)、`followSession`(`:59`)、`unfollowSession`(`:62`)、`callMethod`(`:68`)、`callMethodValue`(`:75`)、`respond`(`:82`)。

漏 `setBaseUrl` → 仍做 token→cookie 握手连真 DSH；漏 `openStreams`/`followSession` → 仍连 `api/remote.mux` 与扩展抢通道。

## 3.2 分支内的 9 个易漏点

| # | 点 | 漏掉的后果 |
|---|---|---|
| 1 | **三种结果语义分开**：`callMethod` 成功=object（`DshApiClient.cpp:410-413` 包了 `toObject()`）、`callMethodValue` 成功=裸值（`:437`）、失败=`RpcError` | 裸数组被当对象处理 |
| 2 | 回填入口**不要直接复用 `handleParsedResponse`** | 它第一件事查 `m_parsing`（`:586`），而接管路径的条目只在 `m_pending` 里 → 命中 `:587-590` 的 "stale parsed response" 分支，**只打一条 qWarning 就返回、回调被静默丢弃** |
| 3 | `respond` 的 `m_clientId.isEmpty()` 前置检查（`:447-456`）在接管态要绕过 | 接管后不连 mux ⇒ `m_clientId` 永远为空 ⇒ **审批/提问应答根本不会被转发，扩展收不到请求** |
| 4 | 分支位置与 `m_authQueue` 的先后（`:491-506` 在 `m_pending.insert`(`:509`) 之前） | 接管态下仍可能"排队等真 DSH 的认证握手"（`m_authenticated`/`m_launchToken`/`m_authInFlight` 照常被读） |
| 5 | **超时** | 现状唯一兜底是认证硬失败时的 `failAuthQueue`（`:221`）；扩展不回填 = 永久挂起、无告警 |
| 6 | 接管态下**读接口的返回值**（见 D6） | UI 拿到指向真 DSH 的误导值 |
| 7 | **错误语义自定义**（见 D7） | 已查证上层不比较 code；但**扩展侧的等价失败（后端连不上/超时）仍需映射成 `RpcError`**，否则上层拿不到 code/message |
| 8 | 回填路径上**对调用方寿命的保护** | `m_destroyed` 检查只在 HTTP 路径上（`:542-545`）；`PendingCall`（`DshApiClient.h:122-127`）存的是裸 `std::function`。回填时机改由扩展决定 → "调用方已析构而回调才到"概率上升，而那条保护在接管路径上不存在 |
| 9 | 请求下发的载体必须可序列化 | 回调不能随请求一起交出去（`std::function` 非 metatype） |

---

# 4. 暂缓与已关闭

## 4.1 暂缓【暂缓】

| 项 | 说明 |
|---|---|
| 测试覆盖 | 现成 **209 个用例全部只走未接管路径**（含 `TestDshApiClient` 的 11 个；实测 209 passed / 0 failed / 3 skipped）。缓的理由：测试的价值在语义稳定之后。接受的代价：新分支零回归保护；且两套语义今后会漂移（宿主未接管路径变了、分支不一定同步，没有测试会报警） |
| 接口 ABI 纪律 | 虚方法**只许末尾追加**（插中间会让已发布插件调错槽位）；**改 IID 会让旧插件静默拿到 nullptr、功能无声消失**（`misc/DESIGN_NOTES.zh-CN.md:27-28`）。缓的理由：只在"接口发布给外部插件且之后还要演进"时才生效。**接受的代价**：这条线从**发布那一刻**起生效 |
| owner 记名 / 独占校验 / 返回值语义 | 见 2.5-C |

## 4.2 已关闭（决定"不做"，留档备查）

**宿主侧兜底 —— 不做。** 将来要做保护机制时，两条更彻底的思路：

1. `ClientExtension::remove()` 里加一行无条件复位 —— 位置**不能**照 `UiStage::releaseForOwner`（`ClientExtension.cpp:224`，位于 `:213` 的 `return true` 之后，顺利路径走不到），必须覆盖全部退出路径。
2. 状态改成 **owner 记名**，把"是否被接管"判成「owner 非空 **且** 该 owner 仍是装载中的扩展」→ 自动回落、从根上消除卡死。代价是 `DshApiClient` 需要一条查询"某扩展是否仍装载"的途径（`ClientExtension::loadedNames()` 现成，但会引入 `network → ExtensionSystem` 依赖，或由宿主注入回调查询）。

---

# 5. 已排除 / 已撤回的结论（避免重复讨论）

| 曾经的结论 | 状态 | 原因 |
|---|---|---|
| 接口放在 `DshApiClient` 类定义里，加 `virtual Takenover(bool)` | **已排除** | 插件不能碰 out-of-line 成员 → `LNK2019`；接口必须放独立全内联头文件（见 2.2） |
| "回调没办法交给扩展"是阻塞问题 | **已排除** | 回调留在宿主（`m_pending`），扩展只回传 `rpcId` + 结果，宿主触发回调 |
| "只 gate `callMethod` 会漏掉其它出站" | **已排除** | 8 个出站方法全在同一个类里，分支覆盖它们即覆盖全部 DSH API 流量 |
| "接管意图信号会留下半接管状态（未接管入口照打真 DSH）" | **已排除** | 那是"接管信号"路线的问题；类内分流下任何入口都必须经过出站方法 |
| "B 类回执信号会断链、B3（`MessageQuery`/`HistoryLoader`）接不了" | **已排除** | 宿主上层（`DSHHub`/`MessageHost`/服务层/`MessageQuery`/`HistoryLoader`）一行不改，照常调出站、照常收回调、照常 emit 回执 |
| "依赖 `TakeoverConnection`，而它因裸签名缺陷不可用" | **已排除** | 本方案用虚接口回填，不需要 `TakeoverConnection` |
| "三种结果语义／回填函数／`respond` 门槛／认证队列顺序／超时／错误语义 都是阻塞问题" | **降级**为第 3.2 节的清单 | 都能在分支内解决，属于"必须写"而非"做不到" |
| "宿主必须做卸载兜底，否则会因插件崩溃而卡死" | **已排除** | 进程内 DLL 崩溃 = 整个客户端消失，标志随之消失；真正剩下的只有"插件写了 `detachHost()` 却忘了复位"这一种情形，已决定接受 |

---

# 6. 附录：事实与证据

## 6.1 出站是单一咽喉

- 全部出站方法都在 `DshApiClient` 上，且该类**零 `virtual`**；`class DshApiClient : public QObject`（`source/include/network/DshApiClient.h:25`）。
- 18 个一元端点：`session/create`、`session/list`、`session/page`、`session/prompt`、`session/cancel`、`session/selectModel`、`session/modelCatalog`、`workspace/create`、`workspace/archiveSession`、`llm/listConfigurableProviders`、`llm/discoverModels`、`settings/describe`、`settings/mutate`、`settings/update`、`credentials/describe`、`credentials/set`、`agentPresets/list`、`$events/result`。
- 4 条 mux 逻辑流的 open + cancel：`$events`、`workspace/follow`、`session/follow`、`session/control`。
- 认证握手：`GET /?token=` → 303 + `Set-Cookie: dsh-auth-<hash>`；流通道 `api/remote.mux`（`makeUrl`，`DshApiClient.cpp:472-481`）。

## 6.2 出站调用链

```
业务代码（DSHHub / MessageHost / Sidebar / 服务层）
  └─ callMethod(method, payload, onSuccess, onError)        DshApiClient.cpp:389
       ├─ 生成 rpcId(:395)，组 body{type,rpcId,method,payload:{args}}(:401-405)
       └─ post("/api/"+method, body, onSuccess', onError)   :409 / :484
            ├─ 未认证 → 回调进 m_authQueue 并返回            :491-506
            └─ 已认证 → m_pending[rpcId] = {onSuccess,onError}  :509-513
                        → QNetworkAccessManager::post()      :525
                        → connect(reply, finished, onReplyFinished)  :531（普通 connect，不进登记表）
                             └─ 线程池解析 → handleParsedResponse      :580
                                  └─ 查 m_parsing → 调回调               :586-609
```

## 6.3 入站

- 12 个信号、15 处 `emit`（`DshApiClient.cpp`）：`connected`、`muxFrameReceived`(×2)、`sessionSnapshotReady`、`sessionProjectionsReady`、`sessionProjectionsBaselineReady`、`sessionProjectionChanged`、`workspaceSnapshotReady`、`workspaceUpserted`、`workspaceRemoved`、`workspaceReordered`、`workspaceArchiveChanged`、`transportError`(×3)。
- `DshApiClient` 是全项目**唯一的 `QWebSocket`**（`:49`）→ 流数据单一漏斗。

## 6.4 宿主注册表实际登记项

`mainWindow`（DSHHub）、`sidebar`（Sidebar）、`topbar`（TopBar）、`themeManager`（ThemeManager）、`connectionManager`（ConnectionManager）。`DshApiClient` **不在其中**（本方案要加，见 2.5-B）。

## 6.5 意图信号与出站的关系（供参考；本方案下非主要手段）

- A 类语义意图信号共 17 个，其中**只有 8 个真出站**：`newWorkspaceRequested`、`createSessionInWorkspaceRequested`、`sendWithoutSession`、`sendRequested`、`stopRequested`、`deleteSessionRequested`、`modelChanged`、`thinkingDepthChanged`。
- 其余 9 个是本地或纯转发：`sessionClicked`、`deleteRequested`、`clearRequested`、`settingsRequested`、`pluginsRequested`、`themeToggleRequested`、`extensionsRequested`、`modelChosen`、`levelChosen`。
- 有一条绕过 `DSHHub` 的出站入口：`DSHHub.cpp:1107` 把 `m_api` 递给 Sidebar → `Sidebar::createSession(api, wsId)`（`source/include/ui/Sidebar.h:211`）→ `SessionService::createSession`（`source/src/common/session/SessionService.cpp:70`）自己发 `session/create`。（本方案下无所谓——它照样要调 `callMethod`，一样进分支。）

## 6.6 未接管路径的测试基线

`build/windows-ninja/DSH Hub.Tests.exe`（用 `DSHHUB_TEST_REPORT_DIR` 取逐类报告，必须从**仓库根**运行）：**209 passed / 0 failed / 3 skipped**（3 个 skip 是依赖 `test extension/` 样例目录的 `DllCaller` 用例，该目录不在仓库里）。接管实现落地后整编重测，仍是这个数。

> ⚠️ **这个构建目录记不到头部依赖，改头文件之后必须整编，否则会得到"坏掉但不明显"的二进制。**
> Ninja 规则是 `deps = msvc`（`/showIncludes`），但本机 MSVC 输出本地化前缀 `注意: 包含文件:`
> （`VSLANG=1033` 也不改），而 Ninja 只认英文 `Note: including file:` ⇒ 所有 `.obj` 的 depfile
> 里依赖数为 0（`ninja -t deps` 里显示 `#deps 0`），**改头文件不会触发任何重编**。
> 后果不是"没生效"而是**同一个类在不同 TU 里布局不一致**（ODR）⇒ 随机 `0xC0000005`
> 或莫名的断言失败；换个编译开关/加 `/Zi` 就"自动好了"，极难查。
> 实测复现：给 `DshApiClient` 只加两个**没人用**的空成员、其它一律不动，`dshhub_tests`
> 就会在 `TestDshApiClient::setBaseUrl` 崩掉；整编之后同样的代码 209/0/3 全过。
> 判断办法：改过头文件后 `ninja -n` **什么都不列**就是踩了（应列出该头文件的所有包含者）。
> 整编办法：`Get-ChildItem source,tests -Recurse -Include *.cpp,*.h | % { $_.LastWriteTime = (Get-Date) }`
> 再构建（或删掉构建目录重建）。
