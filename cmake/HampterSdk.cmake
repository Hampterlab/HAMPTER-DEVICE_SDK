# Import this file before ESP-IDF's project.cmake.
get_filename_component(HAMPTER_SDK_DIR
  "${CMAKE_CURRENT_LIST_DIR}/.."
  ABSOLUTE)

list(APPEND EXTRA_COMPONENT_DIRS
  "${HAMPTER_SDK_DIR}/components/hampter_device")
