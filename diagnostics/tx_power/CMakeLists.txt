cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
add_compile_definitions(ARDUINO_USB_MODE=1 ARDUINO_USB_CDC_ON_BOOT=1)

project(hampter_tx_power_diagnostic)
