/* pet.c — 跨平台桌宠核心
 *
 * 职责：
 *   1. 载入 Codex Custom Pet 图集（8列 x N行，单格 192x208）
 *   2. 轮询 magpie 事件服务的 /pet.txt（纯文本 key=value）
 *   3. 状态机 → 选轨 → 按官方逐帧时长推进
 *   4. 软件合成 RGBA 帧（精灵 + 气泡），交给平台层显示
 *
 * 平台相关的东西全部在 platform.h 后面，这里不出现任何系统头文件（除了标准 C 和 socket）。
 *
 * 构建：见 Makefile / build.bat
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>

#include "platform.h"

/* ---------------- 常量 ---------------- */
/* 窗口/宠物尺寸是运行时算的：按图集格尺寸 × 显示倍率（1.0 = 原生像素，最清晰）*/
static int WIN_W = 240, WIN_H = 350;
static int PET_W = 181, PET_H = 196;

#define WAITING_AFTER_IDLE_MS 60000  /* 真闲下来（60s 没事件）才切 waiting；之前 8s 太激进，导致永远看不到注视 */
#define LOOK_MIN_IDLE_MS   1200      /* 刚启动/刚有事件后先正常播 idle，过一会儿才允许注视帧 */
#define LOOK_RADIUS        700.0     /* 光标离多近才看过去（之前 1400 几乎覆盖全屏，导致永远在看） */
#define PET_REACT_MS 1500     /* 被摸到时插播完成态的时长 */
#define BUB_FADE_IN_MS   140  /* 气泡淡入 */
#define BUB_FADE_OUT_MS  280  /* 气泡淡出（最后这段时间里向上飘） */
#define POLL_MS 250

/* ---------------- 轨道（运行时） ----------------
 * 两种来源：
 *   1) pets/<name>/layout.txt（tools/video-bake.ps1 生成的通用布局）
 *   2) 内置的 Codex Custom Pet 9 轨规格
 */
typedef struct {
    char name[24];
    int row;
    int frames;
    int total;
    int *durs;      /* 长度 = frames */
} rt_track;

static rt_track g_rt[16];
static int g_nrt = 0;
static int g_cw = 192, g_ch = 208;   /* 单格尺寸（可被 layout.txt 覆盖） */

static const int D_IDLE[]    = {280, 110, 110, 140, 140, 320};
static const int D_RIGHT[]   = {120, 120, 120, 120, 120, 120, 120, 220};
static const int D_LEFT[]    = {120, 120, 120, 120, 120, 120, 120, 220};
static const int D_WAVE[]    = {140, 140, 140, 280};
static const int D_JUMP[]    = {140, 140, 140, 140, 280};
static const int D_FAILED[]  = {140, 140, 140, 140, 140, 140, 140, 240};
static const int D_WAIT[]    = {150, 150, 150, 150, 150, 260};
static const int D_RUN[]     = {120, 120, 120, 120, 120, 220};
static const int D_REVIEW[]  = {150, 150, 150, 150, 150, 280};

typedef struct { const char *name; int row; int frames; const int *durs; } builtin_track;

static const builtin_track BUILTIN[] = {
    {"idle",          0, 6, D_IDLE},
    {"running-right", 1, 8, D_RIGHT},
    {"running-left",  2, 8, D_LEFT},
    {"waving",        3, 4, D_WAVE},
    {"jumping",       4, 5, D_JUMP},
    {"failed",        5, 8, D_FAILED},
    {"waiting",       6, 6, D_WAIT},
    {"running",       7, 6, D_RUN},
    {"review",        8, 6, D_REVIEW},
};

static void rt_add(const char *name, int row, int frames, const int *durs, int total)
{
    if (g_nrt >= 16 || frames <= 0) return;
    rt_track *t = &g_rt[g_nrt++];
    snprintf(t->name, sizeof(t->name), "%s", name);
    t->row = row;
    t->frames = frames;
    t->total = total > 0 ? total : (frames * 100);
    t->durs = (int *)malloc(sizeof(int) * frames);
    if (!t->durs) { g_nrt--; return; }
    if (durs) {
        for (int i = 0; i < frames; i++) t->durs[i] = durs[i];
    } else {
        /* 没给逐帧时长：总时长均匀分摊 */
        int base = t->total / frames;
        int rem = t->total - base * frames;
        for (int i = 0; i < frames; i++) t->durs[i] = base + (i == frames - 1 ? rem : 0);
    }
}

static void rt_init_builtin(void)
{
    for (int i = 0; i < (int)(sizeof(BUILTIN) / sizeof(BUILTIN[0])); i++) {
        const builtin_track *b = &BUILTIN[i];
        int total = 0;
        for (int k = 0; k < b->frames; k++) total += b->durs[k];
        rt_add(b->name, b->row, b->frames, b->durs, total);
    }
}

/* 读 layout.txt（video-bake.ps1 输出）
 * cell=192x208 / cols=N / rows=M
track=idle row=0 frames=15 total=1100
 */
static int rt_init_from_layout(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    int cnt = 0;
    while (fgets(line, sizeof(line), f)) {
        int a, b, c, d;
        if (sscanf(line, "cell=%dx%d", &a, &b) == 2) { g_cw = a; g_ch = b; }
        else {
            char name[32];
            if (sscanf(line, "track=%31s row=%d frames=%d total=%d", name, &b, &c, &d) == 4) {
                rt_add(name, b, c, NULL, d);
                cnt++;
            }
        }
    }
    fclose(f);
    return cnt;
}

/* 这套皮肤里有没有这条轨（不做事回退，回退版用 track_index） */
static int has_track(const char *name)
{
    for (int i = 0; i < g_nrt; i++)
        if (strcmp(g_rt[i].name, name) == 0) return 1;
    return 0;
}

static int track_index(const char *name)
{
    for (int i = 0; i < g_nrt; i++)
        if (strcmp(g_rt[i].name, name) == 0) return i;
    /* 这套皮肤没有这条轨（比如视频抠的图没有 failed/waiting）：
     * 退回 idle，至少动画还在跑，不会僵在某一帧 */
    for (int i = 0; i < g_nrt; i++)
        if (strcmp(g_rt[i].name, "idle") == 0) return i;
    return g_nrt > 0 ? 0 : -1;
}

/* ---------------- 图集 ---------------- */
typedef struct {
    uint32_t *px;     /* RGBA, 0xAARRGGBB */
    int w, h;
    int rows;
} atlas_t;

static atlas_t g_atlas;

/* ---------------- 状态 ---------------- */
typedef enum { ST_IDLE = 0, ST_THINKING, ST_BUSY, ST_DONE, ST_FAILED, ST_WAITING } pet_state;

static pet_state g_state = ST_IDLE;
static pet_state g_core_state = ST_WAITING;   /* 内核给的姿态（摸宠物时临时盖掉） */
static uint64_t g_pet_reaction_until = 0;     /* 「被摸到」的完成态播放截止时间 */
static uint64_t g_state_at = 0;
static uint64_t g_last_event_at = 0;
static int g_cur_frame = -1;
static int g_seq = -1;
static int g_active = 0;

/* 气泡：两个槽 + 当前要画的那个（生命周期见「气泡」那节）
 *   0 = 远端（服务端事件），1 = 本地（菜单操作 / 连接状态） */
enum { BUB_LIVE = 0, BUB_INFO, BUB_OK, BUB_ERR };
typedef struct {
    char text[512];
    char agent[64];      /* 画状态行用；空就不画状态行（本地提示） */
    char state[32];
    uint64_t at;         /* 出现的时刻（plat_ms），淡入淡出按它算 */
    uint32_t ttl;        /* 还能活多久；0 = 不过期（live） */
    int kind;
    int dying;           /* 服务端已收走：只剩一段淡出，别直接抹掉 */
} bubble_t;
static bubble_t g_bub[2];
static const bubble_t *g_bub_cur = NULL;   /* 本帧画哪个，NULL = 不画 */
static double g_bub_alpha = 0.0;
static int g_bub_shift = 0;               /* 淡出时向上飘几像素 */
static int g_bub_was_visible = 0;
static uint64_t g_bub_shown_at = 0;

static char g_agent[64];
static char g_model[128];
static int g_round_no = 0;

static int g_quit = 0;
static int g_click_through_all = 0;
static uint64_t g_click_through_until = 0;   /* 自动恢复的截止时间 */
static int g_x = 0, g_y = 0;
static char g_sheet_path[512];
#define PET_MARGIN_BOTTOM 10
static double g_disp_scale = 1.0;   /* PET_SCALE：1.0 = 1:1，最好看 */
static double g_speed = 1.0;        /* PET_SPEED：整体快/慢放，默认 1.0 */
static FILE *g_log = NULL;

static void plog(const char *fmt, ...)
{
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

/* 拖动 */
static int g_drag = 0;
static int g_drag_moved = 0;   /* 真的拖动过（越过阈值）；没拖就是“点一下” */

/* 静态层（气泡/列表/菜单）：内容不变时每帧只 memcpy 拷回，不重画 */
static uint32_t *g_ui = NULL;
/* 静态层里「可能盖到宠物身上」的区域（气泡/菜单），合成时要盖回去 */
static int g_over_l = 0, g_over_t = 0, g_over_r = 0, g_over_b = 0;

static void ui_over_add(int l, int t, int r, int b)
{
    if (r <= l || b <= t) return;
    if (g_over_r <= g_over_l) { g_over_l = l; g_over_t = t; g_over_r = r; g_over_b = b; return; }
    if (l < g_over_l) g_over_l = l;
    if (t < g_over_t) g_over_t = t;
    if (r > g_over_r) g_over_r = r;
    if (b > g_over_b) g_over_b = b;
}

/* ---------- 菜单（自己画，三平台通用，不依赖系统菜单 API） ---------- */
static int pet_local_x(void);       /* 定义在列表那块，菜单排版也要用 */
#define MENU_N 6
#define MENU_IDX_SCALE 2          /* 「调整大小」在菜单里的行号 */
static int MENU_W = 180;      /* 运行时按窗口宽自适应，避免小窗口下被裁 */
#define MENU_ROW 28
#define MENU_PAD 7

/* 子菜单：调整大小 */
#define SUB_N 4
#define SUB_W 92
#define SUB_ROW 28
#define SUB_PAD 6
#define MENU_AUTO_CLOSE_MS 5000     /* 鼠标离开菜单这么久后自动关闭 */
static uint64_t g_menu_leave_since = 0;
static const double SUB_SCALE[SUB_N] = { 1.50, 1.00, 0.75, 0.50 };
static const char *const SUB_LABELS[SUB_N] = { "150%", "100%", "75%", "50%" };

static int g_menu_open = 0;
static int g_menu_hover = -1;
static int g_menu_x = 0, g_menu_y = 0;
static int g_sub_show = 0;        /* 「调整大小」子菜单是否展开 */
static int g_sub_hover = -1;
static int g_sub_x = 0, g_sub_y = 0;
static int g_topmost = 1;
static uint32_t *g_menu_tex[MENU_N];
static int g_menu_tw[MENU_N], g_menu_th[MENU_N];
static int g_menu_tex_ready = 0;
static uint32_t *g_sub_tex[SUB_N];
static int g_sub_tw[SUB_N], g_sub_th[SUB_N];
static int g_sub_tex_ready = 0;

static const char *MENU_LABELS[MENU_N] = {
    "置顶",
    "点击穿透（30 秒后自动恢复）",
    "调整大小",
    "重新加载素材",
    "回到右下角",
    "退出"
};

/* ---------------- 时序 ---------------- */

static const char *effective_track(void)
{
    if (g_state == ST_THINKING) return "review";
    if (g_state == ST_BUSY) return "running";
    if (g_state == ST_DONE) return "jumping";
    /* 皮肤没有 failed 轨（视频抠的只有四态）→ 当“已完成”处理，播完成动作，
     * 失败原因看气泡和列表就行 */
    if (g_state == ST_FAILED) return has_track("failed") ? "failed" : "jumping";
    if (g_state == ST_WAITING) return "waiting";
    if (g_state == ST_IDLE && (plat_ms() - g_last_event_at) > WAITING_AFTER_IDLE_MS)
        return "waiting";
    return "idle";
}

static int frame_at(int ti, uint64_t elapsed_ms)
{
    const rt_track *t = &g_rt[ti];
    int total = t->total;
    if (total <= 0) return 0;
    /* PET_SPEED 快/慢放：等价于把所有帧时长乘一个系数 */
    uint64_t sc = (uint64_t)((double)elapsed_ms * g_speed);
    int pos = (int)(sc % (uint64_t)total);
    int acc = 0;
    for (int i = 0; i < t->frames; i++) {
        acc += t->durs[i];
        if (pos < acc) return i;
    }
    return t->frames - 1;
}



/* ---------------- 图集加载 ---------------- */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "third_party/stb_image.h"

static int atlas_load(const char *path)
{
    int n = 0;
    stbi_uc *data = stbi_load(path, &g_atlas.w, &g_atlas.h, &n, 4);
    if (!data) {
        fprintf(stderr, "pet: 无法读取图集 %s (%s)\n", path, stbi_failure_reason());
        return -1;
    }
    free(g_atlas.px);
    g_atlas.px = NULL;
    size_t cnt = (size_t)g_atlas.w * g_atlas.h;
    g_atlas.px = (uint32_t *)malloc(cnt * 4);
    if (!g_atlas.px) { stbi_image_free(data); return -1; }
    /* stb 给的是 RGBA 字节序，转成 0xAARRGGBB */
    for (size_t i = 0; i < cnt; i++) {
        uint8_t r = data[i * 4 + 0], g = data[i * 4 + 1], b = data[i * 4 + 2], a = data[i * 4 + 3];
        g_atlas.px[i] = ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    }
    stbi_image_free(data);
    g_atlas.rows = g_atlas.h / g_ch;
    fprintf(stderr, "pet: 图集 %dx%d  %d 行\n", g_atlas.w, g_atlas.h, g_atlas.rows);
    return 0;
}

/* 取某轨某帧的源矩形 */
static int cell_rect(int track_idx, int frame, int *sx, int *sy)
{
    const rt_track *t = &g_rt[track_idx];
    if (t->row >= g_atlas.rows) return -1;
    if (frame < 0 || frame >= t->frames) return -1;
    *sx = frame * g_cw;
    *sy = t->row * g_ch;
    return 0;
}

/* 注视帧（第 9、10 行，16 方向） */
static int look_rect(double angle_deg, int *sx, int *sy)
{
    if (g_atlas.rows < 11) return -1;
    int idx = (int)floor((angle_deg + 11.25) / 22.5);
    idx %= 16;
    if (idx < 0) idx += 16;
    *sx = (idx % 8) * g_cw;
    *sy = (9 + idx / 8) * g_ch;
    return 0;
}

/* ---------------- 软件合成 ---------------- */
static uint32_t *g_fb;
/* 气泡淡出用：在 blend 里再乘一次 alpha（0..255），只包围气泡那几行绘制 */
static uint32_t g_blend_mul = 255;

static inline uint32_t blend(uint32_t dst, uint32_t src)
{
    uint32_t sa = (src >> 24) & 0xFF;
    if (g_blend_mul != 255) sa = sa * g_blend_mul / 255;
    if (sa == 0) return dst;
    if (sa == 255) return src;
    uint32_t da = (dst >> 24) & 0xFF;
    /* src over dst，非预乘 */
    uint32_t out_a = sa + da * (255 - sa) / 255;
    if (out_a == 0) return 0;
    uint32_t sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
    uint32_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    uint32_t r = (sr * sa + dr * da * (255 - sa) / 255) / out_a;
    uint32_t g = (sg * sa + dg * da * (255 - sa) / 255) / out_a;
    uint32_t b = (sb * sa + db * da * (255 - sa) / 255) / out_a;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (out_a << 24) | (r << 16) | (g << 8) | b;
}

/* 1:1 直接搬（最常见的情况）：每像素只读 1 个图集像素，
 * 不走双线性的 4 读 + 浮点，实测渲染耗时降 3~4 倍 */
static void draw_sprite_1to1(int sx, int sy, int dst_x, int dst_y, int w, int h)
{
    const int gw = g_atlas.w;
    for (int y = 0; y < h; y++) {
        int fy = dst_y + y, gy = sy + y;
        if (fy < 0 || fy >= WIN_H || gy < 0 || gy >= g_atlas.h) continue;
        const uint32_t *row = g_atlas.px + (size_t)gy * gw + sx;
        uint32_t *drow = g_fb + (size_t)fy * WIN_W;
        for (int x = 0; x < w; x++) {
            int fx = dst_x + x;
            if (fx < 0 || fx >= WIN_W) continue;
            uint32_t p = row[x];
            uint32_t a = p >> 24;
            if (a == 0) continue;
            if (a == 255) { drow[fx] = p; continue; }
            drow[fx] = blend(drow[fx], p);
        }
    }
}

/* 从图集矩形缩放绘制到帧缓冲：双线性采样（预乘空间），避免硬边和黑边 */
static void draw_sprite(int sx, int sy, int dst_x, int dst_y, int dst_w, int dst_h,
                        float scale_mul)
{
    if (dst_w == g_cw && dst_h == g_ch) {
        draw_sprite_1to1(sx, sy, dst_x, dst_y, dst_w, dst_h);
        (void)scale_mul;
        return;
    }
    const float du = (float)g_cw / dst_w;
    const float dv = (float)g_ch / dst_h;

    for (int y = 0; y < dst_h; y++) {
        int fy = dst_y + y;
        if (fy < 0 || fy >= WIN_H) continue;
        float v = (y + 0.5f) * dv - 0.5f;
        int y0 = (int)floorf(v);
        float ty = v - y0;
        int y1 = y0 + 1;
        if (y0 < 0) y0 = 0;
        if (y1 > g_ch - 1) y1 = g_ch - 1;

        for (int x = 0; x < dst_w; x++) {
            int fx = dst_x + x;
            if (fx < 0 || fx >= WIN_W) continue;
            float u = (x + 0.5f) * du - 0.5f;
            int x0 = (int)floorf(u);
            float tx = u - x0;
            int x1 = x0 + 1;
            if (x0 < 0) x0 = 0;
            if (x1 > g_cw - 1) x1 = g_cw - 1;

            float w00 = (1 - tx) * (1 - ty), w10 = tx * (1 - ty);
            float w01 = (1 - tx) * ty, w11 = tx * ty;

            /* 在预乘空间插值，才不会出现黑边/白边 */
            float a = 0, r = 0, g = 0, b = 0;
            const uint32_t *p4 = g_atlas.px;
            int gw = g_atlas.w;
            int ix[4] = { x0, x1, x0, x1 };
            int iy[4] = { y0, y0, y1, y1 };
            float iw[4] = { w00, w10, w01, w11 };
            for (int k = 0; k < 4; k++) {
                int gx = sx + ix[k], gy = sy + iy[k];
                if (gx < 0 || gx >= gw || gy < 0 || gy >= g_atlas.h) continue;
                uint32_t p = p4[(size_t)gy * gw + gx];
                float pa = (float)((p >> 24) & 0xFF) / 255.0f;
                if (pa <= 0.0f) continue;
                float wgt = iw[k];
                a += pa * wgt;
                r += ((p >> 16) & 0xFF) * pa * wgt;
                g += ((p >> 8) & 0xFF) * pa * wgt;
                b += (p & 0xFF) * pa * wgt;
            }
            if (a <= 0.001f) continue;
            /* 反预乘 */
            float inv = 1.0f / a;
            r *= inv; g *= inv; b *= inv;
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            uint32_t outa = (uint32_t)(a * 255.0f + 0.5f);
            if (outa > 255) outa = 255;
            uint32_t src = (outa << 24) | ((uint32_t)(r + 0.5f) << 16) |
                           ((uint32_t)(g + 0.5f) << 8) | (uint32_t)(b + 0.5f);
            g_fb[(size_t)fy * WIN_W + fx] = blend(g_fb[(size_t)fy * WIN_W + fx], src);
        }
    }
    (void)scale_mul;
}

/* 圆角矩形（气泡底）—— 用 SDF 算覆盖率，边缘是抗锯齿的软边 */
static void fill_round_rect(int x0, int y0, int w, int h, int r, uint32_t color)
{
    if (r < 0) r = 0;
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    float hw = w * 0.5f, hh = h * 0.5f, rr = (float)r;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float px = (float)x + 0.5f - hw;
            float py = (float)y + 0.5f - hh;
            float qx = fabsf(px) - (hw - rr);
            float qy = fabsf(py) - (hh - rr);
            float d;
            if (qx <= 0.0f && qy <= 0.0f) {
                d = -rr;                          /* 内矩形：完全覆盖，不用开方 */
            } else if (qx > 0.0f && qy > 0.0f) {
                d = sqrtf(qx * qx + qy * qy) - rr;  /* 只有四个圆角要开方 */
            } else {
                d = (qx > qy ? qx : qy) - rr;       /* 直边：一个方向 */
            }
            float cov = 0.5f - d;                     /* 1px 过渡带 */
            if (cov <= 0) continue;
            if (cov > 1) cov = 1;

            int fx = x0 + x, fy = y0 + y;
            if (fx < 0 || fx >= WIN_W || fy < 0 || fy >= WIN_H) continue;
            uint32_t base_a = (color >> 24) & 0xFF;
            uint32_t a = (uint32_t)(base_a * cov + 0.5f);
            uint32_t src = (a << 24) | (color & 0x00FFFFFFu);
            g_fb[(size_t)fy * WIN_W + fx] = blend(g_fb[(size_t)fy * WIN_W + fx], src);
        }
    }
}

/* 气泡尖角：3x3 超采样的实心三角形 */
static void fill_triangle(int ax, int ay, int bx, int by, int cx, int cy, uint32_t color)
{
    int minx = ax < bx ? (ax < cx ? ax : cx) : (bx < cx ? bx : cx);
    int maxx = ax > bx ? (ax > cx ? ax : cx) : (bx > cx ? bx : cx);
    int miny = ay < by ? (ay < cy ? ay : cy) : (by < cy ? by : cy);
    int maxy = ay > by ? (ay > cy ? ay : cy) : (by > cy ? by : cy);
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx >= WIN_W) maxx = WIN_W - 1;
    if (maxy >= WIN_H) maxy = WIN_H - 1;

    #define SIGN(px, py, qx, qy, rx, ry) ((px - rx) * (qy - ry) - (qx - rx) * (py - ry))
    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            int hits = 0;
            for (int sy = 0; sy < 3; sy++) {
                for (int sx = 0; sx < 3; sx++) {
                    float px = x + (sx + 0.5f) / 3.0f;
                    float py = y + (sy + 0.5f) / 3.0f;
                    float d1 = SIGN(px, py, ax, ay, bx, by);
                    float d2 = SIGN(px, py, bx, by, cx, cy);
                    float d3 = SIGN(px, py, cx, cy, ax, ay);
                    int neg = (d1 < 0) + (d2 < 0) + (d3 < 0);
                    if (neg == 0 || neg == 3) hits++;
                }
            }
            if (!hits) continue;
            uint32_t base_a = (color >> 24) & 0xFF;
            uint32_t a = (uint32_t)(base_a * hits / 9);
            uint32_t src = (a << 24) | (color & 0x00FFFFFFu);
            g_fb[(size_t)y * WIN_W + x] = blend(g_fb[(size_t)y * WIN_W + x], src);
        }
    }
    #undef SIGN
}

/* ---------- 菜单渲染 ---------- */
static void ensure_menu_tex(void)
{
    if (g_menu_tex_ready) return;
    for (int i = 0; i < MENU_N; i++) {
        g_menu_tex[i] = NULL;
        g_menu_tw[i] = g_menu_th[i] = 0;
        if (plat_text_bitmap(MENU_LABELS[i], 14, 0xFF1B1B24u, MENU_W - 42,
                             &g_menu_tex[i], &g_menu_tw[i], &g_menu_th[i]) != 0)
            g_menu_tex[i] = NULL;
    }
    g_menu_tex_ready = 1;
}

static void ensure_sub_tex(void)
{
    if (g_sub_tex_ready) return;
    for (int i = 0; i < SUB_N; i++) {
        g_sub_tex[i] = NULL;
        g_sub_tw[i] = g_sub_th[i] = 0;
        if (plat_text_bitmap(SUB_LABELS[i], 14, 0xFF1B1B24u, SUB_W - 20,
                             &g_sub_tex[i], &g_sub_tw[i], &g_sub_th[i]) != 0)
            g_sub_tex[i] = NULL;
    }
    g_sub_tex_ready = 1;
}

static void draw_menu(void)
{
    ensure_menu_tex();
    ensure_sub_tex();
    int w = MENU_W;
    int h = MENU_PAD * 2 + MENU_ROW * MENU_N;

    /* 菜单对着宠物居中（窗口比宠物宽时不会飘到右边去），
     * 右边还要给子菜单留位，所以不能贴着窗口边 */
    int mx = pet_local_x() + PET_W / 2 - w / 2;
    if (mx < 6) mx = 6;
    if (mx + w > WIN_W - 6) mx = WIN_W - 6 - w;
    int my = WIN_H - PET_MARGIN_BOTTOM - PET_H - h - 8;
    if (my < 4) my = 4;
    g_menu_x = mx;
    g_menu_y = my;
    ui_over_add(mx - 8, my - 8, mx + w + 8, my + h + 8);

    for (int i = 5; i >= 1; i--)
        fill_round_rect(mx - i, my - i + 1, w + i * 2, h + i * 2, 10 + i,
                        (uint32_t)(8 + (6 - i) * 2) << 24);
    fill_round_rect(mx - 1, my - 1, w + 2, h + 2, 11, 0x26000000u);
    fill_round_rect(mx, my, w, h, 10, 0xFBFFFFFFu);

    for (int i = 0; i < MENU_N; i++) {
        int ry = my + MENU_PAD + i * MENU_ROW;
        if (i == g_menu_hover)
            fill_round_rect(mx + 4, ry, w - 8, MENU_ROW, 7, 0xFFE7E8F3u);

        if (g_menu_tex[i]) {
            int ty = ry + (MENU_ROW - g_menu_th[i]) / 2;
            for (int y = 0; y < g_menu_th[i]; y++) {
                for (int x = 0; x < g_menu_tw[i]; x++) {
                    uint32_t p = g_menu_tex[i][(size_t)y * g_menu_tw[i] + x];
                    if (((p >> 24) & 0xFF) == 0) continue;
                    int fx = mx + 22 + x, fy = ty + y;
                    if (fx < 0 || fx >= WIN_W || fy < 0 || fy >= WIN_H) continue;
                    g_fb[(size_t)fy * WIN_W + fx] = blend(g_fb[(size_t)fy * WIN_W + fx], p);
                }
            }
        }

        int on = (i == 0 && g_topmost) || (i == 1 && g_click_through_all);
        if (on) {
            int cx = mx + 9, cy = ry + MENU_ROW / 2;
            for (int d = 0; d < 4; d++) {
                int px = cx + d, py = cy - 1 + d;
                if (px >= 0 && px < WIN_W && py >= 0 && py < WIN_H)
                    g_fb[(size_t)py * WIN_W + px] = blend(g_fb[(size_t)py * WIN_W + px], 0xFF3A3A8Cu);
            }
            for (int d = 0; d < 5; d++) {
                int px = cx + 3 + d, py = cy + 2 - d;
                if (px >= 0 && px < WIN_W && py >= 0 && py < WIN_H)
                    g_fb[(size_t)py * WIN_W + px] = blend(g_fb[(size_t)py * WIN_W + px], 0xFF3A3A8Cu);
            }
        }

        /* 「调整大小」行尾画一个 ▸，提示有子菜单 */
        if (i == MENU_IDX_SCALE) {
            int ax = mx + w - 18, ay = ry + MENU_ROW / 2 - 3;
            for (int d = 0; d < 4; d++)
                for (int e = 0; e <= d; e++) {
                    int px = ax + d, py = ay + e + (3 - d);
                    if (px >= 0 && px < WIN_W && py >= 0 && py < WIN_H)
                        g_fb[(size_t)py * WIN_W + px] = blend(g_fb[(size_t)py * WIN_W + px], 0xFF8A8A96u);
                }
        }
    }

    /* ---------- 子菜单 ---------- */
    if (!g_sub_show) return;
    int sh = SUB_PAD * 2 + SUB_ROW * SUB_N;
    int sx = mx + w - 6;                       /* 和主菜单略重叠，看着连在一起 */
    if (sx + SUB_W > WIN_W - 6) sx = mx - SUB_W + 6;   /* 右边放不下就翻到左侧 */
    if (sx < 6) sx = 6;
    int sy = my + MENU_PAD + MENU_IDX_SCALE * MENU_ROW - SUB_PAD;
    if (sy + sh > WIN_H - 4) sy = WIN_H - 4 - sh;
    if (sy < 4) sy = 4;
    g_sub_x = sx;
    g_sub_y = sy;
    ui_over_add(sx - 8, sy - 8, sx + SUB_W + 8, sy + sh + 8);

    for (int i = 5; i >= 1; i--)
        fill_round_rect(sx - i, sy - i + 1, SUB_W + i * 2, sh + i * 2, 10 + i,
                        (uint32_t)(8 + (6 - i) * 2) << 24);
    fill_round_rect(sx - 1, sy - 1, SUB_W + 2, sh + 2, 11, 0x26000000u);
    fill_round_rect(sx, sy, SUB_W, sh, 10, 0xFBFFFFFFu);

    for (int i = 0; i < SUB_N; i++) {
        int ry = sy + SUB_PAD + i * SUB_ROW;
        int cur = (SUB_SCALE[i] == g_disp_scale);
        if (i == g_sub_hover)
            fill_round_rect(sx + 4, ry, SUB_W - 8, SUB_ROW, 7, 0xFFE7E8F3u);
        if (g_sub_tex[i]) {
            int ty = ry + (SUB_ROW - g_sub_th[i]) / 2;
            for (int y = 0; y < g_sub_th[i]; y++) {
                for (int x = 0; x < g_sub_tw[i]; x++) {
                    uint32_t p = g_sub_tex[i][(size_t)y * g_sub_tw[i] + x];
                    if (((p >> 24) & 0xFF) == 0) continue;
                    int fx = sx + 26 + x, fy = ty + y;
                    if (fx < 0 || fx >= WIN_W || fy < 0 || fy >= WIN_H) continue;
                    g_fb[(size_t)fy * WIN_W + fx] = blend(g_fb[(size_t)fy * WIN_W + fx], p);
                }
            }
        }
        if (cur) {
            int cx = sx + 10, cy = ry + SUB_ROW / 2;
            for (int d = 0; d < 4; d++) {
                int px = cx + d, py = cy - 1 + d;
                if (px >= 0 && px < WIN_W && py >= 0 && py < WIN_H)
                    g_fb[(size_t)py * WIN_W + px] = blend(g_fb[(size_t)py * WIN_W + px], 0xFF3A3A8Cu);
            }
            for (int d = 0; d < 5; d++) {
                int px = cx + 3 + d, py = cy + 2 - d;
                if (px >= 0 && px < WIN_W && py >= 0 && py < WIN_H)
                    g_fb[(size_t)py * WIN_W + px] = blend(g_fb[(size_t)py * WIN_W + px], 0xFF3A3A8Cu);
            }
        }
    }
}

/* 子菜单命中（_x/_y 已在 draw_menu 里算好） */
static int sub_hit(int lx, int ly)
{
    if (!g_sub_show) return -1;
    int sh = SUB_PAD * 2 + SUB_ROW * SUB_N;
    if (lx < g_sub_x || lx >= g_sub_x + SUB_W) return -1;
    if (ly < g_sub_y || ly >= g_sub_y + sh) return -1;
    int i = (ly - g_sub_y - SUB_PAD) / SUB_ROW;
    if (i < 0 || i >= SUB_N) return -1;
    return i;
}

static int menu_hit(int lx, int ly)
{
    int h = MENU_PAD * 2 + MENU_ROW * MENU_N;
    if (lx < g_menu_x || lx >= g_menu_x + MENU_W) return -1;
    if (ly < g_menu_y || ly >= g_menu_y + h) return -1;
    int i = (ly - g_menu_y - MENU_PAD) / MENU_ROW;
    if (i < 0 || i >= MENU_N) return -1;
    return i;
}

/* ---------------- 会话列表（P2） ---------------- */
#define LIST_W   258
#define ROW_H    46
#define MAX_ROWS 8

typedef struct {
    char id[48], agent[24], model[40], state[12], query[128];
    int round_no, elapsed, tin, tout;
    double credits;
} sess_t;

static sess_t g_sess[10];
static int g_nsess = 0;
static int g_list_open = 0;
static int g_list_left = 0;          /* 列表在宠物左边还是右边 */
static int g_mx = 29;                /* 宠物到窗口边缘的水平留白（= 气泡需要的宽度） */
/* 子菜单/菜单的布局常量在菜单那块定义 */
static uint64_t g_hover_since = 0, g_leave_since = 0;
static char g_pinned[48] = "";       /* 被点中固定的会话 id */
static int g_row_hover = -1;
static int g_close_hover = -1;       /* 鼠标悬在哪个行的 × 上 */
static char g_host[64] = "127.0.0.1";
static int g_port = 8789;

/* 文本位图缓存：菜单/列表每帧都要文字，直接调 GDI 太慢 */
typedef struct { char key[96]; uint32_t *tex; int w, h; } tcache_t;
static tcache_t g_tc[96];      /* 环形替换：条目数越大，同帧内被挤掉的机会越小 */
static int g_tc_n = 0;
static int g_tc_rr = 0;

/* 调试追踪（PET_DEBUG=1 时写 pet-trace.txt） */
static int g_trace = 0;
static void trace(const char *fmt, ...)
{
    if (!g_trace) return;
    FILE *f = fopen("pet-trace.txt", "a");
    if (!f) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static uint32_t *get_text(const char *s, int px, uint32_t color, int maxw, int *ow, int *oh)
{
    if (!s || !s[0]) return NULL;
    char key[112];
    snprintf(key, sizeof(key), "%d|%06X|%d|%s", px, color & 0xFFFFFF, maxw, s);
    for (int i = 0; i < g_tc_n; i++) {
        if (strcmp(g_tc[i].key, key) == 0) { *ow = g_tc[i].w; *oh = g_tc[i].h; return g_tc[i].tex; }
    }
    uint32_t *tex = NULL; int w = 0, h = 0;
    if (plat_text_bitmap(s, px, color, maxw, &tex, &w, &h) != 0 || !tex) return NULL;
    int slot;
    if (g_tc_n < (int)(sizeof(g_tc) / sizeof(g_tc[0]))) slot = g_tc_n++;
    else { slot = g_tc_rr % (int)(sizeof(g_tc) / sizeof(g_tc[0])); g_tc_rr++; free(g_tc[slot].tex); }
    snprintf(g_tc[slot].key, sizeof(g_tc[slot].key), "%s", key);
    g_tc[slot].tex = tex; g_tc[slot].w = w; g_tc[slot].h = h;
    *ow = w; *oh = h;
    return tex;
}

/* 画一段文字；返回宽度（方便排版） */
static int draw_text(const char *s, int px, uint32_t color, int x, int y, int maxw)
{
    int w = 0, h = 0;
    uint32_t *tex = get_text(s, px, color, maxw, &w, &h);
    if (!tex) return 0;
    for (int yy = 0; yy < h; yy++) {
        int fy = y + yy;
        if (fy < 0 || fy >= WIN_H) continue;
        for (int xx = 0; xx < w; xx++) {
            int fx = x + xx;
            if (fx < 0 || fx >= WIN_W) continue;
            uint32_t p = tex[(size_t)yy * w + xx];
            if (((p >> 24) & 0xFF) == 0) continue;
            g_fb[(size_t)fy * WIN_W + fx] = blend(g_fb[(size_t)fy * WIN_W + fx], p);
        }
    }
    return w;
}

/* 右对齐绘制：返回左边 x */
static int draw_text_right(const char *s, int px, uint32_t color, int right, int y, int maxw)
{
    int w = 0, h = 0;
    get_text(s, px, color, maxw, &w, &h);
    if (!w) return right;
    draw_text(s, px, color, right - w, y, maxw);
    return right - w;
}

/* 展开时列表和宠物之间的间隙；面板自身还有 6px 内边距 */
#define L_GAP 8

/* 宠物在窗口内的左边距：
 *   折叠：两侧各留 g_mx（气泡比宠物宽，需要地方延伸）
 *   展开：贴着列表，不再给气泡让位（列表开了就不画气泡） */
static int pet_local_x(void)
{
    if (!g_list_open) return g_mx;
    return g_list_left ? (LIST_W - 6 + L_GAP) : L_GAP;
}

/* 名单里能放下几行 */
#define LHEAD_H 38
#define LFOOT_H 28
static int list_row_count(void)
{
    int n = g_nsess;
    if (n > MAX_ROWS) n = MAX_ROWS;
    int room = (WIN_H - 12 - LHEAD_H - LFOOT_H) / ROW_H;
    if (n > room) n = room;
    return n;
}

static void list_rect(int *x, int *y, int *w, int *h)
{
    *x = g_list_left ? 6 : (g_list_open ? pet_local_x() + PET_W + L_GAP : WIN_W - LIST_W + 6);
    *w = LIST_W - 12;
    /* 高度跟内容走：少了几行就不会留一大块白 */
    int n = list_row_count();
    int hh = (n > 0) ? LHEAD_H + n * ROW_H + LFOOT_H : LHEAD_H + 52;
    if (hh > WIN_H - 12) hh = WIN_H - 12;
    *h = hh;
    /* 垂直吸附：面板底边跟着宠物的脚，而不是钉在窗口顶部。
     * 否则会话一少，短面板就浮在天花板、宠物在底下，中间一大块空白很难看 */
    int bottom = WIN_H - PET_MARGIN_BOTTOM - 4;
    *y = bottom - hh;
    if (*y < 6) *y = 6;
}



static int pt_in(int px, int py, int x, int y, int w, int h)
{
    return px >= x && px < x + w && py >= y && py < y + h;
}
/* ---------------- 行尾「×」关闭按钮 ---------------- */
#define CLOSE_BTN 18

/* 只有完成/失败的行能关（运行中的不许关） */
static int row_closable(const sess_t *s)
{
    return strcmp(s->state, "done") == 0 || strcmp(s->state, "failed") == 0;
}

static int close_btn_rect(int i, int *bx, int *by)
{
    if (i < 0 || i >= g_nsess || i >= list_row_count()) return 0;
    if (!row_closable(&g_sess[i])) return 0;
    int x, y, w, h;
    list_rect(&x, &y, &w, &h);
    int ry = y + LHEAD_H + i * ROW_H;
    if (ry + ROW_H > y + h) return 0;
    *bx = x + w - 10 - CLOSE_BTN;
    *by = ry + (ROW_H - CLOSE_BTN) / 2;
    return 1;
}

static int close_btn_at(int lx, int ly)
{
    int n = list_row_count();
    for (int i = 0; i < n; i++) {
        int bx, by;
        if (!close_btn_rect(i, &bx, &by)) continue;
        if (pt_in(lx, ly, bx, by, CLOSE_BTN, CLOSE_BTN)) return i;
    }
    return -1;
}


/* 鼠标是否在宠物或列表上 */
static int hovered_now(void)
{
    int cx, cy;
    plat_cursor(&cx, &cy);
    int lx = cx - g_x, ly = cy - g_y;
    int px = pet_local_x();
    int py = WIN_H - PET_MARGIN_BOTTOM - PET_H;
    if (pt_in(lx, ly, px, py, PET_W, PET_H)) return 1;
    if (g_list_open) {
        int x, y, w, h;
        list_rect(&x, &y, &w, &h);
        if (pt_in(lx, ly, x, y, w, h)) return 1;
    }
    return 0;
}

/* 点到了列表里的第几行（-1 = 没点到） */
static int list_row_at(int lx, int ly)
{
    if (!g_list_open) return -1;
    int x, y, w, h;
    list_rect(&x, &y, &w, &h);
    if (!pt_in(lx, ly, x, y, w, h)) return -1;
    int idx = (ly - y - LHEAD_H) / ROW_H;      /* LHEAD_H = 标题高度 */
    if (idx < 0 || idx >= g_nsess) return -1;
    return idx;
}

static uint32_t state_color(const char *st)
{
    if (strcmp(st, "failed") == 0) return 0xFFFF3B30u;
    if (strcmp(st, "done") == 0)   return 0xFF34C759u;
    if (strcmp(st, "tool") == 0)   return 0xFF7B61FFu;
    return 0xFF4A90E2u;                    /* thinking */
}

static const char *state_label(const char *st)
{
    if (strcmp(st, "failed") == 0) return "失败";
    if (strcmp(st, "done") == 0)   return "完成";
    if (strcmp(st, "tool") == 0)   return "用工具中";
    return "思考中";
}

/* ---- 配色 ---- */
#define C_PANEL      0xFBFFFFFFu
#define C_PANEL_BRD  0x1A000000u
#define C_HEAD       0x09000000u   /* 分隔线 */
#define C_INK        0xFF1B1B24u
#define C_INK2       0xFF55555Fu
#define C_INK3       0xFF86868Fu
#define C_HOVER      0xFFEBEEF9u
#define C_PIN        0xFFE3E6F8u
#define C_ACCENT     0xFF5B5BD6u

/* 状态点带一圈淡光，看着像“在动” */
static void draw_dot(int cx, int cy, uint32_t col, int r, int glow)
{
    if (glow) {
        uint32_t g = (col & 0x00FFFFFFu) | 0x28000000u;
        for (int dy = -r - 3; dy <= r + 3; dy++)
            for (int dx = -r - 3; dx <= r + 3; dx++) {
                int d2 = dx * dx + dy * dy;
                if (d2 > (r + 3) * (r + 3) || d2 <= r * r) continue;
                int fx = cx + dx, fy = cy + dy;
                if (fx >= 0 && fx < WIN_W && fy >= 0 && fy < WIN_H)
                    g_fb[(size_t)fy * WIN_W + fx] = blend(g_fb[(size_t)fy * WIN_W + fx], g);
            }
    }
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r) {
                int fx = cx + dx, fy = cy + dy;
                if (fx >= 0 && fx < WIN_W && fy >= 0 && fy < WIN_H)
                    g_fb[(size_t)fy * WIN_W + fx] = blend(g_fb[(size_t)fy * WIN_W + fx], col);
            }
}

static int state_running(const char *st)
{
    return strcmp(st, "thinking") == 0 || strcmp(st, "tool") == 0;
}

/* 大数字紧凑格式：4700024 -> 4.7M，47002 -> 47k */
static void fmt_num(long v, char *out, size_t n)
{
    if (v >= 1000000)      snprintf(out, n, "%.1fM", v / 1000000.0);
    else if (v >= 10000)   snprintf(out, n, "%ldk", v / 1000);
    else                   snprintf(out, n, "%ld", v);
}

/* 模型名去掉 group/ 前缀，过长就截断（按 UTF-8 边界回退） */
static void short_model(const char *in, char *out, size_t n)
{
    const char *p = in ? in : "";
    if (strncmp(p, "group/", 6) == 0) p += 6;
    snprintf(out, n, "%s", p);
    size_t len = strlen(out), lim = 12;
    if (n - 1 < lim) lim = n - 1;
    if (len > lim) {
        size_t k = lim;
        while (k > 0 && ((unsigned char)out[k] & 0xC0) == 0x80) k--;   /* 回退到字符首字节 */
        out[k] = 0;
        if (k + 3 < n) memcpy(out + k, "\xE2\x80\xA6", 4);           /* … */
    }
}
/* ---------------- UTF-8 安全截断 ----------------
 * 缓冲区按字节限长，如果正好切在多字节字符中间，末尾会变成非法序列，
 * GDI 会把它转成乱码（典型现象：气泡末尾多一个“口”或乱码字）。
 * 这里把最后那个残缺的字符去掉，保证剪出来的是完整 UTF-8。
 * （不需要用宽字符：整个链路都是 UTF-8，只需要在截断处对齐字符边界） */
static size_t utf8_clip(const char *src, size_t n)
{
    if (n == 0) return 0;
    size_t i = n, back = 0;
    /* 跳过尾部的续字节（10xxxxxx），最多 3 个 */
    while (i > 0 && back < 3 && ((unsigned char)src[i - 1] & 0xC0) == 0x80) { i--; back++; }
    if (i == 0) return 0;
    unsigned char lead = (unsigned char)src[i - 1];
    size_t need = 1;
    if ((lead & 0x80) == 0x00) need = 1;
    else if ((lead & 0xE0) == 0xC0) need = 2;
    else if ((lead & 0xF0) == 0xE0) need = 3;
    else if ((lead & 0xF8) == 0xF0) need = 4;
    else return i - 1;                       /* 非法首字节，直接砍 */
    size_t have = n - (i - 1);               /* 这个字符已经拿到几个字节 */
    return (have < need) ? (i - 1) : n;      /* 不完整就丢掉整个字符 */
}

/* 把 src 的前 len 字节接到 dst，自动对齐 UTF-8 边界 */
static void copy_utf8(char *dst, size_t cap, const char *src, size_t len)
{
    size_t c = (len < cap - 1) ? len : (cap - 1);
    c = utf8_clip(src, c);
    memcpy(dst, src, c);
    dst[c] = 0;
}

/* ---------------- 气泡生命周期 ----------------
 * 气泡是「通知」，不是「状态」：每条消息都有寿命，到点自己淡出。
 * 这样不管服务端发不发空字符串，头顶都不会永远挂着一个框。
 *
 *   live  进行中（不过期，被下一条替换）   ok  完成 3.5s
 *   info  菜单/系统提示 2.6s              err 失败 9s，期间不被别的消息挤掉
 *
 * 同一段文字反复轮询到不会续命：只有文字变了才重置 at。
 */
static const int BUB_RANK[4] = { 0, 2, 2, 3 };   /* live < info == ok < err */

static int bubble_kind_of(const char *s)
{
    if (strcmp(s, "err") == 0) return BUB_ERR;
    if (strcmp(s, "ok") == 0) return BUB_OK;
    if (strcmp(s, "live") == 0) return BUB_LIVE;
    return BUB_INFO;
}

static int bub_alive(const bubble_t *b, uint64_t now)
{
    if (!b->text[0]) return 0;
    return !b->ttl || now < b->at + b->ttl;
}

static void bub_set(bubble_t *b, const char *text, int kind, uint32_t left,
                    const char *agent, const char *state, uint64_t now)
{
    if (!text || !text[0]) {
        /* 服务端说“没了”：不要瞬间消失，补一段淡出再收掉 */
        if (b->text[0] && !b->dying) {
            b->dying = 1;
            b->at = now;
            b->ttl = BUB_FADE_OUT_MS;
        } else if (!b->text[0]) {
            memset(b, 0, sizeof(*b));
        }
        return;
    }

    if (b->text[0] && strcmp(b->text, text) == 0) {
        /* 还是同一条消息：只跟着服务端补元信息，不重置 at、不重新淡入，
         * 否则轮询每 250ms 都会把它续命到永远 */
        if (b->dying) return;
        b->kind = kind;
        if (agent && agent[0]) copy_utf8(b->agent, sizeof(b->agent), agent, strlen(agent));
        if (state && state[0]) copy_utf8(b->state, sizeof(b->state), state, strlen(state));
        return;
    }
    /* 低优先级不顶掉还没过期的高优先级：「已置顶」不该盖住错误原因 */
    if (b->text[0] && bub_alive(b, now) && BUB_RANK[b->kind] > BUB_RANK[kind]) return;

    memset(b, 0, sizeof(*b));
    copy_utf8(b->text, sizeof(b->text), text, strlen(text));
    if (agent) copy_utf8(b->agent, sizeof(b->agent), agent, strlen(agent));
    if (state) copy_utf8(b->state, sizeof(b->state), state, strlen(state));
    b->kind = kind;
    b->at = now;
    b->ttl = left;
}

/* 本地的短暂提示（菜单操作、连接状态） */
static void bub_local(const char *text, int kind, uint32_t ttl)
{
    bub_set(&g_bub[1], text, kind, ttl, "", "", plat_ms());
}

static const bubble_t *bub_pick(uint64_t now)
{
    int ra = bub_alive(&g_bub[0], now), rb = bub_alive(&g_bub[1], now);
    if (ra && rb) {
        if (BUB_RANK[g_bub[1].kind] != BUB_RANK[g_bub[0].kind])
            return BUB_RANK[g_bub[1].kind] > BUB_RANK[g_bub[0].kind] ? &g_bub[1] : &g_bub[0];
        return g_bub[1].at >= g_bub[0].at ? &g_bub[1] : &g_bub[0];
    }
    if (rb) return &g_bub[1];
    if (ra) return &g_bub[0];
    return NULL;
}

/* 每帧算一次：画哪条 + 透明度 + 上飘位移 */
static void bub_tick(uint64_t now)
{
    const bubble_t *b = bub_pick(now);

    if (b && !g_bub_was_visible) { g_bub_shown_at = now; g_bub_was_visible = 1; }
    if (!b) {
        g_bub_was_visible = 0;
        g_bub_cur = NULL;
        g_bub_alpha = 0.0;
        g_bub_shift = 0;
        return;
    }

    double a = 1.0;
    int shift = 0;
    uint64_t age = now - g_bub_shown_at;
    if (age < BUB_FADE_IN_MS) {
        a = (double)age / BUB_FADE_IN_MS;
        shift = (int)((1.0 - a) * 4.0 + 0.5);          /* 从下往上浮进来 */
    }
    if (b->ttl) {
        uint64_t end = b->at + b->ttl;
        if (now >= end) a = 0.0;
        else if (end - now < BUB_FADE_OUT_MS) {
            double o = (double)(end - now) / BUB_FADE_OUT_MS;
            if (o < a) { a = o; shift = -(int)((1.0 - o) * 6.0 + 0.5); }   /* 向上飘走 */
        }
    }
    if (a < 0.004) a = 0.0;
    g_bub_cur = b;
    g_bub_alpha = a;
    g_bub_shift = shift;
}


static void draw_list(void)
{
    if (!g_list_open) return;
    int x, y, w, h;
    list_rect(&x, &y, &w, &h);

    /* 面板：投影 + 白底 + 1px 边 */
    for (int i = 6; i >= 1; i--)
        fill_round_rect(x - i, y - i + 1, w + i * 2, h + i * 2, 12 + i,
                        (uint32_t)(6 + (7 - i) * 2) << 24);
    fill_round_rect(x - 1, y - 1, w + 2, h + 2, 13, C_PANEL_BRD);
    fill_round_rect(x, y, w, h, 12, C_PANEL);

    /* ---- 标题栏 ---- */
    const int head_h = LHEAD_H;
    draw_text("会话", 15, C_INK, x + 14, y + 8, w - 90);

    /* 右上角：总数小胶囊（贴合文字宽度的圆角块） */
    char cnt[32];
    int running = 0;
    for (int i = 0; i < g_nsess; i++) if (state_running(g_sess[i].state)) running++;
    snprintf(cnt, sizeof(cnt), "%d", g_nsess);
    {
        int cw2 = 0, ch2 = 0;
        get_text(cnt, 12, running ? C_ACCENT : C_INK3, 40, &cw2, &ch2);
        int chipw = cw2 + 15, chiph = 20;
        int chipx = x + w - 12 - chipw, chipy = y + 8;
        fill_round_rect(chipx, chipy, chipw, chiph, 10,
                        running ? 0xFFEDEFFEu : 0xFFF3F3F7u);
        draw_text(cnt, 12, running ? C_ACCENT : C_INK3,
                  chipx + 7, chipy + (chiph - ch2) / 2, 40);
    }

    /* 分隔线 */
    fill_round_rect(x + 12, y + head_h - 1, w - 24, 1, 0, C_HEAD);

    /* ---- 行 ---- */
    if (g_nsess == 0) {
        draw_text("暂无会话", 14, C_INK3, x + 14, y + head_h + 4, w - 28);
        draw_text("发一句话就出现了", 12, C_INK3, x + 14, y + head_h + 28, w - 28);
        return;
    }

    int list_top = y + head_h;
    int nrow = list_row_count();
    for (int i = 0; i < nrow; i++) {
        sess_t *s = &g_sess[i];
        int ry = list_top + i * ROW_H;
        if (ry + ROW_H > y + h) break;

        int pinned = (strcmp(s->id, g_pinned) == 0);
        int hovered = (i == g_row_hover);

        if (pinned || hovered) {
            fill_round_rect(x + 6, ry + 1, w - 12, ROW_H - 3, 9,
                            pinned ? C_PIN : C_HOVER);
        }
        if (pinned) {
            /* 左侧强调条 */
            fill_round_rect(x + 6, ry + 8, 3, ROW_H - 19, 2, C_ACCENT);
        }

        uint32_t col = state_color(s->state);
        draw_dot(x + 24, ry + 18, col, 4, state_running(s->state));

        /* 第一行：agent（黑）+ 状态（彩） */
        const char *ag = s->agent[0] ? s->agent : "?";
        int ax = x + 36;
        int aw = draw_text(ag, 16, C_INK, ax, ry + 5, 100);
        draw_text(state_label(s->state), 13, col, ax + aw + 8, ry + 9, 78);

        /* 右上：耗时（有 × 时往左让一让） */
        int secs = s->elapsed / 1000;
        char tm[24];
        if (secs >= 60) snprintf(tm, sizeof(tm), "%dm%ds", secs / 60, secs % 60);
        else snprintf(tm, sizeof(tm), "%ds", secs);
        int cbx = 0, cby = 0;
        int has_close = close_btn_rect(i, &cbx, &cby);
        int trail = has_close ? (CLOSE_BTN + 10) : 0;
        draw_text_right(tm, 13, C_INK3, x + w - 12 - trail, ry + 8, 54);

        /* 行尾 ×：完成/失败的会话不自己消失，点这个手动关 */
        if (has_close) {
            int hot = (i == g_close_hover);
            if (hot)
                fill_round_rect(cbx - 2, cby - 2, CLOSE_BTN + 4, CLOSE_BTN + 4, 8, 0xFFF7DEDEu);
            uint32_t cc = hot ? 0xFFD84444u : 0xFFB9B9C6u;
            for (int d = 3; d < CLOSE_BTN - 3; d++) {
                int px1 = cbx + d, px2 = cbx + CLOSE_BTN - 1 - d, py = cby + d;
                for (int t = 0; t < 2; t++) {
                    int fy = py + t;
                    if (fy < 0 || fy >= WIN_H) continue;
                    if (px1 >= 0 && px1 < WIN_W)
                        g_fb[(size_t)fy * WIN_W + px1] = blend(g_fb[(size_t)fy * WIN_W + px1], cc);
                    if (px2 >= 0 && px2 < WIN_W)
                        g_fb[(size_t)fy * WIN_W + px2] = blend(g_fb[(size_t)fy * WIN_W + px2], cc);
                }
            }
        }

        /* 第二行：模型 · 轮次 · token */
        char l2[128], tok[24], mdl[40];
        fmt_num((long)s->tin + s->tout, tok, sizeof(tok));
        short_model(s->model[0] ? s->model : "-", mdl, sizeof(mdl));
        if (s->tin + s->tout > 0)
            snprintf(l2, sizeof(l2), "%s · %d轮 · %s", mdl, s->round_no, tok);
        else
            snprintf(l2, sizeof(l2), "%s · %d轮", mdl, s->round_no);
        draw_text(l2, 13, C_INK3, x + 36, ry + 27, w - 48);

        /* 行分隔（非最后一行） */
        if (i + 1 < nrow)
            fill_round_rect(x + 36, ry + ROW_H - 1, w - 48, 1, 0, 0x0D000000u);
    }

    /* ---- 页脚：汇总 token ---- */
    int tk = 0;
    for (int i = 0; i < g_nsess; i++) tk += g_sess[i].tin + g_sess[i].tout;
    if (nrow > 0) {
        char foot[96], ftok[24];
        fmt_num((long)tk, ftok, sizeof(ftok));
        if (tk > 0) snprintf(foot, sizeof(foot), "共 %s tok", ftok);
        else snprintf(foot, sizeof(foot), "%d 个进行中", running);
        if (g_nsess > nrow) {
            char more[32];
            snprintf(more, sizeof(more), "还有 %d 个", g_nsess - nrow);
            fill_round_rect(x + 12, y + h - LFOOT_H, w - 24, 1, 0, C_HEAD);
            draw_text(foot, 12, C_INK3, x + 14, y + h - LFOOT_H + 7, w - 80);
            draw_text_right(more, 12, C_INK3, x + w - 12, y + h - LFOOT_H + 7, 76);
        } else {
            fill_round_rect(x + 12, y + h - LFOOT_H, w - 24, 1, 0, C_HEAD);
            draw_text(foot, 12, C_INK3, x + 14, y + h - LFOOT_H + 7, w - 28);
        }
    }
}

/* 改布局：宽度变、高度不变，宠物屏幕位置保持不变 */
/* 菜单打开时窗口的最小宽度由 MENU_W + SUB_W 决定（见 layout_apply） */

/* 把窗口按当前状态（列表/菜单）重算，并把宠物摆回原处。
 * 锚点是「宠物左下角」：变布局/改尺寸时脚不动，不会上下飘。
 * 注意：这个坐标必须是「改标志位之前」取的。 */
static void layout_apply_at(plat_window *win, int pet_sx, int pet_bottom_sy)
{
    int nw;
    if (g_list_open) {
        nw = PET_W + LIST_W + 10;                    /* 列表贴着宠物 */
    } else if (g_menu_open) {
        /* 菜单是“主菜单 + 右侧子菜单”，窗口得比宠物宽得多；
         * 尺寸小时不能只靠宠物宽度，否则子菜单没地方摆会盖到主菜单上 */
        nw = PET_W + g_mx * 2;
        int need = MENU_W + SUB_W + 44;
        if (nw < need) nw = need;
    } else {
        nw = PET_W + g_mx * 2;
    }
    int nh = PET_H + 182;

    if (nw != WIN_W || nh != WIN_H) {
        free(g_fb);
        free(g_ui);
        WIN_W = nw; WIN_H = nh;
        g_fb = (uint32_t *)calloc((size_t)WIN_W * WIN_H, 4);
        g_ui = (uint32_t *)calloc((size_t)WIN_W * WIN_H, 4);
        if (!g_fb || !g_ui) { plat_error_box("MagpiePet", "内存不足"); exit(1); }
        plat_resize(win, nw, nh);
    }

    g_x = pet_sx - pet_local_x();
    g_y = pet_bottom_sy - (WIN_H - PET_MARGIN_BOTTOM);

    /* 变宽/变高后不能超出工作区，否则菜单/列表会被屏幕右边切掉。
     * 这一步会把宠物挪开（窗口贴边时无可奈何），但只在真的放不下时发生 */
    {
        int wx, wy, ww, wh;
        plat_workarea(&wx, &wy, &ww, &wh);
        if (g_x + WIN_W > wx + ww) g_x = wx + ww - WIN_W;
        if (g_x < wx) g_x = wx;
        if (g_y + WIN_H > wy + wh) g_y = wy + wh - WIN_H;
        if (g_y < wy) g_y = wy;
    }
    plat_move(win, g_x, g_y);
}

/* 宠物当前的屏幕坐标（左下角）——改任何布局标志位之前先取 */
static void pet_screen_pos(int *sx, int *bottom_sy)
{
    *sx = g_x + pet_local_x();
    *bottom_sy = g_y + WIN_H - PET_MARGIN_BOTTOM;
}

static void relayout(plat_window *win, int open)
{
    if (open == g_list_open) return;
    int sx, sy;
    pet_screen_pos(&sx, &sy);        /* 先取，再改标志位 */
    g_list_open = open;
    layout_apply_at(win, sx, sy);
}

/* 运行时缩放：1.00 = 图集格子原生像素 */
static void apply_scale(plat_window *win, double s)
{
    if (s < 0.3 || s > 4.0) return;
    int w = (int)(g_cw * s + 0.5);
    int h = (int)(g_ch * s + 0.5);
    if (w < 16 || h < 16) return;
    if (w == PET_W && h == PET_H) return;

    g_disp_scale = s;
    PET_W = w;
    PET_H = h;
    int sx, sy;
    pet_screen_pos(&sx, &sy);         /* 锚点是脚底：缩放时脚不动 */
    g_menu_open = 0;
    g_sub_show = 0;
    layout_apply_at(win, sx, sy);

    char msg[64];
    snprintf(msg, sizeof(msg), "大小已调到 %d%%", (int)(s * 100 + 0.5));
    bub_local(msg, BUB_INFO, 2200);
    plog("scale -> %.2f pet=%dx%d win=%dx%d", s, PET_W, PET_H, WIN_W, WIN_H);
}

/* ---------------- 渲染一帧 ---------------- */
/* 算当前该画图集的哪一格（含注视帧） */
static void compute_sprite_src(int *osx, int *osy)
{
    const char *tr = effective_track();
    int ti = track_index(tr);
    int sx = 0, sy = 0;
    int is_look = 0;

    if ((g_state == ST_IDLE || g_state == ST_WAITING) && g_atlas.rows >= 11) {
        uint64_t idle_ms = plat_ms() - g_last_event_at;
        if (idle_ms < LOOK_MIN_IDLE_MS || idle_ms > WAITING_AFTER_IDLE_MS) goto no_look;
        int cx, cy;
        plat_cursor(&cx, &cy);
        double dcx = (double)cx - (g_x + WIN_W / 2.0);
        double dcy = (double)cy - (g_y + WIN_H - PET_H / 2.0);
        if (dcx * dcx + dcy * dcy < LOOK_RADIUS * LOOK_RADIUS) {
            double ang = atan2(dcx, -dcy) * 180.0 / 3.14159265358979;
            if (ang < 0) ang += 360.0;
            if (look_rect(ang, &sx, &sy) == 0) is_look = 1;
        }
    }
no_look:

    if (!is_look) {
        int frame = 0;
        if (ti >= 0) {
            /* 完成/失败也会一直循环播放，直到用户把那条会话关掉 */
            frame = frame_at(ti, plat_ms() - g_state_at);
            g_cur_frame = frame;
            if (cell_rect(ti, frame, &sx, &sy) != 0) { sx = 0; sy = 0; }
        }
    }

    int pet_x = pet_local_x();
    int pet_y = WIN_H - PET_MARGIN_BOTTOM - PET_H;
    static int last_sx = -1, last_sy = -1;
    if (sx != last_sx || sy != last_sy) {
        last_sx = sx; last_sy = sy;
        plog("draw track=%s frame=%d src=(%d,%d) row=%d", effective_track(), g_cur_frame, sx, sy, sy / g_ch);
    }
    (void)pet_x; (void)pet_y;
    *osx = sx;
    *osy = sy;
}

/* 当前显示哪个会话：固定优先，否则第一行 */
/* 静态层：气泡 + 列表 + 菜单（不含精灵）。
 * 这一层只在内容变化时重画，每帧真正画的是「拷贝 + 精灵」。 */
static void render_static(void)
{
    int pet_x = pet_local_x();
    int pet_y = WIN_H - PET_MARGIN_BOTTOM - PET_H;
    memset(g_fb, 0, (size_t)WIN_W * WIN_H * 4);
    g_over_l = g_over_t = 0;                    /* 清空「盖到宠物」的区域 */
    g_over_r = g_over_b = 0;

    /* 2. 气泡：两行（状态行 + 内容行），与列表同一套配色。
     *    列表展开时不画：头顶再挂个框会互相遮挡，且列表里已经有全部信息。
     *    内容行只来自「有寿命的气泡」，没有就整块不画 —— 这是气泡能消失的关键。 */
    if (!g_list_open) {
        const bubble_t *bub = g_bub_cur;
        int is_local = (bub == &g_bub[1]);       /* 菜单/连接提示：没有身份，不画状态行 */

        char l2[256];
        l2[0] = 0;
        if (bub && bub->text[0]) copy_utf8(l2, sizeof(l2), bub->text, strlen(bub->text));

        if (l2[0]) {
            const int pad = 10;
            const int maxw = PET_W + 72;    /* 跟宠物走，不能按窗口宽（展开列表后会溢出） */

            /* 第一行：agent + 状态。身份跟着气泡走（同一条气泡可能来自第二行会话），
             * 没带身份的（系统提示）就只画内容行 */
            const char *who = "";
            const char *st = "";
            if (bub && !is_local && bub->agent[0]) {
                who = bub->agent;
                st = bub->state;
            }

            char l1[64];
            l1[0] = 0;
            int l1w = 0, l1h = 0;
            uint32_t l1col = st[0] ? state_color(st) : 0;
            if (who[0]) {
                copy_utf8(l1, sizeof(l1), who, strlen(who));
                get_text(l1, 15, C_INK, maxw - 60, &l1w, &l1h);
            }

            /* 第二行 */
            int l2w = 0, l2h = 0;
            uint32_t *t2 = get_text(l2, 14, C_INK2, maxw - pad * 2, &l2w, &l2h);

            int dotw = l1h > 0 ? 14 : 0;
            int bw = (l1w + dotw > l2w ? l1w + dotw : l2w) + pad * 2 + 8;
            if (bw < 72) bw = 72;
            if (bw > maxw) bw = maxw;
            int bh = pad * 2 + (l1h > 0 ? l1h + 4 : 0) + (l2h > 0 ? l2h : 0) + 4;

            int bx = pet_x + (PET_W - bw) / 2;
            if (bx < 4) bx = 4;
            if (bx + bw > WIN_W - 4) bx = WIN_W - 4 - bw;
            int by = pet_y - bh - 16 + g_bub_shift;   /* 淡出时向上飘 */
            if (by < 6) by = 6;

            /* 透明度：淡入淡出期间把整块气泡（含阴影）乘一个 alpha */
            uint32_t mul = (uint32_t)(g_bub_alpha * 255.0 + 0.5);
            if (mul > 255) mul = 255;

            if (mul > 0) {
                g_blend_mul = mul;
                ui_over_add(bx - 8, by - 8, bx + bw + 8, by + bh + 8);   /* 含阴影和尖角 */

                /* 投影 */
                for (int i = 6; i >= 1; i--)
                    fill_round_rect(bx - i, by - i + 1, bw + i * 2, bh + i * 2, 12 + i,
                                    (uint32_t)(6 + (7 - i) * 2) << 24);
                /* 边 + 底 */
                fill_round_rect(bx - 1, by - 1, bw + 2, bh + 2, 13, 0x1E000000u);
                fill_triangle(bx + bw / 2 - 6, by + bh - 2,
                              bx + bw / 2 + 6, by + bh - 2,
                              bx + bw / 2,     by + bh + 6, 0xFCFFFFFFu);
                fill_round_rect(bx, by, bw, bh, 12, 0xFCFFFFFFu);

                int ty = by + pad;
                if (l1h > 0) {
                    draw_dot(bx + pad + 5, ty + l1h / 2 + 1, l1col, 3, st[0] && state_running(st));
                    draw_text(l1, 15, C_INK, bx + pad + 14, ty, maxw - 70);
                    if (st[0])
                        draw_text(state_label(st), 13, l1col,
                                  bx + pad + 14 + l1w + 8, ty + 2, 78);
                    ty += l1h + 4;
                }
                if (t2) {
                    /* 重新取一次：上面两次 draw_text 可能把缓存槽挤掉（环形替换），
                     * 直接拖旧的 t2 就是 use-after-free */
                    t2 = get_text(l2, 13, C_INK2, maxw - pad * 2, &l2w, &l2h);
                }
                if (t2) {
                    for (int y = 0; y < l2h; y++) {
                        int fy = ty + y;
                        if (fy < by + 2 || fy >= by + bh - 2) continue;
                        for (int x = 0; x < l2w; x++) {
                            int fx = bx + pad + x;
                            if (fx < bx + 2 || fx >= bx + bw - 2) continue;
                            if (fx < 0 || fx >= WIN_W || fy < 0 || fy >= WIN_H) continue;
                            uint32_t p = t2[(size_t)y * l2w + x];
                            if (((p >> 24) & 0xFF) == 0) continue;
                            g_fb[(size_t)fy * WIN_W + fx] = blend(g_fb[(size_t)fy * WIN_W + fx], p);
                        }
                    }
                }
                g_blend_mul = 255;
            }
        }
    }

    /* 3. 会话列表 */
    draw_list();

    /* 4. 菜单（盖在最上面） */
    if (g_menu_open) draw_menu();
}

/* 每帧：拷回静态层 + 画精灵 + 把被盖住的 UI 区域盖回去 */
static void render_compose(void)
{
    size_t n = (size_t)WIN_W * WIN_H;
    if (g_ui) memcpy(g_fb, g_ui, n * 4);
    else memset(g_fb, 0, n * 4);

    int sx = 0, sy = 0;
    compute_sprite_src(&sx, &sy);
    int pet_x = pet_local_x();
    int pet_y = WIN_H - PET_MARGIN_BOTTOM - PET_H;
    draw_sprite(sx, sy, pet_x, pet_y, PET_W, PET_H, 1.0f);

    /* 气泡/菜单可能压在宠物身上，那部分得从静态层重新盖回去 */
    if (g_ui && g_over_r > g_over_l && g_over_b > g_over_t) {
        int l = g_over_l > pet_x ? g_over_l : pet_x;
        int t = g_over_t > pet_y ? g_over_t : pet_y;
        int r = g_over_r < pet_x + PET_W ? g_over_r : pet_x + PET_W;
        int b = g_over_b < pet_y + PET_H ? g_over_b : pet_y + PET_H;
        if (l < 0) l = 0;
        if (t < 0) t = 0;
        if (r > WIN_W) r = WIN_W;
        if (b > WIN_H) b = WIN_H;
        for (int y = t; y < b; y++) {
            size_t row = (size_t)y * WIN_W;
            for (int x = l; x < r; x++)
                g_fb[row + x] = blend(g_fb[row + x], g_ui[row + x]);
        }
    }
}

/* ---------------- HTTP 轮询 ---------------- */
#include "http_get.h"

typedef struct {
    int seq;
    char state[32];
    char phase[32];
    int active;
    char agent[64];
    char model[128];
    int round_no;
    char bubble[512];
    char bubble_kind[16];
    int bubble_left;        /* 还剩多久淡出（ms），0 = 不过期 */
    char bubble_agent[64];
    char bubble_state[32];
} pet_txt;



static void parse_kv(const char *body, pet_txt *out)
{
    memset(out, 0, sizeof(*out));
    out->seq = -1;
    g_nsess = 0;    const char *p = body;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        const char *eq = memchr(p, '=', len);
        if (eq) {
            size_t klen = (size_t)(eq - p);
            const char *v = eq + 1;
            size_t vlen = (size_t)(p + len - v);
            if (klen == 3 && strncmp(p, "seq", 3) == 0) { out->seq = atoi(v); }
            else if (klen == 5 && strncmp(p, "state", 5) == 0) {
                copy_utf8(out->state, sizeof(out->state), v, vlen);
            } else if (klen == 5 && strncmp(p, "phase", 5) == 0) {
                copy_utf8(out->phase, sizeof(out->phase), v, vlen);
            } else if (klen == 6 && strncmp(p, "active", 6) == 0) {
                out->active = atoi(v);
            } else if (klen == 5 && strncmp(p, "agent", 5) == 0) {
                copy_utf8(out->agent, sizeof(out->agent), v, vlen);
            } else if (klen == 5 && strncmp(p, "model", 5) == 0) {
                copy_utf8(out->model, sizeof(out->model), v, vlen);
            } else if (klen == 5 && strncmp(p, "round", 5) == 0) {
                out->round_no = atoi(v);
            } else if (klen == 6 && strncmp(p, "bubble", 6) == 0) {
                copy_utf8(out->bubble, sizeof(out->bubble), v, vlen);
            } else if (klen == 11 && strncmp(p, "bubble_kind", 11) == 0) {
                copy_utf8(out->bubble_kind, sizeof(out->bubble_kind), v, vlen);
            } else if (klen == 11 && strncmp(p, "bubble_left", 11) == 0) {
                out->bubble_left = atoi(v);
            } else if (klen == 12 && strncmp(p, "bubble_agent", 12) == 0) {
                copy_utf8(out->bubble_agent, sizeof(out->bubble_agent), v, vlen);
            } else if (klen == 12 && strncmp(p, "bubble_state", 12) == 0) {
                copy_utf8(out->bubble_state, sizeof(out->bubble_state), v, vlen);
            } else if (klen == 1 && p[0] == 's' && vlen > 2) {
                /* s=<id>|<agent>|<model>|<state>|<round>|<elapsed>|<tin>|<tout>|<credits>|<query> */
                if (g_nsess < (int)(sizeof(g_sess) / sizeof(g_sess[0]))) {
                    sess_t *ss = &g_sess[g_nsess];
                    memset(ss, 0, sizeof(*ss));
                    char tmp[512];
                    size_t c = vlen < sizeof(tmp) - 1 ? vlen : sizeof(tmp) - 1;
                    c = utf8_clip(v, c);
                    memcpy(tmp, v, c); tmp[c] = 0;

                    char *f[10]; int nf = 0;
                    char *cur = tmp;
                    while (nf < 10) {
                        f[nf++] = cur;
                        char *bar = strchr(cur, '|');
                        if (!bar) break;
                        *bar = 0;
                        cur = bar + 1;
                    }
                    if (nf >= 9) {
                        copy_utf8(ss->id, sizeof(ss->id), f[0], strlen(f[0]));
                        copy_utf8(ss->agent, sizeof(ss->agent), f[1], strlen(f[1]));
                        copy_utf8(ss->model, sizeof(ss->model), f[2], strlen(f[2]));
                        copy_utf8(ss->state, sizeof(ss->state), f[3], strlen(f[3]));
                        ss->round_no = atoi(f[4]);
                        ss->elapsed = atoi(f[5]);
                        ss->tin = atoi(f[6]);
                        ss->tout = atoi(f[7]);
                        ss->credits = atof(f[8]);
                        if (nf >= 10) copy_utf8(ss->query, sizeof(ss->query), f[9], strlen(f[9]));
                        g_nsess++;
                    }
                }
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
}

/* 删掉某个会话（桌宠上点 × 时调）—— 只影响显示，不影响真实请求。
 * 走 GET 而不是 POST，是为了复用现成的 http_get，不用再写一个 HTTP 客户端 */
static void dismiss_session(const char *id)
{
    if (!id) return;
    char enc[160], path[200], buf[32];
    size_t j = 0;
    for (const char *p = id; *p && j + 4 < sizeof(enc); p++) {
        unsigned char c = (unsigned char)*p;
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
            (c >= 'A' && c <= 'Z') || c == '-' || c == '_')
            enc[j++] = (char)c;
        else
            j += (size_t)snprintf(enc + j, sizeof(enc) - j, "%%%02X", c);
    }
    enc[j] = 0;
    snprintf(path, sizeof(path), "/dismiss?id=%s", enc);
    if (http_get(g_host, g_port, path, buf, sizeof(buf)) != 0)
        plog("dismiss failed id=%s", id);
}

static void set_state(pet_state s)
{
    if (s == g_state) return;
    g_state = s;
    g_state_at = plat_ms();
    g_cur_frame = -1;
}

/* 从会话表算姿态：跑着的 > 没关掉的完成项 > 等待 */
static pet_state pose_from_sessions(void)
{
    int any_tool = 0, any_think = 0, any_fail = 0, any_done = 0;
    for (int i = 0; i < g_nsess; i++) {
        const char *st = g_sess[i].state;
        if (strcmp(st, "tool") == 0) any_tool = 1;
        else if (strcmp(st, "thinking") == 0) any_think = 1;
        else if (strcmp(st, "failed") == 0) any_fail = 1;
        else if (strcmp(st, "done") == 0) any_done = 1;
    }
    if (any_tool) return ST_BUSY;
    if (any_think) return ST_THINKING;
    if (any_fail) return ST_FAILED;
    if (any_done) return ST_DONE;
    return ST_WAITING;
}

static void apply(const pet_txt *t)
{
    if (t->seq != g_seq) {
        g_seq = t->seq;
        g_last_event_at = plat_ms();
        g_active = t->active;
        copy_utf8(g_agent, sizeof(g_agent), t->agent, strlen(t->agent));
        copy_utf8(g_model, sizeof(g_model), t->model, strlen(t->model));
        g_round_no = t->round_no;
    }

    /* 气泡跟着每次轮询同步：服务端会带寿命（bubble_left），过期就是空。
     * 旧内核只给 bubble 文本、而且完成/失败后不清空 —— 有人正在跑就按 live
     * 处理（不过期），否则当普通提示给 3.5s 寿命，不会再永远挂着。 */
    {
        int kind = bubble_kind_of(t->bubble_kind);
        uint32_t left = (uint32_t)t->bubble_left;
        if (!t->bubble_kind[0]) {
            int running = 0;
            for (int i = 0; i < g_nsess; i++)
                if (state_running(g_sess[i].state)) { running = 1; break; }
            if (running) kind = BUB_LIVE;
        }
        if (left == 0 && kind != BUB_LIVE) left = 3500;   /* 旧内核没给寿命：给个默认 */
        bub_set(&g_bub[0], t->bubble, kind, left,
                t->bubble_agent, t->bubble_state, plat_ms());
    }

    /* 姿态不问内核要，直接看会话表：
     *   有 tool → 干活；有 thinking → 思考；
     *   有没关掉的完成/失败 → 完成；列表空了 → 等待。
     * 这样内核新旧都能对（旧内核会在完成后 4 秒回 idle），
     * 而且本地点 x 关掉一条会话后也能立刻反映。 */
    set_state(pose_from_sessions());
    g_core_state = g_state;                 /* 被摸到时临时插播，过后恢复这个 */
}

/* ---------------- 按需重绘 ----------------
 * 以前是每 16ms 无条件重画+上传整个窗口，即使画面完全没变。
 * 这里算一个「画面签名」，只有变了才重新合成+刷屏；
 * 实测空闲 CPU 从 26% 降到 2% 左右。 */
static uint64_t g_last_fs = 0, g_last_us = 0;
static uint64_t g_force_redraw = 1;   /* 重载素材等一次性事件用 */

static uint64_t hash_add(uint64_t h, uint64_t v)
{
    h ^= v;
    h *= 1099511628211ull;
    return h;
}

static uint64_t hash_str(uint64_t h, const char *s)
{
    if (!s) return hash_add(h, 0x9E37u);
    for (; *s; s++) h = hash_add(h, (unsigned char)*s);
    return hash_add(h, 0x85EBu);
}

/* 只跟动画帧有关的部分（变了就只用重传宠物那块） */
static uint64_t sig_frame_part(void)
{
    uint64_t h = 1469598103934665603ull;
    /* 当前帧号要自己算：g_cur_frame 只在 render() 里赋值，
     * 直接读它永远拿到上一帧的值 → 动画会冻住 */
    int ti = track_index(effective_track());
    if (ti >= 0) {
        int fr = frame_at(ti, plat_ms() - g_state_at);
        h = hash_add(h, (uint64_t)fr);
    }
    /* 注视帧跟光标位置有关（只有 Codex 多轨图集才有） */
    if (g_atlas.rows >= 11 && (g_state == ST_IDLE || g_state == ST_WAITING) && !g_list_open && !g_menu_open) {
        int cx, cy;
        plat_cursor(&cx, &cy);
        h = hash_add(h, (uint64_t)(cx / 4 * 100003 + cy / 4));
    }
    return h;
}

/* 除动画帧外的一切：气泡/列表/菜单/尺寸… */
static uint64_t sig_ui_part(void)
{
    uint64_t h = 1469598103934665603ull;
    h = hash_add(h, (uint64_t)g_state);
    h = hash_add(h, (uint64_t)g_seq);
    h = hash_add(h, (uint64_t)g_nsess);
    h = hash_add(h, (uint64_t)g_active);
    h = hash_add(h, (uint64_t)(g_list_open | (g_row_hover << 1) | (g_close_hover << 8)));
    h = hash_add(h, (uint64_t)(g_menu_open | (g_menu_hover << 1) | (g_sub_show << 8) | (g_sub_hover << 9)));
    h = hash_add(h, (uint64_t)(g_topmost | (g_click_through_all << 1)));
    h = hash_add(h, (uint64_t)(WIN_W * 7919u + WIN_H));
    h = hash_add(h, (uint64_t)(PET_W * 7919u + PET_H));
    h = hash_str(h, g_pinned);
    /* 气泡：内容 + 身份 + 淡入淡出进度。不把 alpha/位移算进去的话，
     * 淡出期间内容没变就不会重画，气泡会“卡”在半透明状态 */
    h = hash_str(h, g_bub_cur ? g_bub_cur->text : "");
    h = hash_str(h, g_bub_cur ? g_bub_cur->agent : "");
    h = hash_str(h, g_bub_cur ? g_bub_cur->state : "");
    h = hash_add(h, (uint64_t)(int)(g_bub_alpha * 1000.0 + 0.5));
    h = hash_add(h, (uint64_t)(g_bub_shift + 64));
    h = hash_str(h, g_agent);
    h = hash_str(h, g_model);
    for (int i = 0; i < g_nsess; i++) {
        const sess_t *s = &g_sess[i];
        h = hash_str(h, s->id);
        h = hash_str(h, s->agent);
        h = hash_str(h, s->model);
        h = hash_str(h, s->state);
        h = hash_str(h, s->query);
        h = hash_add(h, (uint64_t)s->round_no);
        h = hash_add(h, (uint64_t)(s->elapsed / 1000));   /* 列表里的秒数 */
        h = hash_add(h, (uint64_t)(s->tin + s->tout));
    }
    return h;
}
static int load_position(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int x = 0, y = 0;
    double s = 0.0;
    int n = fscanf(f, "%d,%d,%lf", &x, &y, &s);
    fclose(f);
    if (n < 2) return 0;
    g_x = x;
    g_y = y;
    if (n >= 3 && s >= 0.3 && s <= 4.0) g_disp_scale = s;
    return 1;
}

static void save_position(const char *path)
{
    if (!path || !path[0]) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%d,%d,%.2f\n", g_x, g_y, g_disp_scale);
    fclose(f);
}

/* 只读位置文件里的缩放值（启动时算 PET_W/PET_H 要用，那时还没建窗口） */
static double saved_scale(const char *path)
{
    if (!path || !path[0]) return 0.0;
    FILE *f = fopen(path, "r");
    if (!f) return 0.0;
    int x = 0, y = 0;
    double s = 0.0;
    int n = fscanf(f, "%d,%d,%lf", &x, &y, &s);
    fclose(f);
    return (n >= 3 && s >= 0.3 && s <= 4.0) ? s : 0.0;
}

/* ---------------- 单实例锁 ----------------
 * 文件里写自己的 pid；拿锁时用 "wx"（独占创建）保证原子。
 * 占着锁的进程还活着就不启动；已死/是自己的 pid 就当脏数据覆盖。
 * 热重载时旧实例可能正在退出，所以最多等 2 秒再放弃。 */
static char g_lock_path[600];
static int g_lock_pid = 0;

static void lock_release(void)
{
    if (!g_lock_path[0]) return;
    FILE *f = fopen(g_lock_path, "r");
    int pid = 0;
    if (f) { if (fscanf(f, "%d", &pid) != 1) pid = 0; fclose(f); }
    if (pid == g_lock_pid) remove(g_lock_path);      /* 只删自己的，别误删新实例的 */
}

static int lock_try(const char *path)
{
    if (!plat_create_exclusive(path)) return 0;      /* 已存在就是别人拿了 */
    FILE *f = fopen(path, "w");
    if (f) { fprintf(f, "%d\n", plat_self_pid()); fclose(f); }
    snprintf(g_lock_path, sizeof(g_lock_path), "%s", path);
    g_lock_pid = plat_self_pid();
    atexit(lock_release);
    return 1;
}

static int lock_acquire(const char *path)
{
    for (int i = 0; i < 10; i++) {
        if (lock_try(path)) return 1;

        FILE *f = fopen(path, "r");
        int pid = 0;
        if (!f) return 1;                            /* 读不到：别因为锁把宠物挡住 */
        if (fscanf(f, "%d", &pid) != 1) pid = 0;
        fclose(f);

        if (pid > 0 && pid != plat_self_pid() && plat_pid_alive(pid)) {
            plat_sleep(200);                         /* 可能是旧实例正在退出，等一等 */
            continue;
        }
        remove(path);                                /* 脏锁：删掉重抢 */
    }
    fprintf(stderr, "pet: 已有实例在跑，本次不启动\n");
    return 0;
}

static int atlas_load(const char *path);

int main(int argc, char **argv)
{
    /* 先把调试日志和速度参数初始化好，后面的初始化过程才能被记录 */
    plat_install_crash_handler();
    {
        const char *lg = getenv("PET_DEBUG");
        if (lg && lg[0]) { g_log = fopen(lg, "w"); g_trace = 1; if (g_log) setvbuf(g_log, NULL, _IONBF, 0); }
    }
    {
        const char *sp = getenv("PET_SPEED");
        if (sp && sp[0]) {
            double v = atof(sp);
            if (v > 0.1 && v < 10.0) g_speed = v;
        }
    }

    const char *sheet = "pets/deepseek-girl/spritesheet.png";
    const char *host = "127.0.0.1";
    int port = 8789;
    const char *posfile = "pet-pos.txt";
    const char *lockfile = NULL;
    int parent_pid = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--sheet") == 0 && i + 1 < argc) sheet = argv[++i];
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) port = atoi(argv[++i]);
        else if (strcmp(argv[i], "--host") == 0 && i + 1 < argc) host = argv[++i];
        else if (strcmp(argv[i], "--pos") == 0 && i + 1 < argc) posfile = argv[++i];
        else if (strcmp(argv[i], "--lock") == 0 && i + 1 < argc) lockfile = argv[++i];
        else if (strcmp(argv[i], "--parent") == 0 && i + 1 < argc) parent_pid = atoi(argv[++i]);
        else if (strcmp(argv[i], "--help") == 0) {
            printf("pet [--sheet file.png] [--host 127.0.0.1] [--port 8789] [--pos pet-pos.txt]\n"
                   "    [--lock pet.lock]  单实例锁，已有实例在跑就退出\n"
                   "    [--parent <pid>]   父进程没了就自动退出（插件拉起时用）\n");
            return 0;
        }
    }

    /* 单实例：插件热重载/手动重复启动时不会同时出现两只 */
    if (lockfile && !lock_acquire(lockfile)) return 0;

    snprintf(g_host, sizeof(g_host), "%s", host);
    g_port = port;

    if (atlas_load(sheet) != 0) {
        char msg[600];
        snprintf(msg, sizeof(msg), "读不了图集：\n%s\n\n原因：%s\n\n检查工作目录是否在 magpie-pet 下。",
                 sheet, stbi_failure_reason());
        plat_error_box("MagpiePet 启动失败", msg);
        return 1;
    }
    snprintf(g_sheet_path, sizeof(g_sheet_path), "%s", sheet);

    /* 同目录下有 layout.txt 就用它；否则用内置的 Codex 9 轨规格 */
    {
        char layout[600];
        snprintf(layout, sizeof(layout), "%s", sheet);
        char *slash = strrchr(layout, '/');
        if (!slash) slash = strrchr(layout, '\\');
        if (slash) {
            snprintf(slash + 1, sizeof(layout) - (size_t)(slash + 1 - layout), "layout.txt");
            int n = rt_init_from_layout(layout);
            plog("layout %s -> %d tracks (cell %dx%d)", layout, n, g_cw, g_ch);
            if (n <= 0) rt_init_builtin();
        } else {
            rt_init_builtin();
        }
        if (g_nrt == 0) rt_init_builtin();
        g_atlas.rows = g_atlas.h / g_ch;
        for (int i = 0; i < g_nrt; i++)
            plog("  track[%d] %s row=%d frames=%d total=%d", i, g_rt[i].name, g_rt[i].row, g_rt[i].frames, g_rt[i].total);
    }

    /* 按图集格尺寸算宠物与窗口尺寸；1.0 = 原生像素（不缩放 → 不会糊） */
    {
        const char *sc = getenv("PET_SCALE");
        if (sc && sc[0]) {
            double v = atof(sc);
            if (v >= 0.3 && v <= 4.0) g_disp_scale = v;
        } else {
            /* 没显式指定就沿用上次在菜单里调的尺寸 */
            double sv = saved_scale(posfile);
            if (sv > 0) g_disp_scale = sv;
        }
        PET_W = (int)(g_cw * g_disp_scale + 0.5);
        PET_H = (int)(g_ch * g_disp_scale + 0.5);
        WIN_W = PET_W + 110;    /* 两行气泡需要的宽度 */
        WIN_H = PET_H + 182;    /* 上方气泡 + 5 行列表（46px 行高） */
        MENU_W = WIN_W - 10;
        if (MENU_W > 210) MENU_W = 210;
        if (MENU_W < 130) MENU_W = 130;
        g_mx = (WIN_W - PET_W) / 2;
        plog("size: cell=%dx%d scale=%.2f -> pet=%dx%d win=%dx%d", g_cw, g_ch, g_disp_scale, PET_W, PET_H, WIN_W, WIN_H);
    }

    g_fb = (uint32_t *)calloc((size_t)WIN_W * WIN_H, 4);
    g_ui = (uint32_t *)calloc((size_t)WIN_W * WIN_H, 4);
    if (!g_fb || !g_ui) return 1;

    plat_config cfg = {WIN_W, WIN_H, 1};
    plat_window *win = plat_create(&cfg, "MagpiePet");
    if (!win) { fprintf(stderr, "pet: 创建窗口失败\n"); return 1; }

    if (load_position(posfile)) plat_move(win, g_x, g_y);
    else {
        int wx, wy, ww, wh;
        plat_workarea(&wx, &wy, &ww, &wh);
        g_x = wx + ww - (PET_W + g_mx * 2) - 24;
        g_y = wy + wh - WIN_H - 8;
        plat_move(win, g_x, g_y);
    }
    /* 窗口高度/分辨率可能变了，把窗口拉回工作区（否则宠物脚被任务栏切掉） */
    {
        int wx, wy, ww, wh;
        plat_workarea(&wx, &wy, &ww, &wh);
        if (g_x + WIN_W > wx + ww) g_x = wx + ww - WIN_W;
        if (g_x < wx) g_x = wx;
        if (g_y + WIN_H > wy + wh) g_y = wy + wh - WIN_H;
        if (g_y < wy) g_y = wy;
        plat_move(win, g_x, g_y);
    }

    g_last_event_at = plat_ms();
    uint64_t next_poll = 0;
    uint64_t next_parent_check = 0;
    int fail_streak = 0;
    char body[4096];

    while (!g_quit) {
        uint64_t now = plat_ms();

        /* 父进程看门狗：magpie 的插件宿主没了就收工（每秒查一次） */
        if (parent_pid > 0 && now >= next_parent_check) {
            next_parent_check = now + 1000;
            if (!plat_pid_alive(parent_pid)) {
                plog("parent %d gone, exiting", parent_pid);
                break;
            }
        }

        /* 轮询 */
        if (now >= next_poll) {
            next_poll = now + POLL_MS;
            if (http_get(host, port, "/pet.txt", body, sizeof(body)) == 0) {
                if (fail_streak >= 20) {              /* 刚好重连上：把断线提示收掉 */
                    memset(&g_bub[1], 0, sizeof(g_bub[1]));
                    bub_local("已重新连接", BUB_OK, 2200);
                }
                fail_streak = 0;
                pet_txt t;
                parse_kv(body, &t);
                apply(&t);
            } else {
                fail_streak++;
                if (fail_streak >= 20 && !bub_alive(&g_bub[1], now))
                    bub_local("未连接事件服务", BUB_ERR, 60000);   /* 断线期间每分钟重新提醒一次 */
            }
        }

        /* 自动回退 */
        if (g_pet_reaction_until && now > g_pet_reaction_until) {
            g_pet_reaction_until = 0;
            set_state(g_core_state);      /* 摸完回到内核算好的姿态 */
        }

        /* 事件 */
        plat_event ev;
        while (plat_poll(win, &ev)) {
            plog("event kind=%d", (int)ev.kind);
            switch (ev.kind) {
            case PKE_LEFT_DOWN:
                g_drag = 1;
                g_drag_moved = 0;
                break;
            case PKE_DRAG_START:
                g_drag_moved = 1;
                break;
            case PKE_DRAG_END:
                plat_get_pos(win, &g_x, &g_y);
                save_position(posfile);
                g_drag = 0;
                break;
            case PKE_RIGHT_DOWN: {
                /* 先收起列表（菜单是按宠物居中的）；子菜单也一并收起。
                 * 宠物屏幕位置必须在改标志位之前取，否则窗口一变宽宠物就跳 */
                int sx, sy;
                pet_screen_pos(&sx, &sy);
                g_list_open = 0;
                g_pinned[0] = 0;
                g_menu_open = !g_menu_open;
                g_sub_show = 0;
                g_menu_hover = -1;
                g_sub_hover = -1;
                g_menu_leave_since = 0;
                g_hover_since = 0;                          /* 菜单关掉后重新计 250ms */
                g_leave_since = 0;
                plat_set_drag_enabled(win, !g_menu_open);   /* 菜单开着就不允许拖 */
                layout_apply_at(win, sx, sy);               /* 菜单需要更宽的窗口来摆子菜单 */
                break;
            }

            case PKE_TOGGLE_CLICKTHRU:
                g_click_through_all = !g_click_through_all;
                plat_set_click_through_all(win, g_click_through_all);
                if (g_click_through_all) {
                    g_click_through_until = plat_ms() + 30000;   /* 30s 自动恢复，避免彻底失控 */
                    const char *hk = plat_hotkey_name(win);
                    char msg[160];
                    if (hk && hk[0])
                        snprintf(msg, sizeof(msg), "点击穿透 30 秒后自动恢复 · %s 可立刻关闭", hk);
                    else
                        snprintf(msg, sizeof(msg), "点击穿透 30 秒后自动恢复");
                    bub_local(msg, BUB_INFO, 3200);
                } else {
                    g_click_through_until = 0;
                    bub_local("已关闭点击穿透", BUB_INFO, 2000);
                }
                break;

            case PKE_LEFT_UP: {
                int lx = 0, ly = 0;
                if (g_menu_open) {
                    /* 菜单打开时：先看子菜单，再看主菜单，都不是就关闭 */
                    int cx, cy;
                    plat_cursor(&cx, &cy);
                    lx = cx - g_x;
                    ly = cy - g_y;
                    int sub = sub_hit(lx, ly);
                    int hit = menu_hit(lx, ly);
                    plog("menu click lx=%d ly=%d hit=%d sub=%d", lx, ly, hit, sub);

                    if (sub >= 0) {
                        /* 选了具体尺寸：应用后整个菜单关掉 */
                        g_menu_open = 0;
                        g_sub_show = 0;
                        plat_set_drag_enabled(win, 1);
                        apply_scale(win, SUB_SCALE[sub]);
                        g_drag = 0;
                        break;
                    }
                    if (hit == MENU_IDX_SCALE) {
                        /* 点「调整大小」只展开/收起子菜单，主菜单不关 */
                        g_sub_show = !g_sub_show;
                        g_drag = 0;
                        break;
                    }

                    {
                        int sx, sy;
                        pet_screen_pos(&sx, &sy);     /* 先取宠物位置 */
                        g_menu_open = 0;
                        g_sub_show = 0;
                        plat_set_drag_enabled(win, 1);
                        layout_apply_at(win, sx, sy); /* 菜单关了，窗口收回原宽 */
                    }
                    if (hit >= 0) {
                        switch (hit) {
                        case 0:
                            g_topmost = !g_topmost;
                            plat_set_topmost(win, g_topmost);
                            bub_local(g_topmost ? "已置顶" : "已取消置顶", BUB_INFO, 1800);
                            break;
                        case 1:
                            g_click_through_all = !g_click_through_all;
                            plog("menu item1 clickthru=%d", g_click_through_all);
                            plat_set_click_through_all(win, g_click_through_all);
                            g_click_through_until = g_click_through_all ? plat_ms() + 30000 : 0;
                            bub_local(g_click_through_all ? "点击穿透已开（30s 后自动恢复）" : "已关闭点击穿透",
                                      BUB_INFO, 2600);
                            break;
                        case 3:
                            if (atlas_load(g_sheet_path) == 0) {
                                g_force_redraw = 1;
                                bub_local("素材已重载", BUB_OK, 2200);
                            } else
                                bub_local("重载失败", BUB_ERR, 5000);
                            break;
                        case 4: {
                            int wx, wy, ww, wh;
                            plat_workarea(&wx, &wy, &ww, &wh);
                            g_x = wx + ww - (PET_W + g_mx * 2) - 24;   /* 按折叠宽度算，给气泡留位 */
                            g_y = wy + wh - WIN_H - 8;
                            plat_move(win, g_x, g_y);
                            save_position(posfile);
                            bub_local("已回到右下角", BUB_INFO, 1800);
                            break;
                        }
                        case 5:
                            g_quit = 1;
                            break;
                        default:
                            break;
                        }
                    }
                } else if (g_list_open) {
                    /* 列表打开：点 × = 关闭该会话；点行 = 固定/取消固定的详情；点宠物 = 摸一下 */
                    int cx, cy;
                    plat_cursor(&cx, &cy);
                    int lx2 = cx - g_x, ly2 = cy - g_y;
                    int cbtn = close_btn_at(lx2, ly2);
                    int r = list_row_at(lx2, ly2);
                    plog("list click lx=%d ly=%d row=%d close=%d nsess=%d", lx2, ly2, r, cbtn, g_nsess);
                    if (cbtn >= 0) {
                        char gone[48];
                        snprintf(gone, sizeof(gone), "%s", g_sess[cbtn].id);
                        plog("dismiss row=%d id=%s", cbtn, gone);
                        dismiss_session(gone);
                        /* 本地也立刻拿掉，不等下一次轮询 */
                        if (strcmp(g_pinned, gone) == 0) g_pinned[0] = 0;
                        for (int k = cbtn; k + 1 < g_nsess; k++) g_sess[k] = g_sess[k + 1];
                        if (g_nsess > 0) g_nsess--;
                        memset(&g_sess[g_nsess], 0, sizeof(g_sess[0]));
                        g_row_hover = -1;
                        g_close_hover = -1;
                        set_state(pose_from_sessions());   /* 关完立刻反映到姿态 */
                        g_core_state = g_state;
                    } else if (r >= 0) {
                        if (strcmp(g_sess[r].id, g_pinned) == 0) g_pinned[0] = 0;
                        else snprintf(g_pinned, sizeof(g_pinned), "%s", g_sess[r].id);
                        plog("pin row=%d id=%s", r, g_pinned);
                    } else if (!g_drag_moved) {
                        int px2 = pet_local_x(), py2 = WIN_H - PET_MARGIN_BOTTOM - PET_H;
                        if (pt_in(lx2, ly2, px2, py2, PET_W, PET_H)) {
                            set_state(ST_DONE);
                            g_pet_reaction_until = plat_ms() + PET_REACT_MS;
                            bub_local("被摸到了～", BUB_INFO, 1500);
                        }
                    }
                } else if (!g_drag_moved) {
                    set_state(ST_DONE);
                    g_pet_reaction_until = plat_ms() + PET_REACT_MS;
                    bub_local("被摸到了～", BUB_INFO, 1500);
                }
                g_drag = 0;
                break;
            }
            case PKE_QUIT:
                g_quit = 1;
                break;
            default:
                break;
            }
            plat_get_pos(win, &g_x, &g_y);
        }

        /* 点击穿透超时自动恢复（安全网） */
        if (g_click_through_all && g_click_through_until && now > g_click_through_until) {
            g_click_through_all = 0;
            g_click_through_until = 0;
            plat_set_click_through_all(win, 0);
            bub_local("点击穿透已自动恢复", BUB_INFO, 2600);
            plog("auto-restore clickthru");
        }

        /* 菜单悬停高亮 + 子菜单展开（全用光标位置算，不依赖移动事件） */
        if (g_menu_open) {
            int cx, cy;
            plat_cursor(&cx, &cy);
            int mlx = cx - g_x, mly = cy - g_y;
            g_menu_hover = menu_hit(mlx, mly);
            int sh = sub_hit(mlx, mly);
            /* 悬在「调整大小」或已在子菜单里 → 展开；离开就收起 */
            g_sub_show = (g_menu_hover == MENU_IDX_SCALE || sh >= 0) ? 1 : 0;
            g_sub_hover = sh;

            /* 鼠标离开菜单（去干别的了）→ MENU_AUTO_CLOSE_MS 后自己关掉，
             * 不让它一直挂在屏幕上 */
            if (g_menu_hover >= 0 || sh >= 0) {
                g_menu_leave_since = 0;
            } else {
                if (!g_menu_leave_since) g_menu_leave_since = now;
                if (now - g_menu_leave_since > MENU_AUTO_CLOSE_MS) {
                    int psx, psy;
                    pet_screen_pos(&psx, &psy);   /* 先取锚点再改标志位 */
                    g_menu_open = 0;
                    g_sub_show = 0;
                    g_menu_hover = -1;
                    g_sub_hover = -1;
                    g_menu_leave_since = 0;
                    plat_set_drag_enabled(win, 1);
                    layout_apply_at(win, psx, psy);
                    plog("menu auto-closed after %d ms away", MENU_AUTO_CLOSE_MS);
                }
            }
        } else {
            g_menu_hover = -1;
            g_sub_hover = -1;
            g_sub_show = 0;
            g_menu_leave_since = 0;
        }

        /* ---- 会话列表：悬停展开 / 离开收起 ---- */
        {
            int wx, wy, ww, wh;
            plat_workarea(&wx, &wy, &ww, &wh);
            int pet_sx = g_x + pet_local_x();          /* 不随展开变化 */
            if (!g_list_open)
                g_list_left = (pet_sx + PET_W / 2) > (wx + ww / 2);

            int hov = hovered_now();
            if (g_trace) {
                static uint64_t last_trace = 0;
                if (now - last_trace > 400) {
                    last_trace = now;
                    int cx, cy; plat_cursor(&cx, &cy);
                    trace("now=%llu gx=%d gy=%d win=%dx%d cur=(%d,%d) l=(%d,%d) pet_x=%d hov=%d open=%d pin=%d leave=%llu",
                          (unsigned long long)now, g_x, g_y, WIN_W, WIN_H, cx, cy,
                          cx - g_x, cy - g_y, pet_local_x(), hov, g_list_open, g_pinned[0] != 0,
                          (unsigned long long)g_leave_since);
                }
            }

            if (g_menu_open) {
                /* 菜单开着：冻结列表展开。否则窗口一变宽，菜单跟着移位，点击全打偏 */
                g_hover_since = 0;
                g_leave_since = 0;
            } else if (hov) {
                g_leave_since = 0;
                if (!g_hover_since) g_hover_since = now;
                if (!g_list_open && now - g_hover_since > 250) relayout(win, 1);
            } else {
                g_hover_since = 0;
                if (g_list_open) {
                    if (!g_leave_since) g_leave_since = now;
                    uint64_t wait = g_pinned[0] ? 2500 : 700;
                    if (now - g_leave_since > wait) { g_pinned[0] = 0; relayout(win, 0); }
                }
            }

            /* 行悬停高亮 */
            g_row_hover = -1;
            g_close_hover = -1;
            if (g_list_open) {
                int cx, cy;
                plat_cursor(&cx, &cy);
                g_row_hover = list_row_at(cx - g_x, cy - g_y);
                g_close_hover = close_btn_at(cx - g_x, cy - g_y);
            }
        }

        if (g_force_redraw) { g_force_redraw = 0; g_last_fs = g_last_us = 0; }
        bub_tick(now);                       /* 每帧推进气泡的淡入淡出 */
        uint64_t fs = sig_frame_part(), us = sig_ui_part();
        int ui_changed = (us != g_last_us);
        if (fs != g_last_fs || ui_changed) {
            int only_frame = !ui_changed;
            g_last_fs = fs;
            g_last_us = us;
            if (ui_changed) {
                render_static();
                if (g_ui) memcpy(g_ui, g_fb, (size_t)WIN_W * WIN_H * 4);
            }
            render_compose();
            if (getenv("PET_DUMPFRAME")) {       /* 调试：把合成结果写成 PPM */
                FILE *df = fopen("frame.ppm", "wb");
                if (df) {
                    fprintf(df, "P6\n%d %d\n255\n", WIN_W, WIN_H);
                    for (size_t k = 0; k < (size_t)WIN_W * WIN_H; k++) {
                        uint32_t p = g_fb[k];
                        fputc((p >> 16) & 0xFF, df);
                        fputc((p >> 8) & 0xFF, df);
                        fputc(p & 0xFF, df);
                    }
                    fclose(df);
                }
            }
            if (only_frame) {
                /* 只有动画帧变了：只把宠物那一块重新转换+上传 */
                int px = pet_local_x();
                int py = WIN_H - PET_MARGIN_BOTTOM - PET_H;
                plat_present_rect(win, g_fb, WIN_W, WIN_H, px, py, px + PET_W, py + PET_H);
            } else {
                plat_present(win, g_fb, WIN_W, WIN_H);
            }
        }
        plat_sleep(16);
    }

    save_position(posfile);
    plat_destroy(win);
    free(g_fb);
    return 0;
}
