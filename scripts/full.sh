#!/usr/bin/env bash
#
# Phase complète du coffre Niphar : rebuild depuis zéro.
#
# Appelé par scripts/check.sh — ne pas l'invoquer depuis un hook directement.
# La phase rapide construit en incrémental ; celle-ci repart de rien, pour que
# jamais un artefact périmé ne maquille une erreur de compilation.
#
set -euo pipefail
cd "$(dirname "$0")/.."

if ! command -v idf.py >/dev/null 2>&1; then
    # shellcheck disable=SC1091
    . "${IDF_EXPORT:-$HOME/esp/esp-idf/export.sh}" >/dev/null 2>&1
fi

idf.py fullclean >/dev/null
exec idf.py build
