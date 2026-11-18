# METAR weather when the sky is quiet (WEATHER.md): the vendor 4.2 record, its
# formatters, the three mini layouts and owned icons. Portable C; also built
# for desktop tests by tests/run_host_tests.py.
idf_component_register(SRCS "fw_weather.c" INCLUDE_DIRS "include" REQUIRES fw_text fw_clock)
