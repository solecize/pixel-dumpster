idf_component_register(
    SRCS "lodepng.c"
    INCLUDE_DIRS "include"
)

# Compile as C, decoder only — no encoder, no disk I/O, no C++ features
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    LODEPNG_NO_COMPILE_ENCODER
    LODEPNG_NO_COMPILE_DISK
    LODEPNG_NO_COMPILE_ANCILLARY_CHUNKS
    LODEPNG_NO_COMPILE_CPP
)

# lodepng uses some constructs that trigger warnings in strict C mode
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function)
