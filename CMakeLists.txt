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
# Volontairement PAS add_compile_definitions() : la version change à chaque
# commit, et une définition globale invaliderait tous les objets du projet
# (34 s de rebuild au lieu de 2). main/CMakeLists.txt l'applique au seul
# composant qui l'utilise.
set(NIPHAR_VERSION "${NIPHAR_VERSION}" CACHE INTERNAL "")

# Build « trimmé » : main et ses dépendances uniquement.
set(COMPONENTS main)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(niphar_chest)
