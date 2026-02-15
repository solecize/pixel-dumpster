cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")
set(PARTITION_TABLE_CUSTOM_FILENAME "partitions.csv")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(pixel-dumpster)
