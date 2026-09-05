#!/usr/bin/env bash
#
# Phase complète du coffre Niphar : rebuild des trois cartes, depuis zéro.
#
# Appelé par scripts/check.sh — ne pas l'invoquer depuis un hook directement.
# La phase rapide ne construit que la carte par défaut, en incrémental ; celle-ci
# repart de rien pour qu'aucun artefact périmé ne maquille une erreur, et couvre
# niphar_chest — la carte qui n'existe pas encore en matériel — c'est le seul
# endroit où son firmware est compilé.
#
# Chaque carte a son dossier de build ET son sdkconfig, sinon la configuration
# de l'une fuit dans l'autre.
#
# Elle porte AUSSI le harnais hôte sous sanitiseurs — voir plus bas.
#
set -euo pipefail

# --- Repli sur l'atelier Nix quand la toolchain manque ---------------------
#
# Sur NixOS, le venv d'ESP-IDF (~/.espressif/python_env/...) capture des chemins
# /nix/store qui finissent ramassés par le garbage collector : export.sh échoue
# alors avec « virtual environment not found » et idf.py reste introuvable. Le
# tripwire devient rouge par ABSENCE D'OUTIL, ce qui est indiscernable d'une
# régression pour qui lit le verdict — exactement le mode de panne que
# CLAUDE.md interdit (« un outil absent n'est pas une régression »).
#
# On se relance donc une fois dans le devShell du flake, qui fournit IDF 5.5.2
# sans venv. Le sentinelle évite la récursion ; le coût n'est payé QUE si la
# toolchain manque vraiment.
if ! command -v idf.py >/dev/null 2>&1 \
   && [ -z "${NIPHAR_NIX_IDF:-}" ] \
   && command -v nix >/dev/null 2>&1 \
   && [ -f "${NIPHAR_IDF_FLAKE:-$HOME/nixos-config}/flake.nix" ]; then
    export NIPHAR_NIX_IDF=1
    exec nix develop "${NIPHAR_IDF_FLAKE:-$HOME/nixos-config}#esp-idf" \
         --command "$0" "$@"
fi
cd "$(dirname "$0")/.."

# --- Harnais hôte sous ASan/UBSan ----------------------------------------
#
# ICI et pas dans scripts/fast.sh : instrumenter coûte un rebuild complet du
# harnais plus une exécution deux à trois fois plus lente, ce que la phase
# rapide — jouée à chaque fin de tour par le hook Stop — ne peut pas porter.
# La phase complète, elle, tourne au pre-push : c'est le bon endroit pour un
# oracle cher.
#
# CE QUE ÇA RAPPORTE, mesuré et pas supposé : la ronde 4 de la tâche 4 de
# cette branche a produit un mutant que la suite non instrumentée déclarait
# vert et que SEUL ASan attrapait. Un débordement de quelques octets dans un
# tampon de pile ne change presque jamais le résultat d'une assertion — il
# écrase une variable voisine que le test ne regarde pas. Sur du code qui
# analyse des trames venues de l'hôte, c'est exactement la classe de défaut
# qu'on ne veut pas livrer.
#
# UBSan est fatal (halt_on_error) : sans ça il imprime et continue, et un
# décalage de bits indéfini passerait pour un avertissement dans un log que
# personne ne relit.
#
# En PREMIER dans ce fichier : il tourne en secondes là où les trois builds
# ESP-IDF prennent des minutes, et un débordement de pile rend le reste sans
# objet.
echo "=== harnais hôte sous ASan/UBSan ==="
rm -rf test/build-asan
SAN_FLAGS="-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -g"
cmake -S test -B test/build-asan \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_C_FLAGS="$SAN_FLAGS" \
      -DCMAKE_EXE_LINKER_FLAGS="$SAN_FLAGS" >/dev/null
cmake --build test/build-asan >/dev/null
ASAN_OPTIONS=detect_leaks=0:abort_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    ./test/build-asan/test_runner

if ! command -v idf.py >/dev/null 2>&1; then
    # shellcheck disable=SC1091
    . "${IDF_EXPORT:-$HOME/esp/esp-idf/export.sh}" >/dev/null 2>&1
fi

for board in jc_devkit niphar_chest wt9932_key; do
    echo "=== build complet : $board ==="
    rm -rf "build_$board"
    idf.py -B "build_$board" -DBOARD="$board" -DSDKCONFIG="build_$board/sdkconfig" build
done
