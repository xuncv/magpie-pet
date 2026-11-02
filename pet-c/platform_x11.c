/* platform_x11.c — Linux 后端（X11）
 *
 * 透明 + 置顶 + 不占任务栏 + 逐像素点击穿透：
 *   - 用 32 位 ARGB visual 创建窗口（需要合成器， GNOME/KDE 默认都有）
 *   - override_redirect = 跳过窗口管理器装饰
 *   - _NET_WM_STATE_ABOVE 置顶，_NET_WM_WINDOW_TYPE_DOCK 不进任务栏
 *   - XShapeCombineRectangles(XShapeInput) 只把 alpha>0 的像素留在输入区域里
 *     → 透明处点击直接落到下面的窗口
 *
 * 编译：gcc ... -lX11 -lXext
 *
 * 注意：这条路径在本机（Windows）无法测试，仅做静态检查。见 README 的「测试状态」。
 */

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/shape.h>
#include <X11/Xresource.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <errno.h>

#include "platform.h"

struct plat_window {
    Display *dpy;
    Window win;
    GC gc;
    Visual *visual;
    int depth;
    int w, h;
    uint32_t *argb;       /* 本窗口的影子缓冲（用于算输入区域） */

    int dragging, drag_started;
    int drag_enabled;
    int drag_origin_x, drag_origin_y;
    int win_origin_x, win_origin_y;
    int click_through_all;
    uint64_t drag_t0;

    plat_event evq[64];
    int evn;
};

static int x_error_handler(Display *d, XErrorEvent *e)
{
    (void)d; (void)e;
    return 0;   /* 忽略非致命的 X 错误（例如没有合成器） */
}

/* 找一个 32 位真彩 ARGB visual */
static Visual *find_argb_visual(Display *dpy, int *depth_out)
{
    XVisualInfo vinfo;
    if (XMatchVisualInfo(dpy, DefaultScreen(dpy), 32, TrueColor, &vinfo)) {
        *depth_out = 32;
        return vinfo.visual;
    }
    if (XMatchVisualInfo(dpy, DefaultScreen(dpy), 24, TrueColor, &vinfo)) {
        *depth_out = 24;
        return vinfo.visual;
    }
    *depth_out = DefaultDepth(dpy, DefaultScreen(dpy));
    return DefaultVisual(dpy, DefaultScreen(dpy));
}

static void set_window_type(Display *dpy, Window w)
{
    Atom type_dock = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    Atom type = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    XChangeProperty(dpy, w, type, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&type_dock, 1);

    Atom state_above = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);
    Atom state = XInternAtom(dpy, "_NET_WM_STATE", False);
    XChangeProperty(dpy, w, state, XA_ATOM, 32, PropModeReplace,
                    (unsigned char *)&state_above, 1);

    /* 不进任务栏 / 不抢焦点 */
    XWMHints hints;
    memset(&hints, 0, sizeof(hints));
    hints.flags = InputHint | StateHint;
    hints.input = False;
    hints.initial_state = NormalState;
    XSetWMHints(dpy, w, &hints);
}

plat_window *plat_create(const plat_config *cfg, const char *title)
{
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { fprintf(stderr, "pet: 无法连接 X 服务器\n"); return NULL; }
    XSetErrorHandler(x_error_handler);

    plat_window *w = (plat_window *)calloc(1, sizeof(plat_window));
    if (!w) { XCloseDisplay(dpy); return NULL; }
    w->dpy = dpy;
    w->w = cfg->width;
    w->h = cfg->height;

    int depth = 0;
    Visual *vis = find_argb_visual(dpy, &depth);
    w->visual = vis;
    w->depth = depth;

    Window root = DefaultRootWindow(dpy);
    XSetWindowAttributes attrs;
    memset(&attrs, 0, sizeof(attrs));
    attrs.background_pixmap = None;
    attrs.background_pixel = 0;
    attrs.border_pixel = 0;
    attrs.colormap = XCreateColormap(dpy, root, vis, AllocNone);
    attrs.override_redirect = True;   /* 无边框、不受 WM 摆放 */
    attrs.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask |
                       PointerMotionMask | StructureNotifyMask;

    int wx, wy, ww, wh;
    plat_workarea(&wx, &wy, &ww, &wh);

    w->win = XCreateWindow(dpy, root,
                           wx + ww - cfg->width - 24,
                           wy + wh - cfg->height - 8,
                           cfg->width, cfg->height,
                           0, depth, InputOutput, vis,
                           CWBackPixmap | CWBackPixel | CWBorderPixel | CWColormap |
                           CWOverrideRedirect | CWEventMask,
                           &attrs);
    if (!w->win) { free(w); XCloseDisplay(dpy); return NULL; }

    XStoreName(dpy, w->win, title ? title : "pet");
    set_window_type(dpy, w->win);

    w->gc = XCreateGC(dpy, w->win, 0, NULL);
    w->drag_enabled = 1;
    w->argb = (uint32_t *)calloc((size_t)cfg->width * cfg->height, 4);

    /* 让整个窗口一开始就穿透，等到第一帧再按 alpha 收紧 */
    XShapeCombineRectangles(dpy, w->win, ShapeInput, 0, 0, NULL, 0, ShapeSet, Unsorted);

    XMapRaised(dpy, w->win);
    XFlush(dpy);
    return w;
}

/* 按 alpha>0 的像素生成输入区域（合并成横向连续区间，避免塞几万个小矩形） */
static void update_input_shape(plat_window *w)
{
    if (!w || !w->argb) return;
    if (w->click_through_all) {
        XShapeCombineRectangles(w->dpy, w->win, ShapeInput, 0, 0, NULL, 0, ShapeSet, Unsorted);
        XFlush(w->dpy);
        return;
    }

    /* 每 4 行扫一次，够用且快 */
    int maxrects = 4096;
    XRectangle *rects = (XRectangle *)malloc(sizeof(XRectangle) * maxrects);
    if (!rects) return;
    int n = 0;

    for (int y = 0; y < w->h && n < maxrects - 1; y += 4) {
        int run_start = -1;
        for (int x = 0; x < w->w; x++) {
            uint32_t p = w->argb[(size_t)y * w->w + x];
            int solid = ((p >> 24) & 0xFF) > 24;
            if (solid && run_start < 0) run_start = x;
            else if (!solid && run_start >= 0) {
                rects[n].x = (short)run_start;
                rects[n].y = (short)y;
                rects[n].width = (unsigned short)(x - run_start);
                rects[n].height = 4;
                n++;
                run_start = -1;
                if (n >= maxrects - 1) break;
            }
        }
        if (run_start >= 0 && n < maxrects) {
            rects[n].x = (short)run_start;
            rects[n].y = (short)y;
            rects[n].width = (unsigned short)(w->w - run_start);
            rects[n].height = 4;
            n++;
        }
    }

    XShapeCombineRectangles(w->dpy, w->win, ShapeInput, 0, 0, rects, n, ShapeSet, Unsorted);
    XFlush(w->dpy);
    free(rects);
}

void plat_present(plat_window *w, const uint32_t *argb, int width, int height)
{
    if (!w) return;
    if (w->argb) memcpy(w->argb, argb, (size_t)width * height * 4);

    XImage *img = XCreateImage(w->dpy, w->visual, (unsigned)w->depth, ZPixmap, 0,
                               (char *)argb, (unsigned)width, (unsigned)height, 32, 0);
    if (img) {
        XPutImage(w->dpy, w->win, w->gc, img, 0, 0, 0, 0,
                  (unsigned)width, (unsigned)height);
        img->data = (char *)argb;   /* 不归 Xlib 释放 */
        XDestroyImage(img);
    }
    XFlush(w->dpy);
    update_input_shape(w);
}

int plat_poll(plat_window *w, plat_event *ev)
{
    if (!w) return 0;
    while (XPending(w->dpy)) {
        XEvent e;
        XNextEvent(w->dpy, &e);
        switch (e.type) {
        case ButtonPress:
            if (e.xbutton.button == Button1) {
                w->dragging = w->drag_enabled ? 1 : 0;
                w->drag_started = 0;
                w->drag_origin_x = e.xbutton.x_root;
                w->drag_origin_y = e.xbutton.y_root;
                w->win_origin_x = 0;
                w->win_origin_y = 0;
                plat_get_pos(w, &w->win_origin_x, &w->win_origin_y);
                if (w->evn < 63) { w->evq[w->evn].kind = PKE_LEFT_DOWN; w->evn++; }
            } else if (e.xbutton.button == Button3) {
                if (w->evn < 63) { w->evq[w->evn].kind = PKE_RIGHT_DOWN; w->evn++; }
            }
            break;
        case MotionNotify:
            if (w->dragging) {
                int dx = e.xmotion.x_root - w->drag_origin_x;
                int dy = e.xmotion.y_root - w->drag_origin_y;
                if (!w->drag_started && (dx * dx + dy * dy) > 36) {
                    w->drag_started = 1;
                    if (w->evn < 63) { w->evq[w->evn].kind = PKE_DRAG_START; w->evn++; }
                }
                if (w->drag_started) {
                    XMoveWindow(w->dpy, w->win, w->win_origin_x + dx, w->win_origin_y + dy);
                    XFlush(w->dpy);
                }
            }
            break;
        case ButtonRelease:
            if (e.xbutton.button == Button1 && w->dragging) {
                if (w->drag_started) {
                    if (w->evn < 63) { w->evq[w->evn].kind = PKE_DRAG_END; w->evn++; }
                } else {
                    if (w->evn < 63) { w->evq[w->evn].kind = PKE_LEFT_UP; w->evn++; }
                }
                w->dragging = 0;
                w->drag_started = 0;
            }
            break;
        case DestroyNotify:
            if (w->evn < 63) { w->evq[w->evn].kind = PKE_QUIT; w->evn++; }
            break;
        default:
            break;
        }
    }
    if (w->evn == 0) return 0;
    *ev = w->evq[0];
    for (int i = 1; i < w->evn; i++) w->evq[i - 1] = w->evq[i];
    w->evn--;
    return 1;
}

void plat_move(plat_window *w, int x, int y)
{
    if (!w) return;
    XMoveWindow(w->dpy, w->win, x, y);
    XFlush(w->dpy);
}

void plat_get_pos(plat_window *w, int *x, int *y)
{
    if (!w) return;
    Window child;
    int rx = 0, ry = 0;
    XTranslateCoordinates(w->dpy, w->win, DefaultRootWindow(w->dpy), 0, 0, &rx, &ry, &child);
    *x = rx;
    *y = ry;
}

void plat_cursor(int *x, int *y)
{
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { *x = *y = 0; return; }
    Window root = DefaultRootWindow(dpy), ret_root, child;
    int wx, wy, rx, ry;
    unsigned int mask;
    XQueryPointer(dpy, root, &ret_root, &child, &rx, &ry, &wx, &wy, &mask);
    *x = rx;
    *y = ry;
    XCloseDisplay(dpy);
}

void plat_workarea(int *x, int *y, int *w, int *h)
{
    Display *dpy = XOpenDisplay(NULL);
    if (!dpy) { *x = 0; *y = 0; *w = 1920; *h = 1080; return; }

    Atom prop = XInternAtom(dpy, "_NET_WORKAREA", True);
    Atom actual;
    int fmt;
    unsigned long n = 0, left = 0;
    unsigned char *data = NULL;
    int ok = 0;
    if (prop != None &&
        XGetWindowProperty(dpy, DefaultRootWindow(dpy), prop, 0, 4, False,
                           XA_CARDINAL, &actual, &fmt, &n, &left, &data) == Success && data) {
        long *v = (long *)data;
        *x = (int)v[0];
        *y = (int)v[1];
        *w = (int)v[2];
        *h = (int)v[3];
        XFree(data);
        ok = 1;
    }
    if (!ok) {
        int scr = DefaultScreen(dpy);
        *x = 0;
        *y = 0;
        *w = DisplayWidth(dpy, scr);
        *h = DisplayHeight(dpy, scr);
    }
    XCloseDisplay(dpy);
}

void plat_sleep(int ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

uint64_t plat_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)(ts.tv_nsec / 1000000);
}

void plat_set_click_through_all(plat_window *w, int on)
{
    if (!w) return;
    w->click_through_all = on;
    update_input_shape(w);
}

void plat_set_drag_enabled(plat_window *w, int on)
{
    if (!w) return;
    w->drag_enabled = on;
    if (!on) { w->dragging = 0; w->drag_started = 0; }
}

void plat_resize(plat_window *w, int width, int height)
{
    if (!w) return;
    w->w = width;
    w->h = height;
    uint32_t *n = (uint32_t *)realloc(w->argb, (size_t)width * height * 4);
    if (n) w->argb = n;
    XResizeWindow(w->dpy, w->win, (unsigned)width, (unsigned)height);
    XFlush(w->dpy);
}

const char *plat_hotkey_name(plat_window *w)
{
    (void)w;
    return "";   /* X11/macOS 版本暂未提供全局热键，靠 30 秒自动恢复 */
}

void plat_set_topmost(plat_window *w, int on)
{
    if (!w) return;
    Atom state_above = XInternAtom(w->dpy, "_NET_WM_STATE_ABOVE", False);
    Atom state = XInternAtom(w->dpy, "_NET_WM_STATE", False);
    XEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.xclient.type = ClientMessage;
    ev.xclient.window = w->win;
    ev.xclient.message_type = state;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = on ? 1 : 0;   /* _NET_WM_STATE_ADD / REMOVE */
    ev.xclient.data.l[1] = (long)state_above;
    ev.xclient.data.l[3] = 2;            /* source: pager */
    XSendEvent(w->dpy, DefaultRootWindow(w->dpy), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &ev);
    if (!on) XLowerWindow(w->dpy, w->win);
    XRaiseWindow(w->dpy, w->win);
    XFlush(w->dpy);
}

/* X11 上没接字体栈（不引入 freetype 依赖），气泡只画底色不画字。
 * 想要中文气泡可以在编译时加 -DHAVE_XFT 并实现这里。 */
int plat_text_bitmap(const char *utf8, int px, uint32_t color, int maxw,
                     uint32_t **out, int *out_w, int *out_h)
{
    (void)utf8; (void)px; (void)color; (void)maxw;
    (void)out; (void)out_w; (void)out_h;
    return 1;
}

void plat_destroy(plat_window *w)
{
    if (!w) return;
    if (w->gc) XFreeGC(w->dpy, w->gc);
    if (w->win) XDestroyWindow(w->dpy, w->win);
    XCloseDisplay(w->dpy);
    free(w->argb);
    free(w);
}

/* ---------------- 进程身份（单实例锁 / 父进程看门狗） ---------------- */

int plat_self_pid(void)
{
    return (int)getpid();
}

int plat_pid_alive(int pid)
{
    if (pid <= 0) return 0;
    if (kill((pid_t)pid, 0) == 0) return 1;
    return errno == EPERM;                  /* 存在但没权限 */
}

int plat_create_exclusive(const char *path)
{
    int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
    if (fd < 0) return 0;
    close(fd);
    return 1;
}

void plat_install_crash_handler(void) { }

void plat_error_box(const char *title, const char *msg)
{
    fprintf(stderr, "%s: %s\n", title ? title : "pet", msg ? msg : "");
}

void plat_present_rect(plat_window *w, const uint32_t *argb, int width, int height,
                       int l, int t, int r, int b)
{
    (void)l; (void)t; (void)r; (void)b;
    plat_present(w, argb, width, height);
}
