#pragma once

/*
 * Remplace le keyboard_config.h de KeSp_firmware, dont la pile de sécurité
 * n'emprunte qu'une seule chose : l'espace de noms NVS. C'est tout le couplage
 * qu'elle avait au clavier.
 *
 * Le nom est conservé à l'identique : changer d'espace de noms rendrait
 * illisibles les secrets d'un dongle KeSp migré, sans aucun bénéfice.
 */
#define STORAGE_NAMESPACE "storage"
