# Text rendering with the stock firmware's 5x7 font and character tables.
# Portable C; also built for desktop tests by tests/run_host_tests.py.
idf_component_register(SRCS "fw_text.c" "font5x7.c" "vendor_charmap.c" INCLUDE_DIRS "include")
