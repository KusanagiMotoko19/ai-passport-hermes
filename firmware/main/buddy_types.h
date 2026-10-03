#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BUDDY_NAME_MAX 32
#define BUDDY_OWNER_MAX 32
#define BUDDY_MESSAGE_MAX 256
#define BUDDY_ENTRY_MAX 96
#define BUDDY_ENTRY_COUNT 4
#define BUDDY_PROMPT_ID_MAX 96
#define BUDDY_TOOL_MAX 48
#define BUDDY_COMMAND_MAX 32
#define BUDDY_HINT_MAX 320
#define BUDDY_JSON_LINE_MAX 4096
/* 正文缓冲：屏幕只显示 SPK 摘要（约 170 汉字）就够，**别加大** ——
 * ⚠ 这是静态内存，整机（LVGL 画布 38KB + GB2312 字符集 + BLE 栈）本来就紧：
 *   10/03 晚试过 3072 + 10×1024 历史，设备直接不广播 BLE（编译不报错，运行时静默失败）。 */
#define BUDDY_BODY_MAX 512
/* ▼ 短按能回看的对话条数（10/03 晚，科长：最多循环十次） */
#define BUDDY_HISTORY_MAX 10
/* 历史每条的单条上限（10 条 × 512 = 5KB 静态，实测已是上限） */
#define BUDDY_HISTORY_ENTRY_MAX 512

typedef enum {
    BUDDY_CONNECTION_OFFLINE,
    BUDDY_CONNECTION_CONNECTED,
    BUDDY_CONNECTION_PAIRING,
    BUDDY_CONNECTION_CONFIRMING,
} buddy_connection_t;

typedef enum {
    BUDDY_CHARACTER_SLEEP,
    BUDDY_CHARACTER_IDLE,
    BUDDY_CHARACTER_BUSY,
    BUDDY_CHARACTER_ATTENTION,
    BUDDY_CHARACTER_DIZZY,
    BUDDY_CHARACTER_HEART,
    BUDDY_CHARACTER_CELEBRATE,
    BUDDY_CHARACTER_PAIRING,
    BUDDY_CHARACTER_CONFIRMATION,
} buddy_character_t;

typedef enum {
    BUDDY_PAGE_HOME,
    BUDDY_PAGE_STATUS = BUDDY_PAGE_HOME,
    BUDDY_PAGE_PET,
    BUDDY_PAGE_INFO,
    BUDDY_PAGE_TRANSCRIPT,
    BUDDY_PAGE_SETTINGS,
} buddy_page_t;

typedef enum {
    BUDDY_MENU_SETTINGS,
    BUDDY_MENU_TURN_OFF,
    /* 「演示」已按科长 10/03 要求删除（宠物动画没人看）。
     * 「按键」= 10/04 科长要求加回的**按键速查页**（信息页第 2 页）：
     *   他记不清三枚键各是什么作用，菜单里要能一眼查到。 */
    BUDDY_MENU_KEYS,
    BUDDY_MENU_ABOUT,
    BUDDY_MENU_CLOSE,
    BUDDY_MENU_COUNT,
} buddy_menu_item_t;

typedef enum {
    BUDDY_CONFIRM_NONE,
    BUDDY_CONFIRM_UNPAIR,
    BUDDY_CONFIRM_FACTORY_RESET,
} buddy_confirmation_t;

typedef enum {
    BUDDY_SETTINGS_BRIGHTNESS,     /* 亮度 0-4（5 档） */
    BUDDY_SETTINGS_BACKLIGHT,      /* 背光特效（原「灯效」）：关/呼吸/脉冲/常亮 */
    BUDDY_SETTINGS_VOLUME,         /* 扬声器音量 0-100（设备侧，播报与提示音都吃它） */
    BUDDY_SETTINGS_SPEAK,          /* 播报开关 —— 上报桥，由桥决定念不念 */
    BUDDY_SETTINGS_BEEP,           /* 提示音开关 —— 上报桥 */
    BUDDY_SETTINGS_BLE,            /* 蓝牙开关 */
    BUDDY_SETTINGS_CLOCK_ROTATION, /* 钟向 */
    BUDDY_SETTINGS_RESET,
    BUDDY_SETTINGS_BACK,
    BUDDY_SETTINGS_COUNT,
} buddy_settings_item_t;

typedef enum {
    BUDDY_RESET_RESTORE_DEFAULTS,   /* 重置设置（原「删除角色」：首页改用头像后按了毫无反馈） */
    BUDDY_RESET_FACTORY_RESET,
    BUDDY_RESET_UNPAIR,
    BUDDY_RESET_BACK,
    BUDDY_RESET_COUNT,
} buddy_reset_item_t;

typedef enum {
    BUDDY_KEY_NONE,
    BUDDY_KEY_UP,
    BUDDY_KEY_DOWN,
    BUDDY_KEY_OK,
    BUDDY_KEY_BACK,
} buddy_key_t;

typedef enum {
    BUDDY_EVENT_NONE,
    BUDDY_EVENT_HEARTBEAT,
    BUDDY_EVENT_PROMPT,
    BUDDY_EVENT_TIME,
    BUDDY_EVENT_NAME,
    BUDDY_EVENT_OWNER,
    BUDDY_EVENT_STATUS,
    BUDDY_EVENT_STATUS_REQUEST,
    BUDDY_EVENT_UNPAIR_CONFIRMATION,
    BUDDY_EVENT_BLE_CONNECTED,
    BUDDY_EVENT_BLE_DISCONNECTED,
    BUDDY_EVENT_BLE_PASSKEY,
    BUDDY_EVENT_BLE_ENCRYPTION,
    BUDDY_EVENT_BOND_DELETE_RESULT,
    BUDDY_EVENT_PERMISSION_SEND_RESULT,
    BUDDY_EVENT_KEY_CLICK,
    BUDDY_EVENT_KEY_LONG,
    /* 双击（10/04）：目前只用于「下键双击 = 往下翻一条历史」，见 buddy_state.c 同名分支。
     * ⚠ 单独立一种事件、不复用 CLICK —— 桥侧 keymap 只认 click/long，复用会让桥
     *   把这一下也当成一次翻页送进 Hermes，凭空多翻一次。 */
    BUDDY_EVENT_KEY_DOUBLE,
    BUDDY_EVENT_TICK,
    BUDDY_EVENT_TEXT,
    BUDDY_EVENT_AUDIO,
} buddy_event_type_t;

typedef enum {
    BUDDY_ACTION_NONE,
    BUDDY_ACTION_UI_REFRESH,
    BUDDY_ACTION_PERMISSION,
    BUDDY_ACTION_SETTINGS,
    BUDDY_ACTION_STATUS,
    BUDDY_ACTION_UNPAIR_CONFIRMED,
    BUDDY_ACTION_FACTORY_RESET_CONFIRMED,
    BUDDY_ACTION_BLE_TOGGLE,
    BUDDY_ACTION_UI_SCROLL,
    BUDDY_ACTION_DISPLAY_BACKLIGHT,
    BUDDY_ACTION_SCREEN_OFF,
    BUDDY_ACTION_RESET_SETTINGS,
    BUDDY_ACTION_VOLUME,          /* 调扬声器音量（存 NVS） */
    BUDDY_ACTION_SETTING_SYNC,    /* 把菜单开关同步给桥（{"cmd":"setting",...}） */
} buddy_action_type_t;

typedef enum {
    BUDDY_PERMISSION_NONE,
    BUDDY_PERMISSION_ONCE,
    BUDDY_PERMISSION_ALWAYS,
    BUDDY_PERMISSION_DENY,
} buddy_permission_decision_t;

typedef enum {
    BUDDY_PERMISSION_DELIVERY_NONE,
    BUDDY_PERMISSION_DELIVERY_SENDING,
    BUDDY_PERMISSION_DELIVERY_SENT,
    BUDDY_PERMISSION_DELIVERY_FAILED,
} buddy_permission_delivery_t;

typedef struct {
    char id[BUDDY_PROMPT_ID_MAX];
    char tool[BUDDY_TOOL_MAX];
    char hint[BUDDY_HINT_MAX];
    size_t id_length;
    unsigned running;
    bool id_truncated;
    bool tool_truncated;
    bool hint_truncated;
    bool connected;
} buddy_prompt_t;

typedef struct {
    char message[BUDDY_MESSAGE_MAX];
    char entries[BUDDY_ENTRY_COUNT][BUDDY_ENTRY_MAX];
    unsigned total;
    unsigned running;
    unsigned waiting;
    uint64_t tokens;
    uint64_t tokens_today;
    bool connected;
    bool message_truncated;
    bool entries_truncated[BUDDY_ENTRY_COUNT];
    buddy_prompt_t prompt;
} buddy_heartbeat_t;

typedef struct {
    int64_t epoch_seconds;
    int32_t timezone_offset_seconds;
} buddy_time_sync_t;

typedef struct {
    char name[BUDDY_COMMAND_MAX];
    char value[BUDDY_MESSAGE_MAX];
    bool value_truncated;
} buddy_command_t;

/* 下行正文（Hermes 回复的文字），显示到设备转录页。 */
typedef struct {
    char body[BUDDY_BODY_MAX];
    size_t body_length;
    bool body_truncated;
} buddy_text_t;

/* 音频流声明：{"cmd":"audio","rate":8000,"codec":"ulaw","bytes":N}
 * 或 {"cmd":"audio_end"}。正文为 u-law 裸字节流，紧随声明之后。 */
typedef struct {
    uint32_t rate;
    uint32_t bytes;
    bool end;
} buddy_audio_t;

typedef struct {
    char name[BUDDY_NAME_MAX];
    char owner[BUDDY_OWNER_MAX];
    uint64_t approval_count;
    uint64_t denial_count;
    uint64_t highest_celebrated_level;
    bool ble_enabled;
    /* ── 10/03 晚新增：菜单里这些项要能活过重启 ── */
    uint8_t brightness_level;   /* 0-4 */
    uint8_t led_effect;         /* 背光特效 0-3 */
    uint8_t rotation;           /* 钟向 0-3 */
    uint8_t volume;             /* 音量 0-100 */
    bool speak_enabled;         /* 播报 */
    bool beep_enabled;          /* 提示音 */
} buddy_settings_snapshot_t;

typedef struct {
    uint32_t passkey;
    uint32_t connection_generation;
    int status;
    bool secure;
    bool success;
} buddy_ble_state_event_t;

typedef struct {
    char id[BUDDY_PROMPT_ID_MAX];
    size_t id_length;
    buddy_permission_decision_t decision;
    bool success;
} buddy_permission_result_event_t;

typedef struct {
    buddy_event_type_t type;
    buddy_key_t key;
    buddy_heartbeat_t heartbeat;
    buddy_prompt_t prompt;
    buddy_time_sync_t time;
    buddy_command_t command;
    buddy_text_t text;
    buddy_audio_t audio;
    buddy_ble_state_event_t ble;
    buddy_permission_result_event_t permission_result;
    char observed_prompt_id[BUDDY_PROMPT_ID_MAX];
    size_t observed_prompt_id_length;
    bool has_observed_prompt_id;
    bool observed_prompt_id_truncated;
} buddy_event_t;

typedef struct {
    char id[BUDDY_PROMPT_ID_MAX];
    char tool[BUDDY_TOOL_MAX];
    char hint[BUDDY_HINT_MAX];
    buddy_permission_decision_t decision;
    uint32_t connection_generation;
} buddy_permission_action_t;

typedef struct {
    buddy_action_type_t type;
    buddy_permission_action_t permission;
    buddy_settings_snapshot_t settings;
    char message[BUDDY_MESSAGE_MAX];
    int scroll_delta;
    uint8_t brightness_percent;
    uint32_t connection_generation;
    bool ble_enabled;
    bool confirmation_acknowledge;
    uint8_t volume;                    /* BUDDY_ACTION_VOLUME */
    char setting_key[16];              /* BUDDY_ACTION_SETTING_SYNC: "speak"/"beep" */
    bool setting_value;                /* BUDDY_ACTION_SETTING_SYNC */
} buddy_action_t;

typedef struct {
    buddy_connection_t connection;
    buddy_character_t character;
    buddy_page_t page;
    char name[BUDDY_NAME_MAX];
    char owner[BUDDY_OWNER_MAX];
    char time[BUDDY_MESSAGE_MAX];
    char message[BUDDY_MESSAGE_MAX];
    char body[BUDDY_BODY_MAX];
    char entries[BUDDY_ENTRY_COUNT][BUDDY_ENTRY_MAX];
    char prompt_id[BUDDY_PROMPT_ID_MAX];
    char prompt_tool[BUDDY_TOOL_MAX];
    char prompt_hint[BUDDY_HINT_MAX];
    unsigned total;
    unsigned running;
    unsigned waiting;
    uint64_t tokens;
    uint64_t tokens_today;
    int64_t epoch_seconds;
    int32_t timezone_offset_seconds;
    uint64_t time_received_ms;
    bool heartbeat_stale;
    bool confirmation_pending;
    buddy_confirmation_t confirmation;
    buddy_settings_item_t settings_selection;
    buddy_reset_item_t reset_selection;
    buddy_menu_item_t menu_selection;
    uint8_t pet_page;
    uint8_t info_page;
    bool menu_open;
    bool reset_open;
    bool screen_off;
    uint8_t led_effect;        /* 背光特效：0=关 1=呼吸 2=脉冲 3=常亮 */
    uint8_t volume;            /* 扬声器音量 0-100 */
    bool speak_enabled;        /* 播报开关（由桥决定念不念） */
    bool beep_enabled;         /* 提示音开关（由桥决定响不响） */
    uint8_t rotation;
    bool demo_running;
    uint8_t brightness_level;
    uint8_t species;
    bool approval_locked;
    buddy_permission_delivery_t permission_delivery;
    bool ble_connected;
    bool ble_encrypted;
    bool ble_enabled;
    bool battery_available;
    bool passkey_visible;
    uint32_t prompt_connection_generation;
    uint32_t confirmation_connection_generation;
    uint32_t passkey;
    uint8_t battery_percent;
    uint16_t battery_mv;
} buddy_ui_snapshot_t;
