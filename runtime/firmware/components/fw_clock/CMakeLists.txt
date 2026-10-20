# The stock clock screen (CLOCK.md): local time with its DST rules, 12/24-hour
# and localised date text, and the stock line layout. Portable C; also built
# for desktop tests by tests/run_host_tests.py.
idf_component_register(SRCS "fw_clock.c" "clock_tables.c" INCLUDE_DIRS "include" REQUIRES fw_text)
