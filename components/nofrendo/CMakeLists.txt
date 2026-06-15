set(COMPONENT_SRCDIRS "nes mappers .")
set(COMPONENT_ADD_INCLUDEDIRS ".")
set(COMPONENT_REQUIRES "")
register_component()

target_compile_options(${COMPONENT_LIB} PRIVATE -O3 -Wno-array-bounds -Wno-error=format -Wno-format)
