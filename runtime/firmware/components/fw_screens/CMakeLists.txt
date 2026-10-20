# Screen layouts for the 128x64 panel, drawn through an FwSurface. Portable C:
# the board draws to the panel, tools/preview_screens.py to a PNG.
idf_component_register(SRCS "fw_screens.c" "fw_canvas.c" "fw_brand.c" INCLUDE_DIRS "include" REQUIRES fw_text fw_logo fw_flight qrcode)
