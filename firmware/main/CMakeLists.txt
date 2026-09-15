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
