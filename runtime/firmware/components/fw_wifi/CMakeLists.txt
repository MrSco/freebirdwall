# Stock's saved Wi-Fi network list and its join / lose / reconnect rules
# (SETUP.md). Portable C;
# also built for desktop tests by tests/run_host_tests.py.
idf_component_register(SRCS "fw_nets.c" "fw_link.c" INCLUDE_DIRS "include" REQUIRES json)
