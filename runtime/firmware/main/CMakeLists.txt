idf_component_register(
    SRCS "app_main.c" "memory_probe.c" "route_probe.cpp" "owner_descriptor.c" "display.cpp" "setup_server.c" "saved_networks.c" "wifi_link.c" "games_server.c" "owned_settings.c" "owner_update.c" "feed.c" "aliases.c" "settings_store.c" "dashboard_api.c" "community.cpp" "big_buffer.c" "stats.c" "crash_guard.c" "wall_log.c" "board_check.c" "sync_store.c" "app_settings.cpp" "aero_key.c" "panic_note.c" "position_fix.cpp" "net_watch.c"
         "../../../shared/owner_ota.c" "../../../shared/owner_sha256.c"
         "../../../shared/owner_alias.c"
    INCLUDE_DIRS "."
    EMBED_FILES "icon.png" "icon.svg"
    PRIV_INCLUDE_DIRS "../../../shared"
    PRIV_REQUIRES fw_settings fw_analytics fw_community fw_feed fw_sync fw_wifi fw_games fw_tls hub75 fw_text fw_screens fw_clock fw_flight fw_logo fw_assets fw_airports nvs_flash efuse spi_flash esp_wifi esp_netif esp_event app_update esp_timer esp_http_server esp_https_server json lwip)

# The pages go out gzip-compressed (Content-Encoding: gzip): a third of the
# bytes, so a third of the time each phone's connection holds send buffers in
# the wall's small heap (UPDATES.md "Network watchdog").
idf_build_get_property(fw_python PYTHON)
foreach(page "games_page.html" "dashboard.html" "preview.wasm")
    set(gz "${CMAKE_CURRENT_BINARY_DIR}/${page}.gz")
    add_custom_command(OUTPUT "${gz}"
        COMMAND "${fw_python}" "${CMAKE_CURRENT_LIST_DIR}/../tools/gzip_asset.py" "${CMAKE_CURRENT_LIST_DIR}/${page}" "${gz}"
        DEPENDS "${CMAKE_CURRENT_LIST_DIR}/${page}" "${CMAKE_CURRENT_LIST_DIR}/../tools/gzip_asset.py"
        VERBATIM)
    target_add_binary_data(${COMPONENT_LIB} "${gz}" BINARY DEPENDS "${gz}")
endforeach()

# A crash writes a line into the log ring first (panic_note.c).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_panic_handler" "-Wl,--wrap=panic_abort"
                      "-u __wrap_esp_panic_handler" "-u __wrap_panic_abort")

# route_probe.cpp (http_body.h) and display.cpp are C++17.
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>)
