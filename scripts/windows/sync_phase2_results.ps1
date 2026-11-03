# 用途：把 Ubuntu 上第二阶段的压测结果复制回 Windows 项目目录。
# 默认从 yjj@192.168.184.128 下载最新一轮；如果虚拟机 IP 变了，可以用 -SshTarget 修改。
# Linux 结果目录：/home/yjj/rpc-bench/ordersystem_phase2_async/
# Windows 保存目录：项目根目录/perf-results/phase2_async/
# 需要 Windows 已安装 OpenSSH Client（ssh.exe 和 scp.exe）；首次连接时按提示确认主机密钥并输入 SSH 密码。

param(
    [string]$SshTarget = "yjj@192.168.184.128",
    [int]$Port = 22,
    [string]$RemoteResultsRoot = "/home/yjj/rpc-bench/ordersystem_phase2_async",
    [string]$RunId = "",
    [string]$IdentityFile = ""
)

$ErrorActionPreference = "Stop"

$ssh = Get-Command ssh -ErrorAction SilentlyContinue
$scp = Get-Command scp -ErrorAction SilentlyContinue

if (-not $ssh -or -not $scp) {
    throw "找不到 ssh.exe 或 scp.exe。请在 Windows 的「可选功能」中安装 OpenSSH Client。"
}

# 脚本现在位于 scripts\windows，向上两级才是 OrderSystem 项目根目录。
$projectRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$localResultsRoot = Join-Path $projectRoot "perf-results\phase2_async"

New-Item -ItemType Directory -Force -Path $localResultsRoot | Out-Null

$sshArgs = @("-p", "$Port")
$scpArgs = @("-P", "$Port", "-r")

if (-not [string]::IsNullOrWhiteSpace($IdentityFile)) {
    if (-not (Test-Path $IdentityFile)) {
        throw "SSH 私钥文件不存在：$IdentityFile"
    }
    $sshArgs += @("-i", $IdentityFile)
    $scpArgs += @("-i", $IdentityFile)
}

if ([string]::IsNullOrWhiteSpace($RunId)) {
    $findCommand = "find '$RemoteResultsRoot' -mindepth 1 -maxdepth 1 -type d -printf '%T@ %f\n' | sort -nr | head -n 1 | cut -d' ' -f2-"
    $latest = & $ssh.Source @sshArgs $SshTarget $findCommand
    if ($LASTEXITCODE -ne 0) {
        throw "无法读取 Linux 测试结果目录：$RemoteResultsRoot"
    }

    $latestLine = $latest | Select-Object -First 1
    if ($null -ne $latestLine) {
        $RunId = ([string]$latestLine).Trim()
    }
    if ([string]::IsNullOrWhiteSpace($RunId)) {
        throw "Linux 上还没有测试结果：$RemoteResultsRoot"
    }
}

if ($RunId -notmatch '^[A-Za-z0-9._-]+$') {
    throw "RunId 只能包含英文字母、数字、点、下划线和短横线。"
}

$remoteRunPath = "$RemoteResultsRoot/$RunId"
$remoteSpec = "${SshTarget}:$remoteRunPath"

Write-Host "准备下载测试结果：" -ForegroundColor Cyan
Write-Host "Linux：$remoteSpec"
Write-Host "Windows：$localResultsRoot"

& $scp.Source @scpArgs $remoteSpec $localResultsRoot
if ($LASTEXITCODE -ne 0) {
    throw "scp 下载失败。请检查 SSH 地址、密码/密钥和远程目录。"
}

Write-Host ""
Write-Host "测试结果已复制到：" -ForegroundColor Green
Write-Host (Join-Path $localResultsRoot $RunId)
