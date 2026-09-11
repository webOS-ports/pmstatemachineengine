# @@@LICENSE
#
#      Copyright (c) 2009-2013 LG Electronics, Inc.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
# LICENSE@@@

# Standalone test build (not wired into the top-level webOS build):
#
#   cd tests && mkdir -p build && cd build && cmake .. && make
#   ./PmStateMachineEngineTest                       # functional tests
#   ./PmStateMachineEngineStress [iterations] [seed] # stress/invariant tests
#
# Options:
#   -DENABLE_SANITIZERS=ON   build with ASan + UBSan (host debugging)
#
# The tests compile the engine sources directly, with webOS-specific
# features (PmLogLib) disabled, so no external dependencies are needed.
# Binaries are built with hardening flags (stack protector, fortified
# libc calls, format-security warnings as errors).

cmake_minimum_required (VERSION 3.10...3.28)

project (PmStateMachineEngineTest C CXX)

option(ENABLE_SANITIZERS "Build tests with AddressSanitizer + UBSan" OFF)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/../include/public
    ${CMAKE_CURRENT_SOURCE_DIR}/../include/public/PmStateMachineEngine)

add_definitions(-DFSM_CONFIG_USE_CUSTOM_ASSERT=0 -DFSM_CONFIG_WEBOS_FEATURES=0)

set(ENGINE_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Fsm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/FsmDbg.c)

# Hardening + diagnostics for all test binaries
add_compile_options(
    -Wall -Wextra
    -Wformat=2 -Werror=format-security
    -fstack-protector-strong)
if (NOT CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT ENABLE_SANITIZERS)
    # _FORTIFY_SOURCE requires optimization and conflicts with ASan
    add_compile_options(-U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2)
endif ()

if (ENABLE_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all)
    add_link_options(-fsanitize=address,undefined)
endif ()

add_executable(PmStateMachineEngineTest
    src/Main.cpp
    src/TestCommon.cpp
    src/CplusplusTest.cpp
    ${ENGINE_SOURCES})

# The legacy functional tests intentionally define handler callbacks
# that ignore some parameters
target_compile_options(PmStateMachineEngineTest PRIVATE -Wno-unused-parameter)

add_executable(PmStateMachineEngineStress
    src/StressTest.cpp
    ${ENGINE_SOURCES})

enable_testing()
add_test(NAME functional COMMAND PmStateMachineEngineTest)
add_test(NAME stress COMMAND PmStateMachineEngineStress 200000)
