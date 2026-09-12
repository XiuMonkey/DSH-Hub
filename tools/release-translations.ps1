# ------------------------------------------------------------------
# release-translations.ps1
# ------------------------------------------------------------------
# 翻译的“一条龙”脚本，做三件事：
#   1) lupdate：扫 src/include，把新文案补进 translations/*.ts（不动已有译文）
#   2) 生成默认语言包（源语言=中文）：把每条 <source> 原样填进 <translation>，
#      lrelease 到 resources/translations/dshhub_zh_CN.qm —— 它被打进 qrc，
#      是"出厂默认显示"的来源；启动时从内置资源加载（见 TranslationManager.cpp），
#      外部 translations\ 里同名文件可覆盖。改了源码文案后重跑本脚本即可同步。
#   3) lrelease：把 translations/*.ts 编译成 .qm 并发布到 x64\<Config>\translations，
#      同时把 .ts 清单一并拷过去（运行时的"就地换文案"靠它认旧文案）。
#
# 为什么 .qm 的发布不走 PostBuildEvent：
#   Qt VS Tools 的 <QtTranslation> 目标不负责把产物拷到程序目录；而项目处于
#   “已是最新”时 MSBuild 默认不执行构建后事件，容易变成"配了却没跑"。
#
# 用法（仓库根目录，用 Windows PowerShell 直接跑即可）：
#   .\tools\release-translations.ps1                 # 发布到 x64\Release
#   .\tools\release-translations.ps1 -Config Debug   # 发布到 x64\Debug
#   .\tools\release-translations.ps1 -UpdateOnly     # 只更新 .ts 与默认包，不发布 .qm
#
# 注意：resources\translations\dshhub_zh_CN.qm 变了要重新构建（它进 qrc）。
# ------------------------------------------------------------------

param(
	[string]$Config = "Release",
	[switch]$UpdateOnly
)

$ErrorActionPreference = "Stop"

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$utf8Bom = New-Object System.Text.UTF8Encoding($true)

$root = Split-Path -Parent $PSScriptRoot
$tsDir = Join-Path $root "translations"
$outDir = Join-Path $root "x64\$Config\translations"

# Qt 工具位置：优先用环境变量 QTDIR，其次回退到本机安装路径
$qtDir = $env:QTDIR
if (-not $qtDir) {
	$qtDir = "D:\Qt\6.11.2\msvc2022_64"
	Write-Host "[warn] 未设置 QTDIR，回退到 $qtDir"
}
$lupdate = Join-Path $qtDir "bin\lupdate.exe"
$lrelease = Join-Path $qtDir "bin\lrelease.exe"

foreach ($tool in @($lupdate, $lrelease)) {
	if (-not (Test-Path $tool)) { throw "找不到 Qt 工具：$tool（请设置 QTDIR）" }
}
if (-not (Test-Path $tsDir)) { throw "找不到翻译目录：$tsDir" }

$tsFiles = Get-ChildItem $tsDir -File -Filter *.ts
if ($tsFiles.Count -eq 0) { throw "$tsDir 下没有 .ts 文件" }

# 1) 从源码抽出新字符串（不覆盖已有译文，只补 type="unfinished" 的新条目）
Write-Host "=== lupdate（扫描 src 与 include）==="
& $lupdate -recursive (Join-Path $root "src") (Join-Path $root "include") -ts ($tsFiles | ForEach-Object { $_.FullName })
if ($LASTEXITCODE -ne 0) { throw "lupdate 失败（exit $LASTEXITCODE）" }

# 2) 默认语言包（中文）：身份翻译 -> resources/translations/dshhub_zh_CN.qm（进 qrc）
$enTs = Join-Path $tsDir "dshhub_en.ts"
$defaultTs = Join-Path $tsDir "dshhub_zh_CN.ts"
$resDir = Join-Path $root "resources\translations"
if (Test-Path $enTs) {
	$en = [System.IO.File]::ReadAllText($enTs, $utf8NoBom)
	$opt = [System.Text.RegularExpressions.RegexOptions]::Singleline
	$pat = '(<source>(?<s>.*?)</source>\s*)<translation type="unfinished"></translation>'
	$zh = [regex]::Replace($en, $pat, '${1}<translation>${s}</translation>', $opt)
	$zh = $zh -replace 'language="en_US"', 'language="zh_CN"'
	[System.IO.File]::WriteAllText($defaultTs, $zh, $utf8Bom)
	New-Item -ItemType Directory -Path $resDir -Force | Out-Null
	& $lrelease $defaultTs -qm (Join-Path $resDir "dshhub_zh_CN.qm") | Out-Null
	if ($LASTEXITCODE -ne 0) { throw "lrelease 失败：默认语言包" }
	Write-Host "已生成默认语言包：$defaultTs -> resources\translations\dshhub_zh_CN.qm（进 qrc，需重新构建生效）"
}

if ($UpdateOnly) {
	Write-Host "只更新 .ts 与默认语言包清单，未发布 .qm（-UpdateOnly）"
	return
}

# 3) 编译成 .qm 并发布到程序目录（同时拷 .ts 清单，供运行时"就地换文案"）
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
Write-Host "`n=== lrelease -> $outDir ==="
foreach ($ts in (Get-ChildItem $tsDir -File -Filter *.ts)) {
	$qm = Join-Path $outDir ($ts.BaseName + ".qm")
	& $lrelease $ts.FullName -qm $qm | Out-Null
	if ($LASTEXITCODE -ne 0) { throw "lrelease 失败：$($ts.Name)" }
	Copy-Item $ts.FullName (Join-Path $outDir $ts.Name) -Force
	Write-Host ("  {0} -> {1} ({2:N0} 字节)" -f $ts.Name, (Split-Path $qm -Leaf), (Get-Item $qm).Length)
}

Write-Host "`n完成。启动程序后可在 设置 -> 外观设置 -> 界面语言 里切换（立即生效）。"
