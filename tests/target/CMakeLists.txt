# On-board test app for lg_core and lg_crypto.
# Runs unit tests, RFC crypto test vectors, and a simulated three-node network,
# then prints LG_TESTS_RESULT: PASS or FAIL over serial.
cmake_minimum_required(VERSION 3.22)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lg_core_tests)
