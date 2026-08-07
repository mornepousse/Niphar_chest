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

# --- Garde-fou 3 : le kit de dev reste un kit de dev ----------------------
# Secure Boot et Flash Encryption brûlent des eFuses au premier boot, et c'est
# IRRÉVERSIBLE : un kit ainsi verrouillé ne redevient jamais un outil de
# développement. Ces options n'ont leur place que dans une config propre au
# coffre, décidée exprès — jamais dans les defaults partagés.
# Boucle plutôt qu'un glob passé à grep : un motif sans correspondance ferait
# sortir grep en statut 2 (erreur), que `if` traite comme « rien trouvé » —
# le garde verrait la violation et se tairait.
for f in sdkconfig.defaults sdkconfig.defaults.*; do
    [ -f "$f" ] || continue
    if grep -nE '^CONFIG_(SECURE_BOOT|SECURE_FLASH_ENC_ENABLED|SECURE_BOOT_V2_ENABLED)=y' "$f"; then
        echo "ERREUR : Secure Boot ou Flash Encryption dans $f."
        echo "         Ces options brûlent des eFuses de façon irréversible et"
        echo "         transformeraient le kit de dev en carte verrouillée."
        fail=1
    fi
done

# --- Garde-fou 4 : la béquille de confirmation ne part pas en production ----
# sec_gate_console_confirm n'existe que sur une carte sans lien. Si ce symbole
# apparaît hors d'un bloc conditionné à BOARD_LINK_AVAILABLE, la béquille
# pourrait se retrouver dans le firmware du coffre — indistinguable, à l'usage,
# d'un dispositif qui fonctionne.
if grep -rn 'sec_gate_console_confirm' main/ --include='*.c' --include='*.h' \
        | grep -vE '^main/security/sec_gate\.(c|h):' \
        | grep -vE '^main/console/console\.c:'; then
    echo "ERREUR : la béquille de confirmation est référencée hors des deux"
    echo "         fichiers qui la conditionnent à BOARD_LINK_AVAILABLE."
    fail=1
fi

# Un seul point de sortie pour TOUS les garde-fous : en ajouter un après ce
# test le rendrait bavard mais inoffensif — c'est exactement l'erreur commise
# ici le 2026-08-07, et elle ne s'est vue qu'en vérifiant le code de sortie.
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
