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

/* V1 — nominal : SD + USB + prêt, PSO:CDS en attente, 42 confirmations. */
static const uint8_t k_vec_v1[LINK_REG_SIZE] = {
    0x4E, 0x49, 0x50, 0x48, 0x01, 0x07, 0x01, 0x00, 0x2A, 0x00,
    0x00, 0x00, 0xAF, 0xEA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
/* V4 — mot magique faux d'un octet, tout le reste identique à V1. */
static const uint8_t k_vec_v4[LINK_REG_SIZE] = {
    0x4E, 0x49, 0x50, 0x58, 0x01, 0x07, 0x01, 0x00, 0x2A, 0x00,
    0x00, 0x00, 0xAF, 0xEA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
/* V5 — version 2 annoncée, CRC RECALCULÉ et juste : refusé sur la version
 * seule, pas sur une corruption. */
static const uint8_t k_vec_v5[LINK_REG_SIZE] = {
    0x4E, 0x49, 0x50, 0x48, 0x02, 0x07, 0x01, 0x00, 0x2A, 0x00,
    0x00, 0x00, 0x7F, 0x60, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
/* V6 — un bit de la charge utile retourné (42 → 43), CRC laissé tel quel. */
static const uint8_t k_vec_v6[LINK_REG_SIZE] = {
    0x4E, 0x49, 0x50, 0x48, 0x01, 0x07, 0x01, 0x00, 0x2B, 0x00,
    0x00, 0x00, 0xAF, 0xEA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
/* V6b — l'inverse : charge utile intacte, un bit retourné DANS le champ CRC. */
static const uint8_t k_vec_v6b[LINK_REG_SIZE] = {
    0x4E, 0x49, 0x50, 0x48, 0x01, 0x07, 0x01, 0x00, 0x2A, 0x00,
    0x00, 0x00, 0xAE, 0xEA, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
/* V8 — V1 au seul octet du maître près : une confirmation posée et pas encore
 * lue. Le CRC est le MÊME qu'en V1 (0xEAAF), et c'est tout l'argument. */
static const uint8_t k_vec_v8[LINK_REG_SIZE] = {
    0x4E, 0x49, 0x50, 0x48, 0x01, 0x07, 0x01, 0x00, 0x2A, 0x00,
    0x00, 0x00, 0xAF, 0xEA, 0x00, 0x00, 0x5A, 0x00, 0x00, 0x00,
};
/* V9 — coffre présent et PAS prêt : aucun bit d'état, rien en attente. Son CRC
 * non nul est ce qui le distingue d'un bloc absent. */
static const uint8_t k_vec_v9[LINK_REG_SIZE] = {
    0x4E, 0x49, 0x50, 0x48, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x61, 0x7A, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

static void test_shared_vectors_accepted(void)
{
    link_status_t out;

    memset(&out, 0, sizeof(out));
    TEST_ASSERT(link_proto_parse_status(k_vec_v1, LINK_REG_SIZE, &out), "V1 accepté");
    TEST_ASSERT_EQ(out.version, 1, "V1 version");
    TEST_ASSERT_EQ(out.state, 0x07, "V1 état");
    TEST_ASSERT_EQ(out.pending_op, 1, "V1 opération en attente");
    TEST_ASSERT_EQ(out.confirm_count, 42, "V1 compteur");
    TEST_ASSERT(!link_proto_is_absent(k_vec_v1, LINK_REG_SIZE), "V1 pas absent");

    /* V8 ne diffère de V1 que par l'octet du maître — donc même verdict ET
     * mêmes champs décodés. C'est l'étendue du CRC rendue visible. */
    memset(&out, 0, sizeof(out));
    TEST_ASSERT(link_proto_parse_status(k_vec_v8, LINK_REG_SIZE, &out), "V8 accepté");
    TEST_ASSERT_EQ(out.state, 0x07, "V8 état identique à V1");
    TEST_ASSERT_EQ(out.pending_op, 1, "V8 opération identique à V1");
    TEST_ASSERT_EQ(out.confirm_count, 42, "V8 compteur identique à V1");
    TEST_ASSERT_EQ(memcmp(k_vec_v1, k_vec_v8, LINK_REG_CRC + 2), 0,
                   "V1 et V8 partagent octets couverts par le CRC, CRC compris");

    memset(&out, 0, sizeof(out));
    TEST_ASSERT(link_proto_parse_status(k_vec_v9, LINK_REG_SIZE, &out), "V9 accepté");
    TEST_ASSERT_EQ(out.state, 0x00, "V9 aucun bit d'état");
    TEST_ASSERT_EQ(out.pending_op, 0, "V9 rien en attente");
    TEST_ASSERT_EQ(out.confirm_count, 0, "V9 compteur nul");
    TEST_ASSERT(!link_proto_is_absent(k_vec_v9, LINK_REG_SIZE),
                "V9 présent et non prêt, pas absent");
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
    TEST_ASSERT(!link_proto_parse_status(k_vec_v6b, LINK_REG_SIZE, &out), "V6b champ CRC corrompu rejeté");

    /* V7 — les 19 premiers octets de V1, annoncés pour ce qu'ils sont. */
    TEST_ASSERT(!link_proto_parse_status(k_vec_v1, LINK_REG_SIZE - 1, &out), "V7 tronqué rejeté");

    /* V4, V5, V6, V6b ne sont pas des blocs absents : leur rejet vient bien du
     * contrôle annoncé et pas d'une ligne flottante. */
    TEST_ASSERT(!link_proto_is_absent(k_vec_v4, LINK_REG_SIZE), "V4 pas un bloc absent");
    TEST_ASSERT(!link_proto_is_absent(k_vec_v5, LINK_REG_SIZE), "V5 pas un bloc absent");
    TEST_ASSERT(!link_proto_is_absent(k_vec_v6, LINK_REG_SIZE), "V6 pas un bloc absent");
    TEST_ASSERT(!link_proto_is_absent(k_vec_v6b, LINK_REG_SIZE), "V6b pas un bloc absent");
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
    };
    memset(regs, 0, sizeof(regs));
    link_proto_pack_status(regs, &nominal);
    TEST_ASSERT_EQ(memcmp(regs, k_vec_v1, LINK_REG_SIZE), 0,
                   "le coffre publie exactement V1");

    const link_status_t booting = { .state = 0, .pending_op = 0, .confirm_count = 0 };
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
    TEST_RUN(test_shared_vectors_rejected);
    TEST_RUN(test_shared_vectors_are_what_the_chest_publishes);
    TEST_RUN(test_crc_variant_check_value);
}
