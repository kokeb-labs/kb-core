# kb_port_esp32: the ESP32 family port of kb-core (ADR-0007). Its own project: it sees kb-core only through the
# installed package (off target) or the ESP-IDF component `kb` (on target), never through kb-core's sources.
#   Off target (seam check): cmake -S components/ports/esp32 -B build-port -DCMAKE_PREFIX_PATH=<kb-core install prefix>
#   ESP-IDF: add components/ports/esp32 to EXTRA_COMPONENT_DIRS together with the kb-core root.
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
cmake_minimum_required(VERSION 3.24)

if(ESP_PLATFORM)
  # Sources arrive with the first boundary file; the SDK components are private to this port.
  idf_component_register(INCLUDE_DIRS "inc"
                         REQUIRES kb
                         PRIV_REQUIRES esp_timer esp_driver_gpio esp_driver_i2c esp_driver_spi esp_driver_uart)
  return()
endif()

project(kb_port_esp32 DESCRIPTION "kb-core ESP32 port" LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(kb CONFIG REQUIRED COMPONENTS interfaces rtos utils)

add_library(kb_port_esp32 INTERFACE)
add_library(kb::port_esp32 ALIAS kb_port_esp32)
target_include_directories(kb_port_esp32 INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/inc>")
target_link_libraries(kb_port_esp32 INTERFACE kb::interfaces kb::rtos kb::utils)
message(STATUS "[port-esp32] found kb ${kb_VERSION}; off target only the seam is checked (no SDK)")
