# kb-core ESP32 examples: main component. The app source comes from app_config.yml (APP_SOURCE_FILE).
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The kb-core authors
if(NOT DEFINED APP_SOURCE_FILE)
  set(APP_SOURCE_FILE "${DEFAULT_SOURCE_FILE}")
endif()
idf_component_register(SRCS "${APP_SOURCE_FILE}" INCLUDE_DIRS "." REQUIRES kb log freertos)
# -Wpedantic stays off here: ESP-IDF's own headers use GCC extensions (#include_next).
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wconversion -Wsign-conversion -Wshadow
                       -Wformat=2 -Werror)
