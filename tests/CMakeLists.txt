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
#   ./PmStateMachineEngineTest
#
# The tests compile the engine sources directly, with webOS-specific
# features (PmLogLib) disabled, so no external dependencies are needed.

cmake_minimum_required (VERSION 3.10...3.28)

project (PmStateMachineEngineTest C CXX)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/../include/public
    ${CMAKE_CURRENT_SOURCE_DIR}/../include/public/PmStateMachineEngine)

add_definitions(-DFSM_CONFIG_USE_CUSTOM_ASSERT=0 -DFSM_CONFIG_WEBOS_FEATURES=0)

add_executable(PmStateMachineEngineTest
    src/Main.cpp
    src/TestCommon.cpp
    src/CplusplusTest.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/Fsm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/FsmDbg.c)
