# 可选：把桌宠丢进「启动」文件夹，随 Windows 登录一起启动。
#
# 现在不需要了 —— magpie 的插件宿主会在启动时自己拉起 pet.exe，
# 宿主退出时宠物也跟着退出（见 pet.hooks.js / README「伴随 magpie 启动」）。
#
# 只有在你希望「没开 magpie 也先摆着」时才用（此时它显示的是断线状态）。
#
#   .\install-startup.ps1            # 安装
#   .\install-startup.ps1 -Remove    # 撤销

param([switch]$Remove)

$ErrorActionPreference = "Stop"
$root = $PSScriptRoot
$exe = Join-Path $root "pet-c\pet.exe"
$link = Join-Path ([Environment]::GetFolderPath("Startup")) "MagpiePet.lnk"

if ($Remove) {
    if (Test-Path $link) { Remove-Item $link -Force; Write-Host "已移除开机启动: $link" -ForegroundColor Yellow }
    else { Write-Host "本来就没有装" }
    return
}

if (-not (Test-Path $exe)) { throw "找不到 $exe，先跑 pet-c\build.bat" }

$sh = New-Object -ComObject WScript.Shell
$sc = $sh.CreateShortcut($link)
$sc.TargetPath       = $exe
# --lock：和插件拉起来的那只共用单实例锁，不会出现两只
$sc.Arguments        = '--sheet "pets/deepseek-girl/spritesheet.png" --port 8789 --pos "pet-pos.txt" --lock "pet.lock"'
$sc.WorkingDirectory = $root
$sc.IconLocation     = "$exe,0"
$sc.Description      = "Magpie 桌面宠物"
$sc.Save()

if (Test-Path $link) {
    Write-Host "已加入开机启动: $link" -ForegroundColor Green
    Write-Host "注意：插件也会拉一只，两个同时启动时会靠 pet.lock 只留一个。" -ForegroundColor DarkGray
} else {
    throw "创建快捷方式失败"
}
