# ------------------------------------------------------------------
# release-translations.ps1
# ------------------------------------------------------------------
# 语言包发布。源码里只有 key（qtTrId("...")），文案只存在于 translations\*.ts，
# 所以本脚本不再"从源码生成 .ts"，而是做四件事：
#
#   0) 规范化：正式 .ts 是 ID-based（<message id="..."> + <translation>）。
#      <source> 不是查表键、运行期也不使用，这里直接从正式 .ts 里删掉，
#      避免同一句文案在 <source> 和 <translation> 里各维护一遍。
#   1) lupdate 抽一份**临时**清单，只为对比：源码新增/删除了哪些 key。
#      绝不能拿它更新正式 .ts —— lupdate 会把译文回标成 type="unfinished"
#      （lrelease 默认跳过这种条目），那样编出来的包会缺文案，
#      界面直接显示 topbar_settings 这类代号。
#   2) 校验：正式 .ts 与源码的 id 集合必须一致；源码里有、.ts 里没有的 key
#      会以代号显示给用户，所以脚本直接报错停下。
#   3) lrelease：把 .ts 编成 .qm 发布到 x64\<Config>\translations；内置语言包
#      另存一份到 resources\translations\（进 qrc，作为开箱保底），
#      并把 .ts 清单一并拷过去 —— 运行时的"就地换文案"靠它把界面文案认回 id，
#      缺了这份清单该机制会静默降级。
#
# 改文案 / 加 key 的正确做法：
#   在 translations\dshhub_*.ts 里手工加一条
#     <message id="xxx"><translation>译文</translation></message>
#   不要写 <source>；即使写了，本脚本的规范化步骤也会删掉。
#   再跑本脚本；漏了哪条它会列出来。
#
# 用法（仓库根目录，Windows PowerShell 直接跑）：
#   .\misc\tools\release-translations.ps1                 # 发布到 x64\Release
#   .\misc\tools\release-translations.ps1 -Config Debug   # 发布到 x64\Debug
#   .\misc\tools\release-translations.ps1 -CheckOnly      # 只做 key 一致性校验
#
# 注意：resources\translations\*.qm 变了要重新构建（它们进 qrc）。
# ------------------------------------------------------------------

param(
	[string]$Config = "Release",
	[switch]$CheckOnly
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$tsDir = Join-Path $root "assets\translations"
$resDir = Join-Path $root "assets\resources\translations"
$outDir = Join-Path $root "x64\$Config\translations"

# 与 TranslationManager.cpp 的 kBuiltinPacks / assets\resources\DSHHub.qrc 保持一致：
# 这几个包除了发布到程序目录，还会内置进 qrc 作为开箱保底。
$builtinPacks = @("dshhub_zh_CN", "dshhub_en")

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

$tsFiles = @(Get-ChildItem $tsDir -File -Filter *.ts)
if ($tsFiles.Count -eq 0) { throw "$tsDir 下没有 .ts 文件" }

function Get-Ids([string]$path) {
	# 默认 ReadAllText 会自动识别并剥离 BOM；TrimStart 只是兜底
	$text = [System.IO.File]::ReadAllText($path)
	$text = $text.TrimStart([char]0xFEFF)
	$set = New-Object 'System.Collections.Generic.HashSet[string]'
	foreach ($m in [regex]::Matches($text, '<message\s+id="([^"]+)"')) {
		[void]$set.Add($m.Groups[1].Value)
	}
	return ,$set
}

# 原生命令往 stderr 写东西时，$ErrorActionPreference="Stop" 会把它当成致命错误抛出。
# lupdate 的 "Message with id 'xxx' has no source" 警告是我们**故意**触发的
#（ID-based 模式下源码本来就没有原文），所以这里临时降级并用 2>&1 合并丢弃。
function Invoke-Quiet([string]$exe, [string[]]$argv) {
	$prev = $ErrorActionPreference
	$ErrorActionPreference = "Continue"
	& $exe @argv 2>&1 | Out-Null
	$code = $LASTEXITCODE
	$ErrorActionPreference = $prev
	return $code
}

function Remove-SourceElements([string]$path) {
	# ID-based .ts 只需要 id + translation；<source> 不参与查表，删掉以免重复维护。
	$text = [System.IO.File]::ReadAllText($path)
	$stripped = [regex]::Replace($text, '(?s)[ \t]*<source>.*?</source>\r?\n?', '')
	if ($stripped -ne $text) {
		$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
		[System.IO.File]::WriteAllText($path, $stripped, $utf8NoBom)
		return $true
	}
	return $false
}

# 0) 规范化正式 .ts：删掉 <source>
Write-Host "`n=== 规范化 .ts（移除 <source>）==="
foreach ($ts in $tsFiles) {
	if (Remove-SourceElements $ts.FullName) {
		Write-Host ("  已移除 <source>: {0}" -f $ts.Name)
	}
	else {
		Write-Host ("  无需改动: {0}" -f $ts.Name)
	}
}

# 1) 抽临时清单（只用于对比，不碰正式文件）
Write-Host "=== lupdate（抽临时清单用于对比）==="
$tmpTs = Join-Path $env:TEMP ("dshhub-lupdate-" + [guid]::NewGuid().ToString("N") + ".ts")
$code = Invoke-Quiet $lupdate @("-recursive", (Join-Path $root "source\src"), (Join-Path $root "source\include"), "-ts", $tmpTs)
if ($code -ne 0 -or -not (Test-Path $tmpTs)) {
	Remove-Item $tmpTs -Force -ErrorAction SilentlyContinue
	throw "lupdate 失败（exit $code）"
}
$srcIds = Get-Ids $tmpTs
Remove-Item $tmpTs -Force -ErrorAction SilentlyContinue

$tsIds = New-Object 'System.Collections.Generic.HashSet[string]'
foreach ($ts in $tsFiles) {
	foreach ($id in (Get-Ids $ts.FullName)) { [void]$tsIds.Add($id) }
}

Write-Host ("源码 key：{0} 个；.ts key：{1} 个（{2} 份文件）" -f $srcIds.Count, $tsIds.Count, $tsFiles.Count)

$missing = @($srcIds | Where-Object { -not $tsIds.Contains($_) } | Sort-Object)
$stale = @($tsIds | Where-Object { -not $srcIds.Contains($_) } | Sort-Object)

if ($stale.Count -gt 0) {
	Write-Host "[warn] 这些 key 在源码里已不存在（.ts 里仍留着，可自行清理）："
	foreach ($id in $stale) { Write-Host "        $id" }
}

if ($missing.Count -gt 0) {
	Write-Host "[error] 以下 key 在源码里用了、却没进任何 .ts —— 界面上会显示成代号："
	foreach ($id in $missing) { Write-Host "        $id" }
	throw "请先在 translations\dshhub_*.ts 里补上这些 id 的 <translation>"
}
Write-Host "key 一致性校验通过。"

if ($CheckOnly) {
	Write-Host "`n只做校验，未编译与发布（-CheckOnly）"
	return
}

# 2) 编译：全部 .ts -> .qm；内置包另存到 resources\translations（进 qrc）
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
New-Item -ItemType Directory -Path $resDir -Force | Out-Null
Write-Host "`n=== lrelease ==="
foreach ($ts in $tsFiles) {
	$qm = Join-Path $outDir ($ts.BaseName + ".qm")
	& $lrelease $ts.FullName -qm $qm | Out-Null
	if ($LASTEXITCODE -ne 0) { throw "lrelease 失败：$($ts.Name)" }
	Write-Host ("  {0} -> x64\{1}\translations\{2}" -f $ts.Name, $Config, (Split-Path $qm -Leaf))

	if ($builtinPacks -contains $ts.BaseName) {
		Copy-Item $qm (Join-Path $resDir ($ts.BaseName + ".qm")) -Force
		Write-Host ("         -> resources\translations\{0}.qm（进 qrc，需重新构建生效）" -f $ts.BaseName)
	}
}

# 3) 发布 .ts 清单：只有一份就够（id 集合与语言无关），取默认语言那份，
#    它带完整中文原文，便于就地查阅。
$manifest = Join-Path $tsDir "dshhub_zh_CN.ts"
if (Test-Path $manifest) {
	Copy-Item $manifest (Join-Path $outDir "dshhub_zh_CN.ts") -Force
	Write-Host "`n已发布 .ts 清单：x64\$Config\translations\dshhub_zh_CN.ts"
}
else {
	Write-Host "[warn] 找不到 $manifest —— 运行时的“就地换文案”会自动降级为不做"
}

Write-Host "`n完成。启动程序后可在 设置 -> 外观设置 -> 界面语言 里切换（立即生效）。"
