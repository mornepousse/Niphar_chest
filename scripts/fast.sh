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

# Le sélecteur venu du lien, confiné par le MÊME raisonnement — et déclaré ici
# plutôt que d'élargir le grep ci-dessus.
#
# Le coffre a besoin que quelque chose puisse changer son mode USB : sa console
# n'a aucun pouvoir (BOARD_CONSOLE_ACTIONS 0), donc sans le lien il démarre en
# USB_MODE_NONE et rien ne l'en sort. Ajouter link_spi.c à la liste des fichiers
# autorisés à nommer usb_mode_set aurait rendu le garde plus permissif POUR TOUS
# afin de servir un seul appelant. usb_mode_apply_wire() est le point d'entrée
# nommé qui le sert sans rien relâcher — même motif que usb_mode_cycle_next()
# pour hmi.c — et il mérite le même confinement, sans quoi il deviendrait
# lui-même le contournement du garde-fou qu'il évite d'affaiblir.
if grep -rn 'usb_mode_apply_wire' main/ --include='*.c' --include='*.h' \
        | grep -vE '^main/usb/usb_mode\.(c|h):' \
        | grep -vE '^main/link/link_spi\.c:'; then
    echo "ERREUR : usb_mode_apply_wire est référencé hors de usb_mode.{c,h} et"
    echo "         de link_spi.c. Ce point d'entrée existe pour que le LIEN"
    echo "         puisse changer de mode sans que usb_mode_set sorte de ses"
    echo "         deux fichiers ; l'ouvrir à un troisième appelant reviendrait"
    echo "         à élargir le garde-fou par la porte de derrière."
    fail=1
fi

# --- Garde-fou 7 : le coffre ne doit pas redevenir inerte -------------------
# La v1 du protocole du lien n'avait AUCUN champ pour demander un mode USB, et
# la console du coffre n'a aucun pouvoir (BOARD_CONSOLE_ACTIONS 0) : il démarrait
# donc en USB_MODE_NONE et RIEN ne pouvait l'en sortir. microSD qui répond,
# applets présents, lien qui fonctionne — et l'hôte qui ne voit jamais rien.
# Ça n'a produit aucune erreur, aucun log, aucun rouge : c'est exactement la
# classe de panne qu'un check doit attraper, parce que l'usage ne la distingue
# pas d'un appareil qui marche mais qu'on tient mal.
#
# Le contrôle est POSITIF et BINAIRE : sur une carte qui a le lien, link_spi.c
# DOIT référencer les trois symboles qui font de lui un sélecteur et un relais.
# Un grep de source dirait la même chose en moins : le corps entier de ce
# fichier vit derrière « #if BOARD_LINK_AVAILABLE », donc le texte peut être là
# sans être compilé — et c'est précisément ce que le coffre ne pardonne pas.
#
#   usb_mode_apply_wire     -> le lien peut CHANGER de mode (sinon : inerte)
#   sec_confirm_peek_armed  -> l'instance vient du compteur d'armements, pas
#                              d'une devinette sur le code de l'opération
#   sec_confirm_authorize   -> l'appui est bien relayé
link_obj_vu=0
for d in build build_jc_devkit build_niphar_chest build_wt9932_key; do
    obj="$d/esp-idf/main/CMakeFiles/__idf_main.dir/link/link_spi.c.obj"
    [ -f "$obj" ] || continue
    board="$(sed -n 's/^BOARD:[^=]*=//p' "$d/CMakeCache.txt" 2>/dev/null | head -1)"
    [ -n "$board" ] && [ -f "boards/$board/board.h" ] || continue
    link_avail="$(sed -n 's/^[[:space:]]*#define[[:space:]]\{1,\}BOARD_LINK_AVAILABLE[[:space:]]\{1,\}//p' \
            "boards/$board/board.h" | head -1 | awk '{print $1}')"
    # Une carte sans lien compile des souches : rien à exiger d'elle.
    [ "$link_avail" = "1" ] || continue
    # `|| true` : grep sort en 1 sans correspondance, et `set -e` couperait le
    # script avant les garde-fous suivants.
    syms="$(nm -u "$obj" 2>/dev/null \
            | grep -oE '(usb_mode_apply_wire|sec_confirm_peek_armed|sec_confirm_authorize)$' \
            | sort -u || true)"
    for sym in usb_mode_apply_wire sec_confirm_peek_armed sec_confirm_authorize; do
        if ! printf '%s\n' "$syms" | grep -qx "$sym"; then
            echo "ERREUR : $d ($board, carte à lien) — link_spi.c.obj ne référence pas $sym."
            echo "         Le coffre n'a pas d'autre chemin : sa console n'a aucun"
            echo "         pouvoir, donc sans le lien il démarre en USB_MODE_NONE et"
            echo "         rien ne l'en sort. Voir docs/LINK_CONTRACT.md et"
            echo "         LINK_PROTO_VERSION 2 dans main/link/link_proto.h."
            fail=1
        fi
    done
    link_obj_vu=1
done
if [ "$link_obj_vu" -eq 0 ]; then
    echo "note : aucun build de carte à lien présent — contrôle binaire du"
    echo "       garde-fou 7 non exécuté (il le sera à la phase complète)."
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

# --- Garde-fou 5 : le cloisonnement du magasin, sens OTP -> OATH -----------
# main/security/otp_hid.c n'entre PAS dans le harnais hôte (test/) : il appelle
# esp_timer et esp_log, donc il ne compile pas sur la machine. Choix assumé —
# mais il laisse la ligne la plus sensible de ce fichier sans aucun oracle.
#
# Cette ligne est le refus de `hook_compute_hmac()` quand le slot visé n'est
# PAS un slot CR-HMAC. Ce qu'elle empêche : le mapping OTP est figé à
# 0x30 -> slot 0 et 0x38 -> slot 1, tandis qu'`oath_do_put()` attribue le
# premier slot vide en partant de 0. Sur une clé neuve, les deux premiers
# comptes TOTP de Mae atterrissent donc exactement là. Sans ce refus, un hôte
# ferait signer par la clé un défi de 64 octets QU'IL CHOISIT avec un secret
# TOTP — c'est-à-dire un oracle HMAC sur la graine d'un compte —, sous un écran
# annonçant « CLE OTP ». Le sens inverse, lui, est gardé par
# oath_slot_is_oath() ; ici, rien d'autre que cette ligne.
#
# Deux étages, comme le garde-fou 4 : la forme du source, puis le binaire.

# 5a. Dans hook_compute_hmac(), le refus doit précéder la lecture du secret.
# Vérifier la seule PRÉSENCE du symbole laisserait passer un appel déplacé
# APRÈS sec_store_get_secret() — le secret serait déjà sorti du magasin.
if ! awk '
/^static bool hook_compute_hmac\(/ { dans = 1; next }
dans && /^}/                       { dans = 0 }
dans && /if \(!sec_store_is_hmac_slot\(/ { garde = FNR }
dans && /sec_store_get_secret\(/         { if (!lecture) lecture = FNR }
END {
    if (!garde)   { print "  le refus « if (!sec_store_is_hmac_slot(...) »  est absent"; exit 1 }
    if (!lecture) { print "  sec_store_get_secret() ne figure plus dans la fonction"; exit 1 }
    if (garde > lecture) {
        printf "  le refus est ligne %d, la lecture du secret ligne %d : trop tard\n", garde, lecture
        exit 1
    }
}
' main/security/otp_hid.c; then
    echo "ERREUR : main/security/otp_hid.c — hook_compute_hmac() ne refuse plus"
    echo "         un slot qui n'est pas CR-HMAC avant d'en lire le secret."
    echo "         Le mode OTP calculerait alors un HMAC sur un secret TOTP :"
    echo "         un hôte choisit le défi, donc obtient un oracle sur la graine"
    echo "         d'un compte de Mae, sous un écran annonçant « CLE OTP »."
    echo "         Ce fichier n'est dans aucun test hôte — d'où ce garde-fou."
    fail=1
fi

# 5b. Le binaire. Une référence non résolue dans otp_hid.c.obj prouve que
# l'appel a VRAIMENT été compilé — un « #if 0 » ou une macro vidée passeraient
# le contrôle de forme ci-dessus. Le témoin positif est sec_store_get_secret :
# s'il manque aussi, c'est nm qui est muet (LTO, chemin déplacé), et l'absence
# du premier symbole ne prouverait alors plus rien.
otp_obj_vu=0
for d in build build_jc_devkit build_niphar_chest build_wt9932_key; do
    obj="$d/esp-idf/main/CMakeFiles/__idf_main.dir/security/otp_hid.c.obj"
    [ -f "$obj" ] || continue
    # `|| true` : grep sort en 1 quand il ne trouve rien, et `set -e` couperait
    # le script avant les garde-fous suivants.
    syms="$(nm -u "$obj" 2>/dev/null | grep -oE '(sec_store_is_hmac_slot|sec_store_get_secret)$' | sort -u || true)"
    printf '%s\n' "$syms" | grep -qx sec_store_get_secret || {
        echo "ERREUR : $d — nm ne voit pas sec_store_get_secret dans otp_hid.c.obj."
        echo "         Le contrôle binaire du garde-fou 5 ne prouve donc rien."
        echo "         Si hook_compute_hmac() a changé exprès, corriger ce témoin."
        fail=1
        continue
    }
    if ! printf '%s\n' "$syms" | grep -qx sec_store_is_hmac_slot; then
        echo "ERREUR : $d — otp_hid.c.obj lit un secret sans appeler"
        echo "         sec_store_is_hmac_slot : le cloisonnement OTP -> OATH est"
        echo "         compilé hors du firmware. Voir sec_store.h et le"
        echo "         commentaire de hook_compute_hmac()."
        fail=1
    fi
    otp_obj_vu=1
done
if [ "$otp_obj_vu" -eq 0 ]; then
    echo "note : aucun otp_hid.c.obj présent — contrôle binaire du garde-fou 5"
    echo "       non exécuté (il le sera à la phase complète)."
fi

# --- Garde-fou 6 : l'appui armé ne vaut que pour SON slot -------------------
# Même fichier, même angle mort (otp_hid.c n'entre pas dans test/, cf. garde-fou
# 5) : `hook_confirm_state()` porte une deuxième défense en profondeur, sans
# oracle non plus.
#
# `s_armed_idx` retient QUEL slot sec_store un appui a armé ; sur
# SEC_CONFIRM_AUTHORIZED, le hook refuse un octroi accordé pour un AUTRE slot
# que celui-là (miroir du `slot == CCID_CONFIRM_SLOT` de ccid.c). Sans ce
# refus : un appui physique armé pour le slot 0 confirmerait aussi un HMAC
# calculé entre-temps sur le slot 1 — la garde physique cesserait de garantir
# QUEL secret l'appui autorise, seulement QU'un appui a eu lieu.
#
# Deux étages, comme le garde-fou 5, et pour la même raison précise : un
# `#if 0` autour de la comparaison, avec un `return 1;` nu juste après,
# laisse le texte de la comparaison en place — l'étage source (bête, il ne
# comprend pas le préprocesseur) le verrait et se tairait à tort. Vérifié à la
# main le 2026-09-29 : cette mutation précise compile, passe l'étage source,
# et c'est l'étage binaire qui la voit (`hook_confirm_state` passe de 0x66 à
# 0x46 octets, et `s_armed_idx` de section `d` — sa valeur initiale -1 compte
# encore — à `b`, gaufre pré-zérotée : dès que plus rien ne lit la variable,
# le compilateur cesse de préserver son -1 initial).

# 6a. Le retour de SEC_CONFIRM_AUTHORIZED doit encore comparer out_slot à
# s_armed_idx — pas juste contenir les deux noms (un `||` au lieu du `&&`
# affaiblirait la garde en gardant tous les mots).
if ! awk '
/^static int hook_confirm_state\(void\)/ { dans = 1; next }
dans && /^}/                              { dans = 0 }
dans && /case SEC_CONFIRM_AUTHORIZED:/    { cas = FNR }
dans && cas && !ligne && /return/         { ligne = $0 }
END {
    if (!cas)   { print "  le cas SEC_CONFIRM_AUTHORIZED a disparu de hook_confirm_state()"; exit 1 }
    if (!ligne) { print "  aucun « return » ne suit SEC_CONFIRM_AUTHORIZED"; exit 1 }
    if (ligne !~ /s_armed_idx/ || ligne !~ /out_slot/ || ligne !~ /==/ \
        || ligne !~ /&&/ || ligne !~ />= *0/) {
        print "  le retour de SEC_CONFIRM_AUTHORIZED ne compare plus out_slot a s_armed_idx"
        exit 1
    }
}
' main/security/otp_hid.c; then
    echo "ERREUR : main/security/otp_hid.c — hook_confirm_state() n'exige plus"
    echo "         que l'octroi porte sur le slot armé par CETTE transaction."
    echo "         Un appui destiné à un slot autoriserait alors le HMAC d'un"
    echo "         AUTRE slot calculé entre-temps. Voir s_armed_idx et le"
    echo "         commentaire au-dessus de sa déclaration."
    fail=1
fi

# 6b. Le binaire. Le symbole s_armed_idx doit rester en section « d » (donnée
# initialisée) : son -1 de départ n'a de sens que si quelque chose le LIT.
# Un `#if 0` qui retire la lecture laisse l'écriture seule dans le fichier ;
# sans lecture nulle part, le compilateur cesse de distinguer -1 de 0 et
# range la variable en « b » (bss, pré-zérotée) — signal vérifié à la main
# (voir le commentaire au-dessus du garde-fou).
otp2_obj_vu=0
for d in build build_jc_devkit build_niphar_chest build_wt9932_key; do
    obj="$d/esp-idf/main/CMakeFiles/__idf_main.dir/security/otp_hid.c.obj"
    [ -f "$obj" ] || continue
    ligne="$(nm "$obj" 2>/dev/null | grep -E ' [bBdD] s_armed_idx$' || true)"
    if [ -z "$ligne" ]; then
        echo "ERREUR : $d — nm ne voit pas s_armed_idx (type b/d) dans"
        echo "         otp_hid.c.obj. Le contrôle binaire du garde-fou 6 ne"
        echo "         prouve donc rien — si le fichier a changé exprès,"
        echo "         corriger ce témoin."
        fail=1
        otp2_obj_vu=1
        continue
    fi
    type="$(printf '%s\n' "$ligne" | awk '{print $2}')"
    if [ "$type" != "d" ] && [ "$type" != "D" ]; then
        echo "ERREUR : $d — s_armed_idx est en section '$type', pas 'd' :"
        echo "         plus rien dans otp_hid.c.obj ne lit ce slot armé, donc"
        echo "         plus rien ne le compare — la garde du garde-fou 6a a"
        echo "         été compilée hors du firmware (#if 0 ou équivalent)."
        fail=1
    fi
    otp2_obj_vu=1
done
if [ "$otp2_obj_vu" -eq 0 ]; then
    echo "note : aucun otp_hid.c.obj présent — contrôle binaire du garde-fou 6"
    echo "       non exécuté (il le sera à la phase complète)."
fi

# --- Garde-fou 10 : le nombre de comptes ne redevient pas 1 ------------------
# Un RESET efface jusqu'a seize secrets sur UN appui, et l'ecran du CLAVIER ne
# le sait que par l'octet 0x0F du lien : il ne recoit qu'un code d'operation.
# Le contrat publie a KeSp annonce le nombre reel (vecteur V16, op_count = 12).
#
# CE GARDE-FOU EXISTE PARCE QUE LE DEFAUT A DEJA EU LIEU. La plomberie
# (sec_confirm_arm_counted) a ete ecrite, et RIEN ne l'appelait : le chemin
# RESET passait par ccid_confirm_named(), qui arme avec 1. Le contrat decrivait
# donc un comportement que le firmware ne produisait pas — et les vecteurs ne
# pouvaient pas l'attraper, puisqu'ils verifient que pack_status EMBALLE
# fidelement ce qu'on lui donne, jamais que le coffre PRODUIT la bonne valeur.
# Aucun test hote ne le peut non plus : le chemin est de l'ESP-IDF.
#
# Deux assertions : la variante comptee est employee, et la variante qui
# defaute a 1 n'apparait PAS dans mode_oath.c. Commentaires depouilles — le
# fichier cite ccid_confirm_named() dans sa prose.
if ! command -v python3 >/dev/null 2>&1; then
    echo "» garde-fou 10 SAUTE (python3 absent) — le nombre de comptes non verifie"
else
    python3 - <<'PYGUARD' || fail=1
import re, sys

src = open("main/usb/mode_oath.c", encoding="utf-8").read()

# Commentaires et chaines remplaces par du blanc (memes lignes conservees).
out, i, n = [], 0, len(src)
while i < n:
    c = src[i]
    if c == '/' and i + 1 < n and src[i+1] == '*':
        j = src.find('*/', i + 2); j = n if j == -1 else j + 2
        out.append(''.join(ch if ch == '\n' else ' ' for ch in src[i:j])); i = j
    elif c == '/' and i + 1 < n and src[i+1] == '/':
        j = src.find('\n', i); j = n if j == -1 else j
        out.append(' ' * (j - i)); i = j
    elif c == '"':
        j = i + 1
        while j < n and src[j] != '"':
            j += 2 if src[j] == '\\' else 1
        j = min(j + 1, n)
        out.append(''.join(ch if ch == '\n' else ' ' for ch in src[i:j])); i = j
    else:
        out.append(c); i += 1
code = ''.join(out)

bad = 0

if re.search(r"\bccid_confirm_named\s*\(", code):
    print("\033[0;31m✗ OATH : mode_oath.c appelle ccid_confirm_named(), qui arme avec 1\033[0m",
          file=sys.stderr)
    print("         Un RESET annoncerait « 1 compte » au clavier alors qu'il en", file=sys.stderr)
    print("         efface douze. Utiliser ccid_confirm_named_counted().", file=sys.stderr)
    bad += 1

m = re.search(r"ccid_confirm_named_counted\s*\(([^;]*?)\)\s*==", code, re.S)
if not m:
    print("\033[0;31m✗ OATH : aucun appel a ccid_confirm_named_counted() dans mode_oath.c\033[0m",
          file=sys.stderr)
    print("         Le nombre de comptes vises ne traverse plus jusqu'au lien.", file=sys.stderr)
    bad += 1
elif "s_ctx.touch_count" not in m.group(1):
    print("\033[0;31m✗ OATH : le nombre passe a la confirmation n'est pas s_ctx.touch_count\033[0m",
          file=sys.stderr)
    print("         C'est le seul chiffre que oath_dispatch() vient de compter, et", file=sys.stderr)
    print("         celui que oath_reset_label() a mis en toutes lettres dans", file=sys.stderr)
    print("         l'etiquette. Deux formes, une seule source — ou elles divergent.", file=sys.stderr)
    bad += 1

sys.exit(1 if bad else 0)
PYGUARD
fi

# --- Garde-fou 9 : l'appui qui detruit des secrets ne devient pas decoratif --
# main/usb/mode_oath.c enonce DEUX regles et dit lui-meme que rien ne les
# protege (« Rendre cette attente non bloquante casserait la garantie sans
# qu'aucun test ne le dise »). oath_touch_commit() CROIT son booleen : elle
# efface jusqu'a seize secrets sans rien reverifier. Un test ne peut pas
# atteindre ca — l'appui est du materiel — donc c'est un grep ou rien.
#
#   A. `granted` ne vient que du retour de ccid_confirm_named(). Un `true`
#      litteral vide le magasin sans que personne n'ait rien demande.
#   B. dongle_confirm_named() reste une ATTENTE BLOQUANTE : c'est elle qui
#      empeche l'hote d'intercaler une commande entre la demande et l'appui,
#      et c'est ce qui rend suffisant le simple index memorise dans
#      `touch_slot`. La transformer en machine a etats casserait la garantie
#      en silence.
#
# Les commentaires sont DEPOUILLES avant la recherche : mode_oath.c cite
# « oath_touch_commit(&s_ctx, true, …) » en exemple de ce qu'il ne faut pas
# faire, et un grep naif crierait sur la documentation de la regle.
if ! command -v python3 >/dev/null 2>&1; then
    echo "» garde-fou 9 SAUTE (python3 absent) — appui OATH non verifie"
else
    python3 - <<'PYGUARD' || fail=1
import re, sys

def strip_comments(src):
    # Remplace commentaires et chaines par du blanc, en preservant les retours
    # a la ligne pour que les numeros restent justes.
    out, i, n = [], 0, len(src)
    while i < n:
        c = src[i]
        if c == '/' and i + 1 < n and src[i+1] == '*':
            j = src.find('*/', i + 2)
            j = n if j == -1 else j + 2
            out.append(''.join(ch if ch == '\n' else ' ' for ch in src[i:j])); i = j
        elif c == '/' and i + 1 < n and src[i+1] == '/':
            j = src.find('\n', i)
            j = n if j == -1 else j
            out.append(' ' * (j - i)); i = j
        elif c == '"':
            j = i + 1
            while j < n and src[j] != '"':
                j += 2 if src[j] == '\\' else 1
            j = min(j + 1, n)
            out.append(''.join(ch if ch == '\n' else ' ' for ch in src[i:j])); i = j
        else:
            out.append(c); i += 1
    return ''.join(out)

bad = 0

# ---- A : aucun appelant ne force l'accord ----
for path in ("main/usb/mode_oath.c",):
    code = strip_comments(open(path, encoding="utf-8").read())
    for m in re.finditer(r"oath_(?:touch_commit|finish_calculate)\s*\([^;]*?\)", code, re.S):
        call = m.group(0)
        args = call[call.index("(") + 1:]
        if re.search(r"(?:\(|,)\s*(?:true|1)\s*(?:,|\))", args):
            line = code[:m.start()].count("\n") + 1
            print("\033[0;31m✗ OATH : %s:%d force l'accord (« true » litteral)\033[0m"
                  % (path, line), file=sys.stderr)
            print("         `granted` ne doit venir QUE du retour de ccid_confirm_named().", file=sys.stderr)
            print("         Un accord force efface des comptes sans appui physique.", file=sys.stderr)
            bad += 1

# ---- B : l'attente reste bloquante ----
code = strip_comments(open("main/security/ccid.c", encoding="utf-8").read())
m = re.search(r"static int dongle_confirm_named\s*\([^)]*\)\s*\{", code)
if not m:
    print("\033[0;31m✗ OATH : dongle_confirm_named() introuvable dans ccid.c\033[0m", file=sys.stderr)
    print("         Renommee ou supprimee ? Le garde-fou ne peut plus rien affirmer.", file=sys.stderr)
    bad += 1
else:
    i, depth = m.end() - 1, 0
    while i < len(code):
        if code[i] == '{': depth += 1
        elif code[i] == '}':
            depth -= 1
            if depth == 0: break
        i += 1
    body = code[m.end():i]
    if "for (;;)" not in body:
        print("\033[0;31m✗ OATH : dongle_confirm_named() n'est plus une boucle d'attente\033[0m", file=sys.stderr)
        bad += 1
    if "vTaskDelay" not in body:
        print("\033[0;31m✗ OATH : dongle_confirm_named() ne scrute plus (vTaskDelay absent)\033[0m", file=sys.stderr)
        bad += 1
    for r in re.findall(r"return\s+([^;]+);", body):
        if not re.fullmatch(r"[12]|\([^)]*\)\s*\?\s*1\s*:\s*2", r.strip()):
            print("\033[0;31m✗ OATH : dongle_confirm_named() rend « %s » — ni 1 ni 2\033[0m"
                  % r.strip(), file=sys.stderr)
            print("         Un troisieme retour est la signature d'une attente NON bloquante :", file=sys.stderr)
            print("         l'hote pourrait alors intercaler une commande entre la demande et", file=sys.stderr)
            print("         l'appui, et `touch_slot` ne designerait plus le compte affiche.", file=sys.stderr)
            bad += 1

sys.exit(1 if bad else 0)
PYGUARD
fi

# --- Garde-fou 8 : la prose du contrat ne vieillit pas sous le tableau ------
# Les tests epingles de test_link_proto.c comparent les vecteurs au CODE. Rien
# ne comparait les vecteurs a la PROSE qui les commente — et le 2026-09-29, la
# note de lecture du §11 a survecu a une regeneration en annoncant un CRC
# (« 62 3A ») que plus aucun bloc du tableau ne portait. Releve par KeSp a la
# relecture, pas par un rouge : c'est exactement ce qu'un contrat publie ne peut
# pas se permettre, puisque l'autre moitie implemente contre la prose autant que
# contre la table.
#
# La regle : toute suite de plusieurs octets citee dans les NOTES DE LECTURE doit
# apparaitre telle quelle dans au moins une ligne du tableau des vecteurs. Les
# octets isoles (« 0x11 », « 03 ») sont hors de portee — ils designent des
# offsets ou des valeurs, pas des extraits de bloc. La note de production, qui
# cite legitimement l'ANCIENNE valeur pour dire qu'elle a change, est hors de la
# zone balayee.
CONTRACT="docs/LINK_CONTRACT.md"
if [ -f "$CONTRACT" ]; then
    if ! command -v python3 >/dev/null 2>&1; then
        echo "» garde-fou 8 SAUTE (python3 absent) — coherence prose/vecteurs non verifiee"
    else
        python3 - "$CONTRACT" <<'PYGUARD' || fail=1
import re, sys

doc = open(sys.argv[1], encoding="utf-8").read()

rows = re.findall(r"^\|\s*V[0-9a-zA-Z]+\s*\|\s*`([0-9A-F][0-9A-F ]+)`", doc, re.M)
if not rows:
    print("\033[0;31m✗ garde-fou 8 : aucun vecteur trouve dans %s\033[0m" % sys.argv[1],
          file=sys.stderr)
    print("         le tableau du §11 a-t-il change de forme ? Le garde-fou ne peut", file=sys.stderr)
    print("         plus rien affirmer — l'ajuster plutot que le laisser muet.", file=sys.stderr)
    sys.exit(1)

start = doc.find("Reading notes, since")
if start == -1:
    print("\033[0;31m✗ garde-fou 8 : les notes de lecture du §11 sont introuvables\033[0m",
          file=sys.stderr)
    sys.exit(1)
end = doc.find("\n## ", start)
notes = doc[start:end if end != -1 else len(doc)]

bad = 0
for lit in set(re.findall(r"`([0-9A-F]{2}(?: [0-9A-F]{2})+)`", notes)):
    if not any(lit in r for r in rows):
        print("\033[0;31m✗ contrat : la note de lecture du §11 cite « %s », "
              "qu'aucun vecteur du tableau ne porte\033[0m" % lit, file=sys.stderr)
        bad += 1

if bad:
    print("         La prose a survecu a une regeneration du tableau. Le tableau fait", file=sys.stderr)
    print("         foi (il est engendre en executant link_proto.c) : c'est la note", file=sys.stderr)
    print("         qu'on met a jour, jamais l'inverse.", file=sys.stderr)
    sys.exit(1)
PYGUARD
    fi
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

# --- Tests des outils de build (Python) -----------------------------------
# tools/svg2bitmap.py décode le PNG à la main (chunks, inflate, les cinq filtres
# de reconstruction) pour ne dépendre que de la bibliothèque standard. C'est du
# parsing d'en-têtes, donc la norme TDD s'y applique — mais c'est du Python, donc
# hors du harnais C et hors du ratchet .tripwire-testcount.
#
# Un bug dans le prédicteur Paeth ne casse aucun build, ne lève aucune exception,
# et ne se verrait qu'à l'œil sur un logo déjà committé : sans ces tests, rien
# ne le rattraperait. Ils ne demandent ni Inkscape ni réseau, les PNG sont
# fabriqués en mémoire.
# Volontairement sans garde « si le fichier existe » : un contrôle qui se saute
# tout seul quand sa cible disparaît ne protège rien. Si svg2bitmap.py cesse
# d'être utilisé, on retire ce bloc explicitement.
if ! out=$(python3 tools/test_svg2bitmap.py 2>&1); then
    echo "tests de tools/svg2bitmap.py en échec :" >&2
    printf '%s\n' "$out" | tail -30 >&2
    exit 1
fi

# tools/niphar-oath est le client hôte de l'applet OATH. Trois de ses calculs
# ne sont rattrapés par AUCUN oracle en aval — ni la carte, ni le build :
#   1. le compteur de temps, huit octets gros-boutiens, que la clé ne calcule
#      pas (elle n'a pas d'horloge) ;
#   2. le modulo de la RFC 4226, que la carte NE FAIT PAS exprès
#      (oath_dynamic_binary, main/security/oath_proto.h) ;
#   3. l'absorption des trames WTX pendant l'attente de l'appui.
# Se tromper sur 1 ou 2 rend un code parfaitement formé et faux ; sur 3, un
# client qui conclut à un firmware cassé. Ces tests n'ont besoin ni de la carte
# ni de pyusb — l'import de la bibliothèque est différé dans le client pour
# cette raison précise.
if ! out=$(python3 tools/test_niphar_oath.py 2>&1); then
    echo "tests de tools/niphar-oath en échec :" >&2
    printf '%s\n' "$out" | tail -30 >&2
    exit 1
fi

# --- Build ----------------------------------------------------------------
if ! command -v idf.py >/dev/null 2>&1; then
    # shellcheck disable=SC1091
    . "${IDF_EXPORT:-$HOME/esp/esp-idf/export.sh}" >/dev/null 2>&1
fi

# La carte se lit dans .tripwire-variant, PAS dans le défaut de CMakeLists.txt.
#
# Ce qui était faux avant : un « idf.py build » nu, sans -DBOARD, construisait
# le dossier build/ avec le défaut du projet — jc_devkit, une carte SANS écran.
# Tout le corps de main/hmi/screen.c vit derrière « #if defined(BOARD_OLED_SCL) »
# (boards/wt9932_key/board.h le seul à le définir) : la phase rapide, donc le
# hook Stop, validait du code d'écran sans JAMAIS le compiler. Or c'est cet
# écran qui nomme le compte OATH visé — la décision 4 de la spec vit entièrement
# dans ce fichier, et cette branche est la première à y dessiner de la donnée
# fournie par l'hôte.
#
# Construire la carte réellement flashée (celle que /esp-flash prendrait) rend
# la phase rapide cohérente avec ce que Mae fait tourner. Les deux autres cartes
# restent couvertes par scripts/full.sh, qui les rebâtit toutes les trois.
VARIANT="$(tr -d '[:space:]' < .tripwire-variant 2>/dev/null || true)"
if [ -z "$VARIANT" ] || [ ! -f "boards/$VARIANT/board.h" ]; then
    echo "ERREUR : .tripwire-variant ne nomme pas une carte connue (« $VARIANT »)."
    echo "         La phase rapide refuse de retomber sur un défaut implicite :"
    echo "         c'est exactement ainsi qu'elle a cessé de compiler l'écran."
    exit 1
fi

exec idf.py -B "build_$VARIANT" -DBOARD="$VARIANT" \
            -DSDKCONFIG="build_$VARIANT/sdkconfig" build
