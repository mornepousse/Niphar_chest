#!/usr/bin/env bash
#
# Phase rapide du coffre Niphar : garde-fous matériels, puis build.
#
# Appelé par scripts/check.sh — ne pas l'invoquer depuis un hook directement.
#
# Trois étages, du plus rapide au plus lent : les garde-fous grep, les tests
# hôte (test/), puis le build ESP-IDF. Le build reste un oracle à part entière —
# les _Static_assert de main/board_common.h et des en-têtes de carte ne se
# vérifient qu'à la compilation.
#
set -euo pipefail
cd "$(dirname "$0")/.."

fail=0

# --- Garde-fou 1 : USB-Serial-JTAG (GPIO24/25) ---------------------------
# Le coffre n'a ni bouton reset ni accès matériel au mode download : c'est le
# seul chemin de flash et de debug. Le réaffecter rend une mauvaise version
# irrécupérable sans fer à souder. Le kit de dev, lui, pardonne — donc ce
# garde-fou ne se vérifiera jamais à l'exécution : il tient ici.
# GPIO35 s'y ajoute : c'est le seul strap qui décide entre boot applicatif et
# mode download (TRM table 11.2-2), et le coffre n'a pas de bouton de secours.
# Les en-têtes de carte ont le droit de les nommer : ce sont eux qui les
# déclarent réservés.
if grep -rnE 'GPIO_NUM_(24|25|35)\b' main/ boards/ --include='*.c' --include='*.h' \
        | grep -vE '^(main/board_common\.h|boards/[^/]+/board\.h):'; then
    echo "ERREUR : GPIO24/25 (USB-Serial-JTAG) ou GPIO35 (strap de boot) utilisés"
    echo "         hors des en-têtes de carte."
    echo "         Voir docs/HARDWARE.md — le coffre deviendrait irrécupérable."
    fail=1
fi

# --- Garde-fou 2 : deep-sleep permanent ----------------------------------
# Un deep-sleep dont on ne sort pas coupe l'USB-Serial-JTAG, donc coupe le
# seul moyen de reflasher.
if grep -rn 'esp_deep_sleep_start' main/ --include='*.c' --include='*.h'; then
    echo "ERREUR : deep-sleep permanent. Le coffre n'en sortirait pas, et"
    echo "         l'USB-Serial-JTAG est son unique voie de reflash."
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    exit 1
fi

# --- Tests hôte -----------------------------------------------------------
# Avant le build : ils sont plus rapides, et un échec ici rend le build inutile.
# Seule la logique pure y passe — le reste n'est pas testable sans matériel,
# et c'est précisément ce qui justifie de l'en séparer.
cmake -S test -B test/build >/dev/null || exit 1
cmake --build test/build >/dev/null || exit 1
./test/build/test_runner || exit 1

# --- Build ----------------------------------------------------------------
if ! command -v idf.py >/dev/null 2>&1; then
    # shellcheck disable=SC1091
    . "${IDF_EXPORT:-$HOME/esp/esp-idf/export.sh}" >/dev/null 2>&1
fi

exec idf.py build
