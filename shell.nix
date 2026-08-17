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
    python3 # tools/svg2bitmap.py et ses tests, scripts de sonde
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
