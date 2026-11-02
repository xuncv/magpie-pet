/* platform_cocoa.m — macOS 后端
 *
 * 透明 + 置顶 + 不占 Dock + 逐像素点击穿透：
 *   - borderless NSWindow + opaque=NO + backgroundColor=[NSColor clearColor]
 *   - window.level = NSFloatingWindowLevel，collectionBehavior 让它留在所有空间
 *   - 一个自绘 NSView，把 RGBA 缓冲画成 NSImage
 *   - 命中测试重写 hitTest:，返回 nil 让点击穿透（配合 NSView 的 alpha 判定）
 *   - 文字用 CoreText/NSString drawAtPoint（完整中文支持）
 *
 * 编译：clang ... -framework Cocoa -framework CoreText
 *
 * 注意：这条路径在本机（Windows）无法测试，仅做静态检查。见 README 的「测试状态」。
 */

#import <Cocoa/Cocoa.h>
#import <CoreText/CoreText.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <mach/mach_time.h>

#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

#include "platform.h"

/* ---------- 视图：画 RGBA，命中按 alpha ---------- */
@interface PetView : NSView
@property (nonatomic, assign) uint8_t *rgba;      /* 0xAARRGGBB 的 32 位数组 */
@property (nonatomic, assign) int bufW;
@property (nonatomic, assign) int bufH;
@property (nonatomic, assign) BOOL clickThroughAll;
@end

@implementation PetView

- (BOOL)isOpaque { return NO; }

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    if (!self.rgba) return;

    /* 把 0xAARRGGBB 转成 预乘 BGRA（Cocoa 的 bitmapFormat） */
    size_t n = (size_t)self.bufW * self.bufH;
    uint8_t *tmp = (uint8_t *)malloc(n * 4);
    if (!tmp) return;
    for (size_t i = 0; i < n; i++) {
        uint32_t p = self.rgba[i];
        uint32_t a = (p >> 24) & 0xFF;
        uint32_t r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
        if (a != 255) { r = r * a / 255; g = g * a / 255; b = b * a / 255; }
        tmp[i * 4 + 0] = (uint8_t)b;
        tmp[i * 4 + 1] = (uint8_t)g;
        tmp[i * 4 + 2] = (uint8_t)r;
        tmp[i * 4 + 3] = (uint8_t)a;
    }

    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(tmp, (size_t)self.bufW, (size_t)self.bufH, 8,
                                             (size_t)self.bufW * 4, cs,
                                             kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    if (ctx) {
        CGImageRef img = CGBitmapContextCreateImage(ctx);
        if (img) {
            CGContextRef dst = [[NSGraphicsContext currentContext] CGContext];
            if (dst) {
                CGContextSaveGState(dst);
                CGContextSetInterpolationQuality(dst, kCGInterpolationHigh);
                CGContextDrawImage(dst, self.bounds, img);
                CGContextRestoreGState(dst);
            }
            CGImageRelease(img);
        }
        CGContextRelease(ctx);
    }
    CGColorSpaceRelease(cs);
    free(tmp);
}

/* 命中测试：透明处返回 nil（穿透到下面的窗口） */
- (NSView *)hitTest:(NSPoint)point {
    if (self.clickThroughAll) return nil;
    if (!self.rgba) return nil;
    NSPoint p = [self convertPoint:point fromView:self.superview];
    int x = (int)p.x;
    /* Cocoa 原点在左下，缓冲原点在左上 */
    int y = self.bufH - 1 - (int)p.y;
    if (x < 0 || x >= self.bufW || y < 0 || y >= self.bufH) return nil;
    uint32_t px = self.rgba[(size_t)y * self.bufW + x];
    return (((px >> 24) & 0xFF) > 24) ? [super hitTest:point] : nil;
}

@end

/* ---------- 窗口 ---------- */
struct plat_window {
    __unsafe_unretained NSWindow *win;
    __unsafe_unretained PetView *view;
    int w, h;
    uint32_t *argb;

    int dragging, drag_started;
    int drag_enabled;
    int drag_origin_x, drag_origin_y;
    int win_origin_x, win_origin_y;
    int click_through_all;

    plat_event evq[64];
    int evn;
};

/* 用全局变量把事件从 Cocoa 回调带回主循环（单窗口，够用） */
static plat_window *g_pw = NULL;

@interface PetController : NSObject
@end

@implementation PetController
- (void)rightMouseDown:(NSEvent *)e { (void)e; }
@end

static void push_event(plat_window *w, plat_event_kind k, int x, int y)
{
    if (!w || w->evn >= 63) return;
    w->evq[w->evn].kind = k;
    w->evq[w->evn].x = x;
    w->evq[w->evn].y = y;
    w->evn++;
}

plat_window *plat_create(const plat_config *cfg, const char *title)
{
    @autoreleasepool {
        if (![NSApplication sharedApplication]) return NULL;
        [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];

        plat_window *w = (plat_window *)calloc(1, sizeof(plat_window));
        if (!w) return NULL;
        w->w = cfg->width;
        w->h = cfg->height;
        w->argb = (uint32_t *)calloc((size_t)cfg->width * cfg->height, 4);

        int wx, wy, ww, wh;
        plat_workarea(&wx, &wy, &ww, &wh);
        /* Cocoa 原点在左下 */
        NSRect frame = NSMakeRect(wx + ww - cfg->width - 24,
                                  wy + 8,
                                  cfg->width, cfg->height);

        NSWindow *win = [[NSWindow alloc] initWithContentRect:frame
                                                    styleMask:NSWindowStyleMaskBorderless
                                                      backing:NSBackingStoreBuffered
                                                        defer:NO];
        [win setOpaque:NO];
        [win setBackgroundColor:[NSColor clearColor]];
        [win setHasShadow:NO];
        [win setLevel:NSFloatingWindowLevel];
        [win setIgnoresMouseEvents:NO];
        [win setReleasedWhenClosed:NO];
        [win setCollectionBehavior:NSWindowCollectionBehaviorCanJoinAllSpaces |
                                   NSWindowCollectionBehaviorStationary |
                                   NSWindowCollectionBehaviorIgnoresCycle];
        if (title) [win setTitle:[NSString stringWithUTF8String:title]];

        PetView *view = [[PetView alloc] initWithFrame:NSMakeRect(0, 0, cfg->width, cfg->height)];
        view.bufW = cfg->width;
        view.bufH = cfg->height;
        view.rgba = (uint8_t *)w->argb;
        view.clickThroughAll = NO;
        [win setContentView:view];
        [win makeKeyAndOrderFront:nil];
        [win orderFrontRegardless];

        w->win = win;
        w->view = view;
        w->drag_enabled = 1;
        g_pw = w;
        return w;
    }
}

void plat_present(plat_window *w, const uint32_t *argb, int width, int height)
{
    if (!w) return;
    memcpy(w->argb, argb, (size_t)width * height * 4);
    [w->view setNeedsDisplay:YES];
    /* 让 runloop 转一下，绘制和事件才会发生 */
    @autoreleasepool {
        NSEvent *e = nil;
        while ((e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                       untilDate:[NSDate distantPast]
                                          inMode:NSDefaultRunLoopMode
                                         dequeue:YES])) {
            NSPoint loc = [e locationInWindow];
            switch ([e type]) {
            case NSEventTypeLeftMouseDown:
                w->dragging = w->drag_enabled ? 1 : 0;
                w->drag_started = 0;
                w->drag_origin_x = (int)loc.x;
                w->drag_origin_y = (int)loc.y;
                plat_get_pos(w, &w->win_origin_x, &w->win_origin_y);
                push_event(w, PKE_LEFT_DOWN, (int)loc.x, (int)loc.y);
                break;
            case NSEventTypeLeftMouseDragged: {
                if (!w->dragging) break;
                int dx = (int)loc.x - w->drag_origin_x;
                int dy = (int)loc.y - w->drag_origin_y;
                if (!w->drag_started && (dx * dx + dy * dy) > 36) {
                    w->drag_started = 1;
                    push_event(w, PKE_DRAG_START, 0, 0);
                }
                if (w->drag_started) {
                    /* Cocoa 的 y 轴向上，位移符号反过来 */
                    plat_move(w, w->win_origin_x + dx, w->win_origin_y + dy);
                }
                break;
            }
            case NSEventTypeLeftMouseUp:
                if (w->drag_started) push_event(w, PKE_DRAG_END, 0, 0);
                else push_event(w, PKE_LEFT_UP, (int)loc.x, (int)loc.y);
                w->dragging = 0;
                w->drag_started = 0;
                break;
            case NSEventTypeRightMouseUp:
                push_event(w, PKE_RIGHT_DOWN, (int)loc.x, (int)loc.y);
                break;
            default:
                break;
            }
            [NSApp sendEvent:e];
        }
    }
}

int plat_poll(plat_window *w, plat_event *ev)
{
    if (!w || w->evn == 0) return 0;
    *ev = w->evq[0];
    for (int i = 1; i < w->evn; i++) w->evq[i - 1] = w->evq[i];
    w->evn--;
    return 1;
}

void plat_move(plat_window *w, int x, int y)
{
    if (!w) return;
    @autoreleasepool {
        NSScreen *scr = [NSScreen mainScreen];
        CGFloat sh = scr ? scr.frame.size.height : 1080;
        /* 传入的是「屏幕坐标（左上原点）」，转成 Cocoa（左下原点） */
        NSRect f = [w->win frame];
        f.origin.x = x;
        f.origin.y = sh - y - f.size.height;
        [w->win setFrame:f display:YES];
    }
}

void plat_get_pos(plat_window *w, int *x, int *y)
{
    if (!w) return;
    @autoreleasepool {
        NSRect f = [w->win frame];
        NSScreen *scr = [NSScreen mainScreen];
        CGFloat sh = scr ? scr.frame.size.height : 1080;
        *x = (int)f.origin.x;
        *y = (int)(sh - f.origin.y - f.size.height);
    }
}

void plat_cursor(int *x, int *y)
{
    @autoreleasepool {
        NSPoint p = [NSEvent mouseLocation];   /* 左下原点 */
        NSScreen *scr = [NSScreen mainScreen];
        CGFloat sh = scr ? scr.frame.size.height : 1080;
        *x = (int)p.x;
        *y = (int)(sh - p.y);
    }
}

void plat_workarea(int *x, int *y, int *w, int *h)
{
    @autoreleasepool {
        NSScreen *scr = [NSScreen mainScreen];
        NSRect vis = scr ? [scr visibleFrame] : NSMakeRect(0, 0, 1920, 1080);
        NSRect full = scr ? [scr frame] : vis;
        *x = (int)vis.origin.x;
        *y = (int)(full.size.height - vis.origin.y - vis.size.height);
        *w = (int)vis.size.width;
        *h = (int)vis.size.height;
    }
}

void plat_sleep(int ms)
{
    [NSThread sleepForTimeInterval:ms / 1000.0];
}

uint64_t plat_ms(void)
{
    static mach_timebase_info_data_t tb = {0, 0};
    if (tb.denom == 0) mach_timebase_info(&tb);
    uint64_t t = mach_absolute_time();
    return t * tb.numer / tb.denom / 1000000;
}

void plat_set_click_through_all(plat_window *w, int on)
{
    if (!w) return;
    w->click_through_all = on;
    w->view.clickThroughAll = on ? YES : NO;
    [w->win setIgnoresMouseEvents:on ? YES : NO];
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
    w->view.bufW = width;
    w->view.bufH = height;
    w->view.rgba = (uint8_t *)w->argb;
    @autoreleasepool {
        NSRect f = [w->win frame];
        NSScreen *scr = [NSScreen mainScreen];
        CGFloat sh = scr ? scr.frame.size.height : 1080;
        /* 保持左下角不动，向上/右生长 */
        f.size.width = width;
        f.size.height = height;
        f.origin.y = sh - (sh - f.origin.y - f.size.height) - height;
        [w->win setFrame:f display:YES];
        [w->view setFrame:NSMakeRect(0, 0, width, height)];
    }
}

const char *plat_hotkey_name(plat_window *w)
{
    (void)w;
    return "";   /* macOS 版本暂未提供全局热键，靠 30 秒自动恢复 */
}

void plat_set_topmost(plat_window *w, int on)
{
    if (!w) return;
    [w->win setLevel:on ? NSFloatingWindowLevel : NSNormalWindowLevel];
    if (on) [w->win orderFrontRegardless];
}

/* 文字：用 AppKit 画到离屏位图，再读回 ARGB（完整支持中文） */
int plat_text_bitmap(const char *utf8, int px, uint32_t color, int maxw,
                     uint32_t **out, int *out_w, int *out_h)
{
    @autoreleasepool {
        if (!utf8 || !utf8[0]) return 1;
        uint32_t cr = (color >> 16) & 0xFF;
        uint32_t cg = (color >> 8) & 0xFF;
        uint32_t cb = color & 0xFF;

        NSFont *font = [NSFont systemFontOfSize:px];
        NSDictionary *attrs = @{
            NSFontAttributeName: font,
            NSForegroundColorAttributeName: [NSColor whiteColor]
        };
        NSString *s = [NSString stringWithUTF8String:utf8];
        if (!s) return 1;

        NSRect bounds = [s boundingRectWithSize:NSMakeSize(maxw, 1000)
                                        options:NSStringDrawingUsesLineFragmentOrigin
                                     attributes:attrs];
        int tw = (int)ceil(bounds.size.width);
        int th = (int)ceil(bounds.size.height);
        if (tw < 1 || th < 1) return 1;
        if (tw > maxw) tw = maxw;

        NSBitmapImageRep *rep = [[NSBitmapImageRep alloc]
            initWithBitmapDataPlanes:NULL
                          pixelsWide:tw
                          pixelsHigh:th
                       bitsPerSample:8
                     samplesPerPixel:4
                            hasAlpha:YES
                            isPlanar:NO
                      colorSpaceName:NSDeviceRGBColorSpace
                         bytesPerRow:tw * 4
                        bitsPerPixel:32];
        if (!rep) return 1;

        NSGraphicsContext *g = [NSGraphicsContext graphicsContextWithBitmapImageRep:rep];
        [NSGraphicsContext saveGraphicsState];
        [NSGraphicsContext setCurrentContext:g];
        [[NSColor blackColor] setFill];
        NSRectFill(NSMakeRect(0, 0, tw, th));
        [s drawInRect:NSMakeRect(0, 0, tw, th) withAttributes:attrs];
        [NSGraphicsContext restoreGraphicsState];

        unsigned char *src = [rep bitmapData];
        int stride = (int)[rep bytesPerRow];
        uint32_t *dst = (uint32_t *)malloc((size_t)tw * th * 4);
        if (!dst) return 1;
        for (int y = 0; y < th; y++) {
            for (int x = 0; x < tw; x++) {
                /* 白字黑底 → 亮度当 alpha */
                unsigned char *p = src + y * stride + x * 4;
                unsigned v = p[0];
                if (p[1] > v) v = p[1];
                if (p[2] > v) v = p[2];
                dst[(size_t)y * tw + x] = v ? (((uint32_t)v << 24) | (cr << 16) | (cg << 8) | cb) : 0;
            }
        }
        *out = dst;
        *out_w = tw;
        *out_h = th;
        return 0;
    }
}

void plat_destroy(plat_window *w)
{
    if (!w) return;
    @autoreleasepool {
        if (w->win) [w->win close];
        free(w->argb);
        free(w);
    }
    g_pw = NULL;
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
    return errno == EPERM;
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
    @autoreleasepool {
        NSAlert *a = [[NSAlert alloc] init];
        a.messageText = [NSString stringWithUTF8String:(title ? title : "pet")];
        a.informativeText = [NSString stringWithUTF8String:(msg ? msg : "")];
        a.alertStyle = NSAlertStyleCritical;
        [a runModal];
    }
}

void plat_present_rect(plat_window *w, const uint32_t *argb, int width, int height,
                       int l, int t, int r, int b)
{
    (void)l; (void)t; (void)r; (void)b;
    plat_present(w, argb, width, height);
}
