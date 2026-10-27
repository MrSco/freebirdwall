# Airport locations from OurAirports (public domain; tools/make_airports.py)
# and the route progress for the progress bar (FLIGHT-SCREENS.md). Portable C;
# the table lives in flash.
idf_component_register(SRCS "fw_airports.c" "airports_table.c" INCLUDE_DIRS "include")
