# Stock "Plus Sync" legs, texts and screens (SYNC.md). Portable C; also built
# for desktop tests by tests/run_host_tests.py.
idf_component_register(SRCS "fw_sync.c" "fw_schedule.c" INCLUDE_DIRS "include" REQUIRES fw_text fw_screens json)
