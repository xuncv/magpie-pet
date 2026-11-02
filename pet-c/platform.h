/* platform.h — 跨平台桌宠的窗口后端抽象
 *
 * 三个实现：
 *   platform_win32.c   Windows  (UpdateLayeredWindow，逐像素 alpha + 自动穿透)
 *   platform_x11.c     Linux    (ARGB visual + XShape 输入区域)
 *   platform_cocoa.m   macOS    (borderless NSWindow + clear background)
 *
 * 约定：RGBA 缓冲区是 0xAARRGGBB 的小端 32 位像素（premultiplied 由后端自己处理）。
 */
#ifndef PET_PLATFORM_H
#define PET_PLATFORM_H

#include <stdint.h>

typedef struct {
    int width;
    int height;
    int title;     /* "pet" */
} plat_config;

typedef enum {
    PKE_NONE = 0,
    PKE_LEFT_DOWN,
    PKE_LEFT_UP,
    PKE_MOVE,       /* 拖动中 */
    PKE_DRAG_START, /* 越过阈值，开始拖 */
    PKE_DRAG_END,
    PKE_RIGHT_DOWN, /* 右键：弹菜单 */
    PKE_TOGGLE_CLICKTHRU, /* 热键：开关点击穿透 */
    PKE_QUIT
} plat_event_kind;

typedef struct {
    plat_event_kind kind;
    int x, y;       /* 相对窗口的坐标 */
} plat_event;

typedef struct plat_window plat_window;

/* 创建窗口（透明、置顶、不占任务栏）。失败返回 NULL。 */
plat_window *plat_create(const plat_config *cfg, const char *title);

/* 用一帧 RGBA（0xAARRGGBB，非预乘）刷新窗口。 */
void plat_present(plat_window *w, const uint32_t *argb, int width, int height);

/* 只更新一块区域（局部内容变了、其余没变时用，省一次全屏转换+上传）。
 * r<=l 或 b<=t 表示全量。 */
void plat_present_rect(plat_window *w, const uint32_t *argb, int width, int height,
                       int l, int t, int r, int b);

/* 取事件。返回 0 表示没有更多事件。 */
int plat_poll(plat_window *w, plat_event *ev);

/* 移动窗口（拖动）。 */
void plat_move(plat_window *w, int x, int y);
void plat_get_pos(plat_window *w, int *x, int *y);

/* 屏幕光标坐标（全局）。 */
void plat_cursor(int *x, int *y);

/* 屏幕可视区域（去掉任务栏/菜单栏）。 */
void plat_workarea(int *x, int *y, int *w, int *h);

/* 睡眠毫秒。 */
void plat_sleep(int ms);

/* 单调毫秒时钟。 */
uint64_t plat_ms(void);

/* 可选：把命中区域限制为 alpha>0 的像素（Windows 自动完成，X11 用 XShape）。 */
void plat_set_click_through_all(plat_window *w, int on);

void plat_destroy(plat_window *w);

/* 可选：把 UTF-8 文本渲染成 ARGB 位图（0xAARRGGBB），用作气泡/列表文字。
 * color: 文字颜色（0xAARRGGBB），alpha 分量忽略。
 * 返回 0 成功；非 0 表示不支持（核心会跳过文字）。*out 用 free() 释放。*/
int plat_text_bitmap(const char *utf8, int px, uint32_t color, int maxw,
                     uint32_t **out, int *out_w, int *out_h);

/* 置顶开关（菜单里的「置顶」） */
void plat_set_topmost(plat_window *w, int on);

/* 菜单打开时禁止拖动，否则点菜单项时手一拖就把宠物拖走了 */
void plat_set_drag_enabled(plat_window *w, int on);

/* 改变窗口尺寸（列表展开/收起时用） */
void plat_resize(plat_window *w, int width, int height);

/* 实际注册成功的逃生热键名字（没有则空串） */
const char *plat_hotkey_name(plat_window *w);

/* 当前进程 pid */
int plat_self_pid(void);

/* pid 对应的进程是否还活着（权限不足也算活） */
int plat_pid_alive(int pid);

/* 独占创建文件：成功返回 1，已存在返回 0。
 * （不用 fopen("wx")，MinGW 的 MSVCRT 不支持这个 mode） */
int plat_create_exclusive(const char *path);

/* 安装崩溃处理器（有实现的平台会写 pet-crash.txt） */
void plat_install_crash_handler(void);

/* 弹一个错误框（图集加载失败等致命错误用；不支持就只打日志） */
void plat_error_box(const char *title, const char *msg);

#endif /* PET_PLATFORM_H */