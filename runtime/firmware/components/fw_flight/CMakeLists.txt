# Flight text as the stock firmware builds it (FLIGHT-SCREENS.md): the
# template engine, unit formatting, element templates and tracked-flight
# rules. Portable C; also built for desktop tests by tests/run_host_tests.py.
idf_component_register(SRCS "fw_flight.c" "fw_rotation.c" INCLUDE_DIRS "include" REQUIRES fw_text)
