# Copyright (c) 2021, Alliance for Open Media. All rights reserved
#
# This source code is subject to the terms of the BSD 2 Clause License and the
# Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License was
# not distributed with this source code in the LICENSE file, you can obtain it
# at www.aomedia.org/license/software. If the Alliance for Open Media Patent
# License 1.0 was not distributed with this source code in the PATENTS file, you
# can obtain it at www.aomedia.org/license/patent.

cmake_minimum_required(VERSION 3.6)
project(avifinfo C CXX)
set(CMAKE_C_STANDARD 11)

option(AVIFINFO_BUILD_TESTS
       "Build and enable tests (GoogleTest must be installed)" OFF)

# C library

add_library(avifinfo avifinfo.c)

# C++ tests

if(AVIFINFO_BUILD_TESTS)
  find_package(Threads REQUIRED) # Needed by GoogleTest
  find_package(GTest REQUIRED)

  enable_testing()
  add_executable(avifinfo_test tests/avifinfo_test.cc)
  target_include_directories(avifinfo_test PRIVATE ${GTEST_INCLUDE_DIRS}
                                                   ${CMAKE_CURRENT_SOURCE_DIR})
  target_link_libraries(avifinfo_test PRIVATE ${GTEST_BOTH_LIBRARIES} avifinfo)
  add_test(
    NAME avifinfo_test
    COMMAND ${CMAKE_CURRENT_BINARY_DIR}/avifinfo_test
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests)
endif()
