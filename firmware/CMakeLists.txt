cmake_minimum_required(VERSION 3.16)

# Must be before project() so it applies when components compile.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# PicoTTS upstream trips GCC -Werror=stringop-overflow (false positive on markup tables).
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=stringop-overflow" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-stringop-overflow" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=stringop-overread" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Wno-error=stringop-overflow" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Wno-stringop-overflow" APPEND)

# Arduino `Serial` defaults to UART0 (GPIO43/44). Attach it to the USB
# Serial/JTAG port instead so the web flasher's config line reaches
# input_poll() and FOX_CONFIG_OK comes back over the same cable. These must be
# global compile definitions so the Arduino core itself is built with them.
# (Pattern from espressif/arduino-esp32 example hw_cdc_hello_world.)
list(APPEND compile_definitions "ARDUINO_USB_CDC_ON_BOOT=1")
list(APPEND compile_definitions "ARDUINO_USB_MODE=1")

project(fox)
