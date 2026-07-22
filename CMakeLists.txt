# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mcp_sensors)

# ESP32-P4 ESP-NN kernels can saturate YAMNet INT8 outputs; fall back to reference ops.
if(CONFIG_YAMNET_DISABLE_ESP_NN)
    idf_component_get_property(tflite_lib espressif__esp-tflite-micro COMPONENT_LIB)
    if(tflite_lib)
        target_compile_options(${tflite_lib} PRIVATE -UESP_NN)
        message(STATUS "YAMNet: ESP-NN disabled — using TFLite reference kernels")
    endif()
endif()

spiffs_create_partition_image(storage spiffs FLASH_IN_PROJECT)

# YAMNet 模型分区（单文件 yamnet_cry_distill.tflite）
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/models/yamnet/yamnet_cry_distill.tflite")
    spiffs_create_partition_image(yamnet models/yamnet FLASH_IN_PROJECT)
else()
    message(WARNING "models/yamnet/yamnet_cry_distill.tflite missing — run scripts/fetch_yamnet_model.ps1")
endif()
