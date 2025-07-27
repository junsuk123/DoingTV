set(srcs "st7789.c" "fontx.c")
set(include "st7789.h" "fontx.h")

idf_component_register(SRCS "${srcs}"
                       PRIV_REQUIRES driver
                       INCLUDE_DIRS "include")
