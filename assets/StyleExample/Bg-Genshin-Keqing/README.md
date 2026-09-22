# style/ —— 壁纸版样式快照（按主题自动蒙版）

把本文件夹里的文件拷进 `<exe>/styles/` 即生效，**不用改代码、不用重编译**。

```bash
cp -f "style/"*.qss "style/"theme-*.json "style/"wallpaper.png "x64/Release/styles/"
cp -f "style/"*.qss "style/"theme-*.json "style/"wallpaper.png "x64/Debug/styles/"
```

> 拷的是**文件夹里的文件**，别拷成 `<exe>/styles/style/`。
> 恢复默认：设置 → 外观 → 重置样式；或删掉 `<exe>/styles/` 下的这些文件。

---

## 一、蒙版是怎么做的（这一版的核心）

**壁纸本身是一张带 alpha 的 PNG**（整图不透明度 45%），**蒙版色由 QSS 提供**：

```qss
/* main-window.qss */
#dshhubCentral {
    background-color: {{wallpaperMask}};                              /* ← 蒙版色，随主题换 */
    border-image: url({{wallpaperPath}}) 0 0 0 0 stretch stretch;     /* ← 壁纸（半透明） */
    ...
}
```

```json
/* theme-light.json */   "wallpaperMask": "#FFFFFF"      // 白蒙版：提亮
/* theme-dark.json  */   "wallpaperMask": "#111827"      // 深蒙版：压暗（= 暗色主题的 windowBg）
```

绘制顺序是「先底色（蒙版色）→ 再叠壁纸」，壁纸半透明，所以底色从图里透出来，**合成结果就等于"原图 + 一层蒙版"**，
而蒙版颜色在换主题时自动切换。实测合成值与理论值逐通道吻合：

| | 图 60% + 白底 40% | 实测 |
| --- | --- | --- |
| 测试图 rgb(200,50,50) @alpha0.6 | (222,132,132) | `#de8484` ✓ |

### 为什么必须用「半透明图 + 底色」而不是「不透明图 + 底色」

Qt 的 `#dshhubCentral` 上，**`border-image` 画在 `background-color` 之上**（实测：白底 + 深红图 → 得到深红，
底色被完全盖住）。所以同一控件上想同时有"不透明壁纸"和"底色蒙版"是**做不到**的 ——
只有让壁纸带 alpha、底色从下面透上来这一条路。

主窗口 `QMainWindow#dshHubWindow` 也不能当壁纸宿主：它带 `FramelessWindowHint + WA_TranslucentBackground`，
实测**不绘制 QSS 背景**（连补 `WA_StyledBackground` 也一样），所以没法"父窗口画壁纸、central 画蒙版"。

---

## 二、壁纸

- `style/wallpaper.png` —— **1600×1000**（16:10，与本机 2560×1600 屏幕同比例），**1.37 MB**，带 alpha。
  来源：`C:\Users\Playe\Desktop\v2-eac3c34b781fd26f52fd9dd4f37d9ba3_r (1).jpg`（7680×4320），
  居中裁 16:10（切掉左右各 384px）→ LANCZOS 缩到 1600px → 加常量 alpha 0.45。
- 重新生成 / 换图：`python build/make-wallpaper.py [输出路径] [--alpha 0.45] [--target-w 1600]`
  - **`--alpha` 越小 ⇒ 蒙版越重**。0.45 = 55% 蒙版；0.7 会很清淡；0.3 几乎只剩轮廓。
  - 想更清晰就 `--target-w 2000`（体积涨到约 1.7 MB）。
- **路径写在色板里**（两个 json 各一份），换图只改这一处：`"wallpaperPath": "styles/wallpaper.png"`
  - 这是**相对路径**，基准是**进程工作目录**（不是 exe 目录）。双击启动时 cwd = exe 目录，所以等效于 exe 相对路径；
    但从别的目录用命令行启动，图片会静默找不到、退回蒙版底色。
  - 想绝对可靠就改成绝对正斜杠路径，如 `"C:/.../x64/Release/styles/wallpaper.png"`（**别用反斜杠**，`url()` 里会被当转义符）。
- 色板 key 规则 `[A-Za-z][A-Za-z0-9]*`（**不能有下划线**），值原样替换进 `{{key}}`。

---

## 三、改了哪几个文件

| 文件 | 改动 |
| --- | --- |
| `main-window.qss` | `#dshhubCentral`：`background: {{windowBg}}` → `background-color: {{wallpaperMask}}` + `border-image` 挂壁纸 |
| `chat.qss` | `#chatScrollArea` / `#chatScrollContent` / `#chatPanel` 放透明；新增 viewport 规则；`#inputCapsule` 改半透明 |
| `sidebar.qss` | `#sidebar` 底色 `panelBg` → `wallpaperPanelBg`（半透明） |
| `topbar.qss` | `#topBar` 同上 |
| `theme-light.json` / `theme-dark.json` | 各新增 4 个 key：`wallpaperPath` / `wallpaperMask` / `wallpaperPanelBg` / `wallpaperInputBg` |

**其余 8 个 qss 与内置模板逐字节相同**；两个色板原有的 57 个颜色 key **一个都没动**。

半透明参数（只改色板即可）：

| 部件 | light | dark |
| --- | --- | --- |
| 侧栏 / 顶栏（`wallpaperPanelBg`） | `rgba(255,255,255,0.72)` | `rgba(31,41,55,0.62)` |
| 输入卡片（`wallpaperInputBg`） | `rgba(255,255,255,0.78)` | `rgba(31,41,55,0.70)` |

输入卡片比侧栏稍实一点 —— 它是长时间读字打字的地方，需要更高的文字对比度。
气泡、按钮的底色一律没动（它们是实底，压在壁纸上）。

---

## 四、实测边界（Qt 6.11.2，都是静默失效，没有警告）

1. **不能拉伸**：`background-size` 是 `Unknown property`，普通背景图只能按原尺寸画 →
   要自适应窗口只能用 `border-image: url(...) 0 0 0 0 stretch stretch`。
2. **`background-repeat` 长属性是地雷**：只要出现它（值/位置随便），图片背景整个消失、退回底色且不报警告。
3. **路径**：`file:///C:/...` 在 Windows 上坏掉；**`%20` 会让整份样式表解析失败**（所有规则一起失效）；
   **反斜杠路径也坏**（被当转义）；qrc 写 `:/x.png`。
4. **放透明时别写 `#chatScrollArea > QWidget`** —— `QScrollBar` 也是一级子控件，会把滚动条底色打掉。
5. `border-image` 会顶掉 `#dshhubCentral` 那条 1px 描边（最外 1 个物理像素变透明，0.5 逻辑像素，基本看不出）。
6. **只有 PNG 免插件**：JPEG 解码在 Qt 里是插件（`imageformats/qjpeg.dll`），发布包若不带它，
   QSS 里任何 `.jpg` 图会静默不显示。本项目一律用 PNG，`imageformats/` 已删除。
7. **圆角、最大化都正常**：四角在圆角外 alpha=0；`maximized="true"` 时壁纸照旧铺满。

---

## 五、验证方式

`build/probe-wallpaper/` 是配套探针：按产品**真实控件层级**（`#dshHubWindow → #dshhubCentral → #topBar / #sidebar / #chatPanel → #chatScrollArea → viewport → #chatScrollContent` + `#inputCapsule`）
渲染合成的完整 QSS，输出 `out-light.png` / `out-dark.png` 并做像素判定。

最近一次结果：

```
light  壁纸带唯一颜色 = 2404  OK ；整幅平均亮度 223.4 ；四角 #808080（圆角外）
dark   壁纸带唯一颜色 = 2404  OK ；整幅平均亮度  75.5 ；四角 #808080
fail = 0
```

> 注意：探针进程的工作目录下**也要有 `styles/wallpaper.png`**，否则相对路径解析不到、壁纸会静默消失。
