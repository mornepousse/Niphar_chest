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
# sec_gate_console_confirm n'existe que sur une carte dont la console a le
# pouvoir d'agir. Si ce symbole apparaît hors d'un bloc conditionné à
# BOARD_CONSOLE_ACTIONS, la béquille pourrait se retrouver dans le firmware du
# coffre — indistinguable, à l'usage, d'un dispositif qui fonctionne.
if grep -rn 'sec_gate_console_confirm' main/ --include='*.c' --include='*.h' \
        | grep -vE '^main/security/sec_gate\.(c|h):' \
        | grep -vE '^main/console/console\.c:'; then
    echo "ERREUR : la béquille de confirmation est référencée hors des deux"
    echo "         fichiers qui la conditionnent à BOARD_CONSOLE_ACTIONS."
    fail=1
fi

# Même béquille, même garde : sur une carte dont la console n'a pas le
# pouvoir, le sélecteur de mode USB vient d'ailleurs (le S3, par le lien).
# usb_mode_set peut légitimement être appelé par usb_mode.c lui-même (son
# prototype) et par console.c (la béquille, conditionnée à
# BOARD_CONSOLE_ACTIONS). main.c n'appelle que usb_mode_init(), qui n'est pas
# concerné par ce garde-fou.
if grep -rn 'usb_mode_set' main/ --include='*.c' --include='*.h' \
        | grep -vE '^main/usb/usb_mode\.(c|h):' \
        | grep -vE '^main/console/console\.c:'; then
    echo "ERREUR : usb_mode_set est référencé hors de usb_mode.{c,h} et de"
    echo "         console.c (la béquille, conditionnée à BOARD_CONSOLE_ACTIONS)."
    fail=1
fi

# --- Garde-fou 4 (suite) : ce que les deux greps ci-dessus ne voient PAS ----
# Ils excluent console.c EN BLOC. Ils resteraient donc verts si le
# « #if BOARD_CONSOLE_ACTIONS » qui entoure les deux béquilles disparaissait —
# c'est-à-dire dans le cas exact qu'ils sont censés interdire. Pour
# sec_gate_console_confirm, sec_gate.h:~29 rattrape par une erreur de
# compilation (le prototype n'existe pas quand la console n'a pas le pouvoir) ;
# pour usb_mode_set, RIEN ne rattrape : le sélecteur de mode partirait sur le
# coffre avec un check vert. Relevé par la revue finale de branche.
#
# Deux étages : la forme du source, puis — quand un build existe — le binaire,
# seul contrôle qui morde vraiment.

# 4a. Chaque mention des deux symboles dans console.c doit tomber DANS un bloc
# « #if BOARD_CONSOLE_ACTIONS ». On suit la profondeur des directives plutôt
# que d'en compter les occurrences : un « #if 1 » mis à la place, ou un usage
# déplacé hors du bloc, sont exactement les régressions à attraper.
if ! awk '
/^[[:space:]]*#[[:space:]]*if/ {
    depth++
    guard[depth] = ($0 ~ /^[[:space:]]*#[[:space:]]*if[[:space:]]+BOARD_CONSOLE_ACTIONS[[:space:]]*$/) ? 1 : 0
    next
}
/^[[:space:]]*#[[:space:]]*(else|elif)/ { if (depth > 0) guard[depth] = 0; next }
/^[[:space:]]*#[[:space:]]*endif/       { if (depth > 0) { guard[depth] = 0; depth-- } next }
/sec_gate_console_confirm|usb_mode_set/ {
    inside = 0
    for (i = 1; i <= depth; i++) if (guard[i]) inside = 1
    if (!inside) { printf "  %s:%d: %s\n", FILENAME, FNR, $0; bad = 1 }
}
END { exit bad ? 1 : 0 }
' main/console/console.c; then
    echo "ERREUR : dans main/console/console.c, les lignes ci-dessus mentionnent"
    echo "         une béquille HORS d'un bloc « #if BOARD_CONSOLE_ACTIONS »."
    echo "         Les deux greps ci-dessus excluent ce fichier en bloc et ne"
    echo "         verraient pas la différence — d'où ce contrôle."
    fail=1
fi

# 4b. Le binaire. Une référence non résolue dans console.c.obj est la preuve
# que le code a VRAIMENT été compilé — pas une conjecture sur le source. On ne
# regarde que les dossiers de build déjà présents : la phase rapide ne doit pas
# devenir dépendante d'un build préalable (build_niphar_chest n'est produit que
# par la phase complète). Le témoin positif plus bas empêche ce contrôle de
# devenir silencieusement creux.
witness_seen=0
for d in build build_jc_devkit build_niphar_chest build_wt9932_key; do
    obj="$d/esp-idf/main/CMakeFiles/__idf_main.dir/console/console.c.obj"
    [ -f "$obj" ] || continue
    board="$(sed -n 's/^BOARD:[^=]*=//p' "$d/CMakeCache.txt" 2>/dev/null | head -1)"
    [ -n "$board" ] && [ -f "boards/$board/board.h" ] || continue
    console_actions="$(sed -n 's/^[[:space:]]*#define[[:space:]]\{1,\}BOARD_CONSOLE_ACTIONS[[:space:]]\{1,\}//p' \
            "boards/$board/board.h" | head -1)"
    # `|| true` : aucune correspondance est le cas NORMAL sur le coffre, et
    # `set -e` ferait sortir le script sur le rc=1 de grep — un garde-fou qui
    # s'arrête avant les suivants au lieu de les laisser parler.
    undef="$(nm -u "$obj" 2>/dev/null | grep -oE '(sec_gate_console_confirm|usb_mode_set)$' | sort -u || true)"
    if [ "$console_actions" = "0" ]; then
        if [ -n "$undef" ]; then
            echo "ERREUR : $d ($board, console sans pouvoir) — console.c.obj référence :"
            echo "$undef" | sed 's/^/           /'
            echo "         Les béquilles de développement sont compilées dans le"
            echo "         firmware du coffre. Voir docs/HARDWARE.md et sec_gate.h."
            fail=1
        fi
    else
        # Témoin positif : sur une carte où la console a le pouvoir, les deux
        # symboles DOIVENT apparaître. S'ils manquent, le contrôle ci-dessus ne
        # prouve plus rien (nm muet, LTO, fichier déplacé) et son vert serait
        # creux.
        for sym in sec_gate_console_confirm usb_mode_set; do
            if ! printf '%s\n' "$undef" | grep -qx "$sym"; then
                echo "ERREUR : $d ($board, console avec pouvoir) — nm ne voit pas $sym dans"
                echo "         console.c.obj. Le contrôle binaire du coffre ne prouve"
                echo "         donc plus rien. Si la béquille a été retirée exprès,"
                echo "         retirer aussi ce témoin."
                fail=1
            fi
        done
        witness_seen=1
    fi
done
if [ "$witness_seen" -eq 0 ]; then
    echo "note : aucun build de carte où la console a le pouvoir n'est présent —"
    echo "       contrôle binaire du garde-fou 4 non exécuté (il le sera à la"
    echo "       phase complète)."
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
