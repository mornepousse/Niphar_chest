#!/usr/bin/env bash
#
# Phase complète du coffre Niphar : rebuild des deux cartes, depuis zéro.
#
# Appelé par scripts/check.sh — ne pas l'invoquer depuis un hook directement.
# La phase rapide ne construit que la carte par défaut, en incrémental ; celle-ci
# repart de rien pour qu'aucun artefact périmé ne maquille une erreur, et couvre
# la carte qui n'existe pas encore en matériel — c'est le seul endroit où le
# firmware du coffre est compilé.
#
# Chaque carte a son dossier de build ET son sdkconfig, sinon la configuration
# de l'une fuit dans l'autre.
#
set -euo pipefail
cd "$(dirname "$0")/.."

if ! command -v idf.py >/dev/null 2>&1; then
    # shellcheck disable=SC1091
    . "${IDF_EXPORT:-$HOME/esp/esp-idf/export.sh}" >/dev/null 2>&1
fi

for board in jc_devkit niphar_chest; do
    echo "=== build complet : $board ==="
    rm -rf "build_$board"
    idf.py -B "build_$board" -DBOARD="$board" -DSDKCONFIG="build_$board/sdkconfig" build
done
