#Requires -Version 7.0
<#
.SYNOPSIS
    IRIS 快速构建脚本。

.DESCRIPTION
    对 IRIS 做一次完整的「配置 → 构建 →（可选）测试」，并打印产物路径。

    脚本使用仓库内的 CMakePresets.json，因此：
      * 生成器固定为 Visual Studio 2022 x64；
      * TEMP/TMP 被重定向到 <仓库>/build/tmp。这既规避了 MSVC 在含非 ASCII
        字符的临时目录下的 D8050 问题，也让配置根目录的探测在测试中保持确定性。

.PARAMETER Config
    构建配置，Release（默认）或 Debug。

.PARAMETER Clean
    构建前删除该配置的构建目录，做一次全新的 configure。

.PARAMETER Test
    构建完成后运行 ctest。

.PARAMETER Target
    只构建指定目标（例如 IRIS）。默认构建全部。

.PARAMETER Jobs
    并行编译的进程数。默认交给生成器决定。

.PARAMETER CMakePath
    显式指定 cmake.exe 路径。优先级高于 PATH 与自动探测。

.EXAMPLE
    .\build.ps1
    以 Release 配置构建全部目标。

.EXAMPLE
    .\build.ps1 -Config Debug -Test
    以 Debug 配置构建并运行测试。

.EXAMPLE
    .\build.ps1 -Clean -Target IRIS
    清理后只重新构建 observer DLL。
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug')]
    [string]$Config = 'Release',

    [switch]$Clean,
    [switch]$Test,
    [string]$Target,
    [int]$Jobs = 0,
    [string]$CMakePath
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$Root       = $PSScriptRoot
$BuildRoot  = Join-Path $Root 'build'
$PresetName = $Config.ToLowerInvariant()

# ---------------------------------------------------------------------------
# 输出辅助
# ---------------------------------------------------------------------------
function Write-Step   { param([string]$Text) Write-Host "`n>>> $Text" -ForegroundColor Cyan }
function Write-Ok     { param([string]$Text) Write-Host "    $Text" -ForegroundColor Green }
function Write-Warn   { param([string]$Text) Write-Host "    $Text" -ForegroundColor Yellow }
function Write-Fail   { param([string]$Text) Write-Host "    $Text" -ForegroundColor Red }

# ---------------------------------------------------------------------------
# 1. 定位 CMake
# ---------------------------------------------------------------------------
function Resolve-CMake {
    param([string]$Explicit)

    if ($Explicit) {
        if (-not (Test-Path -LiteralPath $Explicit)) {
            throw "指定的 -CMakePath 不存在：$Explicit"
        }
        return (Resolve-Path -LiteralPath $Explicit).Path
    }

    if ($env:IRIS_CMAKE -and (Test-Path -LiteralPath $env:IRIS_CMAKE)) {
        return (Resolve-Path -LiteralPath $env:IRIS_CMAKE).Path
    }

    $onPath = Get-Command cmake -CommandType Application -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }

    $candidates = @(
        (Join-Path $env:ProgramFiles 'CMake\bin\cmake.exe')
        (Join-Path ${env:ProgramFiles(x86)} 'CMake\bin\cmake.exe')
        (Join-Path $env:LOCALAPPDATA 'Programs\CMake\bin\cmake.exe')
        # 维护者本机的便携安装位置；对其它机器不存在，会被自动跳过
        'F:\DevTools\CMake\bin\cmake.exe'
    )

    # Visual Studio 自带的 CMake（Build Tools 默认不装，装了就能用）
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $installs = & $vswhere -products * -property installationPath 2>$null
        foreach ($install in $installs) {
            $candidates += (Join-Path $install 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe')
        }
    }

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    throw @"
未找到 cmake.exe。请任选一种方式：
  1) 把 CMake 加入 PATH；
  2) 设置环境变量 IRIS_CMAKE 指向 cmake.exe；
  3) 用 -CMakePath 显式指定；
  4) 安装：winget install Kitware.CMake
下载地址：https://cmake.org/download/
"@
}

# ---------------------------------------------------------------------------
# 2. 环境净化
#
#    MSBuild 的 ToolTask 会用 ProcessStartInfo.EnvironmentVariables（.NET 的
#    大小写不敏感哈希表）复制环境变量。如果环境里同时存在仅大小写不同的同名
#    变量——典型来源是代理工具同时设置了 HTTP_PROXY 与 http_proxy——MSBuild
#    会抛 MSB6001「已添加项」，编译器探测随之彻底失败，报错却是
#    "No CMAKE_C_COMPILER could be found"，非常难排查。
#
#    这里在启动子进程前显式重建一份折叠了大小写重复项的环境块。环境本来就干净
#    时该步骤是空操作。
# ---------------------------------------------------------------------------
function Get-SanitizedEnvironment {
    $raw = [System.Environment]::GetEnvironmentVariables('Process')
    $seen = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    $values = [System.Collections.Generic.Dictionary[string, string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    $collapsed = [System.Collections.Generic.List[string]]::new()

    foreach ($key in $raw.Keys) {
        $name = [string]$key
        if ($seen.Add($name)) { $values[$name] = [string]$raw[$key] }
        else                  { $collapsed.Add($name) }
    }
    return [pscustomobject]@{ Values = $values; Collapsed = $collapsed }
}

$Environment = Get-SanitizedEnvironment

function Invoke-Native {
    param(
        [Parameter(Mandatory)][string]$Exe,
        [Parameter(Mandatory)][string[]]$Arguments
    )
    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName               = $Exe
    $startInfo.UseShellExecute        = $false
    $startInfo.WorkingDirectory       = $Root
    $startInfo.Environment.Clear()
    foreach ($pair in $Environment.Values.GetEnumerator()) {
        $startInfo.Environment[$pair.Key] = $pair.Value
    }
    foreach ($argument in $Arguments) { $startInfo.ArgumentList.Add($argument) }

    $process = [System.Diagnostics.Process]::Start($startInfo)
    $process.WaitForExit()
    return $process.ExitCode
}

# ---------------------------------------------------------------------------
# 主流程
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "==================== IRIS 构建 ====================" -ForegroundColor White

$cmake = Resolve-CMake -Explicit $CMakePath
Write-Ok "CMake          : $cmake"
Write-Ok "配置           : $Config"
Write-Ok "源码目录       : $Root"
if ($Target) { Write-Ok "目标           : $Target" }

if ($Environment.Collapsed.Count -gt 0) {
    Write-Warn "折叠了 $($Environment.Collapsed.Count) 个大小写重复的环境变量（规避 MSBuild MSB6001）："
    foreach ($name in $Environment.Collapsed) { Write-Warn "  - $name" }
}

# --- 清理 ---
if ($Clean) {
    Write-Step "清理构建目录"
    if (Test-Path -LiteralPath $BuildRoot) {
        $resolved = (Resolve-Path -LiteralPath $BuildRoot).Path
        # 只允许删除本仓库内的构建目录
        if (-not $resolved.StartsWith($Root, [System.StringComparison]::OrdinalIgnoreCase)) {
            throw "拒绝删除仓库之外的路径：$resolved"
        }
        Remove-Item -LiteralPath $resolved -Recurse -Force
        Write-Ok "已删除 $resolved"
    } else {
        Write-Ok "构建目录不存在，无需清理"
    }
}

# --- 预设要求 TEMP/TMP 目录存在 ---
$presetTemp = Join-Path $BuildRoot 'tmp'
if (-not (Test-Path -LiteralPath $presetTemp)) {
    New-Item -ItemType Directory -Force -Path $presetTemp | Out-Null
}

# --- 配置 ---
$cacheFile = Join-Path $BuildRoot "$PresetName\CMakeCache.txt"
if (-not (Test-Path -LiteralPath $cacheFile)) {
    Write-Step "配置（cmake --preset $PresetName）"
    $code = Invoke-Native -Exe $cmake -Arguments @('--preset', $PresetName)
    if ($code -ne 0) { Write-Fail "配置失败，退出码 $code"; exit $code }
    Write-Ok "配置完成"
} else {
    Write-Step "复用已有配置（需要重新配置时用 -Clean）"
    Write-Ok $cacheFile
}

# --- 构建 ---
Write-Step "构建（cmake --build --preset $PresetName）"
$buildArguments = @('--build', '--preset', $PresetName)
if ($Target)  { $buildArguments += @('--target', $Target) }
if ($Jobs -gt 0) { $buildArguments += @('--parallel', "$Jobs") }

$timer = [System.Diagnostics.Stopwatch]::StartNew()
$code  = Invoke-Native -Exe $cmake -Arguments $buildArguments
$timer.Stop()

if ($code -ne 0) {
    Write-Fail "构建失败，退出码 $code（耗时 $([math]::Round($timer.Elapsed.TotalSeconds,1))s）"
    exit $code
}
Write-Ok "构建成功，耗时 $([math]::Round($timer.Elapsed.TotalSeconds,1))s"

# --- 测试 ---
if ($Test) {
    Write-Step "测试（ctest --preset $PresetName）"
    $ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
    if (-not (Test-Path -LiteralPath $ctest)) {
        Write-Fail "未在同目录找到 ctest.exe：$ctest"
        exit 1
    }
    $code = Invoke-Native -Exe $ctest -Arguments @('--preset', $PresetName)
    if ($code -ne 0) { Write-Fail "测试失败，退出码 $code"; exit $code }
    Write-Ok "测试全部通过"
}

# --- 产物 ---
$binDir = Join-Path $BuildRoot "$PresetName\bin\$Config"
Write-Step "产物"
if (Test-Path -LiteralPath $binDir) {
    Get-ChildItem -LiteralPath $binDir -File |
        Sort-Object Name |
        ForEach-Object {
            Write-Host ("    {0,-26} {1,8:N1} KB" -f $_.Name, ($_.Length / 1KB))
        }
} else {
    Write-Warn "未找到产物目录 $binDir"
}

Write-Host ""
Write-Host "===================================================" -ForegroundColor White
exit 0
