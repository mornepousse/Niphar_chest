# Coffre Niphar — ESP32-P4.
cmake_minimum_required(VERSION 3.16)

# Sélection de la carte : idf.py -DBOARD=niphar_chest build
#
# Le défaut est le kit, et ce n'est pas arbitraire : c'est le seul matériel qui
# existe, et le sens de l'erreur compte. Flasher du jc_devkit sur un coffre ne
# fait que priver du lien ; l'inverse enverrait du SPI dans le bus I2C du codec
# audio du kit.
if(NOT DEFINED BOARD)
    set(BOARD "jc_devkit" CACHE STRING "Carte cible")
endif()
set(BOARD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD}" CACHE INTERNAL "")
if(NOT EXISTS "${BOARD_DIR}/board.h")
    message(FATAL_ERROR "Carte '${BOARD}' inconnue : ${BOARD_DIR}/board.h est absent")
endif()
message(STATUS "Niphar_chest board: ${BOARD}")

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
