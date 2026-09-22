<#
  make-dependence.ps1 —— 把运行库 DLL 收进 <目标目录>\dependence\。

  为什么要脚本：exe 从子目录加载 DLL 靠的是 manifest 私有程序集，而那个程序集要求
  「目录名 == 程序集 name」且「目录里有一份列出每个 <file> 的 dependence.manifest」。
  人手搬很容易漏一份清单或漏一个 dll，漏了就变成启动即失败。所以这里：

    - DLL 名单**从 source\dependence.manifest 读**（清单是唯一事实来源，改清单即改行为）
    - 把清单本身也拷过去（运行时必需）
    - 幂等：已经是 dependence\ 里的就跳过

  用法：
    .\misc\tools\make-dependence.ps1 -Target x64\Release
    .\misc\tools\make-dependence.ps1 -Target ReleaseBuild\bin

  ⚠️ 前提：该目录里的 exe 必须是**带 dependence 依赖的新构建**（嵌入 manifest）。
     用旧 exe 会出现"文件都在却仍报找不到 DLL"。
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)]
	[string]$Target
)

$ErrorActionPreference = 'Stop'

# misc\tools\ -> 仓库根
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$manifestSrc = Join-Path $root 'source\dependence.manifest'

if (-not (Test-Path -LiteralPath $manifestSrc)) { throw "找不到清单: $manifestSrc" }
if (-not (Test-Path -LiteralPath $Target)) { throw "目标目录不存在: $Target" }
$target = (Resolve-Path -LiteralPath $Target).Path

$depDir = Join-Path $target 'dependence'
New-Item -ItemType Directory -Path $depDir -Force | Out-Null

# 清单即事实：DLL 名单从这里读
[xml]$manifest = Get-Content -LiteralPath $manifestSrc -Raw
$names = @($manifest.assembly.file | ForEach-Object { $_.name })
if ($names.Count -eq 0) { throw "清单里没有 <file> 条目: $manifestSrc" }

$moved = 0
$already = 0
$missing = @()
foreach ($name in $names) {
	$src = Join-Path $target $name
	$dst = Join-Path $depDir $name
	if (Test-Path -LiteralPath $src) {
		Move-Item -LiteralPath $src -Destination $dst -Force
		Write-Host "  移入  $name"
		$moved++
	} elseif (Test-Path -LiteralPath $dst) {
		$already++
	} else {
		$missing += $name
	}
}

Copy-Item -LiteralPath $manifestSrc -Destination (Join-Path $depDir 'dependence.manifest') -Force

Write-Host ""
Write-Host ("目标: {0}" -f $target)
Write-Host ("  本次移入 {0} 个，已在位 {1} 个，清单共 {2} 个" -f $moved, $already, $names.Count)
if ($missing.Count -gt 0) {
	Write-Warning ("有 {0} 个 DLL 既不在目标目录也不在 dependence\: {1}" -f $missing.Count, ($missing -join ', '))
	Write-Warning "  它们可能还没部署到这个目录（Qt VS Tools / 手工拷贝 / windeployqt），补上后再跑一次本脚本。"
	Write-Warning "  ⚠️ 缺任何一个都会让 exe 启动失败（0xc0000135）。"
}
