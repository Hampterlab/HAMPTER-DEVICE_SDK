idf_component_register(
  SRCS
    "src/HampterDevice.cpp"
    "src/HampterHooks.cpp"
    "src/HampterPorts.cpp"
    "src/HampterToolRegistry.cpp"
    "src/CompactDns.cpp"
    "src/CredentialStore.cpp"
    "src/ProvisioningPortal.cpp"
    "src/Runtime.cpp"
  INCLUDE_DIRS "include"
  PRIV_INCLUDE_DIRS "src"
  REQUIRES
    arduino-esp32
    arduinojson
    bootloader_support
    esp_event
    esp_driver_gpio
    esp_http_server
    esp_hw_support
    esp_netif
    esp_pm
    esp_timer
    esp_wifi
    heap
    mbedtls
    nvs_flash
    # Keep the provider in the minimal component graph so its Kconfig choice
    # and default-NVS encryption constructor exist in production builds.
    nvs_sec_provider
    vfs
  PRIV_REQUIRES
    # Arduino 3.3.10 exposes Network/WiFi headers which include this managed
    # component's public types even when selective WiFiProv is disabled.
    # Hampter never calls or registers that provisioning transport.
    espressif__network_provisioning
)

if(CONFIG_HAMPTER_PRODUCTION_BUILD AND
   CONFIG_HAMPTER_DEVELOPMENT_LOG_SOFTAP_PASSWORD)
  message(FATAL_ERROR
    "Production HAMPTER firmware cannot log the SoftAP password")
endif()

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)

# The generic ESP32-C3 Arduino variant treats LED_BUILTIN as a WS2812 and
# otherwise pulls the complete legacy RMT + ring-buffer stack into every
# sketch that links GPIO. Hampter has no built-in-RGB contract; explicit
# rgbLedWrite() remains available and will link RMT only when user code calls
# it. Apply Arduino's documented opt-out to its own translation units.
idf_component_get_property(
  arduino_component_lib espressif__arduino-esp32 COMPONENT_LIB)
target_compile_definitions(${arduino_component_lib}
  PRIVATE ESP32_ARDUINO_NO_RGB_BUILTIN=1)

# Arduino-ESP32 3.3.10 omits every TLS implementation symbol when PSK suites
# are disabled. Generate a hash-pinned, minimally patched translation unit so
# ObjectLink keeps PSK disabled and does not carry unused cipher code.
if(NOT CONFIG_MBEDTLS_PSK_MODES)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/cmake/patch_arduino_ssl.py")
  idf_component_get_property(
    arduino_component_dir espressif__arduino-esp32 COMPONENT_DIR)
  set(arduino_ssl_source
      "${arduino_component_dir}/libraries/NetworkClientSecure/src/ssl_client.cpp")
  set(patched_ssl_source
      "${CMAKE_CURRENT_BINARY_DIR}/hampter_arduino_ssl_client.cpp")
  execute_process(
    COMMAND "${PYTHON}"
            "${CMAKE_CURRENT_LIST_DIR}/cmake/patch_arduino_ssl.py"
            "${arduino_ssl_source}"
            "${patched_ssl_source}"
    RESULT_VARIABLE ssl_patch_result
    OUTPUT_VARIABLE ssl_patch_output
    ERROR_VARIABLE ssl_patch_error)
  if(NOT ssl_patch_result EQUAL 0)
    message(FATAL_ERROR
      "${ssl_patch_output}${ssl_patch_error}")
  endif()
  target_sources(${COMPONENT_LIB} PRIVATE "${patched_ssl_source}")
  target_compile_definitions(${COMPONENT_LIB}
    PRIVATE HAMPTER_ARDUINO_SSL_COMPAT=1)
endif()
