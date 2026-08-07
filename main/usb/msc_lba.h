#pragma once

/*
 * Découpe d'un transfert MSC en accès secteur — logique pure.
 *
 * Extrait de msc_disk.c pour être testable sur l'hôte. Ce n'est pas de la
 * cosmétique : `lba` et `offset` viennent d'un hôte USB qui n'est pas digne de
 * confiance, et c'est ici que se logent les débordements d'entier et les
 * décalages d'un secteur. Le reste de msc_disk ne fait que des entrées-sorties.
 */

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t sector;      /* premier secteur du morceau */
    uint32_t in_sector;   /* décalage en octets dans ce secteur */
    uint32_t bytes;       /* octets couverts par ce morceau */
    uint32_t sectors;     /* nombre de secteurs entiers, 0 si accès partiel */
} msc_span_t;

typedef struct {
    uint64_t addr;        /* adresse absolue courante, en octets */
    uint64_t end;         /* adresse de fin, exclue */
    uint32_t sector_size;
    bool     allow_multi; /* autorise les morceaux de plusieurs secteurs */
} msc_lba_iter_t;

/*
 * Valide la requête et arme l'itérateur.
 *
 * Renvoie false — et n'arme rien — si la taille de secteur est nulle, si la
 * longueur est nulle, ou si l'accès déborde la capacité. Le calcul se fait en
 * 64 bits : `lba * sector_size` déborde 32 bits dès 4 Gio, et un contrôle de
 * bornes naïf laisserait passer l'accès.
 *
 * `allow_multi` dit si l'appelant peut traiter plusieurs secteurs d'un coup —
 * c'est le cas quand son tampon est accessible au DMA.
 */
bool msc_lba_begin(msc_lba_iter_t *it,
                   uint32_t lba, uint32_t offset, uint32_t bufsize,
                   uint32_t sector_size, uint32_t sector_count,
                   bool allow_multi);

/*
 * Morceau suivant. Renvoie false quand la requête est entièrement couverte.
 */
bool msc_lba_next(msc_lba_iter_t *it, msc_span_t *out);
