cmake_minimum_required(VERSION 3.16)

# Reuse the firmware component tree (pd-display, pd-config, lodepng, …).
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../components"
)

# Only build main + its deps; avoid pd-wizard/pd-network (USB/mdns) from the tree.
set(COMPONENTS main)

set(PARTITION_TABLE_CUSTOM_FILENAME "${CMAKE_CURRENT_LIST_DIR}/../../partitions.csv")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(draw-mode-bench)
