/* Tests du protocole du lien S3↔coffre.
 *
 * Écrits avant l'implémentation (norme TDD du CLAUDE.md). Ce qu'ils couvrent
 * est précisément ce qui peut être faux sans être visible : la détection
 * d'absence, l'ordre des octets, et le rejet de ce qui ne doit pas être
 * interprété.
 */
#include "test_framework.h"

#include "cr_crc16.h"
#include "link/link_proto.h"

/* ------------------------------------------------------------------------ */
/* Aller-retour de la carte de registres                                     */
/* ------------------------------------------------------------------------ */

static void test_pack_parse_roundtrip(void)
{
    uint8_t regs[LINK_REG_SIZE];
    const link_status_t in = {
        .state = LINK_STATE_SD_PRESENT | LINK_STATE_READY,
        .pending_op = 0x1234,
        .confirm_count = 0x89ABCDEF,
    };

    link_proto_pack_status(regs, &in);

    link_status_t out;
    memset(&out, 0, sizeof(out));
    TEST_ASSERT(link_proto_parse_status(regs, sizeof(regs), &out), "bloc valide accepté");
    TEST_ASSERT_EQ(out.version, LINK_PROTO_VERSION, "version renvoyée");
    TEST_ASSERT_EQ(out.state, in.state, "état conservé");
    TEST_ASSERT_EQ(out.pending_op, in.pending_op, "opération en attente conservée");
    TEST_ASSERT_EQ(out.confirm_count, in.confirm_count, "compteur conservé");
}

/* Le compteur est sur 4 octets : une erreur d'ordre ne se voit qu'aux valeurs
 * asymétriques et aux extrêmes. */
static void test_confirm_count_full_range(void)
{
    uint8_t regs[LINK_REG_SIZE];
    link_status_t in = { .state = 0, .pending_op = 0, .confirm_count = 0xFFFFFFFFu };
    link_status_t out;

    link_proto_pack_status(regs, &in);
    TEST_ASSERT(link_proto_parse_status(regs, sizeof(regs), &out), "compteur au maximum accepté");
    TEST_ASSERT_EQ(out.confirm_count, 0xFFFFFFFFu, "compteur au maximum conservé");

    in.confirm_count = 1;
    link_proto_pack_status(regs, &in);
    TEST_ASSERT(link_proto_parse_status(regs, sizeof(regs), &out), "compteur à 1 accepté");
    TEST_ASSERT_EQ(out.confirm_count, 1, "compteur à 1 conservé");
}

static void test_state_bits_independent(void)
{
    uint8_t regs[LINK_REG_SIZE];
    link_status_t out;

    const uint8_t bits[] = { LINK_STATE_SD_PRESENT, LINK_STATE_USB_MOUNTED, LINK_STATE_READY };
    for (size_t i = 0; i < sizeof(bits) / sizeof(bits[0]); i++) {
        link_status_t in = { .state = bits[i], .pending_op = 0, .confirm_count = 0 };
        link_proto_pack_status(regs, &in);
        TEST_ASSERT(link_proto_parse_status(regs, sizeof(regs), &out), "bloc à un seul bit accepté");
        TEST_ASSERT_EQ(out.state, bits[i], "bit d'état isolé conservé");
    }
}

/* ------------------------------------------------------------------------ */
/* Rejets                                                                    */
/* ------------------------------------------------------------------------ */

static void test_reject_bad_magic(void)
{
    uint8_t regs[LINK_REG_SIZE];
    const link_status_t in = { .state = 0, .pending_op = 0, .confirm_count = 0 };
    link_status_t out;

    link_proto_pack_status(regs, &in);
    regs[0] ^= 0xFF;   /* un seul octet du mot magique */
    TEST_ASSERT(!link_proto_parse_status(regs, sizeof(regs), &out), "mot magique faux rejeté");
}

static void test_reject_bad_version(void)
{
    uint8_t regs[LINK_REG_SIZE];
    const link_status_t in = { .state = 0, .pending_op = 0, .confirm_count = 0 };
    link_status_t out;

    link_proto_pack_status(regs, &in);
    regs[LINK_REG_VERSION] = LINK_PROTO_VERSION + 1;
    TEST_ASSERT(!link_proto_parse_status(regs, sizeof(regs), &out),
                "version inconnue rejetée plutôt qu'interprétée");
}

/* Le CRC est ce qui distingue une lecture SPI corrompue d'un état plausible :
 * le SPI ne fournit aucune détection d'erreur. */
static void test_reject_corrupted_payload(void)
{
    uint8_t regs[LINK_REG_SIZE];
    const link_status_t in = {
        .state = LINK_STATE_READY,
        .pending_op = 0x0042,
        .confirm_count = 7,
    };
    link_status_t out;

    link_proto_pack_status(regs, &in);
    regs[LINK_REG_CONFIRM_COUNT] ^= 0x01;   /* un bit, ailleurs que dans le magique */
    TEST_ASSERT(!link_proto_parse_status(regs, sizeof(regs), &out), "bit retourné détecté par le CRC");
}

/* La confirmation vient du S3 : elle est hors de la zone couverte par le CRC du
 * coffre, sinon toute écriture du maître invaliderait le bloc. Et pas seulement
 * l'octet de confirmation : AUCUN octet de la plage du maître n'entre dans le
 * CRC — il pourra s'y ajouter un champ sans invalider quoi que ce soit. */
static void test_master_range_outside_crc(void)
{
    uint8_t regs[LINK_REG_SIZE];
    const link_status_t in = { .state = 0, .pending_op = 0, .confirm_count = 0 };
    link_status_t out;

    link_proto_pack_status(regs, &in);
    regs[LINK_REG_USER_CONFIRM] = LINK_USER_CONFIRM_MAGIC;
    TEST_ASSERT(link_proto_parse_status(regs, sizeof(regs), &out),
                "écriture du maître n'invalide pas le CRC du coffre");

    for (unsigned i = 0; i < LINK_REG_MASTER_LEN; i++) {
        link_proto_pack_status(regs, &in);
        regs[LINK_REG_MASTER_BASE + i] ^= 0xFF;
        TEST_ASSERT(link_proto_parse_status(regs, sizeof(regs), &out),
                    "octet quelconque du maître hors du CRC du coffre");
    }
}

/*
 * Plus fort que le test précédent, et pour une autre raison : celui du dessus
 * dit que le CRC ne COUVRE pas la plage du maître, celui-ci que pack_status n'y
 * ÉCRIT rien — sur TOUTE la plage, pas seulement sur l'octet de confirmation.
 *
 * C'est la différence qui porte le transport. link/link_spi.c publie la zone du
 * coffre d'un seul bloc et s'arrête net avant la plage du maître, parce que le
 * S3 peut y avoir posé un appui que le coffre n'a pas encore lu. Cette découpe
 * ne vaut que si pack_status ne prétend rien mettre là. Le jour où il y
 * écrirait un zéro « pour propreté », publier le bloc entier en un seul appel
 * redeviendrait tentant, et effacerait silencieusement une confirmation réelle
 * — un défaut invisible aux tests du CRC, puisque le bloc resterait
 * parfaitement valide.
 */
static void test_pack_leaves_master_range_untouched(void)
{
    uint8_t regs[LINK_REG_SIZE];
    const link_status_t in = {
        .state = LINK_STATE_READY,
        .pending_op = 0x1234,
        .confirm_count = 7,
    };

    /* Motif témoin : n'importe quelle valeur que pack_status n'a aucune raison
     * d'écrire, pour distinguer « laissé tel quel » de « remis à zéro ». */
    memset(regs, 0xA5, sizeof(regs));
    link_proto_pack_status(regs, &in);
    for (unsigned i = 0; i < LINK_REG_MASTER_LEN; i++) {
        TEST_ASSERT_EQ(regs[LINK_REG_MASTER_BASE + i], 0xA5,
                       "octet du maître laissé tel quel");
    }

    /* Le cas qui compte vraiment : un appui déjà posé survit à une
     * republication de l'état du coffre. */
    regs[LINK_REG_USER_CONFIRM] = LINK_USER_CONFIRM_MAGIC;
    link_proto_pack_status(regs, &in);
    TEST_ASSERT_EQ(regs[LINK_REG_USER_CONFIRM], LINK_USER_CONFIRM_MAGIC,
                   "appui non lu non effacé par une republication");
}

/* ------------------------------------------------------------------------ */
/* Propriété des mots — l'invariant qui a fait rouvrir la carte des registres */
/* ------------------------------------------------------------------------ */

/*
 * Le tampon partagé du `spi_slave_hd` s'écrit PAR MOTS de 32 bits côté
 * application, et par octets côté maître. Une écriture qui ne remplit pas un mot
 * entier passe donc par un lire-modifier-écrire — et un appui de la propriétaire
 * qui arrive pendant ces quelques cycles est perdu. Tant qu'un mot porte à la
 * fois un champ du coffre et un champ du maître, ce lire-modifier-écrire est
 * inévitable ; s'ils ne partagent aucun mot, il disparaît.
 *
 * D'où la propriété testée ici : chaque octet du bloc a exactement UN
 * propriétaire, et les quatre octets d'un même mot ont TOUS le même. Elle porte
 * sur les plages, pas sur la liste des offsets — recopier les constantes ne
 * prouverait que la recopie.
 */

#define WORD_BYTES 4

/* Propriétaire déclaré d'un octet, d'après les seules plages. Rend '!' si les
 * deux plages le revendiquent, '?' si aucune ne le couvre. */
static char owner_of(unsigned off)
{
    /* Soustraction non signée plutôt qu'un encadrement en deux comparaisons :
     * un offset sous la base repasse par le haut et sort de la plage tout seul.
     * Une base à zéro rendrait sinon « >= base » toujours vrai, ce que -Wextra
     * refuse à juste titre. */
    const int chest  = (unsigned)(off - LINK_REG_CHEST_BASE)  < LINK_REG_CHEST_LEN;
    const int master = (unsigned)(off - LINK_REG_MASTER_BASE) < LINK_REG_MASTER_LEN;

    if (chest && master) {
        return '!';
    }
    if (chest) {
        return 'C';
    }
    if (master) {
        return 'M';
    }
    return '?';
}

/* Chaque octet appartient à exactement un des deux côtés : ni octet orphelin
 * (que personne ne publierait, donc que personne ne garantirait), ni octet
 * disputé (que les deux écriraient). */
static void test_every_byte_has_exactly_one_owner(void)
{
    for (unsigned off = 0; off < LINK_REG_SIZE; off++) {
        const char o = owner_of(off);
        TEST_ASSERT(o == 'C' || o == 'M', "octet ni orphelin ni disputé");
    }
}

/* Le cœur du sujet : aucun mot de 32 bits n'est à cheval sur les deux côtés. */
static void test_no_word_straddles_the_two_owners(void)
{
    TEST_ASSERT_EQ(LINK_REG_SIZE % WORD_BYTES, 0,
                   "le bloc fait un nombre entier de mots");

    for (unsigned base = 0; base < LINK_REG_SIZE; base += WORD_BYTES) {
        const char first = owner_of(base);
        for (unsigned i = 1; i < WORD_BYTES; i++) {
            TEST_ASSERT_EQ(owner_of(base + i), first,
                           "les quatre octets d'un mot ont le même propriétaire");
        }
    }
}

/* Et les champs nommés tombent du bon côté, sur toute leur largeur : une plage
 * juste qui décrirait mal les champs ne protégerait rien. */
static void test_named_fields_fall_on_their_owner_side(void)
{
    const struct { unsigned off, len; char owner; } fields[] = {
        { LINK_REG_MAGIC,         4, 'C' },
        { LINK_REG_VERSION,       1, 'C' },
        { LINK_REG_STATE,         1, 'C' },
        { LINK_REG_PENDING_OP,    2, 'C' },
        { LINK_REG_CONFIRM_COUNT, 4, 'C' },
        { LINK_REG_CRC,           2, 'C' },
        { LINK_REG_USER_CONFIRM,  1, 'M' },
    };

    for (size_t f = 0; f < sizeof(fields) / sizeof(fields[0]); f++) {
        for (unsigned i = 0; i < fields[f].len; i++) {
            TEST_ASSERT_EQ(owner_of(fields[f].off + i), fields[f].owner,
                           "champ entièrement du côté de son propriétaire");
        }
    }

    /* Le CRC ne couvre que le coffre : l'inclure ferait invalider le bloc à
     * chaque écriture légitime du maître. */
    for (unsigned off = 0; off < LINK_REG_CRC_SPAN; off++) {
        TEST_ASSERT_EQ(owner_of(off), 'C', "la zone couverte par le CRC est au coffre");
    }
}

/* ------------------------------------------------------------------------ */
/* Absence — le cas normal, et celui qu'on code à l'envers                    */
/* ------------------------------------------------------------------------ */

static void test_absent_all_zero(void)
{
    uint8_t regs[LINK_REG_SIZE];
    memset(regs, 0x00, sizeof(regs));
    TEST_ASSERT(link_proto_is_absent(regs, sizeof(regs)), "tout à 0x00 = absent");

    link_status_t out;
    TEST_ASSERT(!link_proto_parse_status(regs, sizeof(regs), &out), "bloc absent non interprété");
}

static void test_absent_all_ones(void)
{
    uint8_t regs[LINK_REG_SIZE];
    memset(regs, 0xFF, sizeof(regs));
    TEST_ASSERT(link_proto_is_absent(regs, sizeof(regs)), "tout à 0xFF = absent");

    link_status_t out;
    TEST_ASSERT(!link_proto_parse_status(regs, sizeof(regs), &out), "bloc absent non interprété");
}

static void test_present_not_absent(void)
{
    uint8_t regs[LINK_REG_SIZE];
    const link_status_t in = { .state = 0, .pending_op = 0, .confirm_count = 0 };

    link_proto_pack_status(regs, &in);
    TEST_ASSERT(!link_proto_is_absent(regs, sizeof(regs)),
                "un coffre à l'état nul n'est pas un coffre absent");
}

/* Le paramètre de longueur existe avant son premier appelant : une transaction
 * SPI courte ne doit pas faire lire au-delà du tampon. */
static void test_reject_short_buffer(void)
{
    uint8_t regs[LINK_REG_SIZE];
    const link_status_t in = { .state = 0, .pending_op = 0, .confirm_count = 0 };
    link_status_t out;

    link_proto_pack_status(regs, &in);
    TEST_ASSERT(!link_proto_parse_status(regs, LINK_REG_SIZE - 1, &out),
                "bloc tronqué d'un octet rejeté");
    TEST_ASSERT(!link_proto_parse_status(regs, 0, &out), "bloc vide rejeté");
    TEST_ASSERT(link_proto_parse_status(regs, LINK_REG_SIZE, &out),
                "bloc complet toujours accepté");
}

/* Un bloc presque uniforme ne doit pas être confondu avec une ligne flottante. */
static void test_almost_uniform_is_present(void)
{
    uint8_t regs[LINK_REG_SIZE];
    memset(regs, 0xFF, sizeof(regs));
    regs[LINK_REG_SIZE - 1] = 0x00;
    TEST_ASSERT(!link_proto_is_absent(regs, sizeof(regs)),
                "un seul octet différent suffit à écarter l'absence");
}


/* ------------------------------------------------------------------------ */
/* Vecteurs partagés — la table de docs/LINK_CONTRACT.md, section 10          */
/* ------------------------------------------------------------------------ */

/*
 * Ces octets SONT le contrat publié à l'équipe KeSp. Ils ont été produits en
 * exécutant link_proto.c, puis recopiés ici et dans le document : c'est cette
 * recopie que ces tests surveillent.
 *
 * Ce qu'ils attrapent n'est pas une régression de link_proto — les tests
 * au-dessus s'en chargent, et mieux, puisqu'ils portent sur des propriétés.
 * Ils attrapent la DIVERGENCE entre le code et le document : le jour où la
 * carte des registres bouge, le contrat publié devient faux en silence, et
 * KeSp implémente contre une table périmée. Ici, le rouge tombe du bon côté.
 *
 * Si l'un de ces tests casse, ce n'est donc pas la table qu'on ajuste : c'est
 * la section 10 de docs/LINK_CONTRACT.md qu'on régénère en exécutant le code,
 * la version du protocole qu'on incrémente, et KeSp qu'on prévient.
 */

/* V1 — nominal : SD + USB monté + prêt, PSO:CDS en attente, 42
 * confirmations, instance 3, et le mode ACTIF qui accompagne USB_MOUNTED :
 * storage. Un bloc annonçant « monté » avec un mode actif `none` serait
 * contradictoire — c'est ce que 0x0D rend impossible à publier sans le dire. */
static const uint8_t k_vec_v1[LINK_REG_SIZE] = {
    0x4E, 0x49, 0x50, 0x48, 0x02, 0x07, 0x01, 0x00, 0x2A, 0x00,
    0x00, 0x00, 0x03, 0x01, 0xEB, 0x2B, 0x00, 0x00, 0x00, 0x00,
};
/* V4 — mot magique faux d'un octet, tout le reste identique à V1. */
static const uint8_t k_vec_v4[LINK_REG_SIZE] = {
    0x4E, 0x49, 0x50, 0x58, 0x02, 0x07, 0x01, 0x00, 0x2A, 0x00,
    0x00, 0x00, 0x03, 0x01, 0xEB, 0x2B, 0x00, 0x00, 0x00, 0x00,
};
/* V5 — version 3 annoncée, CRC RECALCULÉ et juste : refusé sur la version
 * seule, pas sur une corruption. */
static const uint8_t k_vec_v5[LINK_REG_SIZE] = {
    0x4E, 0x49, 0x50, 0x48, 0x03, 0x07, 0x01, 0x00, 0x2A, 0x00,
    0x00, 0x00, 0x03, 0x01, 0xCC, 0x07, 0x00, 0x00, 0x00, 0x00,
};
/* V6 — un bit de la charge utile retourné (42 → 43), CRC laissé tel quel. */
static const uint8_t k_vec_v6[LINK_REG_SIZE] = {
    0x4E, 0x49, 0x50, 0x48, 0x02, 0x07, 0x01, 0x00, 0x2B, 0x00,
    0x00, 0x00, 0x03, 0x01, 0xEB, 0x2B, 0x00, 0x00, 0x00, 0x00,
};
/* V6b — l'inverse : charge utile intacte, un bit retourné dans l'octet BAS du
 * champ CRC. */
static const uint8_t k_vec_v6b[LINK_REG_SIZE] = {
    0x4E, 0x49, 0x50, 0x48, 0x02, 0x07, 0x01, 0x00, 0x2A, 0x00,
    0x00, 0x00, 0x03, 0x01, 0xEA, 0x2B, 0x00, 0x00, 0x00, 0x00,
};
/* V6c — proposé par KeSp, et il manquait : le même bit retourné dans l'octet
 * HAUT du champ CRC. Une implémentation qui ne comparerait que l'octet bas
 * (ou qui rangerait le CRC en gros-boutiste) passerait V6b et tomberait ici. */
static const uint8_t k_vec_v6c[LINK_REG_SIZE] = {
    0x4E, 0x49, 0x50, 0x48, 0x02, 0x07, 0x01, 0x00, 0x2A, 0x00,
    0x00, 0x00, 0x03, 0x01, 0xEB, 0x2A, 0x00, 0x00, 0x00, 0x00,
};
/* V6d — l'instance changée (3 → 2) sans recalcul du CRC. C'est la PREUVE, en
 * octets, que l'instance entre dans l'étendue couverte : la v1 aurait accepté
 * ce bloc. */
static const uint8_t k_vec_v6d[LINK_REG_SIZE] = {
    0x4E, 0x49, 0x50, 0x48, 0x02, 0x07, 0x01, 0x00, 0x2A, 0x00,
    0x00, 0x00, 0x02, 0x01, 0xEB, 0x2B, 0x00, 0x00, 0x00, 0x00,
};
/* V6e — le même argument pour le mode actif : storage → oath sans recalcul du
 * CRC. Sans ce vecteur, 0x0D serait couvert sur le papier et personne ne
 * l'aurait vérifié en octets. */
static const uint8_t k_vec_v6e[LINK_REG_SIZE] = {
    0x4E, 0x49, 0x50, 0x48, 0x02, 0x07, 0x01, 0x00, 0x2A, 0x00,
    0x00, 0x00, 0x03, 0x05, 0xEB, 0x2B, 0x00, 0x00, 0x00, 0x00,
};
/* V8 — V1 plus une confirmation posée et pas encore lue, avec l'écho de
 * l'instance ARMÉE (3). Le CRC est le MÊME qu'en V1, et c'est tout
 * l'argument sur l'étendue. */
static const uint8_t k_vec_v8[LINK_REG_SIZE] = {
    0x4E, 0x49, 0x50, 0x48, 0x02, 0x07, 0x01, 0x00, 0x2A, 0x00,
    0x00, 0x00, 0x03, 0x01, 0xEB, 0x2B, 0x5A, 0x03, 0x00, 0x00,
};
/* V9 — coffre présent et PAS prêt : aucun bit d'état, rien en attente, aucune
 * opération jamais armée (instance 0), aucun mode actif. Son CRC non nul est ce
 * qui le distingue d'un bloc absent — et il n'a PAS bougé en passant à 0x0D,
 * puisque l'octet y valait déjà zéro. */
static const uint8_t k_vec_v9[LINK_REG_SIZE] = {
    0x4E, 0x49, 0x50, 0x48, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x39, 0xD4, 0x00, 0x00, 0x00, 0x00,
};
/* V10 — proposé par KeSp : un bloc uniforme SAUF le dernier octet. Un test
 * d'absence qui s'arrêterait au premier octet, ou qui ne balaierait que les
 * dix-neuf premiers, dirait « absent » sur un coffre qui parle. */
static const uint8_t k_vec_v10[LINK_REG_SIZE] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
};
/* V11 — le défaut de la v1, en octets : bloc du coffre parfaitement valide,
 * confirmation parfaitement formée, écho sur l'instance PRÉCÉDENTE (2 pour une
 * instance armée de 3). La v1 l'aurait accordée. */
static const uint8_t k_vec_v11[LINK_REG_SIZE] = {
    0x4E, 0x49, 0x50, 0x48, 0x02, 0x07, 0x01, 0x00, 0x2A, 0x00,
    0x00, 0x00, 0x03, 0x01, 0xEB, 0x2B, 0x5A, 0x02, 0x00, 0x00,
};
/* V12 — mode demandé inconnu (0x09). Le bloc du coffre reste valide : c'est la
 * DEMANDE qui se refuse, pas le bloc. */
static const uint8_t k_vec_v12[LINK_REG_SIZE] = {
    0x4E, 0x49, 0x50, 0x48, 0x02, 0x07, 0x01, 0x00, 0x2A, 0x00,
    0x00, 0x00, 0x03, 0x01, 0xEB, 0x2B, 0x00, 0x00, 0x09, 0x00,
};
/* V13 — le même, avec une valeur de fil attribuée (0x02, pgp) : appliqué. */
static const uint8_t k_vec_v13[LINK_REG_SIZE] = {
    0x4E, 0x49, 0x50, 0x48, 0x02, 0x07, 0x01, 0x00, 0x2A, 0x00,
    0x00, 0x00, 0x03, 0x01, 0xEB, 0x2B, 0x00, 0x00, 0x02, 0x00,
};
/* V14 — bascule EN COURS : le mode actif vaut l'indéterminé (0xFF) et
 * USB_MOUNTED est retombé. C'est le seul état où demandé et actif diffèrent
 * légitimement, et c'est celui que le clavier doit savoir distinguer d'un
 * refus — il affiche alors sa demande en attente plutôt qu'une arrivée. */
static const uint8_t k_vec_v14[LINK_REG_SIZE] = {
    0x4E, 0x49, 0x50, 0x48, 0x02, 0x05, 0x00, 0x00, 0x2A, 0x00,
    0x00, 0x00, 0x03, 0xFF, 0x5F, 0x2F, 0x00, 0x00, 0x00, 0x00,
};

static void test_shared_vectors_accepted(void)
{
    link_status_t out;

    memset(&out, 0, sizeof(out));
    TEST_ASSERT(link_proto_parse_status(k_vec_v1, LINK_REG_SIZE, &out), "V1 accepté");
    TEST_ASSERT_EQ(out.version, 2, "V1 version");
    TEST_ASSERT_EQ(out.state, 0x07, "V1 état");
    TEST_ASSERT_EQ(out.pending_op, 1, "V1 opération en attente");
    TEST_ASSERT_EQ(out.confirm_count, 42, "V1 compteur");
    TEST_ASSERT_EQ(out.instance, 3, "V1 instance");
    TEST_ASSERT(!link_proto_is_absent(k_vec_v1, LINK_REG_SIZE), "V1 pas absent");

    /* V8 ne diffère de V1 que par la plage du maître — donc même verdict ET
     * mêmes champs décodés. C'est l'étendue du CRC rendue visible. */
    memset(&out, 0, sizeof(out));
    TEST_ASSERT(link_proto_parse_status(k_vec_v8, LINK_REG_SIZE, &out), "V8 accepté");
    TEST_ASSERT_EQ(out.state, 0x07, "V8 état identique à V1");
    TEST_ASSERT_EQ(out.pending_op, 1, "V8 opération identique à V1");
    TEST_ASSERT_EQ(out.confirm_count, 42, "V8 compteur identique à V1");
    TEST_ASSERT_EQ(out.instance, 3, "V8 instance identique à V1");
    TEST_ASSERT_EQ(memcmp(k_vec_v1, k_vec_v8, LINK_REG_CRC + 2), 0,
                   "V1 et V8 partagent octets couverts par le CRC, CRC compris");

    memset(&out, 0, sizeof(out));
    TEST_ASSERT(link_proto_parse_status(k_vec_v9, LINK_REG_SIZE, &out), "V9 accepté");
    TEST_ASSERT_EQ(out.state, 0x00, "V9 aucun bit d'état");
    TEST_ASSERT_EQ(out.pending_op, 0, "V9 rien en attente");
    TEST_ASSERT_EQ(out.confirm_count, 0, "V9 compteur nul");
    TEST_ASSERT_EQ(out.instance, 0, "V9 aucune opération jamais armée");
    TEST_ASSERT(!link_proto_is_absent(k_vec_v9, LINK_REG_SIZE),
                "V9 présent et non prêt, pas absent");

    /* V11, V12 et V13 portent un bloc du coffre identique à V1 : c'est la
     * plage du MAÎTRE qui les distingue, et elle n'entre pas dans le CRC. */
    memset(&out, 0, sizeof(out));
    TEST_ASSERT(link_proto_parse_status(k_vec_v11, LINK_REG_SIZE, &out), "V11 bloc valide");
    TEST_ASSERT_EQ(out.instance, 3, "V11 instance armée");
    TEST_ASSERT(link_proto_parse_status(k_vec_v12, LINK_REG_SIZE, &out), "V12 bloc valide");
    TEST_ASSERT(link_proto_parse_status(k_vec_v13, LINK_REG_SIZE, &out), "V13 bloc valide");
}

/* Les vecteurs qui portent une écriture du maître, et ce que le coffre en
 * fait. C'est là que la v2 se lit en octets. */
static void test_shared_vectors_master_side(void)
{
    link_status_t st;
    link_master_t m;

    /* V8 — l'écho porte l'instance ARMÉE : accepté. */
    TEST_ASSERT(link_proto_parse_status(k_vec_v8, LINK_REG_SIZE, &st), "V8 bloc valide");
    TEST_ASSERT(link_proto_parse_master(k_vec_v8, LINK_REG_SIZE, &m), "V8 plage maître lue");
    TEST_ASSERT_EQ(m.confirm, LINK_USER_CONFIRM_MAGIC, "V8 octet de confirmation");
    TEST_ASSERT_EQ(m.echo, 3, "V8 écho");
    TEST_ASSERT(link_proto_confirm_accepted(&m, st.instance), "V8 confirmation acceptée");

    /* V11 — même bloc, même 0x5A, écho sur l'instance PRÉCÉDENTE : refusé. La
     * v1 aurait accordé, et c'est le défaut que KeSp a relevé. */
    TEST_ASSERT(link_proto_parse_status(k_vec_v11, LINK_REG_SIZE, &st), "V11 bloc valide");
    TEST_ASSERT(link_proto_parse_master(k_vec_v11, LINK_REG_SIZE, &m), "V11 plage maître lue");
    TEST_ASSERT_EQ(m.confirm, LINK_USER_CONFIRM_MAGIC, "V11 confirmation bien formée");
    TEST_ASSERT_EQ(m.echo, 2, "V11 écho périmé");
    TEST_ASSERT(!link_proto_confirm_accepted(&m, st.instance),
                "V11 confirmation périmée refusée en silence");

    /* V12 — mode inconnu : refusé, le coffre reste où il est. */
    TEST_ASSERT(link_proto_parse_master(k_vec_v12, LINK_REG_SIZE, &m), "V12 plage maître lue");
    TEST_ASSERT_EQ(m.usb_mode, 0x09, "V12 valeur de fil inconnue");
    TEST_ASSERT(!link_proto_usb_mode_is_known(m.usb_mode), "V12 mode inconnu");
    TEST_ASSERT_EQ(link_proto_mode_request(m.usb_mode, LINK_USB_MODE_NONE),
                   LINK_MODE_REQ_REFUSE, "V12 refusé");

    /* V13 — même place, valeur attribuée : appliqué. */
    TEST_ASSERT(link_proto_parse_master(k_vec_v13, LINK_REG_SIZE, &m), "V13 plage maître lue");
    TEST_ASSERT_EQ(m.usb_mode, LINK_USB_MODE_PGP, "V13 pgp demandé");
    TEST_ASSERT_EQ(link_proto_mode_request(m.usb_mode, LINK_USB_MODE_NONE),
                   LINK_MODE_REQ_APPLY, "V13 appliqué");
    TEST_ASSERT_EQ(link_proto_mode_request(m.usb_mode, LINK_USB_MODE_PGP),
                   LINK_MODE_REQ_UNCHANGED, "V13 relu une seconde fois : rien");
}

static void test_shared_vectors_rejected(void)
{
    link_status_t out;
    uint8_t absent[LINK_REG_SIZE];

    /* V2 et V3 — absents, et rejetés POUR CETTE RAISON-LÀ. */
    memset(absent, 0x00, sizeof(absent));
    TEST_ASSERT(link_proto_is_absent(absent, LINK_REG_SIZE), "V2 absent");
    TEST_ASSERT(!link_proto_parse_status(absent, LINK_REG_SIZE, &out), "V2 non interprété");
    memset(absent, 0xFF, sizeof(absent));
    TEST_ASSERT(link_proto_is_absent(absent, LINK_REG_SIZE), "V3 absent");
    TEST_ASSERT(!link_proto_parse_status(absent, LINK_REG_SIZE, &out), "V3 non interprété");

    TEST_ASSERT(!link_proto_parse_status(k_vec_v4, LINK_REG_SIZE, &out), "V4 magique faux rejeté");
    TEST_ASSERT(!link_proto_parse_status(k_vec_v5, LINK_REG_SIZE, &out), "V5 version inconnue rejetée");
    TEST_ASSERT(!link_proto_parse_status(k_vec_v6, LINK_REG_SIZE, &out), "V6 charge utile corrompue rejetée");
    TEST_ASSERT(!link_proto_parse_status(k_vec_v6b, LINK_REG_SIZE, &out), "V6b CRC octet bas corrompu rejeté");
    TEST_ASSERT(!link_proto_parse_status(k_vec_v6c, LINK_REG_SIZE, &out), "V6c CRC octet haut corrompu rejeté");
    TEST_ASSERT(!link_proto_parse_status(k_vec_v6d, LINK_REG_SIZE, &out), "V6d instance corrompue rejetée");
    TEST_ASSERT(!link_proto_parse_status(k_vec_v6e, LINK_REG_SIZE, &out),
                "V6e mode actif corrompu rejeté");

    /* V7 — les 19 premiers octets de V1, annoncés pour ce qu'ils sont. */
    TEST_ASSERT(!link_proto_parse_status(k_vec_v1, LINK_REG_SIZE - 1, &out), "V7 tronqué rejeté");

    /* V10 — uniforme sauf le dernier octet : PAS un bloc absent, et rejeté par
     * le mot magique, pas par l'absence. */
    TEST_ASSERT(!link_proto_is_absent(k_vec_v10, LINK_REG_SIZE),
                "V10 un seul octet différent suffit à écarter l'absence");
    TEST_ASSERT(!link_proto_parse_status(k_vec_v10, LINK_REG_SIZE, &out), "V10 non interprété");

    /* V4, V5, V6, V6b, V6c, V6d ne sont pas des blocs absents : leur rejet
     * vient bien du contrôle annoncé et pas d'une ligne flottante. */
    TEST_ASSERT(!link_proto_is_absent(k_vec_v4, LINK_REG_SIZE), "V4 pas un bloc absent");
    TEST_ASSERT(!link_proto_is_absent(k_vec_v5, LINK_REG_SIZE), "V5 pas un bloc absent");
    TEST_ASSERT(!link_proto_is_absent(k_vec_v6, LINK_REG_SIZE), "V6 pas un bloc absent");
    TEST_ASSERT(!link_proto_is_absent(k_vec_v6b, LINK_REG_SIZE), "V6b pas un bloc absent");
    TEST_ASSERT(!link_proto_is_absent(k_vec_v6c, LINK_REG_SIZE), "V6c pas un bloc absent");
    TEST_ASSERT(!link_proto_is_absent(k_vec_v6d, LINK_REG_SIZE), "V6d pas un bloc absent");
}

/* V1 et V9 doivent rester ce que pack_status PRODUIT, pas seulement ce qu'il
 * accepte : un contrat qui ne décrirait que les blocs tolérés laisserait le
 * coffre publier autre chose. */
static void test_shared_vectors_are_what_the_chest_publishes(void)
{
    uint8_t regs[LINK_REG_SIZE];

    const link_status_t nominal = {
        .state = LINK_STATE_SD_PRESENT | LINK_STATE_USB_MOUNTED | LINK_STATE_READY,
        .pending_op = 1,
        .confirm_count = 42,
        .instance = 3,
        .usb_mode_active = LINK_USB_MODE_STORAGE,
    };
    memset(regs, 0, sizeof(regs));
    link_proto_pack_status(regs, &nominal);
    TEST_ASSERT_EQ(memcmp(regs, k_vec_v1, LINK_REG_SIZE), 0,
                   "le coffre publie exactement V1");

    const link_status_t booting = { .state = 0, .pending_op = 0, .confirm_count = 0,
                                    .instance = 0,
                                    .usb_mode_active = LINK_USB_MODE_NONE };
    memset(regs, 0, sizeof(regs));
    link_proto_pack_status(regs, &booting);
    TEST_ASSERT_EQ(memcmp(regs, k_vec_v9, LINK_REG_SIZE), 0,
                   "le coffre publie exactement V9 avant d'être prêt");
}

/*
 * La variante de CRC, par sa valeur de contrôle plutôt que par son nom : le
 * document donne 0x6F91 à KeSp pour qu'ils comparent la leur. cr_crc16.h
 * annonce « CRC-16/X-25 », qui vaudrait 0x906E — c'est le nom qui est faux, pas
 * la fonction, et c'est précisément pourquoi le contrat publie le nombre.
 */
static void test_crc_variant_check_value(void)
{
    static const uint8_t digits[9] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    TEST_ASSERT_EQ(cr_crc16(digits, sizeof(digits)), 0x6F91,
                   "valeur de contrôle du CRC publiée dans LINK_CONTRACT.md");
}


/* ------------------------------------------------------------------------ */
/* v2 — la confirmation porte une INSTANCE, pas seulement un code             */
/* ------------------------------------------------------------------------ */

/*
 * Le défaut que ces tests ferment, et qu'aucun test de la v1 ne voyait :
 *
 *   la propriétaire appuie pour « CODE OTP GITHUB » ; l'écriture du maître se
 *   perd sur le bus ; l'opération expire ; l'hôte en arme une autre, du MÊME
 *   code (« CODE OTP BANQUE ») ; la reprise du maître, qui ne portait que le
 *   code, la confirme. Elle n'a jamais donné son accord pour ce compte-là.
 *
 * Écrits comme des PROPRIÉTÉS et pas comme des constantes recopiées : « un
 * écho périmé ne passe pas » est vrai pour tous les couples (armée, écho), pas
 * seulement pour celui qu'on aurait choisi. Les boucles accumulent les écarts
 * et n'affirment qu'une fois — un contre-exemple noie sinon la sortie.
 */

static void test_confirm_needs_the_magic_AND_the_instance(void)
{
    unsigned wrong = 0, accepted_total = 0;

    for (unsigned armed = 0; armed < 256; armed++) {
        for (unsigned confirm = 0; confirm < 256; confirm++) {
            for (unsigned echo = 0; echo < 256; echo++) {
                const link_master_t m = {
                    .confirm = (uint8_t)confirm,
                    .echo = (uint8_t)echo,
                    .usb_mode = LINK_USB_MODE_NONE,
                };
                const bool got = link_proto_confirm_accepted(&m, (uint8_t)armed);
                const bool want = (confirm == LINK_USER_CONFIRM_MAGIC) && (echo == armed);
                if (got != want) {
                    wrong++;
                }
                if (got) {
                    accepted_total++;
                }
            }
        }
    }

    TEST_ASSERT_EQ(wrong, 0,
                   "accepté SI ET SEULEMENT SI 0x5A et écho == instance armée");
    /* Exactement un couple (confirm, echo) par instance armée, donc 256 en
     * tout : le « et seulement si » compté plutôt que supposé. Sans lui, une
     * implémentation qui accepterait tout passerait le test ci-dessus le jour
     * où `want` serait écrit de travers. */
    TEST_ASSERT_EQ(accepted_total, 256,
                   "une seule écriture acceptable par instance armée");
}

/* Le scénario de KeSp, joué tel quel : deux opérations du MÊME code, dont la
 * seconde ne doit rien recevoir de l'appui destiné à la première. */
static void test_stale_echo_from_a_same_coded_operation_is_refused(void)
{
    /* Instance 7 : « CODE OTP GITHUB ». Le maître la lit, montre l'opération,
     * obtient l'appui — et son écriture se perd. */
    const uint8_t github = 7;
    /* L'opération expire. L'hôte en arme une autre, même code, instance
     * suivante : « CODE OTP BANQUE ». */
    const uint8_t banque = 8;

    const link_master_t retry = {
        .confirm = LINK_USER_CONFIRM_MAGIC,
        .echo = github,
        .usb_mode = LINK_USB_MODE_NONE,
    };

    TEST_ASSERT(!link_proto_confirm_accepted(&retry, banque),
                "la reprise d'un appui pour GITHUB ne confirme pas BANQUE");
    TEST_ASSERT(link_proto_confirm_accepted(&retry, github),
                "la même reprise reste valable tant que GITHUB est armée");
}

/* Une écriture bien formée mais sans le motif magique ne passe jamais, quel que
 * soit l'écho — l'instance ne remplace pas le filtre anti-bruit, elle s'y
 * ajoute. */
static void test_instance_does_not_replace_the_magic_filter(void)
{
    unsigned accepted = 0;
    for (unsigned confirm = 0; confirm < 256; confirm++) {
        if (confirm == LINK_USER_CONFIRM_MAGIC) {
            continue;
        }
        const link_master_t m = { .confirm = (uint8_t)confirm, .echo = 42,
                                  .usb_mode = LINK_USB_MODE_NONE };
        if (link_proto_confirm_accepted(&m, 42)) {
            accepted++;
        }
    }
    TEST_ASSERT_EQ(accepted, 0,
                   "écho juste mais octet de confirmation faux : jamais accepté");
}

/* ------------------------------------------------------------------------ */
/* v2 — l'instance entre dans le CRC                                          */
/* ------------------------------------------------------------------------ */

/*
 * Ce que ça achète, et qui justifie d'avoir déplacé le CRC en 0x0E : le coffre
 * publie sa zone en une écriture, mais le maître peut lire pendant. Sans
 * couverture, un bloc DÉCHIRÉ — l'opération d'un armement avec l'instance du
 * suivant — passerait le CRC et le maître montrerait une opération en
 * renvoyant l'instance d'une autre. C'est exactement le défaut qu'on ferme,
 * réintroduit par le transport.
 */
static void test_instance_is_covered_by_the_crc(void)
{
    uint8_t regs[LINK_REG_SIZE];
    link_status_t in = { .state = LINK_STATE_READY, .pending_op = 4,
                         .confirm_count = 9, .instance = 0 };
    link_status_t out;

    unsigned accepted = 0;
    for (unsigned i = 0; i < 256; i++) {
        in.instance = (uint8_t)i;
        link_proto_pack_status(regs, &in);

        /* Le bloc intact est accepté, et rend l'instance publiée. */
        if (!link_proto_parse_status(regs, sizeof(regs), &out) || out.instance != i) {
            accepted++;
        }

        /* Le même bloc, instance changée SANS recalcul du CRC : refusé. */
        regs[LINK_REG_INSTANCE] ^= 0xFF;
        if (link_proto_parse_status(regs, sizeof(regs), &out)) {
            accepted++;
        }
    }
    TEST_ASSERT_EQ(accepted, 0,
                   "l'instance est publiée, relue, et protégée par le CRC");
}

/* Plus fort : deux instances différentes ne peuvent pas donner le même CRC sur
 * un bloc par ailleurs identique. C'est ce qui rend la détection sûre et pas
 * seulement probable — un CRC16 sur une erreur d'un seul octet ne s'annule
 * jamais. */
static void test_two_instances_never_share_a_crc(void)
{
    uint16_t crc[256];
    uint8_t regs[LINK_REG_SIZE];
    link_status_t in = { .state = 0x07, .pending_op = 1, .confirm_count = 42,
                         .instance = 0 };

    for (unsigned i = 0; i < 256; i++) {
        in.instance = (uint8_t)i;
        link_proto_pack_status(regs, &in);
        crc[i] = (uint16_t)regs[LINK_REG_CRC] | (uint16_t)((uint16_t)regs[LINK_REG_CRC + 1] << 8);
    }

    unsigned collisions = 0;
    for (unsigned a = 0; a < 256; a++) {
        for (unsigned b = a + 1; b < 256; b++) {
            if (crc[a] == crc[b]) {
                collisions++;
            }
        }
    }
    TEST_ASSERT_EQ(collisions, 0, "256 instances, 256 CRC distincts");
}

/* ------------------------------------------------------------------------ */
/* v2 — la plage du maître                                                    */
/* ------------------------------------------------------------------------ */

static void test_parse_master_reads_the_three_bytes(void)
{
    uint8_t regs[LINK_REG_SIZE];
    const link_status_t in = { .state = 0, .pending_op = 0, .confirm_count = 0,
                               .instance = 0 };
    link_master_t m;

    link_proto_pack_status(regs, &in);
    regs[LINK_REG_USER_CONFIRM] = LINK_USER_CONFIRM_MAGIC;
    regs[LINK_REG_CONFIRM_ECHO] = 0x11;
    regs[LINK_REG_USB_MODE_REQ] = LINK_USB_MODE_OATH;

    TEST_ASSERT(link_proto_parse_master(regs, sizeof(regs), &m), "plage du maître lue");
    TEST_ASSERT_EQ(m.confirm, LINK_USER_CONFIRM_MAGIC, "octet de confirmation");
    TEST_ASSERT_EQ(m.echo, 0x11, "écho d'instance");
    TEST_ASSERT_EQ(m.usb_mode, LINK_USB_MODE_OATH, "mode demandé");

    /* Même garde que parse_status : un transfert court ne doit pas faire lire
     * au-delà du tampon. */
    TEST_ASSERT(!link_proto_parse_master(regs, LINK_REG_SIZE - 1, &m),
                "bloc tronqué refusé");

    /* Et les trois octets se lisent même sur un bloc du coffre invalide : ils
     * ne dépendent pas de son CRC, qui ne les couvre pas. */
    regs[LINK_REG_CRC] ^= 0xFF;
    TEST_ASSERT(link_proto_parse_master(regs, sizeof(regs), &m),
                "plage du maître lisible même si le bloc du coffre est corrompu");
}

/* ------------------------------------------------------------------------ */
/* v2 — le mode USB demandé                                                   */
/* ------------------------------------------------------------------------ */

/*
 * Deux règles, et la seconde est celle qui peut faire mal : une valeur inconnue
 * est REFUSÉE, le coffre reste où il est. Pas de repli sur « aucun » (qui
 * démonterait l'interface que la propriétaire utilise), pas de « la plus
 * proche » (qui exposerait autre chose que ce qu'on a demandé).
 */

static void test_no_unknown_wire_value_ever_applies(void)
{
    unsigned applied_unknown = 0, refused_known = 0, wrong_unchanged = 0;

    for (unsigned requested = 0; requested < 256; requested++) {
        for (unsigned applied = 0; applied < 256; applied++) {
            const link_mode_req_t r =
                link_proto_mode_request((uint8_t)requested, (uint8_t)applied);
            const bool known = link_proto_usb_mode_is_known((uint8_t)requested);

            if (r == LINK_MODE_REQ_APPLY && (!known || requested == applied)) {
                applied_unknown++;
            }
            if (r == LINK_MODE_REQ_REFUSE && known) {
                refused_known++;
            }
            if (r == LINK_MODE_REQ_UNCHANGED && requested != applied) {
                wrong_unchanged++;
            }
        }
    }

    TEST_ASSERT_EQ(applied_unknown, 0,
                   "on n'applique QUE une valeur connue et différente de l'appliquée");
    TEST_ASSERT_EQ(refused_known, 0, "on ne refuse QUE ce qu'on ne connaît pas");
    TEST_ASSERT_EQ(wrong_unchanged, 0,
                   "« inchangé » ne se dit que d'une valeur déjà appliquée");
}

/* Le refus l'emporte sur l'égalité : une valeur inconnue déjà « appliquée » ne
 * peut pas exister (le coffre n'applique que du connu), mais si elle arrivait,
 * la taire serait pire que la dire. */
static void test_unknown_is_refused_even_if_it_matches(void)
{
    TEST_ASSERT_EQ(link_proto_mode_request(0x77, 0x77), LINK_MODE_REQ_REFUSE,
                   "inconnu refusé même s'il égale la dernière valeur appliquée");
}

/* La reprise après un reboot du coffre, telle que KeSp l'a demandée : le
 * tampon partagé repart à zéro, le maître relit 0x12, le voit différent du mode
 * qu'il veut, le réécrit — et le coffre applique. */
static void test_reboot_recovery_is_a_plain_change_of_value(void)
{
    /* Après reboot : rien n'a été appliqué, et le maître veut PGP. */
    TEST_ASSERT_EQ(link_proto_mode_request(LINK_USB_MODE_PGP, LINK_USB_MODE_NONE),
                   LINK_MODE_REQ_APPLY, "0 -> pgp après reboot : appliqué");
    /* Et une fois appliqué, la même valeur relue à chaque cycle ne rebascule
     * rien : « au changement », pas « à chaque lecture ». */
    TEST_ASSERT_EQ(link_proto_mode_request(LINK_USB_MODE_PGP, LINK_USB_MODE_PGP),
                   LINK_MODE_REQ_UNCHANGED, "pgp relu vingt fois par seconde : rien");
}

/*
 * Les six valeurs de fil sont le contrat publié : elles sont recopiées ici
 * EXPRÈS, comme les vecteurs de la section 10, parce que c'est cette recopie
 * qui surveille la divergence entre le code et le document. Les propriétés
 * au-dessus disent ce que le code fait ; celle-ci dit ce que KeSp a dans les
 * mains.
 */
static void test_wire_values_are_the_published_contract(void)
{
    TEST_ASSERT_EQ(LINK_USB_MODE_NONE,    0x00, "0x00 none");
    TEST_ASSERT_EQ(LINK_USB_MODE_STORAGE, 0x01, "0x01 storage");
    TEST_ASSERT_EQ(LINK_USB_MODE_PGP,     0x02, "0x02 pgp");
    TEST_ASSERT_EQ(LINK_USB_MODE_OTP,     0x03, "0x03 otp");
    TEST_ASSERT_EQ(LINK_USB_MODE_FIDO,    0x04, "0x04 fido");
    TEST_ASSERT_EQ(LINK_USB_MODE_OATH,    0x05, "0x05 oath");

    unsigned known = 0;
    for (unsigned w = 0; w < 256; w++) {
        if (link_proto_usb_mode_is_known((uint8_t)w)) {
            known++;
        }
    }
    TEST_ASSERT_EQ(known, LINK_USB_MODE_COUNT,
                   "six valeurs attribuées, et pas une de plus");
}

/* ------------------------------------------------------------------------ */
/* Mode USB ACTIF (0x0D) — v2                                                */
/* ------------------------------------------------------------------------ */

/*
 * POURQUOI CET OCTET EXISTE. Jusqu'ici le protocole ne publiait aucune relecture
 * du mode : LINK_STATE_USB_MOUNTED dit « quelque chose est monté », jamais QUOI.
 * Le clavier ne pouvait donc afficher que le mode qu'il avait DEMANDÉ — les deux
 * sont indiscernables tant que tout va bien, et divergent exactement quand ça ne
 * va pas : une bascule qui échoue et se retente. C'est le symptôme que la
 * propriétaire a rencontré côté coffre (« l'écran affiche sd mais rien »), et il
 * serait arrivé côté clavier sans même une console pour le démentir.
 */

static void test_active_mode_roundtrips(void)
{
    uint8_t regs[LINK_REG_SIZE];
    link_status_t out;

    /* Chaque valeur attribuée, plus l'indéterminé : c'est tout le domaine. */
    for (unsigned w = 0; w < LINK_USB_MODE_COUNT; w++) {
        const link_status_t in = { .version = LINK_PROTO_VERSION,
                                   .state = LINK_STATE_READY,
                                   .pending_op = 0,
                                   .confirm_count = 0,
                                   .instance = 0,
                                   .usb_mode_active = (uint8_t)w };
        memset(regs, 0, sizeof(regs));
        link_proto_pack_status(regs, &in);
        TEST_ASSERT_EQ(regs[LINK_REG_USB_MODE_ACTIVE], w, "octet publié en 0x0D");
        memset(&out, 0, sizeof(out));
        TEST_ASSERT(link_proto_parse_status(regs, sizeof(regs), &out), "bloc valide");
        TEST_ASSERT_EQ(out.usb_mode_active, w, "mode actif relu");
    }

    const link_status_t unk = { .version = LINK_PROTO_VERSION,
                                .state = 0,
                                .pending_op = 0,
                                .confirm_count = 0,
                                .instance = 0,
                                .usb_mode_active = LINK_USB_MODE_UNKNOWN };
    memset(regs, 0, sizeof(regs));
    link_proto_pack_status(regs, &unk);
    memset(&out, 0, sizeof(out));
    TEST_ASSERT(link_proto_parse_status(regs, sizeof(regs), &out), "indéterminé reste un bloc valide");
    TEST_ASSERT_EQ(out.usb_mode_active, LINK_USB_MODE_UNKNOWN, "indéterminé relu tel quel");
}

/*
 * L'indéterminé ne doit jamais pouvoir se lire comme un mode réel. Sans quoi un
 * maître qui comparerait bêtement 0x0D à ce qu'il a demandé finirait par tomber
 * sur une égalité pendant une bascule — et afficherait « arrivé » au milieu du
 * chemin.
 */
static void test_unknown_active_mode_is_not_a_mode(void)
{
    TEST_ASSERT(!link_proto_usb_mode_is_known(LINK_USB_MODE_UNKNOWN),
                "0xFF n'est pas une valeur de fil attribuée");
    TEST_ASSERT(LINK_USB_MODE_UNKNOWN >= LINK_USB_MODE_COUNT,
                "hors de la plage contiguë des modes");
}

/*
 * Même démonstration que pour l'instance, et pour la même raison : l'octet est
 * DANS l'étendue du CRC. Le modifier sans recalculer doit faire rejeter le bloc,
 * sinon un bit retourné sur le fil ferait afficher un mode que le coffre n'a
 * jamais eu — c'est-à-dire précisément le mensonge que cet octet existe pour
 * empêcher.
 */
static void test_active_mode_is_covered_by_the_crc(void)
{
    uint8_t regs[LINK_REG_SIZE];
    const link_status_t in = { .version = LINK_PROTO_VERSION,
                               .state = LINK_STATE_READY | LINK_STATE_USB_MOUNTED,
                               .pending_op = 0,
                               .confirm_count = 0,
                               .instance = 1,
                               .usb_mode_active = LINK_USB_MODE_PGP };
    memset(regs, 0, sizeof(regs));
    link_proto_pack_status(regs, &in);

    link_status_t out;
    TEST_ASSERT(link_proto_parse_status(regs, sizeof(regs), &out), "bloc intact accepté");

    TEST_ASSERT(LINK_REG_USB_MODE_ACTIVE < LINK_REG_CRC_SPAN,
                "0x0D tombe dans l'étendue couverte");

    regs[LINK_REG_USB_MODE_ACTIVE] = LINK_USB_MODE_OATH;   /* sans recalculer */
    TEST_ASSERT(!link_proto_parse_status(regs, sizeof(regs), &out),
                "mode actif modifié sans recalcul du CRC : rejeté");
}

/*
 * Deux modes actifs distincts ne doivent jamais produire le même CRC sur un
 * bloc par ailleurs identique — sinon l'octet serait couvert sur le papier et
 * pas dans les faits.
 */
static void test_two_active_modes_never_share_a_crc(void)
{
    uint16_t crcs[LINK_USB_MODE_COUNT];

    for (unsigned w = 0; w < LINK_USB_MODE_COUNT; w++) {
        uint8_t regs[LINK_REG_SIZE];
        const link_status_t in = { .version = LINK_PROTO_VERSION,
                                   .state = LINK_STATE_READY,
                                   .pending_op = 0x1234,
                                   .confirm_count = 7,
                                   .instance = 9,
                                   .usb_mode_active = (uint8_t)w };
        memset(regs, 0, sizeof(regs));
        link_proto_pack_status(regs, &in);
        crcs[w] = (uint16_t)regs[LINK_REG_CRC] | (uint16_t)((uint16_t)regs[LINK_REG_CRC + 1] << 8);
    }

    for (unsigned i = 0; i < LINK_USB_MODE_COUNT; i++) {
        for (unsigned j = i + 1; j < LINK_USB_MODE_COUNT; j++) {
            TEST_ASSERT(crcs[i] != crcs[j], "deux modes actifs, deux CRC");
        }
    }
}

/*
 * L'octet appartient au coffre, et il est le DERNIER avant le CRC : il n'entame
 * ni la plage du maître, ni le champ de contrôle. Cette vérification existe
 * parce que la carte a déjà bougé une fois entre les deux dépôts, et que la
 * divergence s'était vue à la relecture et non au rouge.
 */
static void test_active_mode_sits_where_the_contract_says(void)
{
    TEST_ASSERT_EQ(LINK_REG_USB_MODE_ACTIVE, 0x0D, "0x0D, comme publié");
    TEST_ASSERT_EQ(LINK_REG_USB_MODE_ACTIVE + 1, LINK_REG_CRC, "juste avant le CRC");
    TEST_ASSERT(LINK_REG_USB_MODE_ACTIVE < LINK_REG_MASTER_BASE,
                "côté coffre de la frontière");
}

/*
 * V14 — le seul état où « demandé » et « actif » diffèrent sans que rien ne soit
 * cassé. Il mérite son vecteur parce que c'est celui que le maître risque de
 * traiter comme un refus : sans lui, une implémentation qui afficherait une
 * erreur dès que 0x0D ne vaut pas 0x12 passerait tous les autres vecteurs.
 */
static void test_shared_vector_switch_in_progress(void)
{
    link_status_t out;
    memset(&out, 0, sizeof(out));
    TEST_ASSERT(link_proto_parse_status(k_vec_v14, LINK_REG_SIZE, &out), "V14 accepté");
    TEST_ASSERT_EQ(out.usb_mode_active, LINK_USB_MODE_UNKNOWN, "V14 mode actif indéterminé");
    TEST_ASSERT(!(out.state & LINK_STATE_USB_MOUNTED),
                "V14 rien n'est monté pendant la bascule");
    TEST_ASSERT(!link_proto_usb_mode_is_known(out.usb_mode_active),
                "V14 l'indéterminé ne peut pas se lire comme un mode");
}

/*
 * LA COHÉRENCE ENTRE LE BIT ET L'OCTET, sur tous les vecteurs publiés.
 *
 * « Monté » et « mode actif » décrivent le même fait vu de deux endroits ; un
 * bloc qui annoncerait USB_MOUNTED avec un mode actif `none` serait une
 * contradiction que le maître n'a aucun moyen d'arbitrer. C'est exactement ce
 * que V1 disait avant l'ajout de 0x0D, et rien ne l'aurait signalé : le vecteur
 * était valide, son CRC juste, et sa contradiction invisible.
 */
static void test_mounted_bit_and_active_mode_agree(void)
{
    static const uint8_t *const blocks[] = { k_vec_v1, k_vec_v8, k_vec_v9,
                                             k_vec_v11, k_vec_v12, k_vec_v13,
                                             k_vec_v14 };

    for (unsigned i = 0; i < sizeof(blocks) / sizeof(blocks[0]); i++) {
        link_status_t st;
        memset(&st, 0, sizeof(st));
        TEST_ASSERT(link_proto_parse_status(blocks[i], LINK_REG_SIZE, &st),
                    "vecteur de cohérence accepté");

        if (st.state & LINK_STATE_USB_MOUNTED) {
            TEST_ASSERT(link_proto_usb_mode_is_known(st.usb_mode_active),
                        "monté : le mode actif est une valeur attribuée");
            TEST_ASSERT(st.usb_mode_active != LINK_USB_MODE_NONE,
                        "monté : le mode actif n'est pas « aucun »");
        } else {
            TEST_ASSERT(st.usb_mode_active == LINK_USB_MODE_NONE
                        || st.usb_mode_active == LINK_USB_MODE_UNKNOWN,
                        "pas monté : aucun mode, ou bascule en cours");
        }
    }
}

/* ------------------------------------------------------------------------ */

void test_link_proto(void)
{
    TEST_SUITE("link_proto");
    TEST_RUN(test_pack_parse_roundtrip);
    TEST_RUN(test_confirm_count_full_range);
    TEST_RUN(test_state_bits_independent);
    TEST_RUN(test_reject_bad_magic);
    TEST_RUN(test_reject_bad_version);
    TEST_RUN(test_reject_corrupted_payload);
    TEST_RUN(test_master_range_outside_crc);
    TEST_RUN(test_pack_leaves_master_range_untouched);
    TEST_RUN(test_every_byte_has_exactly_one_owner);
    TEST_RUN(test_no_word_straddles_the_two_owners);
    TEST_RUN(test_named_fields_fall_on_their_owner_side);
    TEST_RUN(test_absent_all_zero);
    TEST_RUN(test_absent_all_ones);
    TEST_RUN(test_present_not_absent);
    TEST_RUN(test_almost_uniform_is_present);
    TEST_RUN(test_reject_short_buffer);
    TEST_RUN(test_shared_vectors_accepted);
    TEST_RUN(test_shared_vectors_master_side);
    TEST_RUN(test_shared_vectors_rejected);
    TEST_RUN(test_shared_vectors_are_what_the_chest_publishes);
    TEST_RUN(test_crc_variant_check_value);
    TEST_RUN(test_confirm_needs_the_magic_AND_the_instance);
    TEST_RUN(test_stale_echo_from_a_same_coded_operation_is_refused);
    TEST_RUN(test_instance_does_not_replace_the_magic_filter);
    TEST_RUN(test_instance_is_covered_by_the_crc);
    TEST_RUN(test_two_instances_never_share_a_crc);
    TEST_RUN(test_parse_master_reads_the_three_bytes);
    TEST_RUN(test_no_unknown_wire_value_ever_applies);
    TEST_RUN(test_unknown_is_refused_even_if_it_matches);
    TEST_RUN(test_reboot_recovery_is_a_plain_change_of_value);
    TEST_RUN(test_wire_values_are_the_published_contract);
    TEST_RUN(test_active_mode_roundtrips);
    TEST_RUN(test_unknown_active_mode_is_not_a_mode);
    TEST_RUN(test_active_mode_is_covered_by_the_crc);
    TEST_RUN(test_two_active_modes_never_share_a_crc);
    TEST_RUN(test_active_mode_sits_where_the_contract_says);
    TEST_RUN(test_shared_vector_switch_in_progress);
    TEST_RUN(test_mounted_bit_and_active_mode_agree);
}
