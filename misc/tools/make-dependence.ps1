<#
  make-dependence.ps1 —— 把运行库 DLL 收进 <目标目录>\dependence\。

  为什么要脚本：exe 从子目录加载 DLL 靠的是 manifest 私有程序集，而那个程序集要求
  「目录名 == 程序集 name」且「目录里有一份列出每个 <file> 的 dependence.manifest」。
  人手搬很容易漏一份清单或漏一个 dll，漏了就变成启动即失败。所以这里：

    - DLL 名单**从清单里读**（清单是唯一事实来源，改清单即改行为）
    - 按配置自动选清单：目录里有 Qt6Cored.dll 就用 Debug 版，否则用 Release 版
    - 把清单本身也拷过去（运行时必需）
    - 幂等：已经是 dependence\ 里的就跳过

  用法：
    .\misc\tools\make-dependence.ps1 -Target x64\Release
    .\misc\tools\make-dependence.ps1 -Target x64\Debug
    .\misc\tools\make-dependence.ps1 -Target <buildDir> -Config Release -QtBin D:\Qt\6.11.2\msvc2022_64\bin

  参数：
    -Target  部署目录（exe 所在目录）
    -Config  强制 Debug / Release；不给则自动判断
    -QtBin   该目录里缺 DLL 时允许从这里拷。CMake 的构建目录里根本没有 Qt DLL，
             要靠它补齐，构建产物才能直接运行。

  ⚠️ 前提：该目录里的 exe 必须是**带 dependence 依赖的新构建**（嵌入 manifest）。
     用旧 exe 会出现"文件都在却仍报找不到 DLL"。
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)]
	[string]$Target,

	[ValidateSet('Debug', 'Release')]
	[string]$Config,

	[string]$QtBin,

	# MSVC 运行库（Release 版）所在目录；不给则自动从 VS 的 redist 里找
	[string]$CrtBin
)

$ErrorActionPreference = 'Stop'

# 没给 -QtBin 时按项目惯例用 QTDIR（见 misc/tools/rebuild.sh 与 CMakeLists 的说明）
if (-not $QtBin -and $env:QTDIR) { $QtBin = Join-Path $env:QTDIR 'bin' }

# MSVC 运行库在 VS 的 redist 里（跟 Qt 不在一起），构建目录需要它们才算完整。
# ⚠️ 布局是 <VS根>\<版本>\<版本名>\VC\Redist\MSVC\<工具集>\x64\Microsoft.VC*.CRT，
#    版本名下面还有工具集版本（本机是 18\Community\VC\Redist\MSVC\14.44.35112\x64\），所以要按通配逐层找。
if (-not $CrtBin) {
	$redist = Get-ChildItem -Path 'C:\Program Files\Microsoft Visual Studio\*\*\VC\Redist\MSVC\*' -Directory -ErrorAction SilentlyContinue
	foreach ($toolset in ($redist | Sort-Object Name -Descending)) {
		$crt = Get-ChildItem -LiteralPath (Join-Path $toolset.FullName 'x64') -Directory -ErrorAction SilentlyContinue |
			Where-Object { $_.Name -like 'Microsoft.VC*.CRT' } | Select-Object -First 1
		if ($crt) { $CrtBin = $crt.FullName; break }
	}
	if (-not $CrtBin) { Write-Warning "没找到 VS 的 VC redist 目录，-CrtBin 请手工给（Release 需要那 6 个 CRT dll）" }
}

# misc\tools\ -> 仓库根
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)

# MSBuild 传 $(OutDir) 时结尾带反斜杠：放进引号会变成转义引号，路径里就混进一个多余引号，
# Test-Path 会报 ItemExistsArgumentError。这里统一去掉尾部的反斜杠 / 引号 / 空格。
$Target = $Target.Trim().TrimEnd([char]92, [char]34, ' ')
if (-not (Test-Path -LiteralPath $Target)) { throw "目标目录不存在: $Target" }
$target = (Resolve-Path -LiteralPath $Target).Path
$depDir = Join-Path $target 'dependence'

# ---- 选清单：Debug 用带 d 后缀的那份 ----
if (-not $Config) {
	# 顶层没有就看 dependence\ 里（首次运行搬进去之后，顶层就再也看不到 dll 了）
	if ((Test-Path -LiteralPath (Join-Path $target 'Qt6Cored.dll')) -or
		(Test-Path -LiteralPath (Join-Path $target 'dependence\Qt6Cored.dll'))) {
		$Config = 'Debug'
	} else {
		$Config = 'Release'
	}
}
$manifestSrc = if ($Config -eq 'Debug') {
	Join-Path $root 'source\dependence.debug.manifest'
} else {
	Join-Path $root 'source\dependence.manifest'
}
if (-not (Test-Path -LiteralPath $manifestSrc)) { throw "找不到清单: $manifestSrc" }

[xml]$manifest = Get-Content -LiteralPath $manifestSrc -Raw
$names = @($manifest.assembly.file | ForEach-Object { $_.name })
if ($names.Count -eq 0) { throw "清单里没有 <file> 条目: $manifestSrc" }
$manifestDst = Join-Path $depDir 'dependence.manifest'

# ---- 1) 在 目标目录 / dependence\ / QtBin / CrtBin 四处找齐 DLL ----
$missing = @()
foreach ($name in $names) {
	if ((Test-Path -LiteralPath (Join-Path $target $name)) -or (Test-Path -LiteralPath (Join-Path $depDir $name))) { continue }
	$from = $null
	foreach ($cand in @($QtBin, $CrtBin)) {
		if ($cand -and (Test-Path -LiteralPath (Join-Path $cand $name))) { $from = Join-Path $cand $name; break }
	}
	if ($from) {
		New-Item -ItemType Directory -Path $depDir -Force | Out-Null
		Copy-Item -LiteralPath $from -Destination (Join-Path $depDir $name) -Force
		Write-Host "  从外部拷入  $name"
		continue
	}
	$missing += $name
}

# ---- 2) 缺一个都不行：宁可不留清单 ----
# 半份清单（清单在、dll 缺）会让加载器直接硬失败（SxS 报错），比"dll 散在 exe 旁边"更难查。
if ($missing.Count -gt 0) {
	Remove-Item -LiteralPath $manifestDst -Force -ErrorAction SilentlyContinue
	Write-Warning ("[{0}] 缺 {1} 个 DLL，未生成 dependence\ 清单（否则会变成 SxS 硬失败）: {2}" -f $Config, $missing.Count, ($missing -join ', '))
	Write-Warning "  先把它们部署到 $target（Qt VS Tools / 手工拷贝 / windeployqt），或加 -QtBin <Qt的bin目录> 让本脚本去拷。"
	exit 0
}

# ---- 3) 齐了：搬进 dependence\ 并放好清单 ----
New-Item -ItemType Directory -Path $depDir -Force | Out-Null
$moved = 0
$already = 0
foreach ($name in $names) {
	$src = Join-Path $target $name
	$dst = Join-Path $depDir $name
	if (Test-Path -LiteralPath $src) {
		Move-Item -LiteralPath $src -Destination $dst -Force
		Write-Host "  移入  $name"
		$moved++
	} else {
		$already++
	}
}
Copy-Item -LiteralPath $manifestSrc -Destination $manifestDst -Force

Write-Host ("[{0}] {1}: 本次移入 {2} 个，已在位 {3} 个，清单共 {4} 个" -f $Config, $target, $moved, $already, $names.Count)
