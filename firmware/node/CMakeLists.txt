# LocalGrid infrastructure node firmware (prototype).
cmake_minimum_required(VERSION 3.22)

if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/../common/lg_secrets.h")
    message(FATAL_ERROR "firmware/common/lg_secrets.h is missing. Run: python tools/gen_secrets.py")
endif()

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lg_node)
