@echo off
rem Windows build (MinGW gcc). Output: pet.exe
setlocal
cd /d "%~dp0"

set CFLAGS=-O2 -std=c11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Wno-unknown-pragmas
set LIBS=-lgdi32 -luser32 -lws2_32

gcc %CFLAGS% -mwindows -o pet.exe pet.c platform_win32.c %LIBS% -lm
if errorlevel 1 (
  echo.
  echo BUILD FAILED
  exit /b 1
)
echo.
echo OK -^> pet.exe
for %%F in (pet.exe) do echo    size: %%~zF bytes
