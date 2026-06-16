file(GLOB NES_SRCS
    "nes/*.c"
    "mappers/*.c"
    "nofrendo.c"
)

idf_component_register(
    SRCS ${NES_SRCS}
    INCLUDE_DIRS "." "nes" "mappers"
    REQUIRES "driver"
)

# Disable format warnings for retro-go nofrendo
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-format -Wno-format-security)
