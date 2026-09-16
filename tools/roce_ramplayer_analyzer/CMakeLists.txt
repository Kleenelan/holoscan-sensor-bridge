# SPDX-FileCopyrightText: Copyright (c) 2026 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
# SPDX-License-Identifier: Apache-2.0
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

# Standalone RoCEv2 receiver/analyzer for the FPGA RAM player. Links only
# the native hololink core (control plane) and libibverbs — no Holoscan
# SDK — so it builds in HOLOLINK_BUILD_ONLY_NATIVE configurations too, as
# long as ibverbs is present on the host.
find_package(ibverbs QUIET)
if(NOT ibverbs_FOUND)
    message(STATUS "ibverbs not found; skipping roce_ramplayer_analyzer")
    return()
endif()

add_executable(roce_ramplayer_analyzer
    roce_ramplayer_analyzer.cpp
)

target_link_libraries(roce_ramplayer_analyzer
    PRIVATE
        hololink_core
        ${ibverbs_LIBRARIES}
)

target_include_directories(roce_ramplayer_analyzer
    PRIVATE
        ${ibverbs_INCLUDE_DIRS}
)

target_compile_features(roce_ramplayer_analyzer PRIVATE cxx_std_17)

install(TARGETS roce_ramplayer_analyzer
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT hololink-tools
)

# Companion scripts that drive the RAM player's registers via hololink-write.
install(PROGRAMS
    ramplayer_on.sh
    ramplayer_off.sh
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    COMPONENT hololink-tools
)
