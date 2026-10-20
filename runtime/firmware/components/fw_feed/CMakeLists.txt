# The vendor flight feed: response parser, request body, display settings,
# HTTP response reader (FEED.md). Portable C; also built for desktop tests by
# tests/test_feed.py.
idf_component_register(SRCS "fw_feed_msgpack.c" "fw_feed_config.c" "fw_feed_http.c" INCLUDE_DIRS "include"
                       REQUIRES fw_flight fw_sync json)
