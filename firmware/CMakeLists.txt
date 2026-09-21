cmake_minimum_required(VERSION 3.16)

# Must be before project() so it applies when components compile.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# PicoTTS upstream trips GCC -Werror=stringop-overflow (false positive on markup tables).
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=stringop-overflow" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-stringop-overflow" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=stringop-overread" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Wno-error=stringop-overflow" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Wno-stringop-overflow" APPEND)

project(fox)

# Belt-and-suspenders: target-specific flags if the component target exists.
idf_component_get_property(PICOTTS_LIB esp-picotts COMPONENT_LIB)
if(PICOTTS_LIB)
    target_compile_options(${PICOTTS_LIB} PRIVATE
        -Wno-error=stringop-overflow
        -Wno-stringop-overflow
        -Wno-error=stringop-overread
        -Wno-error=format-overflow
        -Wno-error=array-bounds
    )
endif()
