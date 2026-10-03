#include "buddy_text_layout.h"

#include <string.h>

static void append(char *output, size_t size, size_t *used, const char *text, size_t length)
{
    size_t room = size > *used ? size - *used - 1U : 0;
    if (length > room) length = room;
    if (length) memcpy(output + *used, text, length);
    *used += length;
    if (size) output[*used < size ? *used : size - 1U] = '\0';
}

/* 前进一个 UTF-8 字符；截断的多字节序列退化为单字节步进。 */
static const char *utf8_step(const char *value)
{
    unsigned char lead = (unsigned char)value[0];
    size_t width = lead >= 0xF0U ? 4U : lead >= 0xE0U ? 3U : lead >= 0xC0U ? 2U : 1U;
    size_t k;
    for (k = 1; k < width; ++k) {
        if (value[k] == '\0') return value + 1;
    }
    return value + width;
}

/* 向前退一个 UTF-8 字符（用于避头尾：把本行最后一个字留给下一行）。 */
static const char *utf8_prev(const char *start, const char *p)
{
    const char *q = p;

    while (q > start && (((unsigned char)q[-1] & 0xC0U) == 0x80U)) {
        --q;
    }
    return q > start ? q - 1 : start;
}

/* 中文「避头尾」：这些标点不该出现在行首 —— 断行时把它们留在上一行末尾。
 * 只覆盖最常见的中英文标点，够用就行（多列没必要）。 */
static bool is_forbidden_line_start(const char *text)
{
    static const char *const kForbidden[] = {
        "，", "。", "、", "；", "：", "！", "？", "…", "—", "·", "・",
        "）", "】", "」", "』", "》", "〉", "〕", "］", "｝",
        ",", ".", ";", ":", "!", "?", ")", "]", "}",
    };
    size_t index;

    if (text == NULL || text[0] == '\0' || text[0] == '\n' || text[0] == ' ') {
        return false;
    }
    for (index = 0; index < sizeof(kForbidden) / sizeof(kForbidden[0]); ++index) {
        if (strncmp(text, kForbidden[index], strlen(kForbidden[index])) == 0) {
            return true;
        }
    }
    return false;
}


buddy_text_result_t buddy_text_wrap(const char *input, char *output, size_t output_size,
                                    unsigned max_width, unsigned max_lines,
                                    buddy_text_measure_fn measure, void *context)
{
    buddy_text_result_t result = {0};
    const char *cursor = input != NULL ? input : "";
    size_t used = 0;
    if (output_size) output[0] = '\0';
    if (!measure || max_width == 0 || max_lines == 0) return result;
    while (*cursor && result.lines < max_lines) {
        const char *line = cursor;
        const char *last_space = NULL;
        const char *end = cursor;
        /* ⚠ 量宽度必须按**完整字符**（next - line），不能写 end - line + 1：
         * 后者对多字节字符只取到「下一个字符的首字节」，中文（3 字节）会被量成
         * 2/3 个字符宽 → 每行少放一个字，且那个字被挤成**单独一行**
         * （实测现象：满行 → 单字行 → 满行 → 单字行…，科长 10/03 报「第二行一个字」）。 */
        while (*end && *end != '\n') {
            const char *next = utf8_step(end);
            if (measure(line, (size_t)(next - line), context) > max_width) {
                break;
            }
            if (*end == ' ') last_space = end;
            end = next;
        }
        if (*end == ' ') last_space = end;
        if (*end == '\n') {
            append(output, output_size, &used, line, (size_t)(end - line));
            cursor = end + 1;
        } else if (*end == '\0') {
            append(output, output_size, &used, line, (size_t)(end - line));
            cursor = end;
        } else if (last_space != NULL) {
            append(output, output_size, &used, line, (size_t)(last_space - line));
            cursor = last_space + 1;
        } else {
            /* 中文没有空格可断 —— 就按字符断行，**不能丢内容**。
             * ⚠ 原来这里走的是「截断 + 打 '...'」，中文每一行都会命中这一分支，
             *   于是屏上每行末尾都挂着省略号、正文压根发不全（科长 10/03 报障）。 */
            /* 中文避头尾：断点若正落在标点上，那个标点会变成下一行的行首 ——
             * 把本行**最后一个字也留给下一行**（标点跟着那个字走），行首就不会是标点。
             * ⚠ 不能反过来「把标点吃进本行」：屏宽就那么点，超宽部分会被渲染层直接裁掉，
             *   标点连同它前面的字会一起消失（实测丢字）。 */
            if (is_forbidden_line_start(end) && end > line) {
                const char *prev = utf8_prev(line, end);
                if (prev > line) {
                    end = prev;
                }
            }
            append(output, output_size, &used, line, (size_t)(end - line));
            cursor = end;
        }
        ++result.lines;
        if (*cursor && result.lines < max_lines) append(output, output_size, &used, "\n", 1);
    }
    if (*cursor) {
        char *last_line = strrchr(output, '\n');
        size_t prefix;
        size_t length;
        last_line = last_line != NULL ? last_line + 1 : output;
        prefix = (size_t)(last_line - output);
        length = strlen(last_line);
        while (length && last_line[length - 1] == ' ') last_line[--length] = '\0';
        while (length && measure(last_line, length, context) + measure("...", 3, context) > max_width) {
            --length;
            while (length > 0 && ((unsigned char)last_line[length] & 0xC0U) == 0x80U) --length;
        }
        used = prefix + length;
        output[used] = '\0';
        append(output, output_size, &used, "...", 3);
        result.truncated = true;
    }
    return result;
}

buddy_overlay_kind_t buddy_overlay_select(bool confirmation, bool pairing,
                                          bool approval, bool menu)
{
    if (confirmation) return BUDDY_OVERLAY_CONFIRMATION;
    if (pairing) return BUDDY_OVERLAY_PAIRING;
    if (approval) return BUDDY_OVERLAY_APPROVAL;
    if (menu) return BUDDY_OVERLAY_MENU;
    return BUDDY_OVERLAY_NONE;
}
