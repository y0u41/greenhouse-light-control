<#
.SYNOPSIS
    智能温室感光控制 —— 一键构建与自检。

.DESCRIPTION
    把"固件编译 / 上位机构建 / 契约测试"三件事固化成一条命令，并处理两个本机特有的坑：

    1) 源码路径含中文（D:\正点原子\...）：Qt 的 moc（AutoMoc）无法把生成文件写进
       非 ASCII 路径，会报 "moc: Cannot create ... No such file or directory"。
       本脚本自动创建一个纯 ASCII 的目录联接（junction）并经由它构建，
       源码本身仍留在原路径，不做任何搬移。
    2) 工具链不在 PATH：Keil 在 D:\MDK5.36，Qt 在 E:\Qt，脚本自行探测并临时注入 PATH。

    注意：8.3 短路径在本机仍是中文（如 "个人项~1"），因此**不能**用短路径绕过第 1 点。

.PARAMETER Target
    all(默认) / firmware / qt / test

.PARAMETER AsciiAlias
    ASCII 联接路径，默认 <源码所在盘>:\glc

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools\build.ps1
    powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Target qt

.NOTES
    本文件含中文，必须保存为 "UTF-8 with BOM"。
    Windows PowerShell 5.1 会把无 BOM 的 UTF-8 当 ANSI 读，导致中文串被截断、
    解析报 "Unexpected token"，脚本直接不可用。
    若某次编辑（含 AI 工具写文件）后脚本突然报语法错，先恢复 BOM：
      $p='tools\build.ps1'
      $t=[IO.File]::ReadAllText($p,[Text.Encoding]::UTF8)
      [IO.File]::WriteAllText($p,$t,(New-Object Text.UTF8Encoding($true)))
#>
[CmdletBinding()]
param(
    [ValidateSet('all', 'firmware', 'qt', 'test')]
    [string]$Target = 'all',
    [string]$AsciiAlias = ''
)

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)

function Info($m) { Write-Host "[build] $m" }
function Ok($m)   { Write-Host "[ ok ] $m" -ForegroundColor Green }
function Warn($m) { Write-Host "[warn] $m" -ForegroundColor Yellow }
function Fail($m) { Write-Host "[fail] $m" -ForegroundColor Red }

# ---------------------------------------------------------------- ASCII 联接
function Get-AsciiRoot {
    param([string]$RealRoot, [string]$Alias)
    if ($RealRoot -match '^[\x00-\x7F]+$') { return $RealRoot }   # 本来就全是 ASCII

    $drive = (Split-Path -Qualifier $RealRoot)
    if ([string]::IsNullOrWhiteSpace($Alias)) { $Alias = Join-Path $drive 'glc' }

    if (Test-Path $Alias) {
        $item = Get-Item $Alias -Force
        if ($item.LinkType -eq 'Junction' -and $item.Target -contains $RealRoot) {
            return $Alias
        }
        Warn "$Alias 已存在且不是指向本项目的联接，改用 $Alias-build"
        $Alias = "$Alias-build"
        if (Test-Path $Alias) { Remove-Item $Alias -Force -Recurse }
    }
    & cmd /c mklink /J "$Alias" "$RealRoot" | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail "创建 ASCII 联接失败：$Alias"; exit 1 }
    Info "已建立 ASCII 联接：$Alias -> $RealRoot（规避 moc 对非 ASCII 路径的限制）"
    return $Alias
}

# ---------------------------------------------------------------- 工具链探测
function Find-Tool {
    param([string[]]$Candidates, [string]$Name)
    foreach ($c in $Candidates) { if (Test-Path $c) { return $c } }
    Fail "未找到 $Name，已尝试：`n  " + ($Candidates -join "`n  ")
    exit 1
}

function Get-KeilUv4 {
    Find-Tool -Name 'Keil UV4.exe' -Candidates @(
        'D:\MDK5.36\UV4\UV4.exe', 'C:\Keil_v5\UV4\UV4.exe',
        'D:\Keil_v5\UV4\UV4.exe', 'C:\Program Files\Keil_v5\UV4\UV4.exe',
        'C:\Program Files (x86)\Keil_v5\UV4\UV4.exe')
}

function Get-QtEnv {
    $qtRoot = Find-Tool -Name 'Qt 安装（Qt6Config）' -Candidates @(
        'E:\Qt', 'D:\Qt', 'C:\Qt')
    $qtKit = Get-ChildItem (Join-Path $qtRoot '*\mingw_64') -Directory -ErrorAction SilentlyContinue |
             Sort-Object Name -Descending | Select-Object -First 1
    if (-not $qtKit) { Fail "未在 $qtRoot 下找到 *\mingw_64 套件"; exit 1 }
    $cmake = Find-Tool -Name 'CMake' -Candidates @(
        (Join-Path $qtRoot 'Tools\CMake_64\bin\cmake.exe'), 'C:\Program Files\CMake\bin\cmake.exe')
    $ninja = Find-Tool -Name 'Ninja' -Candidates @(
        (Join-Path $qtRoot 'Tools\Ninja\ninja.exe'))
    $mingw = Find-Tool -Name 'MinGW (gcc.exe)' -Candidates @(
        (Join-Path $qtRoot 'Tools\mingw1310_64\bin\gcc.exe'))
    return [pscustomobject]@{
        KitDir = $qtKit.FullName; Cmake = $cmake; Ninja = $ninja
        MingwBin = (Split-Path -Parent $mingw)
    }
}

# ---------------------------------------------------------------- 固件
function Build-Firmware {
    param([string]$AsciiRoot)
    Info '=== 固件：生成 Keil 工程并编译 ==='
    $py = Get-Python
    Push-Location $Root
    & $py 'tools\make_keil_project.py' | ForEach-Object { Info $_ }
    if ($LASTEXITCODE -ne 0) { Pop-Location; Fail 'Keil 工程生成/引用校验失败'; exit 1 }
    Pop-Location

    $uv4 = Get-KeilUv4
    $prj = Join-Path $AsciiRoot 'firmware\Projects\MDK-ARM\greenhouse.uvprojx'
    $log = Join-Path $AsciiRoot 'firmware\Projects\MDK-ARM\build.log'
    Remove-Item $log -ErrorAction SilentlyContinue

    & $uv4 -b $prj -j0 -o $log | Out-Null
    $code = $LASTEXITCODE
    $text = if (Test-Path $log) { Get-Content $log -Encoding Default -Raw } else { '' }

    $sizeLine = ($text -split "`r?`n" | Where-Object { $_ -match 'Program Size' }) -join ''
    $resLine  = ($text -split "`r?`n" | Where-Object { $_ -match 'Error\(s\)' }) -join ''
    if ($sizeLine) { Info $sizeLine.Trim() }
    if ($resLine)  { Info $resLine.Trim() }

    if ($code -ge 2) {
        ($text -split "`r?`n" | Where-Object { $_ -match 'error|Error' }) | Select-Object -First 25 |
            ForEach-Object { Warn $_ }
        Fail "固件编译失败（UV4 退出码 $code），完整日志：$log"
        exit 1
    }
    Ok "固件编译通过（UV4 退出码 $code；0 = 无警告）"
}

# ---------------------------------------------------------------- 上位机
function Build-Qt {
    param([string]$AsciiRoot)
    Info '=== 上位机：CMake 配置并构建 ==='
    $qt = Get-QtEnv
    $env:PATH = "$($qt.MingwBin);$($qt.Cmake | Split-Path -Parent);$($qt.Ninja | Split-Path -Parent);$env:PATH"

    $src = Join-Path $AsciiRoot 'qtapp'
    Push-Location $src
    & $qt.Cmake -S . -B build -G Ninja "-DCMAKE_PREFIX_PATH=$($qt.KitDir)" -DCMAKE_BUILD_TYPE=Release 2>&1 |
        Where-Object { $_ -match 'TTS:|ASR:|Configuring done|Generating done|Error|error' } |
        ForEach-Object { Info $_ }
    if ($LASTEXITCODE -ne 0) { Pop-Location; Fail 'CMake 配置失败'; exit 1 }

    & $qt.Cmake --build build 2>&1 |
        Where-Object { $_ -match 'error:|FAILED|Linking CXX executable|ninja: build stopped' } |
        ForEach-Object { Info $_ }
    $rc = $LASTEXITCODE
    Pop-Location
    if ($rc -ne 0) { Fail '上位机编译失败'; exit 1 }

    $exe = Join-Path $src 'build\GreenhouseLightControl.exe'
    if (Test-Path $exe) { Ok ("上位机构建通过：" + (Get-Item $exe).Length + " B  " + $exe) }
    else { Fail '未生成可执行文件'; exit 1 }
}

# ---------------------------------------------------------------- 自检
function Get-Python {
    $py = Get-Command py -ErrorAction SilentlyContinue
    if ($py) { return 'py' }
    $p = Get-Command python -ErrorAction SilentlyContinue
    if ($p) { return 'python' }
    Fail '未找到 Python（本机 python 是 Store 占位 stub，请用 py）'; exit 1
}

function Invoke-Tests {
    Info '=== 契约与自检 ==='
    $py = Get-Python
    Push-Location $Root
    $failed = 0
    foreach ($t in @(
        @{ n = '协议单一事实源自检'; a = @('tools\protocol_schema.py') },
        @{ n = '协议契约测试';       a = @('tools\contract_test.py') },
        @{ n = '跨层一致性守护';     a = @('tools\consistency_check.py') },
        @{ n = '主机端行为测试';     a = @('tests\host\run_host_tests.py') },
        @{ n = '文档生成区未漂移';   a = @('tools\sync_doc_tables.py', '--check') },
        @{ n = '虚拟节点自检';       a = @('tools\simulate_node.py', '--selftest') },
        @{ n = 'Keil 工程引用校验';  a = @('tools\make_keil_project.py', '--check') },
        @{ n = 'Qt/Python 校验一致性快照'; a = @('tools\check_validate_parity.py', '--self-check') }
    )) {
        & $py @($t.a) *> $null
        if ($LASTEXITCODE -eq 0) { Ok $t.n } else { Fail "$($t.n)（退出码 $LASTEXITCODE）"; $failed++ }
    }
    Info '（firmware\prebuild_check.py 需要 CubeMX 骨架，此处只做提示）'
    & $py 'firmware\prebuild_check.py' *> $null
    if ($LASTEXITCODE -eq 0) {
        Ok '固件 prebuild 自检'
    } else {
        # ★ 阻塞项必须计入 $failed：过去这里只 Warn，于是"重复定义 USART3_IRQHandler"
        #   这类**必然链接失败**的问题被降级成提示，整条 -Target test 仍打"全部自检通过"
        #   并 exit 0。门禁的意义就是"有阻塞项不许绿"，与上面 7 项口径保持一致。
        Fail "固件 prebuild 自检有阻塞项（退出码 $LASTEXITCODE，见单独运行输出）"
        $failed++
    }
    Pop-Location
    if ($failed -gt 0) { exit 1 }
    Ok '全部自检通过'
}

# ---------------------------------------------------------------- 主流程
Info "仓库：$Root"
$ascii = Get-AsciiRoot -RealRoot $Root -Alias $AsciiAlias

switch ($Target) {
    'firmware' { Build-Firmware -AsciiRoot $ascii }
    'qt'       { Build-Qt -AsciiRoot $ascii }
    'test'     { Invoke-Tests }
    default {
        Build-Firmware -AsciiRoot $ascii
        Build-Qt -AsciiRoot $ascii
        Invoke-Tests
    }
}
Ok "完成（Target=$Target）"
