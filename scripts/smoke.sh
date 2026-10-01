#!/usr/bin/env bash
# Quels items du smoke test sont OBLIGATOIRES pour la release en cours.
#
# POURQUOI CE SCRIPT EXISTE. Le smoke test etait une ceremonie : cinq gestes a
# refaire avant chaque tag, qui demandaient la proprietaire disponible. Le vrai
# probleme n'etait pas son temps, c'etait de redemander une preuve que rien
# n'avait invalidee.
#
# Chaque item couvre des FICHIERS. Si une release n'en touche aucun, le
# re-derouler ne prouve rien de neuf — la preuve precedente tient toujours. Si
# elle en touche un, l'item redevient obligatoire, et aucune confiance passee ne
# le remplace.
#
# Usage : ./scripts/smoke.sh [<ref-depuis>]
#   sans argument : depuis le dernier tag.
set -uo pipefail
cd "$(dirname "$0")/.."

DEPUIS="${1:-}"
if [ -z "$DEPUIS" ]; then
    DEPUIS="$(git describe --tags --abbrev=0 2>/dev/null || true)"
fi
if [ -z "$DEPUIS" ]; then
    echo "Aucun tag : premiere release, TOUS les items sont obligatoires."
    DIFF=""
else
    DIFF="$(git diff --name-only "$DEPUIS"..HEAD -- main/ boards/ 2>/dev/null)"
    echo "Depuis $DEPUIS : $(printf '%s\n' "$DIFF" | grep -c . ) fichier(s) de firmware modifie(s)."
fi
echo

# item | ce qu'il prouve | motifs de fichiers qui le rendent obligatoire
ITEMS=(
"1|Le coffre demarre : lien installe, microSD detectee, aucun E( ni W(|main/main.c|main/link/|main/storage/|boards/|main/board_common.h"
"2|Le clavier choisit un mode, le coffre l'expose (lsusb + bit monte)|main/link/|main/usb/usb_mode|main/usb/usb_device"
"3|Console link : le bloc decode, le maitre lit (compteur qui grimpe)|main/link/|main/console/"
"4|Un code TOTP concorde avec oathtool (le SEUL item qui atteint cr_hmac)|main/security/cr_hmac|main/security/oath_proto|main/link/link_proto|main/security/sec_time"
"5|Une annulation efface l'invite sans attendre les quinze secondes|main/security/sec_confirm|main/link/link_proto|main/link/link_spi"
)

OBLIGATOIRES=0
for ligne in "${ITEMS[@]}"; do
    IFS='|' read -r num quoi reste <<< "$ligne"
    motifs="$reste"
    requis=0
    if [ -z "$DEPUIS" ]; then
        requis=1
    else
        IFS='|' read -ra tab <<< "$motifs"
        for m in "${tab[@]}"; do
            [ -z "$m" ] && continue
            if printf '%s\n' "$DIFF" | grep -q -- "$m"; then requis=1; break; fi
        done
    fi
    if [ "$requis" = 1 ]; then
        OBLIGATOIRES=$((OBLIGATOIRES + 1))
        printf '  [A DEROULER] %s. %s\n' "$num" "$quoi"
    else
        printf '  [inchange]   %s. %s\n' "$num" "$quoi"
    fi
done

echo
if [ "$OBLIGATOIRES" -eq 0 ]; then
    echo "Aucun item obligatoire : aucun fichier couvert n'a change depuis $DEPUIS."
    echo "Les preuves precedentes tiennent — elles portent sur le meme code."
else
    echo "$OBLIGATOIRES item(s) a derouler. Les autres sont couverts par la preuve"
    echo "precedente, puisque le code qu'ils exercent n'a pas bouge."
fi
