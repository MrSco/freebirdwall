# Source-built TLS (pinned Mbed TLS 3.6.7, fw_-prefixed symbols), its C port,
# the ESP32 socket transport and the bounded HTTP body reader, as one ESP-IDF
# component.
#
# The pinned Mbed TLS headers and its private configuration are PRIVATE to this
# component. Consumers see only tls_port.h, esp32_transport.h and http_body.h.
# IDF's own mbedtls component (used by the Wi-Fi supplicant) is not a
# requirement here, so its headers and MBEDTLS_CONFIG_FILE never reach these
# sources, and the fw_ symbol prefix keeps the two libraries apart at link time.
set(runtime "${CMAKE_CURRENT_LIST_DIR}/../../..")
get_filename_component(runtime "${runtime}" ABSOLUTE)
set(net "${runtime}/net")
set(mbedtls "${runtime}/build/net-deps/mbedtls")

if(NOT EXISTS "${mbedtls}/.flightwall-source.json")
    message(FATAL_ERROR "Pinned Mbed TLS source missing. Run: py runtime/net/fetch_source.py")
endif()

file(GLOB tls_sources "${mbedtls}/library/*.c")

idf_component_register(
    SRCS ${tls_sources}
         "${net}/tls_client.cpp"
         "${net}/tls_port.cpp"
         "${net}/esp32_transport.c"
         "${runtime}/esp32/http_body.cpp"
    INCLUDE_DIRS "${net}" "${runtime}/esp32"
    PRIV_INCLUDE_DIRS "${mbedtls}/include" "${mbedtls}/library"
    PRIV_REQUIRES esp_timer esp_hw_support freertos)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    MBEDTLS_CONFIG_FILE="fw_mbedtls_config.h"
    FW_TLS_TX_BYTES=2048)

# IDF v4.4 defaults to gnu++11; the client is C++17. Later -std wins in GCC.
target_compile_options(${COMPONENT_LIB} PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>)

# Upstream library code is not ours to restyle; keep its warnings visible but
# non-fatal. Our own sources stay under IDF's -Werror=all.
set_source_files_properties(${tls_sources} PROPERTIES COMPILE_OPTIONS "-Wno-error")
