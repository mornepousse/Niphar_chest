#include "oath_proto.h"

#include <string.h>

uint32_t oath_dynamic_binary(const uint8_t *hmac, uint8_t hmac_len)
{
    if (hmac == NULL || hmac_len < 20) return 0;
    const uint8_t off = (uint8_t)(hmac[hmac_len - 1] & 0x0Fu);
    return ((uint32_t)(hmac[off] & 0x7Fu) << 24)
         | ((uint32_t)hmac[off + 1] << 16)
         | ((uint32_t)hmac[off + 2] << 8)
         | ((uint32_t)hmac[off + 3]);
}

bool oath_tlv_find(const uint8_t *buf, uint16_t len, uint8_t tag,
                   const uint8_t **val, uint16_t *val_len)
{
    if (buf == NULL) return false;
    uint16_t i = 0;
    /* Comparaison en 32 bits, comme la garde voisine : un cast (uint16_t)
     * ici retronquerait i+2 avant la comparaison, et quand i approche 65534
     * avec len=65535, (uint16_t)(65534+2)=(uint16_t)65536=0 rendrait la
     * garde vraie a tort — la boucle lirait alors un octet au-dela du
     * tampon logique avant que la garde interne ne le rattrape. */
    while ((uint32_t)i + 2u <= len) {
        const uint8_t t = buf[i];
        const uint8_t l = buf[i + 1];
        /* Longueur qui deborde : trame malformee, on s'arrete au lieu de lire
         * au-dela du tampon. */
        if ((uint32_t)i + 2u + l > len) return false;
        if (t == tag) {
            if (val)     *val = &buf[i + 2];
            if (val_len) *val_len = l;
            return true;
        }
        i = (uint16_t)(i + 2u + l);
    }
    return false;
}

uint16_t oath_tlv_put(uint8_t *out, uint16_t cap, uint8_t tag,
                      const uint8_t *val, uint16_t val_len)
{
    if (out == NULL || val_len > 0xFFu) return 0;
    if ((uint32_t)val_len + 2u > cap) return 0;
    out[0] = tag;
    out[1] = (uint8_t)val_len;
    if (val && val_len) memcpy(&out[2], val, val_len);
    return (uint16_t)(val_len + 2u);
}

/* ------------------------------------------------------------------------
 * Aiguillage des commandes YKOATH.
 *
 * TOUT ce qui suit lit des octets venus de l'hote. Chaque longueur — nom,
 * secret, defi — se borne avant d'etre utilisee : c'est la seule barriere
 * entre un `ykman` (ou n'importe quel programme parlant CCID) et le magasin.
 * ------------------------------------------------------------------------ */

/* Mots d'etat, une seule definition pour tout le fichier. */
#define SW_OK              0x9000u
#define SW_WRONG_DATA      0x6A80u
#define SW_NOT_SUPPORTED   0x6A81u
#define SW_NOT_FOUND       0x6A82u
#define SW_FULL            0x6A84u
#define SW_INS_UNKNOWN     0x6D00u

/* Taille maximale d'une tranche de reponse : au-dela, l'octet de longueur du
 * 61xx ne saurait plus l'exprimer. */
#define OATH_CHUNK_MAX 255u

/* Octet type|algo du 0x73 : quartet haut 0x10 = HOTP, 0x20 = TOTP. */
#define OATH_TYPE_MASK 0xF0u
#define OATH_TYPE_TOTP 0x20u

static uint16_t sw_only(uint8_t *out, uint16_t cap, uint16_t sw)
{
    if (cap < 2) return 0;
    out[0] = (uint8_t)(sw >> 8);
    out[1] = (uint8_t)(sw & 0xFFu);
    return 2;
}

/*
 * Rend la tranche suivante de `ctx->pending`. Tant qu'il reste des octets, le
 * mot d'etat est 61xx : c'est ce qui fait envoyer un SEND REMAINING a l'hote.
 * Douze comptes depassent 255 octets — omettre cette decoupe marcherait avec
 * trois comptes et casserait avec douze, c'est-a-dire juste apres la
 * migration des vrais secrets.
 */
static uint16_t oath_emit_chunk(uint8_t *out, uint16_t cap, oath_ctx_t *ctx)
{
    if (cap < 2) return 0;
    uint16_t reste = (uint16_t)(ctx->pending_len - ctx->pending_off);
    uint16_t part  = (reste > OATH_CHUNK_MAX) ? (uint16_t)OATH_CHUNK_MAX : reste;
    /* `cap` vient de la couche CCID, pas d'ici : la tranche s'y adapte plutot
     * que de deborder du tampon de l'appelant. */
    if ((uint32_t)part + 2u > cap) part = (uint16_t)(cap - 2u);

    if (part) memcpy(out, &ctx->pending[ctx->pending_off], part);
    ctx->pending_off = (uint16_t)(ctx->pending_off + part);
    reste = (uint16_t)(ctx->pending_len - ctx->pending_off);

    if (reste) {
        out[part] = 0x61;
        /* Un reste de plus de 255 s'annonce par 00 : « au moins 256 », la
         * convention ISO. */
        out[part + 1] = (reste > OATH_CHUNK_MAX) ? 0x00u : (uint8_t)reste;
    } else {
        out[part]     = 0x90;
        out[part + 1] = 0x00;
        ctx->pending_len = 0;
        ctx->pending_off = 0;
    }
    return (uint16_t)(part + 2u);
}

/* Prepare `ctx->pending` puis en rend la premiere tranche. */
static uint16_t oath_reply_long(uint8_t *out, uint16_t cap, oath_ctx_t *ctx,
                                uint16_t len)
{
    ctx->pending_len = len;
    ctx->pending_off = 0;
    return oath_emit_chunk(out, cap, ctx);
}

/*
 * Cherche le slot dont l'etiquette vaut exactement `name`/`name_len`.
 * Comparaison sur la longueur EXACTE : `strncmp` seul ferait qu'un nom hote
 * « A » ouvrirait le compte « A:b ». Rend SEC_N_SLOTS si rien ne correspond.
 */
static uint8_t oath_find_slot(const uint8_t *name, uint16_t name_len)
{
    if (name == NULL || name_len == 0 || name_len >= SEC_LABEL_LEN)
        return SEC_N_SLOTS;
    for (uint8_t i = 0; i < SEC_N_SLOTS; i++) {
        const char *lab = sec_store_label(i);
        if (lab == NULL) continue;
        if (strlen(lab) != name_len) continue;
        if (memcmp(lab, name, name_len) == 0) return i;
    }
    return SEC_N_SLOTS;
}

/*
 * Recopie un nom hote en chaine C bornee. Refuse l'octet nul en son sein : il
 * ferait taire la fin du nom sans que rien ne le signale, et deux comptes
 * distincts pour l'hote deviendraient le meme dans le magasin.
 */
static bool oath_name_copy(const uint8_t *name, uint16_t name_len, char *out)
{
    if (name == NULL || name_len == 0 || name_len >= SEC_LABEL_LEN) return false;
    if (memchr(name, '\0', name_len) != NULL) return false;
    memcpy(out, name, name_len);
    out[name_len] = '\0';
    return true;
}

/*
 * SELECT. Le 0x79 est obligatoire : ykman fait data[TAG_VERSION] sans garde et
 * leve une exception s'il manque. Le 0x74 doit rester ABSENT — c'est son
 * absence qui signale « pas de mot de passe ».
 */
static uint16_t oath_do_select(uint8_t *out, uint16_t cap, oath_ctx_t *ctx)
{
    /* Version annoncee 5.7.1 : ykman refuse certaines commandes en dessous de
     * 5.x, et se declarer plus recent que ce qu'on implemente lui ferait
     * essayer des commandes absentes. */
    static const uint8_t version[3] = { 0x05, 0x07, 0x01 };
    uint8_t body[2 + sizeof(version) + 2 + OATH_SALT_LEN];
    uint16_t n = 0;

    n = (uint16_t)(n + oath_tlv_put(&body[n], (uint16_t)(sizeof(body) - n),
                                    OATH_TAG_VERSION, version, sizeof(version)));
    n = (uint16_t)(n + oath_tlv_put(&body[n], (uint16_t)(sizeof(body) - n),
                                    OATH_TAG_NAME, ctx->salt, OATH_SALT_LEN));

    if ((uint32_t)n + 2u > cap) return sw_only(out, cap, SW_FULL);
    memcpy(out, body, n);
    /* La selection reussit avant l'ecriture du mot d'etat, pas apres : une
     * capacite trop petite est un probleme de transport, pas un refus de
     * l'applet — mais on ne veut pas non plus armer l'applet sur une reponse
     * que l'hote n'a pas recue. D'ou l'ordre : bornes d'abord, etat ensuite. */
    ctx->selected    = true;
    ctx->pending_len = 0;
    ctx->pending_off = 0;
    out[n]     = 0x90;
    out[n + 1] = 0x00;
    return (uint16_t)(n + 2u);
}

/* SEND REMAINING. Sans reste en attente, il n'y a rien a reprendre. */
static uint16_t oath_do_send_remaining(uint8_t *out, uint16_t cap, oath_ctx_t *ctx)
{
    if (ctx->pending_len == 0 || ctx->pending_off >= ctx->pending_len)
        return sw_only(out, cap, SW_NOT_FOUND);
    return oath_emit_chunk(out, cap, ctx);
}

/*
 * LIST. Une entree 0x72 par compte : l'octet type|algo, puis le nom.
 * (yubikit/oath.py, list_credentials.)
 */
static uint16_t oath_do_list(uint8_t *out, uint16_t cap, oath_ctx_t *ctx)
{
    uint16_t n = 0;
    for (uint8_t i = 0; i < SEC_N_SLOTS; i++) {
        const char *lab = sec_store_label(i);
        if (lab == NULL) continue;
        const size_t l = strlen(lab);
        uint8_t entry[1 + SEC_LABEL_LEN];
        entry[0] = sec_store_type(i);
        memcpy(&entry[1], lab, l);
        const uint16_t w = oath_tlv_put(&ctx->pending[n],
                                        (uint16_t)(OATH_PENDING_MAX - n),
                                        OATH_TAG_NAME_LIST, entry,
                                        (uint16_t)(l + 1u));
        /* Refuser plutot que tronquer : une liste amputee ferait croire a
         * l'hote qu'un compte a disparu. */
        if (w == 0) return sw_only(out, cap, SW_FULL);
        n = (uint16_t)(n + w);
    }
    return oath_reply_long(out, cap, ctx, n);
}

/*
 * CALCULATE ALL. Pour chaque compte : le nom (0x71) puis 0x7C — le marqueur
 * « appui requis », sans valeur. JAMAIS de code : en rendre un ici viderait la
 * clef de son unique controle physique, puisque l'hote appelle cette commande
 * de lui-meme des qu'il ouvre la liste.
 */
static uint16_t oath_do_calculate_all(uint8_t *out, uint16_t cap, oath_ctx_t *ctx)
{
    uint16_t n = 0;
    for (uint8_t i = 0; i < SEC_N_SLOTS; i++) {
        const char *lab = sec_store_label(i);
        if (lab == NULL) continue;
        const uint16_t wn = oath_tlv_put(&ctx->pending[n],
                                         (uint16_t)(OATH_PENDING_MAX - n),
                                         OATH_TAG_NAME, (const uint8_t *)lab,
                                         (uint16_t)strlen(lab));
        if (wn == 0) return sw_only(out, cap, SW_FULL);
        n = (uint16_t)(n + wn);
        const uint16_t wt = oath_tlv_put(&ctx->pending[n],
                                         (uint16_t)(OATH_PENDING_MAX - n),
                                         OATH_TAG_TOUCH, NULL, 0);
        if (wt == 0) return sw_only(out, cap, SW_FULL);
        n = (uint16_t)(n + wt);
    }
    return oath_reply_long(out, cap, ctx, n);
}

/*
 * CALCULATE. On valide TOUT avant de demander l'appui : faire clignoter la
 * clef pour une trame qu'on refusera ensuite apprendrait a Mae a confirmer
 * sans regarder.
 */
static uint16_t oath_do_calculate(const apdu_t *cmd, uint8_t *out, uint16_t cap,
                                  oath_ctx_t *ctx)
{
    const uint8_t *name = NULL, *chal = NULL;
    uint16_t name_len = 0, chal_len = 0;

    if (!oath_tlv_find(cmd->data, cmd->lc, OATH_TAG_NAME, &name, &name_len))
        return sw_only(out, cap, SW_WRONG_DATA);
    const uint8_t slot = oath_find_slot(name, name_len);
    if (slot >= SEC_N_SLOTS) return sw_only(out, cap, SW_NOT_FOUND);

    if (!oath_tlv_find(cmd->data, cmd->lc, OATH_TAG_CHALLENGE, &chal, &chal_len))
        return sw_only(out, cap, SW_WRONG_DATA);
    /* Exactement huit octets. Un defi plus court serait complete par de la
     * memoire non initialisee, un plus long tronque en silence : dans les deux
     * cas le code rendu ne correspondrait pas au pas de temps demande. */
    if (chal_len != OATH_CHALLENGE_LEN) return sw_only(out, cap, SW_WRONG_DATA);

    ctx->touch_slot     = slot;
    ctx->touch_truncate = (cmd->p2 == 0x01u);
    memcpy(ctx->touch_challenge, chal, OATH_CHALLENGE_LEN);
    return OATH_SW_NEEDS_TOUCH;
}

/*
 * PUT. Le 0x73 porte [type|algo][chiffres][secret] (oath.py,
 * put_credential). Le drapeau tactile n'est pas negociable : sec_store le
 * force a 1, et aucune propriete envoyee par l'hote ne peut l'abaisser.
 */
static uint16_t oath_do_put(const apdu_t *cmd, uint8_t *out, uint16_t cap)
{
    const uint8_t *name = NULL, *key = NULL;
    uint16_t name_len = 0, key_len = 0;
    char label[SEC_LABEL_LEN];

    if (!oath_tlv_find(cmd->data, cmd->lc, OATH_TAG_NAME, &name, &name_len))
        return sw_only(out, cap, SW_WRONG_DATA);
    if (!oath_name_copy(name, name_len, label))
        return sw_only(out, cap, SW_WRONG_DATA);

    if (!oath_tlv_find(cmd->data, cmd->lc, OATH_TAG_KEY, &key, &key_len))
        return sw_only(out, cap, SW_WRONG_DATA);
    /* Deux octets d'en-tete et au moins un octet de secret. */
    if (key_len < 3) return sw_only(out, cap, SW_WRONG_DATA);

    const uint8_t type   = key[0];
    const uint8_t digits = key[1];
    const uint16_t secret_len = (uint16_t)(key_len - 2u);

    /* HOTP demande un compteur persistant que rien ici ne tient : mieux vaut
     * le refuser franchement que rendre des codes qui ne s'incrementent pas. */
    if ((type & OATH_TYPE_MASK) != OATH_TYPE_TOTP)
        return sw_only(out, cap, SW_NOT_SUPPORTED);
    if (secret_len > SEC_SECRET_MAX) return sw_only(out, cap, SW_WRONG_DATA);
    if (digits != 6 && digits != 8)  return sw_only(out, cap, SW_WRONG_DATA);

    /* Un nom deja connu se remplace — c'est ce que fait ykman quand on
     * reprovisionne un compte — sinon on prend le premier slot libre. */
    uint8_t slot = oath_find_slot(name, name_len);
    if (slot >= SEC_N_SLOTS) {
        for (uint8_t i = 0; i < SEC_N_SLOTS; i++) {
            if (sec_store_type(i) == SEC_SLOT_EMPTY) { slot = i; break; }
        }
    }
    if (slot >= SEC_N_SLOTS) return sw_only(out, cap, SW_FULL);

    if (!sec_store_set_slot(slot, type, label, &key[2], (uint8_t)secret_len))
        return sw_only(out, cap, SW_WRONG_DATA);
    if (!sec_store_set_digits(slot, digits))
        return sw_only(out, cap, SW_WRONG_DATA);
    return sw_only(out, cap, SW_OK);
}

/* DELETE. Un nom inconnu se dit inconnu : rendre 9000 laisserait croire a une
 * suppression qui n'a pas eu lieu. */
static uint16_t oath_do_delete(const apdu_t *cmd, uint8_t *out, uint16_t cap)
{
    const uint8_t *name = NULL;
    uint16_t name_len = 0;
    if (!oath_tlv_find(cmd->data, cmd->lc, OATH_TAG_NAME, &name, &name_len))
        return sw_only(out, cap, SW_WRONG_DATA);
    const uint8_t slot = oath_find_slot(name, name_len);
    if (slot >= SEC_N_SLOTS) return sw_only(out, cap, SW_NOT_FOUND);
    if (!sec_store_clear_slot(slot)) return sw_only(out, cap, SW_WRONG_DATA);
    return sw_only(out, cap, SW_OK);
}

/*
 * RENAME. La trame porte DEUX 0x71 de suite (oath.py, rename_credential) :
 * l'ancien nom puis le nouveau. oath_tlv_find rend toujours le PREMIER — d'ou
 * la seconde recherche, faite sur ce qui suit la premiere valeur. Lire deux
 * fois le premier renommerait le compte en lui-meme, sans erreur visible.
 */
static uint16_t oath_do_rename(const apdu_t *cmd, uint8_t *out, uint16_t cap)
{
    const uint8_t *old = NULL, *neuf = NULL;
    uint16_t old_len = 0, neuf_len = 0;
    char label[SEC_LABEL_LEN];

    if (!oath_tlv_find(cmd->data, cmd->lc, OATH_TAG_NAME, &old, &old_len))
        return sw_only(out, cap, SW_WRONG_DATA);

    /* Decalage du premier octet suivant la valeur du premier 0x71. */
    const uint16_t apres = (uint16_t)((old - cmd->data) + old_len);
    if (apres >= cmd->lc) return sw_only(out, cap, SW_WRONG_DATA);
    if (!oath_tlv_find(&cmd->data[apres], (uint16_t)(cmd->lc - apres),
                       OATH_TAG_NAME, &neuf, &neuf_len))
        return sw_only(out, cap, SW_WRONG_DATA);
    if (!oath_name_copy(neuf, neuf_len, label))
        return sw_only(out, cap, SW_WRONG_DATA);

    const uint8_t slot = oath_find_slot(old, old_len);
    if (slot >= SEC_N_SLOTS) return sw_only(out, cap, SW_NOT_FOUND);

    /* Le secret se relit puis se reecrit : sec_store n'expose pas de
     * renommage, et l'ajouter pour ce seul appel elargirait sa surface. */
    uint8_t secret[SEC_SECRET_MAX];
    uint8_t secret_len = 0;
    if (!sec_store_get_secret(slot, secret, &secret_len))
        return sw_only(out, cap, SW_NOT_FOUND);
    const uint8_t type   = sec_store_type(slot);
    const uint8_t digits = sec_store_digits(slot);
    const bool ok = sec_store_set_slot(slot, type, label, secret, secret_len);
    /* Le secret ne doit pas survivre a cette pile : il n'a rien a faire en
     * memoire une fois recopie. */
    memset(secret, 0, sizeof(secret));
    if (!ok) return sw_only(out, cap, SW_WRONG_DATA);
    if (digits == 6 || digits == 8) sec_store_set_digits(slot, digits);
    return sw_only(out, cap, SW_OK);
}

/* RESET. Efface tout — c'est la commande qui remet la clef a neuf. */
static uint16_t oath_do_reset(uint8_t *out, uint16_t cap, oath_ctx_t *ctx)
{
    for (uint8_t i = 0; i < SEC_N_SLOTS; i++) sec_store_clear_slot(i);
    /* Un reste de reponse portant les comptes d'avant serait servi apres coup
     * par un SEND REMAINING : le jeter fait partie de l'effacement. */
    ctx->pending_len = 0;
    ctx->pending_off = 0;
    return sw_only(out, cap, SW_OK);
}

uint16_t oath_dispatch(const apdu_t *cmd, uint8_t *out, uint16_t cap,
                       oath_ctx_t *ctx)
{
    if (cmd == NULL || out == NULL || ctx == NULL) return 0;

    /* SELECT et CALCULATE ALL valent TOUS DEUX 0xA4 : c'est P1/P2 qui les
     * separe, jamais l'INS seul. Tester la selection en PREMIER, sinon une
     * demande de codes recevrait une reponse de SELECT. */
    if (cmd->ins == 0xA4u && cmd->p1 == 0x04u)
        return oath_do_select(out, cap, ctx);

    if (!ctx->selected)
        return sw_only(out, cap, SW_NOT_FOUND);

    switch (cmd->ins) {
    case 0xA5u: return oath_do_send_remaining(out, cap, ctx);
    case 0x03u: /* SET CODE */
    case 0xA3u: /* VALIDATE */
        /* Le mot de passe YKOATH protegerait des octets que l'appui physique
         * protege deja, et le SELECT annonce justement son absence. */
        return sw_only(out, cap, SW_NOT_SUPPORTED);
    case 0xA4u:
        /* P1 != 04 et P2 != 01 : un 0xA4 qui n'est ni l'un ni l'autre est une
         * trame que nous ne savons pas interpreter — la deviner serait pire
         * que la refuser. */
        if (cmd->p2 != 0x01u) return sw_only(out, cap, SW_WRONG_DATA);
        return oath_do_calculate_all(out, cap, ctx);
    case 0xA1u: return oath_do_list(out, cap, ctx);
    case 0xA2u: return oath_do_calculate(cmd, out, cap, ctx);
    case 0x01u: return oath_do_put(cmd, out, cap);
    case 0x02u: return oath_do_delete(cmd, out, cap);
    case 0x05u: return oath_do_rename(cmd, out, cap);
    case 0x04u: return oath_do_reset(out, cap, ctx);
    default:    return sw_only(out, cap, SW_INS_UNKNOWN);
    }
}
