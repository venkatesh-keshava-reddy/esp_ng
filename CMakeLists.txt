# ESP-NG - ESP32 Next Generation Framework
# Componentized architecture with reusable modules

cmake_minimum_required(VERSION 3.16)

# Set project name
set(PROJECT_NAME "esp_ng")

# Include ESP-IDF's project.cmake
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(${PROJECT_NAME})
