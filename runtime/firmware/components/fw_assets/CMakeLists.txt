# Read-only access to the vendor's asset partition (LittleFS, label "spiffs",
# 0x590000). littlefs v2.11.3 is compiled with LFS_READONLY, so no code that
# could program or erase flash exists in this component; the block device
# also refuses writes. Static buffers only (LFS_NO_MALLOC).
set(lfs "${CMAKE_CURRENT_LIST_DIR}/../../../build/deps/littlefs")
get_filename_component(lfs "${lfs}" ABSOLUTE)
if(NOT EXISTS "${lfs}/lfs.c")
    message(FATAL_ERROR "littlefs missing. Run: py runtime/firmware/tools/fetch_deps.py")
endif()
idf_component_register(SRCS "fw_assets.c" "${lfs}/lfs.c" "${lfs}/lfs_util.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "${lfs}"
                       PRIV_REQUIRES spi_flash freertos)
target_compile_definitions(${COMPONENT_LIB} PRIVATE LFS_READONLY LFS_NO_MALLOC LFS_NO_DEBUG)
