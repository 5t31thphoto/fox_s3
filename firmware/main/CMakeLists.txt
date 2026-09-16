# One Arduino-as-component IDF app. Every .cpp/.c is a source; the .inc files
# are #included by fox_main.cpp / fox_llm.cpp / fox_face.inc and must NOT be
# listed here.
idf_component_register(
    SRCS
        "fox_main.cpp"
        "fox_brain.cpp"
        "fox_voice.cpp"
        "fox_memory.cpp"
        "fox_llm.cpp"
        "fox_sam.c"
    INCLUDE_DIRS "."
)

# Our style uses some compact one-liners and cross-TU helpers that the IDF
# build otherwise promotes to hard errors via -Werror. Keep the code warnings
# visible but DO NOT let these specific categories fail the build. (Real errors
# still fail; this only relaxes the pedantic warning-as-error set.)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=misleading-indentation
    -Wno-error=unused-function
    -Wno-error=unused-variable
    -Wno-error=unused-but-set-variable
    -Wno-error=dangling-reference
    -Wno-error=volatile
    -Wno-error=format
    -Wno-error=sign-compare
    -Wno-error=narrowing
)
