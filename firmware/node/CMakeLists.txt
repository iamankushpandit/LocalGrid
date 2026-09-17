# LocalGrid infrastructure node firmware (prototype).
cmake_minimum_required(VERSION 3.22)

if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/../common/lg_secrets.h")
    message(FATAL_ERROR "firmware/common/lg_secrets.h is missing. Run: python tools/gen_secrets.py")
endif()

# Nodes see only the infrastructure components they use. Listing components/ as a whole also
# brought in the handheld UI stack (lg_bsp, lg_ui) and made the component manager download and
# compile LVGL, which nodes must never carry (D27).
set(LG_COMPONENTS "${CMAKE_CURRENT_LIST_DIR}/../../components")
set(EXTRA_COMPONENT_DIRS "${LG_COMPONENTS}/lg_core" "${LG_COMPONENTS}/lg_crypto" "${LG_COMPONENTS}/lg_identity" "${LG_COMPONENTS}/lg_power")
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lg_node)
