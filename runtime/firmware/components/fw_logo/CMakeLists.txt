# Airline logos drawn exactly as the stock firmware draws them (LOGOS.md).
# Portable C; desktop tests and the preview build the same file.
idf_component_register(SRCS "fw_logo.c" "fw_silhouette.c" "aircraft_families.c" INCLUDE_DIRS "include" REQUIRES fw_text PRIV_REQUIRES pngdec)
