# Owned settings: edit validation and browser pairing (SETTINGS.md). Portable
# C; also built for desktop tests (tests/test_settings.py, run_host_tests.py).
idf_component_register(SRCS "fw_settings.c" "fw_pair.c" "fw_cloud.c""../../../../shared/owner_sha256.c"
                       INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "../../../../shared" REQUIRES json)
