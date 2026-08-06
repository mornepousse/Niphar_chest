# Coffre Niphar — ESP32-P4. Cible de build unique : le kit JC-ESP32P4-M3-DEV et
# le coffre partagent le même pinout (voir docs/HARDWARE.md).
cmake_minimum_required(VERSION 3.16)

# Version : le tag git est la source de vérité, pas de fichier VERSION.
# --always couvre le cas « aucun tag encore posé ».
execute_process(
    COMMAND git describe --tags --always --dirty
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE NIPHAR_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT NIPHAR_VERSION)
    set(NIPHAR_VERSION "unknown")
endif()
message(STATUS "Niphar_chest version: ${NIPHAR_VERSION}")
add_compile_definitions(NIPHAR_VERSION="${NIPHAR_VERSION}")

# Build « trimmé » : main et ses dépendances uniquement.
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(niphar_chest)
