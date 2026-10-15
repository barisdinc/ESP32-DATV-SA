idf_component_register(SRCS "main.c" "oled.c" "panel.c" "beacon_enc.c" "lut8.S" "lutg.S" "lutg_psk8.S" "lutg_p8s8.S" "lutg_a16.S" "lutg_a16s8.S" "lutg_a32.S" "lutb.S" "lutf.S"
                       EMBED_FILES "beacon.bin"
                       REQUIRES esp_phy esp_wifi nvs_flash esp_partition esp_rom esp_hal_mspi esp_timer esp_driver_usb_serial_jtag esp_driver_gpio esp_app_format)

# Fail at link time instead of booting with data in the RF ownership bank.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,-T${CMAKE_CURRENT_LIST_DIR}/rf_bank_guard.ld")
