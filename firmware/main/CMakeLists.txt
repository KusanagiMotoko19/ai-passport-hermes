idf_component_register(
    SRCS "buddy_orchestrator.c" "main.c"
         "buddy_line.c"
         "buddy_ble.c"
         "buddy_ble_store.c"
         "buddy_ble_lifecycle.c"
         "buddy_app_logic.c"
         "buddy_protocol.c"
         "buddy_state.c"
         "buddy_settings.c"
         "buddy_character.c"
        "buddy_ui.c"
        "buddy_i4.c"
        "buddy_sprite.c"
        "buddy_girl.c"
        "buddy_text_layout.c"
        "buddy_adpcm.c"
        "fap_screenshot.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bt esp_hw_support esp_timer json nvs_flash esp_driver_usb_serial_jtag
)

# 中文字形子集（由 assets/fonts/unifont-16.0.04.otf 用 lv_font_conv 生成，bpp=1 保持 A1 位图格式）。
target_sources(${COMPONENT_LIB} PRIVATE
    "${CMAKE_CURRENT_LIST_DIR}/../assets/fonts/ui_font_cjk_16.c"
)

# ESP-IDF 5.5.3 only applies WRITE_ENC to generated CCCDs. Wrap the private
# registration boundary so protected CCCDs also receive READ_ENC.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=ble_att_svr_register")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,-u,__wrap_ble_att_svr_register")
