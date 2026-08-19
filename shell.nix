# Outils de TEST côté hôte pour le coffre Niphar.
#
# PÉRIMÈTRE, à lire avant de s'étonner : ce shell ne construit PAS le firmware.
# ESP-IDF vit dans ~/esp/esp-idf avec son propre environnement Python, et le
# mélanger à un environnement Nix produit des échecs obscurs de résolution de
# paquets. Le firmware se construit comme avant :
#
#     source ~/esp/esp-idf/export.sh
#     idf.py -B build_wt9932_key -DBOARD=wt9932_key ... build
#
# Ce shell fournit ce que la machine n'a pas et que la validation exige — au
# premier chef libfido2, dont les commandes sont le seul moyen d'éprouver
# CTAP-HID avant qu'un navigateur puisse s'en servir.
#
# Pourquoi libfido2 et pas un client de test maison : un client que nous
# écririons partagerait nos propres mélectures de la spécification. libfido2
# est une implémentation indépendante — c'est ce qui en fait un oracle plutôt
# qu'un miroir.
#
#     nix-shell
#     fido2-token -L                 # liste les authentificateurs
#     fido2-token -I /dev/hidrawN    # décrit la clé
#
# Il fournit aussi pyusb, pour tools/niphar-oath — le client hôte de l'applet
# OATH. Celui-là EST un client maison, contrairement au parti pris ci-dessus,
# et pour une raison mesurée : `ykman` exige pcscd, que la configuration NixOS
# de cette machine désactive délibérément parce qu'il saisirait l'interface
# CCID avant scdaemon et casserait `gpg --card-status`. Le firmware, lui, reste
# du YKOATH standard : n'importe quelle machine avec pcscd peut y brancher
# `ykman`, qui reste l'oracle indépendant le jour où il y en aura une.
#
#     nix-shell
#     ./tools/niphar-oath list       # la clé doit être en « usb mode oath »

{ pkgs ? import <nixpkgs> { } }:

pkgs.mkShell {
  name = "niphar-test";

  packages = with pkgs; [
    # --- FIDO2 / U2F ------------------------------------------------------
    libfido2 # fido2-token, fido2-cred, fido2-assert

    # --- Certificat d'attestation U2F (plan FIDO2, tâche 7) ---------------
    # U2F EXIGE un certificat dans la réponse REGISTER : « attestation none »
    # n'existe pas dans ce protocole, contrairement à CTAP2.
    openssl

    # --- Harnais de tests hôte (test/, CMake, compilateur de la machine) ---
    cmake
    gnumake
    gcc

    # --- Outillage du dépôt -----------------------------------------------
    # python3 avec pyusb : tools/svg2bitmap.py, ses tests, et le client CCID
    # tools/niphar-oath. Les TESTS de ce client, eux, tournent sans pyusb —
    # scripts/fast.sh les lance avec le python3 du système, d'où l'import
    # différé dans le client.
    (python3.withPackages (ps: [ ps.pyusb ]))
    usbutils # lsusb, pour vérifier l'énumération des descripteurs
  ];

  shellHook = ''
    echo "niphar-test — outils de validation côté hôte"
    echo "  libfido2 $(fido2-token -V 2>&1 || echo '?')   openssl $(openssl version | cut -d' ' -f2)"
    echo
    echo "  Ce shell ne construit PAS le firmware : source ~/esp/esp-idf/export.sh pour ça."
    echo
    echo "  Accès à /dev/hidraw* : fido2-token a besoin des règles udev de"
    echo "  libfido2, que ce shell ne peut pas installer (elles sont système)."
    echo "  Sans elles, préfixer par sudo — ou ajouter à la configuration NixOS :"
    echo "      services.udev.packages = [ pkgs.libfido2 ];"
  '';
}
