# The following lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS $ENV{IDF_PATH}/examples/common_components/protocol_examples_common)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(DoingTV)
spiffs_create_partition_image(storage1 fonts FLASH_IN_PROJECT)
spiffs_create_partition_image(storage2 images FLASH_IN_PROJECT)
