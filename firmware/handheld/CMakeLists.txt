# LocalGrid handheld firmware (prototype): joins the grid and shows its state on screen.
cmake_minimum_required(VERSION 3.22)

if(NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/../common/lg_secrets.h")
    message(FATAL_ERROR "firmware/common/lg_secrets.h is missing. Run: python tools/gen_secrets.py")
endif()

# Only the components a handheld uses (D27): protocol core, crypto, identity, board support, UI.
set(LG_COMPONENTS "${CMAKE_CURRENT_LIST_DIR}/../../components")
set(EXTRA_COMPONENT_DIRS "${LG_COMPONENTS}/lg_core" "${LG_COMPONENTS}/lg_crypto" "${LG_COMPONENTS}/lg_identity"
                         "${LG_COMPONENTS}/lg_board" "${LG_COMPONENTS}/lg_bsp" "${LG_COMPONENTS}/lg_ui"
                         "${LG_COMPONENTS}/lg_selftest")
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lg_handheld)
