@echo off
rem 不装 magpie 时的独立启动脚本：直接拉起桌宠窗口
rem 需要先编译：pet-c\build.bat（或 make -C pet-c）
rem
rem --lock 与插件拉起的那只共用单实例锁，重复启动只会留一个
cd /d "%~dp0"

if not exist "pet-c\pet.exe" (
  echo pet-c\pet.exe not found. Build it first: pet-c\build.bat
  pause
  exit /b 1
)

start "" "%~dp0pet-c\pet.exe" --sheet "pets/deepseek-girl/spritesheet.png" --port 8789 --pos "pet-pos.txt" --lock "pet.lock"
