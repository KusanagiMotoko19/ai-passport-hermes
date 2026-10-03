#pragma once

#include "buddy_types.h"

typedef struct {
    buddy_connection_t connection;
    buddy_character_t character;
    buddy_page_t page;
    buddy_heartbeat_t heartbeat;
    buddy_prompt_t prompt;
    buddy_settings_snapshot_t settings;
    char last_attempted_prompt_id[BUDDY_PROMPT_ID_MAX];
    char last_successful_decision_id[BUDDY_PROMPT_ID_MAX];
    char name[BUDDY_NAME_MAX];
    char owner[BUDDY_OWNER_MAX];
    char time[BUDDY_MESSAGE_MAX];
    char message[BUDDY_MESSAGE_MAX];
    char body[BUDDY_BODY_MAX];
    /* 10/03 晚：最近 10 条 Hermes 正文（环形，[0] 最新）——
     * ▼ 短按翻对话就是在这个数组里走，看完一圈回到最新。 */
    char body_history[BUDDY_HISTORY_MAX][BUDDY_HISTORY_ENTRY_MAX];
    uint8_t body_history_count;
    uint8_t body_history_pos;
    char entries[BUDDY_ENTRY_COUNT][BUDDY_ENTRY_MAX];
    unsigned total;
    unsigned running;
    unsigned waiting;
    uint64_t tokens;
    uint64_t tokens_today;
    int64_t epoch_seconds;
    int32_t timezone_offset_seconds;
    uint64_t time_received_ms;
    uint64_t highest_celebrated_level;
    uint64_t last_heartbeat_ms;
    uint64_t temporary_until_ms;
    uint32_t prompt_connection_generation;
    uint32_t confirmation_connection_generation;
    uint32_t ble_connection_generation;
    buddy_character_t temporary_character;
    bool connected;
    bool heartbeat_stale;
    bool confirmation_pending;
    buddy_confirmation_t confirmation;
    bool confirmation_acknowledge;
    buddy_settings_item_t settings_selection;
    buddy_reset_item_t reset_selection;
    buddy_menu_item_t menu_selection;
    uint8_t pet_page;
    uint8_t info_page;
    bool menu_open;
    bool reset_open;
    bool screen_off;
    uint8_t led_effect;
    uint8_t rotation;
    uint8_t volume;            /* 10/03 晚：扬声器音量 0-100 */
    bool speak_enabled;        /* 10/03 晚：播报开关 */
    bool beep_enabled;         /* 10/03 晚：提示音开关 */
    bool demo_running;
    uint8_t brightness_level;
    uint8_t species;
    buddy_permission_delivery_t permission_delivery;
    bool approval_locked;
    bool ble_connected;
    bool ble_encrypted;
    bool battery_available;
    bool passkey_visible;
    uint32_t passkey;
    uint8_t battery_percent;
    uint16_t battery_mv;
} buddy_state_t;

void buddy_state_init(buddy_state_t *state, const buddy_settings_snapshot_t *settings);
void buddy_state_reduce(buddy_state_t *state, const buddy_event_t *event,
                        uint64_t now_ms, buddy_action_t *action);
void buddy_state_snapshot(const buddy_state_t *state, buddy_ui_snapshot_t *snapshot);
/* 10/03 晚：桥在连接后把最近几条摘要补推下来填历史（不切页、不动当前正文）。 */
void buddy_state_push_history(buddy_state_t *state, const char *body);
