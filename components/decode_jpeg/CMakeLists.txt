set(srcs "decode_jpeg_v5.c" "decode_jpeg.c")
set(include "decode_jpeg.h")

idf_component_register(SRCS "${srcs}"
                       INCLUDE_DIRS "include")
