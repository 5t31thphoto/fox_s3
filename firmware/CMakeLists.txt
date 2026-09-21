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
