# The community flight source's portable part (FEED.md "Community feed"): the
# shared runtime reader, filters, route cache and request policy
# (runtime/src, esp32/stream_adsb.cpp; tested there by ctest), and
# fw_community_frame.cpp, which turns its records into the screens' list
# (tests/test_community.py). Portable C++17; the board worker is
# main/community.cpp.
set(runtime "${CMAKE_CURRENT_LIST_DIR}/../../..")
get_filename_component(runtime "${runtime}" ABSOLUTE)

idf_component_register(
    SRCS "fw_community_frame.cpp" "fw_aero.cpp"
         "${runtime}/src/flight_data.cpp"
         "${runtime}/src/provider_policy.cpp"
         "${runtime}/src/community_config.cpp"
         "${runtime}/src/community_cache.cpp"
         "${runtime}/esp32/stream_adsb.cpp"
    INCLUDE_DIRS "include" "${runtime}/src" "${runtime}/esp32"
    REQUIRES fw_feed fw_flight fw_airports)

# Eight records per list (the reader's staging, the cache, the shown list):
# five on screen plus spares for filtering, about 3.7 KiB each instead of the
# 14.6 KiB the desktop default of 32 would take. Every user of flight_data.h
# must agree, so it is public.
target_compile_definitions(${COMPONENT_LIB} PUBLIC FLIGHTWALL_MAX_FLIGHTS=8)
target_compile_options(${COMPONENT_LIB} PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>)
