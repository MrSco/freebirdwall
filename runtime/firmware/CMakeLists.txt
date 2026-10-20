# Source-built FlightWall firmware, skeleton stage.
# ESP-IDF v4.4.7 deliberately: it is the SDK the installed stock bootloader was
# built from (byte-identical match recorded in OTA-TEST-REPORT.md), so app
# images from this project are loaded by that bootloader without replacing it.
cmake_minimum_required(VERSION 3.16)

# HUB75 panel library, pinned to the commit the vendor firmware used (PANEL.md).
set(FW_HUB75_DIR "${CMAKE_CURRENT_LIST_DIR}/../build/deps/hub75")
set(FW_HUB75_COMMIT "4de6007534d9f7585b8518346f86d81573ce232c")
if(NOT EXISTS "${FW_HUB75_DIR}/.git/HEAD")
    message(FATAL_ERROR "HUB75 library missing. Run: py runtime/firmware/tools/fetch_deps.py")
endif()
file(READ "${FW_HUB75_DIR}/.git/HEAD" fw_hub75_head)
string(STRIP "${fw_hub75_head}" fw_hub75_head)
if(NOT fw_hub75_head STREQUAL FW_HUB75_COMMIT)
    message(FATAL_ERROR "HUB75 library is at ${fw_hub75_head}, expected ${FW_HUB75_COMMIT}. Run tools/fetch_deps.py")
endif()
set(EXTRA_COMPONENT_DIRS "${FW_HUB75_DIR}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(freebirdwall)
