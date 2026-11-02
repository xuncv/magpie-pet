# 跨平台桌宠 —— Makefile
#
#   make            # 按当前系统自动选后端
#   make windows / linux / macos
#   make clean
#
# Windows 用 MinGW gcc（本机实测：gcc 13.2.0）
# Linux 需要 libX11 + libXext 开发包
# macOS 需要 Xcode 命令行工具

CC      ?= gcc
CFLAGS  ?= -O2 -std=gnu11 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
LDFLAGS ?=

TARGET  = pet
SRC     = pet.c

UNAME_S := $(shell uname -s 2>/dev/null || echo Windows)

ifeq ($(OS),Windows_NT)
  PLAT_SRC = platform_win32.c
  PLAT_LIB = -lgdi32 -luser32 -lws2_32 -mwindows
  TARGET   = pet.exe
else ifeq ($(UNAME_S),Darwin)
  PLAT_SRC = platform_cocoa.m
  PLAT_LIB = -framework Cocoa -framework CoreText
  OBJCFLAGS = -fobjc-arc
else
  PLAT_SRC = platform_x11.c
  PLAT_LIB = -lX11 -lXext
endif

all: $(TARGET)

$(TARGET): $(SRC) $(PLAT_SRC) platform.h http_get.h
	$(CC) $(CFLAGS) $(OBJCFLAGS) -o $(TARGET) $(SRC) $(PLAT_SRC) $(PLAT_LIB) $(LDFLAGS) -lm

windows:
	$(CC) $(CFLAGS) -mwindows -o pet.exe pet.c platform_win32.c -lgdi32 -luser32 -lws2_32 -lm

linux:
	$(CC) $(CFLAGS) -o pet pet.c platform_x11.c -lX11 -lXext -lm

macos:
	clang $(CFLAGS) -fobjc-arc -o pet pet.c platform_cocoa.m -framework Cocoa -framework CoreText -lm

clean:
	rm -f pet pet.exe test_http test_http2

.PHONY: all windows linux macos clean
