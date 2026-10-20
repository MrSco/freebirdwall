# bitbank2/PNGdec 1.1.7 (Apache-2.0), the decoder the vendor firmware uses,
# built from the pinned checkout in build/deps/pngdec as plain C.
#   __LINUX__  selects PNGdec's stdlib headers instead of Arduino.h; nothing
#              else in the library changes behaviour on it except an unused
#              fopen-based PNG_openFile.
#   PNG_MAX_BUFFERED_PIXELS  line buffer sized for 128-pixel-wide RGBA; the
#              logos are 64 wide. The desktop default would be 16 KiB.
# Both are PUBLIC: PNGIMAGE's layout depends on them, so every unit that
# includes PNGdec.h must agree.
set(src "${CMAKE_CURRENT_LIST_DIR}/../../../build/deps/pngdec/src")
get_filename_component(src "${src}" ABSOLUTE)
if(NOT EXISTS "${src}/png.inl")
    message(FATAL_ERROR "PNGdec missing. Run: py runtime/firmware/tools/fetch_deps.py")
endif()
idf_component_register(
    SRCS "pngdec_unit.c" "${src}/adler32.c" "${src}/crc32.c" "${src}/infback.c"
         "${src}/inffast.c" "${src}/inflate.c" "${src}/inftrees.c" "${src}/zutil.c"
    INCLUDE_DIRS "${src}")
target_compile_definitions(${COMPONENT_LIB} PUBLIC __LINUX__=1 "PNG_MAX_BUFFERED_PIXELS=((128*4+1)*2)")
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error -Wno-unused-function -Wno-unused-variable -Wno-implicit-fallthrough)
