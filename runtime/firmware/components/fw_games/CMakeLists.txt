# Pong and Breakout (OWNED-FIRMWARE-PLAN.md "Extras"). Portable C; also built
# for desktop tests by tests/run_host_tests.py.
idf_component_register(SRCS "fw_pong.c" "fw_breakout.c" "fw_arcade.c" INCLUDE_DIRS "include" REQUIRES fw_text)
