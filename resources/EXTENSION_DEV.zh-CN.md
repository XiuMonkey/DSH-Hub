# DSH Hub 扩展（.ext）开发文档

本文档面向希望为 **DSH Hub**（Qt 6 / C++ 桌面客户端 + 本地 DSH 服务端）开发自定义扩展的开发者。

扩展 = 一个 `.ext` 文件（ZIP 格式）。它把三样东西打包在一起：

1. **原生工具逻辑**：一个或多个 DLL（`main.dll` 及可选依赖）；
2. **工具描述**：`regulation.json5`（声明每个 DLL 导出函数如何映射成一个“DSH 工具”）；
3. **可选的 DSH 插件**：`AttachedPlugin/`（`package.json` + `index.js`），在 DSH 服务端注册工具让 Agent 能用；有**两种模板**——手写 `index.js` 逐工具注册，或直接用**通用注册器 + tools.json**（加工具只改 JSON，见 §6.2）。

> 阅读前提：熟悉 JSON/JSON5、Windows DLL 导出、DSH（DeepSeek Harness）插件的 `defineTool` 约定。

---

## 1. 目录结构与打包

推荐布局：

```text
MyExtension.ext            （zip 打包）
├── main.dll               主原生 DLL（必需）
├── regulation.json5       工具描述（必需）
├── AttachedPlugin/        DSH 插件（推荐，工具才能被 Agent 调用）
│   ├── package.json
│   └── index.js
└── bin/                   （可选）运行时依赖（ffmpeg 等第三方 DLL/可执行文件）
```

打包为 `.ext`：

- 压缩包**根目录**必须直接包含 `regulation.json5` 与 `main.dll`（程序支持任意层级查找，但建议放根）；
- 兼容 `\` 与 `/` 两种路径分隔（`Compress-Archive` 等工具产出的反斜杠路径可用）；
- `package.json` / JSON5 里**不要带 UTF-8 BOM**（安装时程序会自动剥离，但最好一开始就没有）。

仓库内可直接参考的样例：

- `test extension/NativeThunkExt/`：native（Thunk）DLL 工具 + **通用注册器模板**（`AttachedPlugin/tools.json`）——新扩展建议照它做；
- `test extension/NativeThunkExt/AttachedPlugin/index.js`：通用加载器本体（可原样复用）；
- （历史样例 FFmpegExt / PPTExt 已不在源码树，仅存于运行目录快照，参考价值有限。）

---

## 2. regulation.json5 —— 工具描述

示例（json 风格）：

```json5
{
  "Name": "FFmpegTools",
  "Description": "FFmpeg media processing tools",
  "Function": [
    {
      "Func": "ffmpeg_probe",
      "LoadingSource": "main.dll",          // 从哪个 DLL 导出（相对描述文件目录）
      "InterfaceType": "default",           // default(默认)/com
      "Calling Convention": {
        "Style": "json",                    // json | native
        "ReturnType": "string",             // string | int | void（json 风格）
        "ResultIsJson": true                // 返回值是否是 JSON 文本
      },
      "Tool": "ffmpeg_probe"                // DSH 工具名（全局唯一）
    }
  ]
}
```

字段说明：

| 字段 | 说明 |
|---|---|
| `Function[].Func` | DLL 导出函数名（`default` 接口必填） |
| `Function[].Tool` | 暴露给 Agent 的工具名；**在整个应用里必须唯一**（多扩展同名会被拒绝加载） |
| `Function[].LoadingSource` | 函数来自哪个 DLL，相对 `regulation.json5` 所在目录解析；同一扩展可从不同 DLL 导出多个工具；缺省 `main.dll` |
| `Function[].InterfaceType` | `default`＝原生 DLL；`com`＝走 COM 自动化（见 §5） |
| `Calling Convention.Style` | `json`：统一桥接签名；`native`：Thunk 按真实 C 签名调用 |
| `Calling Convention.ReturnType` | json: `string/int/void`；native 支持 `void/bool/double/string(const char*)/int` |
| `Calling Convention.ResultIsJson` | 返回内容是否按 JSON 解析 |

---

## 3. json 风格 —— DLL 桥接签名

DLL 按以下三种签名导出（参数一律传 JSON 文本）：

```c
// 方式一：const char* Func(const char* argsJson)
const char* ffmpeg_probe(const char* argsJson);

// 方式二（返回 int 表示错误码，0=成功）：
int    Func(const char* argsJson, char** resultJson);
// 或方式二（void 变体）：
void   Func(const char* argsJson, char** resultJson);
```

### 结果内存契约（ClearMem）

- **新约定**：若 DLL 导出了 `void ClearMem(const char* p)`，说明 json 风格（`ReturnType=string`）返回的是 DLL `malloc` 的堆内存；Qt 端读取**拷走后立即调用 `ClearMem` 归还**。
- **老扩展**：没有 `ClearMem` 导出时，保持“static 缓冲、拷走即用、不释放”的兼容行为。

因此：新 DLL 建议统一用“分配字符串 → 导出 `ClearMem`”的方式；若用 static 缓冲则不要导出 `ClearMem`，也不要并发重入同一函数（§8）。

`ResultIsJson=true` 时返回内容必须是合法的 JSON **对象**。

---

## 4. native 风格 —— Thunk 真实签名

若希望按 DLL 真实 C 签名调用（避免来回 JSON 序列化），声明：

```json5
"Calling Convention": {
  "Style": "native",
  "ReturnType": "int"
},
"Parameters": [
  { "Name": "a", "Type": "int" },
  { "Name": "b", "Type": "double" },
  { "Name": "s", "Type": "string" }
]
```

支持的参数/返回类型：`bool / int / double / string(const char* / char*) / pointer32 / pointer64`（参数数量上限 16）。

Qt 端用 Thunk 生成 x86-64 适配层调用，参数由 JSON 提供、缺省值取 `Parameters[].Default`。

---

## 5. com 接口（InterfaceType = com）

用于调用 Windows COM 自动化（IDispatch）。组件 **ProgId 白名单式声明**在描述里：

```json5
{
  "Func": "",
  "InterfaceType": "com",
  "Com": { "ProgId": "My.Calc" },
  "Tool": "com_my_calc"
}
```

运行时入参（来自 Agent 的 JSON）：

```json
{
  "member": "Add",          // 方法或属性名
  "kind":   "method",       // method | get | put
  "path":   ["PropA"],      // 可选：先沿属性链下钻
  "params": [1, 2],         // kind=method 的参数（仅 JSON 标量）
  "value":  42              // kind=put 的值
}
```

- 每次调用新建组件实例（无状态、无句柄保活），可跨线程并行（内部按线程初始化 COM）；
- 返回对象时只给摘要（`Count`），不支持下钻保活。

---

## 6. AttachedPlugin —— 让 Agent 能用到这些工具（两种模板）

### 6.1 手写 index.js（灵活模板）

光有 DLL + 描述还不够：Agent（模型）只认 DSH 服务端插件注册的工具。因此把工具注册脚本放进扩展：

```js
// AttachedPlugin/index.js
import { defineTool } from "@deepseek-ai/dsh-tools";

export const name = "MyExtensionPlugin";
export const inject = ["tools"];

export function apply(ctx) {
  ctx.tools.register(defineTool({
    name: "my_tool",                    // 必须与 regulation 里 Tool 一致
    description: "…",
    parameters: { /* 与 native/json 约定呼应 */ },
    isConcurrencySafe: () => false,     // 保守建议 false
    async execute(args) {
      return bridge.call("my_tool", args); // 见下：命名管道桥接
    }
  }));
}
```

### 6.2 通用注册器 + tools.json（推荐：加工具只改 JSON）

如果每个工具都只是“参数 → 命名管道 → Qt 调用 DLL”，**不需要手写注册 JS**：把仓库自带的**通用加载器**（`test extension/NativeThunkExt/AttachedPlugin/index.js`）原样放进你的 `AttachedPlugin/`，它会读取同目录的 `tools.json`，逐个注册工具并桥接到 Qt 端。

```text
AttachedPlugin/
├── package.json    // name/main 与示例一致（"type":"module", main: "index.js"）
├── index.js        // 通用加载器（复制示例即可，不用改）
└── tools.json      // { "tools": [ 工具定义... ] }
```

**tools.json 格式**（完整样例：`test extension/NativeThunkExt/AttachedPlugin/tools.json`）：

```json
{
  "tools": [
    {
      "name": "native_add3",             // DSH 工具名（全局唯一）
      "description": "Add three integers…",
      "bridgeTool": "native_add3",       // Qt 端 regulation 里的 Tool 名（缺省 = name）
      "concurrencySafe": false,          // false=Qt 同 DLL 串行；确认线程安全再开
      "parameters": {                    // JSON-Schema 子集，供模型填参
        "a": { "type": "integer", "required": true, "description": "…" },
        "b": { "type": "number",  "required": true, "description": "…" }
      },
      "output": { "type": "object", "additionalProperties": true }
    }
  ]
}
```

- `parameters` 的 `type` 支持 `integer / number / string / boolean / object / array`，每个字段可带 `required` 与 `description`；
- 通用加载器对每个定义执行等价逻辑：`defineTool({ name, description, parameters, output, isConcurrencySafe: () => !!def.concurrencySafe, async execute(args){ return bridge.call(def.bridgeTool || def.name, args); } })`；
- 加载时打印 `loaded tools.json: …` 与 `register tool: …`（走服务端日志），方便确认。

**新增一个工具 = 三步，不碰 JS**：

1. `regulation.json5` 加一条 `Function`（`Func` = DLL 导出名，`Tool` = tools.json 的 `bridgeTool`/`name`）；
2. DLL 实现并导出该函数（json / native 风格，见 §3 / §4）；
3. `tools.json` 的 `tools` 数组加对应定义。

> 通用加载器已统一处理：管道自动重连、30s 超时、错误 reject、插件 dispose 时关闭管道。手写插件需要同样能力可直接复用。

---

### 命名管道桥接

扩展 DLL 跑在 **Qt 客户端进程**里；DSH 插件跑在 **Node 服务端进程**里。两者之间用命名管道：

- 管道名：`\\.\pipe\dshhub-bridge`；
- 协议：**每行一个 JSON**；
  - 请求：`{"id":1,"tool":"my_tool","args":{...}}`
  - 响应：`{"id":1,"ok":true,"result":{...}}` 或 `{"id":1,"ok":false,"error":"..."}`；
- Qt 端收到请求后，按 `tool` 名在已加载扩展描述里查函数并调用（同 DLL 串行、跨 DLL 并行，见 §8），结果回写管道。

可参考 `test extension/NativeThunkExt/AttachedPlugin/index.js` 的桥接实现（自动重连 + 30s 超时）。

> 注意：Node 端请求是异步多发的，响应可能乱序返回——**按请求 `id` 配对**，不要假设顺序。


## 7. 安装 / 卸载 / 重装行为

安装一个 `.ext` 后，程序会：

1. 解压到临时目录并解析 `regulation.json5`；
2. 把运行文件持久化到 `harness/profiles/web/extensions/<插件名>/`；
3. 若带 `AttachedPlugin`，把插件目录复制到 `…/node_modules/<插件名>/`，并向 `cordis.patch.yml` 追加一行启用；
4. 更新 `extensions.json` 注册表（UI 列表的来源）；
5. **重启 DSH 服务端**，插件 `apply()` 注册工具；
6. Qt 侧 `DllCaller` 追加加载该扩展描述（支持多扩展并存，互不覆盖）。

约定与限制：

- **工具名全局唯一**：Qt 侧与 DSH 插件注册侧都对“同名工具”拒绝（会报错并提示先卸载冲突方）；
- **重装同一扩展**：请先在“扩展管理”里**移除**再安装（Qt 会先按名卸载该扩展的 DLL、释放文件占用；否则覆盖/注册会被拒）；
- 移除扩展后服务端会自动重启，工具随之消失；
- 扩展目录名取自 `package.json name`（无插件时为解压目录名）——请用合法的目录名字符，不要含路径分隔符。

---

## 8. 并发与线程模型（Qt 侧）

- 工具请求在 **GUI 线程之外的工作线程池**执行（GUI 不会被长任务卡住）；
- **同一个 DLL 内部串行**（`runMutex`）：老扩展可能是 static 缓冲/非重入实现，跨线程并发不安全；**不同 DLL / 不同扩展并行**；
- `com` 每次调用独立初始化（MTA），可并行；
- 卸载/移除扩展会**等待该 DLL 上在途调用结束**再卸载，不会卸载使用中的库；
- 因此：json 风格 + `ClearMem` 的实现，理论上可放开同库并发；若确实线程安全，可在插件侧说明并考虑放开，但默认按串行更稳。

---

## 9. 调试与常见问题

日志关键字（见 `x64/Debug/log.txt`）：

- `[DllCaller] descriptor loaded / library loaded / tool call succeeded / unknown tool …`
- `[DSH Pipe] request / response`
- `[Market] …` / `[dsh-market-log] …`（插件市场，扩展一般用不到）
- `[DSH Server] …`

排查路径：

1. **模型说找不到工具**：先确认 AttachedPlugin 已随扩展安装并触发过服务端重启；再确认 `cordis.patch.yml` 里插件行存在；
2. **调用报 `unknown tool`**：Qt 侧描述未加载/被同名冲突拒绝——看 `[DllCaller] descriptor loaded` 是否出现该扩展；
3. **调用报 DLL/管道错误**：检查 `bin/` 依赖是否齐全、DLL 是否导出 `ClearMem`（与实现匹配）、管道 30s 超时（长任务请拆小或调大 Node 端超时）；
4. **工具注册冲突**：工具名重复，先卸载旧扩展；
5. **git 类 DSH 插件安装失败（市场）**：与本格式无关，属 dshmarket/pnpm 链路（Qt 已为服务端注入 pnpm PATH，若仍失败看 `[dsh-market-log]`）。

---

## 10. 最佳实践清单

- 工具名全局唯一、语义化（如 `myext_action`）；
- **优先用通用注册器模板**（§6.2）：加工具只改 `tools.json` 与 `regulation.json5`，别复制注册样板；
- DLL 用 `ClearMem` 契约 + 每次调用独立分配/释放，避免 static 缓冲跨线程；
- 参数只用 JSON 标量与对象；返回尽量是 JSON 对象（`ResultIsJson:true`）；
- 长耗时任务要有进度/超时策略（管道侧单次请求超时默认 30s）；
- 依赖 DLL 放 `bin/`，并在 `LoadingSource` 中引用相对路径；
- 一个 `.ext` 只放一套逻辑；需要“多 DLL 导出工具”时用多个 `LoadingSource` 描述同一扩展即可；
- 发布前跑一遍：安装 → 移除 → 重装 → 暗色/亮色主题下调用。

---


