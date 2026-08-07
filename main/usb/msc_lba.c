#include "usb/msc_lba.h"

#include <stddef.h>

bool msc_lba_begin(msc_lba_iter_t *it,
                   uint32_t lba, uint32_t offset, uint32_t bufsize,
                   uint32_t sector_size, uint32_t sector_count,
                   bool allow_multi)
{
    if (it == NULL || sector_size == 0 || sector_count == 0 || bufsize == 0) {
        return false;
    }

    /*
     * Tout se calcule en 64 bits, et dans cet ordre précis, pour qu'aucune étape
     * intermédiaire ne puisse déborder — `lba * sector_size` dépasse 32 bits dès
     * 4 Gio, et un contrôle de bornes fait après un repliement laisserait passer
     * un accès hors média. L'hôte fournit ces trois valeurs sans contrainte.
     */
    if (lba >= sector_count) {
        return false;
    }

    const uint64_t capacity = (uint64_t)sector_count * (uint64_t)sector_size;
    const uint64_t base = (uint64_t)lba * (uint64_t)sector_size;
    const uint64_t avail = capacity - base;   /* > 0 : lba < sector_count */

    if ((uint64_t)offset >= avail) {
        return false;
    }

    const uint64_t addr = base + (uint64_t)offset;
    if ((uint64_t)bufsize > capacity - addr) {
        return false;
    }

    it->addr = addr;
    it->end = addr + (uint64_t)bufsize;
    it->sector_size = sector_size;
    it->allow_multi = allow_multi;
    return true;
}

bool msc_lba_next(msc_lba_iter_t *it, msc_span_t *out)
{
    if (it == NULL || out == NULL || it->addr >= it->end) {
        return false;
    }

    const uint32_t ss = it->sector_size;
    const uint32_t sector = (uint32_t)(it->addr / ss);
    const uint32_t in_sector = (uint32_t)(it->addr % ss);
    const uint64_t remaining = it->end - it->addr;

    uint32_t bytes;
    uint32_t sectors = 0;

    if (in_sector == 0 && remaining >= ss) {
        /* Aligné sur un secteur : on prend le plus gros morceau possible, ou un
         * seul secteur si l'appelant ne peut pas enchaîner (tampon non
         * accessible au DMA, qui devra passer par le rebond). */
        uint64_t whole = remaining / ss;
        if (!it->allow_multi) {
            whole = 1;
        }
        sectors = (uint32_t)whole;
        bytes = sectors * ss;
    } else {
        /* Accès partiel : on s'arrête à la fin du secteur courant. */
        uint64_t chunk = (uint64_t)ss - in_sector;
        if (chunk > remaining) {
            chunk = remaining;
        }
        bytes = (uint32_t)chunk;
    }

    out->sector = sector;
    out->in_sector = in_sector;
    out->bytes = bytes;
    out->sectors = sectors;

    it->addr += bytes;
    return true;
}
