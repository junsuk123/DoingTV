set(srcs "decode_png.c")
set(include "decode_png.h")

idf_component_register(SRCS "${srcs}"
                       INCLUDE_DIRS "include"
                       REQUIRES pngle)
