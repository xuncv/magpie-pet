/* platform_win32.c — Win32 后端
 *
 * 透明 + 置顶 + 不占任务栏 + 逐像素点击穿透：
 *   WS_EX_LAYERED + UpdateLayeredWindow(ULW_ALPHA) + AC_SRC_ALPHA
 *   用 UpdateLayeredWindow 的 layered 窗口，Windows 自带按 alpha 的逐像素命中测试，
 *   完全透明的像素会把鼠标事件透传给下面的窗口 —— 不用自己算区域。
 *
 * 注意：UpdateLayeredWindow 要求 **预乘 alpha** 的 32 位 DIB，所以 present 里做乘法。
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>

#include "platform.h"

struct plat_window {
    HWND hwnd;
    HDC memdc;
    HBITMAP dib;
    void *bits;
    int w, h;

    int dragging;
    int drag_started;
    int drag_enabled;
    const char *hotkey_name;
    POINT drag_origin;   /* 光标起点（屏幕坐标） */
    int win_origin_x, win_origin_y;
    int click_through_all;

    plat_event evq[64];
    int evn;
};

static const char *kClass = "MagpiePetC";

static LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    plat_window *w = (plat_window *)GetWindowLongPtr(h, GWLP_USERDATA);

    switch (msg) {
    /* 不要处理 WM_NCHITTEST！
     * 返回 HTTRANSPARENT 会让整个窗口变成"整块穿透"，鼠标消息根本收不到，自然拖不动。
     * Layered 窗口（UpdateLayeredWindow）的系统默认行为已经是按 alpha 逐像素命中：
     * 完全透明的像素自动透传，不透明的像素正常收消息。交回 DefWindowProc 即可。 */

    case WM_LBUTTONDOWN:
        if (w) {
            w->dragging = w->drag_enabled ? 1 : 0;
            w->drag_started = 0;
            GetCursorPos(&w->drag_origin);
            plat_get_pos(w, &w->win_origin_x, &w->win_origin_y);
            SetCapture(h);
            w->evq[w->evn].kind = PKE_LEFT_DOWN;
            w->evq[w->evn].x = LOWORD(lp);
            w->evq[w->evn].y = HIWORD(lp);
            w->evn++;
        }
        return 0;

    case WM_MOUSEMOVE:
        if (w && w->dragging) {
            /* 防幽灵拖动：万一漏了 WM_LBUTTONUP（丢失捕获、其它程序抢了事件），
             * 这里用真实键位状态自校正，不让宠物一直跟着光标跑 */
            if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {
                w->dragging = 0;
                w->drag_started = 0;
                ReleaseCapture();
                return 0;
            }
            POINT c;
            GetCursorPos(&c);
            int dx = c.x - w->drag_origin.x;
            int dy = c.y - w->drag_origin.y;
            if (!w->drag_started && (dx * dx + dy * dy) > 36) {
                w->drag_started = 1;
                if (w->evn < 63) { w->evq[w->evn].kind = PKE_DRAG_START; w->evn++; }
            }
            if (w->drag_started) {
                SetWindowPos(h, NULL, w->win_origin_x + dx, w->win_origin_y + dy,
                             w->w, w->h, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            }
        }
        return 0;

    case WM_LBUTTONUP:
        if (w) {
            if (w->dragging && w->drag_started) {
                if (w->evn < 63) { w->evq[w->evn].kind = PKE_DRAG_END; w->evn++; }
            } else {
                if (w->evn < 63) {
                    w->evq[w->evn].kind = PKE_LEFT_UP;
                    w->evq[w->evn].x = LOWORD(lp);
                    w->evq[w->evn].y = HIWORD(lp);
                    w->evn++;
                }
            }
            w->dragging = 0;
            w->drag_started = 0;
            ReleaseCapture();
        }
        return 0;

    case WM_RBUTTONUP:
        if (w) {
            if (w->evn < 63) { w->evq[w->evn].kind = PKE_RIGHT_DOWN; w->evn++; }
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

plat_window *plat_create(const plat_config *cfg, const char *title)
{
    plat_window *w = (plat_window *)calloc(1, sizeof(plat_window));
    if (!w) return NULL;
    w->w = cfg->width;
    w->h = cfg->height;

    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassA(&wc);

    /* WS_POPUP：无边框。扩展样式：分层 + 置顶 + 不激活 + 不进任务栏 */
    HWND h = CreateWindowExA(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        kClass, title ? title : "pet",
        WS_POPUP,
        0, 0, cfg->width, cfg->height,
        NULL, NULL, GetModuleHandle(NULL), NULL);
    if (!h) { free(w); return NULL; }

    SetWindowLongPtr(h, GWLP_USERDATA, (LONG_PTR)w);
    w->hwnd = h;
    w->drag_enabled = 1;

    HDC screen = GetDC(NULL);
    w->memdc = CreateCompatibleDC(screen);
    ReleaseDC(NULL, screen);

    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = cfg->width;
    bi.bmiHeader.biHeight = -cfg->height;   /* 负数 = top-down */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    w->dib = CreateDIBSection(w->memdc, &bi, DIB_RGB_COLORS, &w->bits, NULL, 0);
    SelectObject(w->memdc, w->dib);

    /* 默认位置：右下角 */
    int wa_x, wa_y, wa_w, wa_h;
    plat_workarea(&wa_x, &wa_y, &wa_w, &wa_h);
    SetWindowPos(h, HWND_TOPMOST,
                 wa_x + wa_w - cfg->width - 24,
                 wa_y + wa_h - cfg->height - 8,
                 cfg->width, cfg->height,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);

    /* 逃生热键：开了点击穿透后窗口收不到鼠标，靠它恢复。
     * Ctrl+Alt+P 常被浏览器/PowerToys 占掉，所以按顺序尝试，记下真正成功那个。 */
    {
        static const struct { UINT mod; UINT vk; const char *name; } combos[] = {
            { MOD_CONTROL | MOD_ALT | MOD_SHIFT, 'P', "Ctrl+Alt+Shift+P" },
            { MOD_CONTROL | MOD_ALT, VK_F10, "Ctrl+Alt+F10" },
            { MOD_CONTROL | MOD_ALT, VK_F9,  "Ctrl+Alt+F9"  },
            { MOD_CONTROL | MOD_SHIFT, VK_F12, "Ctrl+Shift+F12" },
        };
        for (unsigned i = 0; i < sizeof(combos) / sizeof(combos[0]); i++) {
            if (RegisterHotKey(h, 1, combos[i].mod, combos[i].vk)) {
                w->hotkey_name = combos[i].name;
                break;
            }
        }
        if (!w->hotkey_name) w->hotkey_name = "";
    }
    return w;
}

/* dirty 为 NULL 或空矩形 = 全量更新 */
static void present_impl(plat_window *w, const uint32_t *argb, int width, int height,
                         int dl, int dt, int dr, int db)
{
    if (!w || !w->bits) return;
    if (width != w->w || height != w->h) return;

    /* 转成预乘 alpha 的 BGRX */
    uint32_t *dst = (uint32_t *)w->bits;
    /* 只转换脏矩形内的像素；其余区域上次已经上传过，保持原样即可 */
    int dx = 0, dy = 0, dw = width, dh = height;
    if (dr > dl && db > dt) {
        dx = dl; dy = dt;
        dw = dr - dl;
        dh = db - dt;
        if (dx < 0) { dw += dx; dx = 0; }
        if (dy < 0) { dh += dy; dy = 0; }
        if (dx + dw > width) dw = width - dx;
        if (dy + dh > height) dh = height - dy;
        if (dw <= 0 || dh <= 0) { dx = 0; dy = 0; dw = width; dh = height; }
    }

    for (int y = dy; y < dy + dh; y++) {
        const uint32_t *src_row = argb + (size_t)y * width;
        uint32_t *dst_row = dst + (size_t)y * width;
        for (int x = dx; x < dx + dw; x++) {
            uint32_t p = src_row[x];
            uint32_t a = (p >> 24) & 0xFF;
            if (a == 0) { dst_row[x] = 0; continue; }
            uint32_t r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
            if (a != 255) {
                r = r * a / 255;
                g = g * a / 255;
                b = b * a / 255;
            }
            dst_row[x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }

    HDC screen = GetDC(NULL);
    POINT pt_src = {0, 0};
    SIZE size = {width, height};
    BLENDFUNCTION bf;
    bf.BlendOp = AC_SRC_OVER;
    bf.BlendFlags = 0;
    bf.SourceConstantAlpha = 255;
    bf.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(w->hwnd, screen, NULL, &size, w->memdc, &pt_src, 0, &bf, ULW_ALPHA);
    ReleaseDC(NULL, screen);
}

void plat_present(plat_window *w, const uint32_t *argb, int width, int height)
{
    present_impl(w, argb, width, height, 0, 0, 0, 0);
}

void plat_present_rect(plat_window *w, const uint32_t *argb, int width, int height,
                       int l, int t, int r, int b)
{
    present_impl(w, argb, width, height, l, t, r, b);
}

int plat_poll(plat_window *w, plat_event *ev)
{
    MSG msg;
    while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    if (!w || w->evn == 0) return 0;
    *ev = w->evq[0];
    for (int i = 1; i < w->evn; i++) w->evq[i - 1] = w->evq[i];
    w->evn--;
    return 1;
}

void plat_move(plat_window *w, int x, int y)
{
    if (!w) return;
    SetWindowPos(w->hwnd, NULL, x, y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void plat_get_pos(plat_window *w, int *x, int *y)
{
    if (!w) return;
    RECT r;
    GetWindowRect(w->hwnd, &r);
    *x = r.left;
    *y = r.top;
}

void plat_cursor(int *x, int *y)
{
    POINT p;
    GetCursorPos(&p);
    *x = p.x;
    *y = p.y;
}

void plat_workarea(int *x, int *y, int *w, int *h)
{
    RECT r;
    SystemParametersInfo(SPI_GETWORKAREA, 0, &r, 0);
    *x = r.left;
    *y = r.top;
    *w = r.right - r.left;
    *h = r.bottom - r.top;
}

void plat_sleep(int ms) { Sleep(ms); }

uint64_t plat_ms(void) { return (uint64_t)GetTickCount64(); }

void plat_set_click_through_all(plat_window *w, int on)
{
    if (!w) return;
    w->click_through_all = on;
    LONG_PTR ex = GetWindowLongPtr(w->hwnd, GWL_EXSTYLE);
    if (on) ex |= WS_EX_TRANSPARENT;
    else ex &= ~WS_EX_TRANSPARENT;
    SetWindowLongPtr(w->hwnd, GWL_EXSTYLE, ex);
}

void plat_set_topmost(plat_window *w, int on)
{
    if (!w) return;
    SetWindowPos(w->hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
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
    if (width == w->w && height == w->h) return;
    w->w = width;
    w->h = height;

    /* 重建 DIB：UpdateLayeredWindow 要求 DIB 与窗口尺寸一致 */
    if (w->dib) { DeleteObject(w->dib); w->dib = NULL; }
    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    w->dib = CreateDIBSection(w->memdc, &bi, DIB_RGB_COLORS, &w->bits, NULL, 0);
    if (w->dib) SelectObject(w->memdc, w->dib);

    SetWindowPos(w->hwnd, NULL, 0, 0, width, height,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

const char *plat_hotkey_name(plat_window *w)
{
    return (w && w->hotkey_name) ? w->hotkey_name : "";
}

/* ---------------- 文字（GDI） --------------
 * 做法：白字画在黑底上 → 像素亮度 ≈ 覆盖率，当作 alpha；RGB 统一用墨色。
 * 这样能直接拿到带抗锯齿的 ARGB，还能完整支持中文（GDI 自己选字体）。
 */
int plat_text_bitmap(const char *utf8, int px, uint32_t color, int maxw,
                     uint32_t **out, int *out_w, int *out_h)
{
    if (!utf8 || !utf8[0]) return 1;

    uint32_t cr = (color >> 16) & 0xFF;
    uint32_t cg = (color >> 8) & 0xFF;
    uint32_t cb = color & 0xFF;

    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    if (wlen <= 0) return 1;
    wchar_t *ws = (wchar_t *)malloc((size_t)wlen * sizeof(wchar_t));
    if (!ws) return 1;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, ws, wlen);

    /* 4 倍超采样：每像素 16 个样本。
     * 2 倍只有 5 级 alpha（0,1/4,1/2,3/4,1），中文笔画拐角能看到台阶。
     * PROOF_QUALITY = 不做 grid-fitting，字形按真实轮廓画，
     * 降采样后笔画粗细均匀（ANTIALIASED 会把竖笔对齐像素，缩下来一粗一细）。 */
    const int SS = 4;
    int maxw2 = maxw * SS;

    HDC dc = CreateCompatibleDC(NULL);
    if (!dc) { free(ws); return 1; }
    HFONT font = CreateFontA(px * SS, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS, PROOF_QUALITY,
                             DEFAULT_PITCH | FF_DONTCARE, "Microsoft YaHei UI");
    HGDIOBJ old_font = SelectObject(dc, font);

    RECT rc = {0, 0, maxw2, 0};
    DrawTextW(dc, ws, -1, &rc, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    int tw2 = rc.right - rc.left;
    int th2 = rc.bottom - rc.top;
    if (tw2 < 1) tw2 = 1;
    if (th2 < 1) th2 = 1;
    if (tw2 > maxw2) tw2 = maxw2;

    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = tw2;
    bi.bmiHeader.biHeight = -th2;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!dib) {
        SelectObject(dc, old_font);
        DeleteObject(font);
        DeleteDC(dc);
        free(ws);
        return 1;
    }
    HGDIOBJ old_bmp = SelectObject(dc, dib);
    memset(bits, 0, (size_t)tw2 * th2 * 4);

    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    RECT rc2 = {0, 0, tw2, th2};
    DrawTextW(dc, ws, -1, &rc2, DT_WORDBREAK | DT_NOPREFIX);

    const uint32_t *src = (const uint32_t *)bits;
    int tw = (tw2 + SS - 1) / SS;
    int th = (th2 + SS - 1) / SS;
    uint32_t *dst = (uint32_t *)malloc((size_t)tw * th * 4);
    if (dst) {
        for (int y = 0; y < th; y++) {
            for (int x = 0; x < tw; x++) {
                unsigned acc = 0, cnt = 0;
                for (int sy = 0; sy < SS; sy++) {
                    int yy = y * SS + sy;
                    if (yy >= th2) break;
                    for (int sx = 0; sx < SS; sx++) {
                        int xx = x * SS + sx;
                        if (xx >= tw2) break;
                        uint32_t p = src[(size_t)yy * tw2 + xx];
                        unsigned v = p & 0xFF;
                        unsigned r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF;
                        if (g > v) v = g;
                        if (r > v) v = r;
                        acc += v;
                        cnt++;
                    }
                }
                unsigned a = cnt ? acc / cnt : 0;
                /* 小字号降采样后笔画会偏细，做个轻微 gamma 提亮，看着更清楚 */
                if (a) {
                    unsigned g = (unsigned)(255.0 * pow(a / 255.0, 0.90) + 0.5);
                    a = g > 255 ? 255 : g;
                }
                dst[(size_t)y * tw + x] = a ? ((a << 24) | (cr << 16) | (cg << 8) | cb) : 0;
            }
        }
        *out = dst;
        *out_w = tw;
        *out_h = th;
    }

    SelectObject(dc, old_bmp);
    DeleteObject(dib);
    SelectObject(dc, old_font);
    DeleteObject(font);
    DeleteDC(dc);
    free(ws);
    return dst ? 0 : 1;
}

void plat_destroy(plat_window *w)
{
    if (!w) return;
    if (w->dib) DeleteObject(w->dib);
    if (w->memdc) DeleteDC(w->memdc);
    if (w->hwnd) DestroyWindow(w->hwnd);
    free(w);
}

/* ---------------- 进程身份（单实例锁 / 父进程看门狗） ---------------- */

int plat_self_pid(void)
{
    return (int)GetCurrentProcessId();
}

/* 未捕获异常时落一个文件，否则 -mwindows 程序崩了什么都看不到 */
static LONG WINAPI pet_crash_filter(EXCEPTION_POINTERS *ep)
{
    FILE *f = fopen("pet-crash.txt", "a");
    if (f) {
        DWORD code = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionCode : 0;
        void *addr = (ep && ep->ExceptionRecord) ? ep->ExceptionRecord->ExceptionAddress : NULL;
        fprintf(f, "[%lu] code=0x%08lX addr=%p", (unsigned long)GetTickCount(),
                (unsigned long)code, addr);
#if defined(_M_X64) || defined(__x86_64__)
        if (ep && ep->ContextRecord)
            fprintf(f, "  rip=%p rsp=%p rbp=%p",
                    (void *)ep->ContextRecord->Rip, (void *)ep->ContextRecord->Rsp,
                    (void *)ep->ContextRecord->Rbp);
#endif
        fputc('\n', f);
        fclose(f);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

void plat_install_crash_handler(void)
{
    SetUnhandledExceptionFilter(pet_crash_filter);
}

int plat_pid_alive(int pid)
{
    if (pid <= 0) return 0;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD)pid);
    if (!h) return 0;                       /* 打不开就是没了（或没权限，按没了算） */
    DWORD code = 0;
    int alive = GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}

int plat_create_exclusive(const char *path)
{
    HANDLE h = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return 0;
    CloseHandle(h);
    return 1;
}

void plat_error_box(const char *title, const char *msg)
{
    MessageBoxA(NULL, msg ? msg : "", title ? title : "MagpiePet", MB_ICONERROR | MB_OK);
}
