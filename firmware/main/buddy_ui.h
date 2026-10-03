#pragma once

#include <stdint.h>

#include "buddy_types.h"

/* Every entry point requires the caller to hold bsp_lvgl_lock(). */
void buddy_ui_init(void);
void buddy_ui_render(const buddy_ui_snapshot_t *snapshot);
void buddy_ui_show_passkey(uint32_t passkey);
/* 本地采集提示：seconds >= 0 时在屏幕上盖一层"正在录音 + 已录秒数"横幅，
 * 传 -1 隐藏。用于 OK 键开关的麦克风采集（不依赖 BLE）。 */
void buddy_ui_set_recording(int seconds);
void buddy_ui_tick(uint64_t elapsed_ms);
void buddy_ui_scroll(int delta);
void buddy_ui_text_scroll(int dir);
void buddy_ui_request_text_scroll(int dir);
/* 10/03 晚：给状态机判断「还能不能滚」用（只读写普通 int，不碰 LVGL，
 * 因此按键路径上无锁调用是安全的）。 */
bool buddy_ui_text_at_top(void);
bool buddy_ui_text_at_bottom(void);
void buddy_ui_text_reset(void);
