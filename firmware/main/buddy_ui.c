#include "buddy_ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "buddy_i4.h"
#include "buddy_girl.h"
#include "buddy_sprite.h"
#include "buddy_text_layout.h"
#include "lvgl.h"

LV_FONT_DECLARE(ui_font_cjk_16);

#define UI_W 240
#define UI_H 320
#define COL_BG lv_color_hex(0x080A0C)
#define COL_INK lv_color_hex(0xF7E9D7)
#define COL_DIM lv_color_hex(0x8B8178)
#define COL_LINE lv_color_hex(0x39332F)
#define COL_ORANGE lv_color_hex(0xE17B52)
/* 10/03 晚：菜单选中条 —— 原来是橙色，科长要 Hermes 那种冷峻的黑白灰。
 * 用中性冷灰做底 + 近黑文字（黑底上比纯白条稳，不闪眼）。 */
#define COL_SEL lv_color_hex(0xC3C7CC)
/* 正文滚动：一步约三行（3 × 21px）。 */
#define TEXT_SCROLL_STEP 63
/* ⚠ 实测行距是 21px（unscii_16 字高 + 行间），不是 10 —— 写 10 会让滚动上限偏小一半，
 * 正文一超过一屏就滚不到底（看不到最后几行）。 */
#define TEXT_LINE_H 21
#define COL_RED lv_color_hex(0xEF4B38)
#define COL_GREEN lv_color_hex(0x64C987)
#define COL_YELLOW lv_color_hex(0xF1C75B)
#define COL_BLUE lv_color_hex(0x72A7D8)

static lv_obj_t *s_screen;
static lv_obj_t *s_canvas;
static LV_ATTRIBUTE_MEM_ALIGN uint8_t s_canvas_buffer[
    LV_DRAW_BUF_SIZE(UI_W, UI_H, LV_COLOR_FORMAT_I4)];
static buddy_ui_snapshot_t s_snapshot;
static bool s_have_snapshot;
static uint32_t s_tick;
static uint64_t s_elapsed_ms;
static int s_scroll;
/* 正文滚动偏移（像素），只给 draw_home 用；和设置页的 s_scroll 分开。 */
static int s_text_scroll;
static int s_text_max_scroll;   /* 当前正文的可滚上限（渲染时按行数算出来） */
/* 滚动请求（按键回调可能是别的任务/中断上下文，只累加不碰 LVGL）。 */
static volatile int s_text_scroll_req;
static buddy_i4_surface_t s_surface;

#define I4_PALETTE_BYTES (16U * sizeof(lv_color32_t))

static const uint32_t s_palette_rgb[] = {0x080A0C, 0xF7E9D7, 0x8B8178, 0x39332F,
    0xE17B52, 0xEF4B38, 0x64C987, 0xF1C75B, 0x72A7D8, 0xFFFFFF,
    0xD97757, 0xA96349, 0x8B5CF6, 0x81A1C1, 0xC3C7CC, 0x151719};

static uint8_t color_index(lv_color_t color)
{
    uint32_t rgb = lv_color_to_int(color);
    uint32_t best_distance = UINT32_MAX;
    uint8_t best = 0;
    uint8_t i;
    for (i = 0; i < sizeof(s_palette_rgb) / sizeof(s_palette_rgb[0]); ++i) {
        int dr = (int)((rgb >> 16) & 0xffU) - (int)((s_palette_rgb[i] >> 16) & 0xffU);
        int dg = (int)((rgb >> 8) & 0xffU) - (int)((s_palette_rgb[i] >> 8) & 0xffU);
        int db = (int)(rgb & 0xffU) - (int)(s_palette_rgb[i] & 0xffU);
        uint32_t distance = (uint32_t)(dr * dr + dg * dg + db * db);
        if (distance < best_distance) {
            best_distance = distance;
            best = i;
        }
    }
    return best;
}

static void pixel(int x, int y, uint8_t index)
{
    if ((unsigned)x >= UI_W || (unsigned)y >= UI_H) return;
    buddy_i4_set_pixel(s_canvas_buffer + I4_PALETTE_BYTES, UI_W,
                       (uint16_t)x, (uint16_t)y, index);
}

/* UTF-8：解出当前字符的码点与字节数；非法字节按单字节处理。 */
static uint32_t utf8_next(const char *value, size_t *advance)
{
    const unsigned char *p = (const unsigned char *)value;
    if (p[0] < 0x80U) { *advance = 1; return p[0]; }
    if ((p[0] & 0xE0U) == 0xC0U && (p[1] & 0xC0U) == 0x80U) {
        *advance = 2;
        return ((uint32_t)(p[0] & 0x1FU) << 6) | (uint32_t)(p[1] & 0x3FU);
    }
    if ((p[0] & 0xF0U) == 0xE0U && (p[1] & 0xC0U) == 0x80U && (p[2] & 0xC0U) == 0x80U) {
        *advance = 3;
        return ((uint32_t)(p[0] & 0x0FU) << 12) | ((uint32_t)(p[1] & 0x3FU) << 6) |
               (uint32_t)(p[2] & 0x3FU);
    }
    if ((p[0] & 0xF8U) == 0xF0U && (p[1] & 0xC0U) == 0x80U && (p[2] & 0xC0U) == 0x80U &&
        (p[3] & 0xC0U) == 0x80U) {
        *advance = 4;
        return ((uint32_t)(p[0] & 0x07U) << 18) | ((uint32_t)(p[1] & 0x3FU) << 12) |
               ((uint32_t)(p[2] & 0x3FU) << 6) | (uint32_t)(p[3] & 0x3FU);
    }
    *advance = 1;
    return p[0];
}

/* 拉丁字符沿用像素字体保持原有观感，其余（中文等）走 16px 点阵中文字体。 */
static const lv_font_t *pick_font(const lv_font_t *ascii_font, uint32_t codepoint)
{
    return codepoint < 0x80U ? ascii_font : &ui_font_cjk_16;
}

/* 把偏移回退到 UTF-8 字符边界，避免切断多字节字符。 */
static size_t utf8_floor(const char *value, size_t offset)
{
    while (offset > 0 && ((unsigned char)value[offset] & 0xC0U) == 0x80U) --offset;
    return offset;
}

static int line_width(const lv_font_t *font, const char *start, size_t length, int spacing)
{
    int result = 0;
    size_t i = 0;
    bool any = false;
    while (i < length) {
        size_t step = 1;
        uint32_t codepoint = utf8_next(start + i, &step);
        lv_font_glyph_dsc_t glyph;
        if (i + step > length) break;
        if (lv_font_get_glyph_dsc(pick_font(font, codepoint), &glyph, codepoint, 0)) {
            result += glyph.adv_w + spacing;
            any = true;
        }
        i += step;
    }
    return any ? result - spacing : 0;
}

typedef struct {
    const lv_font_t *font;
    int spacing;
} text_measure_context_t;

static unsigned measure_text(const char *value, size_t length, void *context)
{
    const text_measure_context_t *measure = context;
    return (unsigned)line_width(measure->font, value, length, measure->spacing);
}

static void glyph(int x, int y, uint8_t index, const lv_font_t *font, uint32_t codepoint)
{
    lv_font_glyph_dsc_t dsc;
    const uint8_t *bitmap;
    unsigned row;
    unsigned col;
    if (!lv_font_get_glyph_dsc(font, &dsc, codepoint, 0) || dsc.box_w == 0 || dsc.box_h == 0) return;
    /* UNSCII is generated as immutable plain A1 data, but LVGL 9.5 does not set
     * lv_font_t.static_bitmap on these built-ins. Request the raw bitmap from
     * the font backend directly instead of the guarded convenience wrapper. */
    dsc.req_raw_bitmap = 1;
    bitmap = dsc.resolved_font->get_glyph_bitmap(&dsc, NULL);
    if (!bitmap || dsc.format != LV_FONT_GLYPH_FORMAT_A1) return;
    y += font->line_height - font->base_line - dsc.box_h - dsc.ofs_y;
    x += dsc.ofs_x;
    for (row = 0; row < dsc.box_h; ++row) {
        for (col = 0; col < dsc.box_w; ++col) {
            uint32_t bit = row * dsc.box_w + col;
            if ((bitmap[bit >> 3] & (0x80U >> (bit & 7U))) != 0) pixel(x + col, y + row, index);
        }
    }
}

static void text(lv_layer_t *layer, int x, int y, int width, lv_color_t color,
                 const char *value, bool large, lv_text_align_t align)
{
    const lv_font_t *font = large ? &lv_font_unscii_16 : &lv_font_unscii_8;
    int spacing = large ? 2 : 0;
    uint8_t index = color_index(color);
    const char *cursor = value;
    (void)layer;
    while (*cursor && y < UI_H) {
        const char *end = strchr(cursor, '\n');
        size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
        size_t fit = length;
        size_t i;
        int measured;
        int pen;
        int line_step;
        const lv_font_t *line_font = font;
        for (i = 0; i < length; ++i) {
            if ((unsigned char)cursor[i] >= 0x80U) { line_font = &ui_font_cjk_16; break; }
        }
        /* 行内含中文时按中文字高的行距排版，纯拉丁行保持原样。 */
        line_step = line_font->line_height + (large ? 2 : 1);
        while (fit > 0 && line_width(font, cursor, fit, spacing) > width) {
            --fit;
            fit = utf8_floor(cursor, fit);
        }
        measured = line_width(font, cursor, fit, spacing);
        pen = x;
        if (align == LV_TEXT_ALIGN_CENTER) pen += (width - measured) / 2;
        else if (align == LV_TEXT_ALIGN_RIGHT) pen += width - measured;
        i = 0;
        while (i < fit) {
            size_t step = 1;
            uint32_t codepoint = utf8_next(cursor + i, &step);
            const lv_font_t *glyph_font;
            lv_font_glyph_dsc_t dsc;
            int baseline_shift;
            if (i + step > fit) break;
            glyph_font = pick_font(font, codepoint);
            baseline_shift = (line_font->line_height - glyph_font->line_height) / 2;
            glyph(pen, y + baseline_shift, index, glyph_font, codepoint);
            if (lv_font_get_glyph_dsc(glyph_font, &dsc, codepoint, 0)) pen += dsc.adv_w + spacing;
            i += step;
        }
        y += line_step;
        if (fit < length) cursor += fit;
        else cursor = end ? end + 1 : cursor + length;
    }
}

/* 返回换行后的行数 —— 调用方（转录页）用它算可滚上限。 */
static unsigned wrapped_text(lv_layer_t *layer, int x, int y, int width, lv_color_t color,
                             const char *value, unsigned max_lines)
{
    /* ⚠ 768 而不是 512：换行会往输出里插 '\n'，中文正文（512 字节满）折行后
     * 会比输入长十几个字节，用 512 的缓冲会把最后一行悄悄吃掉。 */
    static char wrapped[768];
    text_measure_context_t measure = {.font = &lv_font_unscii_8, .spacing = 0};
    buddy_text_result_t res = buddy_text_wrap(value, wrapped, sizeof(wrapped),
                                              (unsigned)width, max_lines,
                                              measure_text, &measure);

    text(layer, x, y, width, color, wrapped, false, LV_TEXT_ALIGN_LEFT);
    return res.lines;
}

static void box(lv_layer_t *layer, int x, int y, int w, int h, lv_color_t fill,
                lv_color_t border, int border_width, int radius)
{
    uint8_t fill_index = color_index(fill);
    uint8_t border_index = color_index(border);
    int px;
    int py;
    (void)layer;
    (void)radius;
    for (py = 0; py < h; ++py) {
        for (px = 0; px < w; ++px) {
            bool edge = px < border_width || py < border_width ||
                        px >= w - border_width || py >= h - border_width;
            pixel(x + px, y + py, edge ? border_index : fill_index);
        }
    }
}

static void rule(lv_layer_t *layer, int x, int y, int w, lv_color_t color)
{
    box(layer, x, y, w, 1, color, color, 0, 0);
}

static uint8_t art_state(buddy_character_t state)
{
    switch (state) {
    case BUDDY_CHARACTER_SLEEP: return 0;
    case BUDDY_CHARACTER_BUSY: return 2;
    case BUDDY_CHARACTER_ATTENTION:
    case BUDDY_CHARACTER_PAIRING:
    case BUDDY_CHARACTER_CONFIRMATION: return 3;
    case BUDDY_CHARACTER_CELEBRATE: return 4;
    case BUDDY_CHARACTER_DIZZY: return 5;
    case BUDDY_CHARACTER_HEART: return 6;
    default: return 1;
    }
}

/* Nous Girl 头像：180x180，2-bit **调色板索引**（0=近黑 3=暗灰 2=中灰 1=米白）。
 * 逐像素直接走 pixel()（内部 4bpp 画布），不经 box() 的最近色搜索。
 * ~32k 次像素写只在重绘时发生，实测可接受。 */
static void draw_girl(int ox, int oy)
{
    int row;
    for (row = 0; row < BUDDY_GIRL_H; ++row) {
        const uint8_t *line = &buddy_girl_bits[row * BUDDY_GIRL_STRIDE];
        int col;
        for (col = 0; col < BUDDY_GIRL_W; ++col) {
            uint8_t v = (uint8_t)((line[col >> 2] >> ((3 - (col & 3)) * 2)) & 0x03U);
            pixel(ox + col, oy + row, v);
        }
    }
}

static void draw_buddy(lv_layer_t *layer, const buddy_ui_snapshot_t *s, bool peek)
{
    buddy_i4_clip_t clip = {.x = 0, .y = BUDDY_UI_STAGE_Y,
                            .w = UI_W, .h = BUDDY_UI_STAGE_H};
    buddy_sprite_bounds_t bounds;
    int x = 88;
    int y = peek ? 72 : 65;
    (void)layer;
    if (buddy_sprite_bounds(s->species, art_state(s->character), s_tick, &bounds)) {
        x = (UI_W - bounds.w) / 2 - bounds.x;
    }
    buddy_sprite_render(&s_surface, &clip, s->species, art_state(s->character),
                        s_tick, x, y);
}

static void draw_status_bar(lv_layer_t *layer, const buddy_ui_snapshot_t *s)
{
    char left[32];
    char right[32];
    uint64_t age_ms = s_elapsed_ms >= s->time_received_ms ? s_elapsed_ms - s->time_received_ms : 0;
    time_t epoch = (time_t)(s->epoch_seconds + s->timezone_offset_seconds + age_ms / 1000U);
    struct tm tm_value;

    snprintf(left, sizeof(left), "%s", s->ble_connected ? (s->ble_encrypted ? "BLE+" : "BLE") : "HERMES");
    if (s->epoch_seconds > 0 && gmtime_r(&epoch, &tm_value) != NULL) {
        snprintf(right, sizeof(right), "%02d:%02d", tm_value.tm_hour, tm_value.tm_min);
    } else {
        snprintf(right, sizeof(right), "%s", s->heartbeat_stale ? "休眠" : "在线");
    }
    text(layer, 8, 7, 100, s->ble_connected ? COL_INK : COL_DIM, left, false, LV_TEXT_ALIGN_LEFT);
    /* 电池（10/04 科长定版）：像手机一样贴在**最右上角**，时间左移到它左边。
     * 电量**只画格子不写数字** —— 一眼看格数就知道剩多少，写百分比反而和中间的
     * 录音转圈挤在一起。电量计不应答时整块不画（宁可空着也不显示假电量）。 */
    text(layer, 128, 7, 74, COL_DIM, right, false, LV_TEXT_ALIGN_RIGHT);
    if (s->battery_available) {
        unsigned pct = s->battery_percent > 100U ? 100U : s->battery_percent;
        lv_color_t fill = pct <= 15U ? COL_RED : (pct <= 35U ? COL_YELLOW : COL_GREEN);
        int bw = 20;
        int bh = 11;
        int bx = 210;
        int by = 7;
        int fill_w = (int)((pct * (unsigned)(bw - 4)) / 100U);
        box(layer, bx, by, bw, bh, COL_BG, COL_DIM, 1, 1);                 /* 电池体外框 */
        box(layer, bx + bw, by + 3, 3, bh - 6, COL_DIM, COL_DIM, 0, 1);    /* 正极帽 */
        if (fill_w > 0) {
            box(layer, bx + 2, by + 2, fill_w, bh - 4, fill, fill, 0, 1);  /* 电量填充 */
        }
    }
    rule(layer, 8, 25, 224, COL_LINE);
}

static void draw_home(lv_layer_t *layer, const buddy_ui_snapshot_t *s)
{
    const char *caption = s->message[0] ? s->message :
             (s->ble_connected ? "等待 Hermes" : "启动 Hermes 配对");
    /* 首页主视觉 = Nous Girl 头像（10/03 晚：科长喜欢那个黑白小姑娘，并嫌 132 太小）。
     * 原来的 ASCII 宠物形象与上方的橙色名字撤掉，免得和头像叠在一起。 */
    draw_girl((240 - BUDDY_GIRL_W) / 2, BUDDY_UI_STAGE_Y);
    /* 头像下缘 = 26 + 180 = 206 → 分隔线 210、正文从 218 起（约 4 行）。 */
    rule(layer, 18, 210, 204, COL_LINE);
    /* 正文上下滚动：负偏移整体上移，超屏部分由 LVGL 裁剪。
     * 不再用 max_lines 截断（原来 8 行就没了），传 40 只是限制渲染量。 */
    wrapped_text(layer, 18, 218 - s_text_scroll, 204,
                 s->heartbeat_stale ? COL_DIM : COL_INK, caption, 40);
    /* 底部只留一行按键说明（科长 10/03 晚：一行就够） */
    text(layer, 8, 296, 224, COL_DIM, "OK 说话 · ▲ 首页 · 长按 菜单", false,
         LV_TEXT_ALIGN_CENTER);
}

static void draw_heart(lv_layer_t *layer, int x, int y, bool on)
{
    lv_color_t c = on ? COL_RED : COL_LINE;
    box(layer, x + 2, y, 4, 4, c, c, 0, 0);
    box(layer, x + 8, y, 4, 4, c, c, 0, 0);
    box(layer, x, y + 3, 14, 5, c, c, 0, 0);
    box(layer, x + 3, y + 8, 8, 3, c, c, 0, 0);
    box(layer, x + 6, y + 11, 2, 2, c, c, 0, 0);
}

static void draw_pet(lv_layer_t *layer, const buddy_ui_snapshot_t *s)
{
    char value[64];
    unsigned i;
    uint64_t level = s->tokens / 50000ULL;
    text(layer, 9, 35, 222, COL_DIM, buddy_sprite_name(s->species), false, LV_TEXT_ALIGN_CENTER);
    draw_buddy(layer, s, true);
    rule(layer, 12, BUDDY_UI_INFO_Y, 216, COL_LINE);
    text(layer, 16, 170, 62, COL_DIM, "心情", false, LV_TEXT_ALIGN_LEFT);
    for (i = 0; i < 4; ++i) draw_heart(layer, 84 + (int)i * 25, 168, !s->heartbeat_stale || i < 2);
    snprintf(value, sizeof(value), "等级 %llu", (unsigned long long)level);
    box(layer, 184, 166, 42, 19, COL_LINE, COL_LINE, 0, 3);
    text(layer, 186, 171, 38, COL_BG, value, false, LV_TEXT_ALIGN_CENTER);
    text(layer, 16, 200, 75, COL_DIM, "词元", false, LV_TEXT_ALIGN_LEFT);
    snprintf(value, sizeof(value), "%llu", (unsigned long long)s->tokens);
    text(layer, 92, 200, 132, COL_INK, value, false, LV_TEXT_ALIGN_RIGHT);
    text(layer, 16, 222, 75, COL_DIM, "今日", false, LV_TEXT_ALIGN_LEFT);
    snprintf(value, sizeof(value), "%llu", (unsigned long long)s->tokens_today);
    text(layer, 92, 222, 132, COL_INK, value, false, LV_TEXT_ALIGN_RIGHT);
    text(layer, 16, 248, 75, COL_DIM, "能量", false, LV_TEXT_ALIGN_LEFT);
    for (i = 0; i < 8; ++i) {
        bool on = !s->heartbeat_stale && i < 6;
        box(layer, 93 + (int)i * 16, 248, 11, 8, on ? COL_YELLOW : COL_LINE,
            on ? COL_YELLOW : COL_LINE, 0, 1);
    }
    /* 底部「下:信息 长按:菜单」提示已删（同上） */
}

static void draw_info(lv_layer_t *layer, const buddy_ui_snapshot_t *s)
{
    static const char *const titles[] = {"关于", "按键", "状态", "设备", "蓝牙", "致谢"};
    char body[512];
    unsigned p = s->info_page < 6 ? s->info_page : 0;
    text(layer, 14, 38, 180, COL_INK, titles[p], true, LV_TEXT_ALIGN_LEFT);
    /* 10/04：菜单里「按键」直达第 2 页（titles[1]）、「关于」第 1 页，两页都可达。 */
    rule(layer, 14, 66, 212, COL_LINE);
    switch (p) {
    case 0: snprintf(body, sizeof(body),
                     "AI 工牌 · HERMES 伙伴\n\n"
                     "与 MOND 一起做的\n\n"
                     "致谢\n"
                     "HERMES AGENT / NOUS RESEARCH\n"
                     "CLAUDE DESKTOP BUDDY / FELIX RIESEBERG\n\n"
                     "许可  APACHE-2.0\n\n"
                     /* 编译期烙进去：出问题时报这个日期给 Hermes，不用猜设备跑的是哪一版固件 */
                     "固件  " __DATE__ " " __TIME__); break;
    /* 按键速查（10/04 科长要求）：一屏装得下，写的是**当前固件真实键位** ——
     * 不要再抄上游 claude-buddy 那套（上=切换屏幕、确定=修改，早就不是了）。
     * 版面：正文区 y=82 起、行距 21px、宽 208px（中文约 13 字/行）→ 最多 11 行。
     * ⚠ 只用字体里**确实有**的字形：▲ 有，▼ 与 · 没有（字体按 GB2312 字符集生成，
     *   这两个码点不在集合里，画出来是空白）→ 上下键一律写作「上键 / 下键」。 */
    case 1: snprintf(body, sizeof(body),
                     "首页 / 正文\n"
                     "OK 单击    对 Hermes 说话\n"
                     "OK 长按    打开菜单\n"
                     "上键单击   上滚 / 回首页\n"
                     "下键单击   看更早一条\n"
                     "下键双击   看更新一条\n"
                     "菜单 / 设置\n"
                     "上下键     选择\n"
                     "OK         进入 / 切换\n"
                     "审批窗口\n"
                     "OK 批准    下键拒绝"); break;
    case 2: snprintf(body, sizeof(body), "会话     %u\n运行     %u\n等待     %u\n\n词元     %llu", s->total, s->running, s->waiting, (unsigned long long)s->tokens); break;
    case 3: snprintf(body, sizeof(body), "名称\n%s\n\n归属\n%s\n\n屏幕     240 X 320", s->name[0] ? s->name : "Hermes 伙伴", s->owner[0] ? s->owner : "-"); break;
    case 4: snprintf(body, sizeof(body), "%s\n\n%s\n%s\n\n在 Windows 蓝牙中\n添加设备完成配对", s->name[0] ? s->name : "Hermes-伙伴", s->ble_connected ? "已连接" : "广播中", s->ble_encrypted ? "已加密" : "未加密"); break;
    default: snprintf(body, sizeof(body), "基于 FELIX RIESEBERG 的\nCLAUDE DESKTOP BUDDY\n\nHERMES 移植版\nESP32-C3 AI PASSPORT\n\nAPACHE-2.0"); break;
    }
    wrapped_text(layer, 16, 82 - s_scroll, 208, COL_INK, body, 18);
    /* 底部「下:翻页 长按:菜单」提示已删（同上） */
}

static void draw_list(lv_layer_t *layer, const char *title, const char *const *items,
                      unsigned count, unsigned selected, const buddy_ui_snapshot_t *s)
{
    unsigned first = selected > 5 ? selected - 5 : 0;
    unsigned i;
    text(layer, 14, 34, 212, COL_INK, title, true, LV_TEXT_ALIGN_LEFT);
    rule(layer, 14, 62, 212, COL_LINE);
    for (i = first; i < count && i < first + 7; ++i) {
        int y = 76 + (int)(i - first) * 29;
        bool active = i == selected;
        char row[64];
        const char *suffix = "";
        char value[12];
        static const char *const led_names[] = {"关", "呼吸", "脉冲", "常亮"};
        static const char *const rot_names[] = {"正", "右", "倒", "左"};
        if (!s->reset_open && i == BUDDY_SETTINGS_BRIGHTNESS) { snprintf(value, sizeof(value), "%u/4", s->brightness_level); suffix = value; }
        else if (!s->reset_open && i == BUDDY_SETTINGS_BACKLIGHT) suffix = led_names[s->led_effect % 4];
        else if (!s->reset_open && i == BUDDY_SETTINGS_VOLUME) { snprintf(value, sizeof(value), "%u%%", s->volume); suffix = value; }
        else if (!s->reset_open && i == BUDDY_SETTINGS_SPEAK) suffix = s->speak_enabled ? "开" : "关";
        else if (!s->reset_open && i == BUDDY_SETTINGS_BEEP) suffix = s->beep_enabled ? "开" : "关";
        else if (!s->reset_open && i == BUDDY_SETTINGS_BLE) suffix = s->ble_enabled ? "开" : "关";
        else if (!s->reset_open && i == BUDDY_SETTINGS_CLOCK_ROTATION) suffix = rot_names[s->rotation % 4];
        snprintf(row, sizeof(row), "%s", items[i]);
        if (active) box(layer, 12, y - 7, 216, 24, COL_SEL, COL_SEL, 0, 3);
        text(layer, 20, y, 142, active ? COL_BG : COL_INK, row, false, LV_TEXT_ALIGN_LEFT);
        text(layer, 158, y, 62, active ? COL_BG : COL_DIM, suffix, false, LV_TEXT_ALIGN_RIGHT);
    }
    /* 底部「上下:选择 确定:设置」提示已删（同上） */
}

static void draw_settings(lv_layer_t *layer, const buddy_ui_snapshot_t *s)
{
    static const char *const settings[] = {"亮度", "背光", "音量", "播报", "提示音", "蓝牙", "钟向", "重置", "返回"};
    static const char *const reset[] = {"重置设置", "恢复出厂", "解除配对", "返回"};
    draw_list(layer, s->reset_open ? "重置" : "设置", s->reset_open ? reset : settings,
              s->reset_open ? BUDDY_RESET_COUNT : BUDDY_SETTINGS_COUNT,
              s->reset_open ? s->reset_selection : s->settings_selection, s);
}

static void panel(lv_layer_t *layer, int y, int h, lv_color_t accent, const char *title,
                  const char *body, const char *footer)
{
    box(layer, 10, y, 220, h, lv_color_hex(0x151719), accent, 2, 6);
    box(layer, 10, y, 220, 27, accent, accent, 0, 5);
    text(layer, 18, y + 8, 204, COL_BG, title, false, LV_TEXT_ALIGN_LEFT);
    wrapped_text(layer, 20, y + 42 - s_scroll, 200, COL_INK, body, 5);
    rule(layer, 20, y + h - 34, 200, COL_LINE);
    text(layer, 18, y + h - 23, 204, COL_DIM, footer, false, LV_TEXT_ALIGN_CENTER);
}

/* 本地采集是否进行中：>= 0 = 正在录音（值是已录秒数，见 buddy_ui_set_recording）。 */
static int s_recording_left = -1;

/* 录音指示：状态栏中间空隙处的小圆环，一段亮弧随时间旋转（"转圈圈"）。
 * 位置 (120,12) 落在左字（x ≤ 108）与右字（x ≥ 132）之间，不遮挡任何内容；
 * 角度由 s_elapsed_ms 驱动，配合 buddy_ui_tick 的加密刷新转起来。 */
static void draw_rec_badge(void)
{
    const int cx = 120;
    const int cy = 12;
    const int r = 6;
    const int phase = (int)((s_elapsed_ms / 3U) % 360U);   /* ≈0.9 圈/秒 */
    int a;

    for (a = 0; a < 360; a += 6) {                         /* 暗色底环 */
        float rad = (float)a * 3.14159265F / 180.0F;
        pixel(cx + (int)((float)r * cosf(rad) + 0.5F),
              cy + (int)((float)r * sinf(rad) + 0.5F), 3);
    }
    for (a = 0; a < 100; a += 5) {                         /* 亮弧（≈100°） */
        float rad = (float)((a + phase) % 360) * 3.14159265F / 180.0F;
        pixel(cx + (int)((float)r * cosf(rad) + 0.5F),
              cy + (int)((float)r * sinf(rad) + 0.5F), 5);
    }
    pixel(cx, cy, 5);                                      /* 中心红点 */
}

static void draw_overlay(lv_layer_t *layer, const buddy_ui_snapshot_t *s)
{
    char body[448];
    int x;
    int y;
    buddy_overlay_kind_t overlay = buddy_overlay_select(s->confirmation_pending,
                                                        s->passkey_visible,
                                                        s->prompt_id[0] != '\0',
                                                        s->menu_open);
    if (overlay != BUDDY_OVERLAY_NONE) {
        for (y = BUDDY_UI_STATUS_H; y < BUDDY_UI_ACTION_Y; ++y) {
            for (x = (y & 1); x < UI_W; x += 2) {
                uint8_t current = buddy_i4_get_pixel(s_surface.pixels, UI_W,
                                                     (uint16_t)x, (uint16_t)y);
                if (current != 0) buddy_i4_set_pixel(s_surface.pixels, UI_W,
                                                          (uint16_t)x, (uint16_t)y,
                                                          15);
            }
        }
    }
    if (overlay == BUDDY_OVERLAY_CONFIRMATION) {
        panel(layer, 62, 196, COL_RED, "请确认",
              s->confirmation == BUDDY_CONFIRM_FACTORY_RESET ? "恢复出厂？\n\n设置与统计\n将被清除。" : "解除配对？\n\n已保存的蓝牙\n配对信息将被清除。",
              BUDDY_ACTION_CONFIRM);
    } else if (overlay == BUDDY_OVERLAY_PAIRING) {
        snprintf(body, sizeof(body), "在 Windows 蓝牙中\n输入此配对码\n\n       %06lu", (unsigned long)s->passkey);
        panel(layer, 66, 188, COL_BLUE, "蓝牙配对", body, "请保持此屏");
    } else if (overlay == BUDDY_OVERLAY_APPROVAL) {
        snprintf(body, sizeof(body), "%s\n\n%s", s->prompt_tool, s->prompt_hint);
        panel(layer, 154, 158, s->approval_locked ? COL_DIM : COL_RED, "Hermes 需要批准", body,
              s->approval_locked ? (s->permission_delivery == BUDDY_PERMISSION_DELIVERY_FAILED ? "发送失败" : "发送中...") : BUDDY_ACTION_APPROVAL);
    } else if (overlay == BUDDY_OVERLAY_MENU) {
        /* 「关机」名不符实（10/04 科长反馈）：它只关背光，CPU 与蓝牙照常跑，任意键
         * 即可唤醒 —— 所以文案改成「息屏」。真·断电走硬件电源键。 */
        static const char *const menu[] = {"设置", "息屏", "按键", "关于", "关闭"};
        unsigned i;
        box(layer, 38, 48, 164, 224, lv_color_hex(0x151719), COL_INK, 2, 5);
        text(layer, 52, 61, 136, COL_INK, "菜单", true, LV_TEXT_ALIGN_CENTER);
        rule(layer, 52, 88, 136, COL_LINE);
        for (i = 0; i < BUDDY_MENU_COUNT; ++i) {
            int y = 103 + (int)i * 25;
            bool active = i == (unsigned)s->menu_selection;
            if (active) box(layer, 48, y - 7, 144, 21, COL_SEL, COL_SEL, 0, 2);
            text(layer, 56, y, 128, active ? COL_BG : COL_INK, menu[i], false, LV_TEXT_ALIGN_CENTER);
        }
    }
    if (s_recording_left >= 0) {
        draw_rec_badge();   /* 录音指示：状态栏中间的小转圈，不挡任何内容 */
    }
}

/* 转录页：显示 Hermes 下发的回复正文。512 字节约合 170 汉字，
 * 按 15 字/行折行后正好一屏，超出部分由桥端负责截断。 */
static void draw_transcript(lv_layer_t *layer, const buddy_ui_snapshot_t *s)
{
    int y = BUDDY_UI_STATUS_H + 4;
    const char *body = s->body[0] != '\0' ? s->body : "暂无内容";
    /* 10/03 晚：正文可滚了（上下键短按）—— 原来一屏装不下就看不全后面。
     * 先渲染（带当前偏移）再按量出的行数算上限：短正文 max=0，
     * 下键就不会滚出空白，而是直接翻上一条对话。 */
    unsigned lines = wrapped_text(layer, 8, y - s_text_scroll, UI_W - 16, COL_INK, body, 40);
    int visible = UI_H - y - 6;
    int total = (int)lines * TEXT_LINE_H;

    s_text_max_scroll = total > visible ? total - visible : 0;
    if (s_text_scroll > s_text_max_scroll) {
        s_text_scroll = s_text_max_scroll;
    }
}

static void redraw(void)
{
    lv_layer_t *layer = NULL;
    if (!s_canvas || !s_have_snapshot) return;
    memset(s_canvas_buffer + I4_PALETTE_BYTES, 0, sizeof(s_canvas_buffer) - I4_PALETTE_BYTES);
    draw_status_bar(layer, &s_snapshot);
    switch (s_snapshot.page) {
    case BUDDY_PAGE_PET: draw_pet(layer, &s_snapshot); break;
    case BUDDY_PAGE_INFO: draw_info(layer, &s_snapshot); break;
    case BUDDY_PAGE_SETTINGS: draw_settings(layer, &s_snapshot); break;
    case BUDDY_PAGE_TRANSCRIPT: draw_transcript(layer, &s_snapshot); break;
    default: draw_home(layer, &s_snapshot); break;
    }
    draw_overlay(layer, &s_snapshot);
    lv_obj_invalidate(s_canvas);
}

void buddy_ui_init(void)
{
    unsigned i;
    if (s_screen) return;
    s_screen = lv_obj_create(NULL);
    lv_obj_set_size(s_screen, UI_W, UI_H);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_bg_color(s_screen, COL_BG, 0);
    s_canvas = lv_canvas_create(s_screen);
    lv_canvas_set_buffer(s_canvas, s_canvas_buffer, UI_W, UI_H, LV_COLOR_FORMAT_I4);
    lv_obj_set_pos(s_canvas, 0, 0);
    buddy_i4_surface_init(&s_surface, s_canvas_buffer + I4_PALETTE_BYTES,
                          UI_W, UI_H, UI_W / 2);
    for (i = 0; i < sizeof(s_palette_rgb) / sizeof(s_palette_rgb[0]); ++i)
        lv_canvas_set_palette(s_canvas, i, lv_color_to_32(lv_color_hex(s_palette_rgb[i]), LV_OPA_COVER));
    lv_screen_load(s_screen);
}

void buddy_ui_render(const buddy_ui_snapshot_t *snapshot)
{
    /* 初始值取 0(= 默认朝向)而不是 0xFF:避免开机第一次渲染就调用旋转。
     * lv_display_set_rotation() 会重建显示缓冲,与截屏任务接管的 flush 回调冲突。 */
    static uint8_t s_applied_rotation = 0U;
    if (!snapshot) return;
    buddy_ui_init();
    if (snapshot->rotation != s_applied_rotation) {
        s_applied_rotation = snapshot->rotation;
        lv_display_set_rotation(lv_display_get_default(),
                                (lv_display_rotation_t)snapshot->rotation);
    }
    s_snapshot = *snapshot;
    s_have_snapshot = true;
    s_scroll = 0;
    s_text_scroll = 0;
    redraw();
}

void buddy_ui_show_passkey(uint32_t passkey)
{
    buddy_ui_snapshot_t snapshot = {.passkey_visible = true, .passkey = passkey};
    buddy_ui_render(&snapshot);
}

void buddy_ui_set_recording(int seconds_left)
{
    if (seconds_left == s_recording_left) return;
    s_recording_left = seconds_left;
    redraw();
}

void buddy_ui_tick(uint64_t elapsed_ms)
{
    /* 先消费按键回调攒下的滚动请求（在这里动 LVGL 才安全）。 */
    if (s_text_scroll_req != 0) {
        int req = s_text_scroll_req;
        s_text_scroll_req = 0;
        buddy_ui_text_scroll(req);
    }

    /* 录音时把刷新粒度加密，让状态栏那个"转圈圈"转得动（配合 main.c 录音期间
     * 缩短的 app tick）；平时仍 200ms 一帧，省 CPU。 */
    uint32_t tick = (uint32_t)(elapsed_ms / (s_recording_left >= 0 ? 60U : 200U));
    s_elapsed_ms = elapsed_ms;
    if (tick != s_tick) {
        s_tick = tick;
        redraw();
    }
}

void buddy_ui_scroll(int delta)
{
    s_scroll += delta;
    if (s_scroll < 0) s_scroll = 0;
    if (s_scroll > 160) s_scroll = 160;
    redraw();
}

/* 正文滚动（上下键短按）：10/03 晚改成按**实际正文高度**夹紧 ——
 * 原来硬编码 0..360，短正文也能滚出一屏空白；现在上限由渲染时量出的行数算
 * （s_text_max_scroll），滚到底就停住，再按一下转为翻对话（见 buddy_state.c）。 */
void buddy_ui_text_scroll(int dir)
{
    s_text_scroll += dir * TEXT_SCROLL_STEP;
    if (s_text_scroll < 0) s_text_scroll = 0;
    if (s_text_scroll > s_text_max_scroll) s_text_scroll = s_text_max_scroll;
    redraw();
}

/* ▼ 短按的判据：正文到底了没有（到底 → 改翻上一条对话）。 */
bool buddy_ui_text_at_bottom(void)
{
    return s_text_scroll >= s_text_max_scroll;
}

/* ▲ 短按的判据：正文回到顶了没有（到顶 → 改返回首页）。 */
bool buddy_ui_text_at_top(void)
{
    return s_text_scroll <= 0;
}

/* 翻到另一条对话时把视线拉回开头。 */
void buddy_ui_text_reset(void)
{
    s_text_scroll = 0;
}

/* 请求正文滚动（上/下键短按调用；任意上下文安全，实际滚动在 tick 里做）。 */
void buddy_ui_request_text_scroll(int dir)
{
    s_text_scroll_req += dir;
}
