/* main/security/ctap2.c — reponse CTAP2 authenticatorGetInfo, en logique
 * pure. Voir ctap2.h pour le contrat.
 */
#include "ctap2.h"

#include "cbor_enc.h"
#include "ctaphid.h"

/*
 * AAGUID fixe du MODELE d'authentificateur « Cle FIDO2 Niphar » — genere
 * une seule fois par `uuidgen` le 2026-08-18 (76365535-e558-4f54-b32d-
 * 5fc79426a628), puis committe en dur ici.
 *
 * Un AAGUID identifie un MODELE, pas un exemplaire : TOUTES les cartes
 * Niphar_chest, quel que soit leur numero de serie ou leur MAC, DOIVENT
 * renvoyer exactement cette meme valeur. Un AAGUID unique par carte serait
 * un identifiant de tracage que chaque relying party pourrait correler
 * entre plusieurs enregistrements de la meme personne sur des services
 * differents — precisement ce que WebAuthn/CTAP2 cherche a rendre
 * difficile. Ne JAMAIS deriver cette valeur d'un identifiant materiel
 * (numero de serie, adresse MAC, etc.), et ne jamais la regenerer par
 * carte : elle doit rester identique a travers toute la production.
 */
static const uint8_t s_aaguid[16] = {
    0x76, 0x36, 0x55, 0x35, 0xe5, 0x58, 0x4f, 0x54,
    0xb3, 0x2d, 0x5f, 0xc7, 0x94, 0x26, 0xa6, 0x28,
};

size_t ctap2_build_get_info(uint8_t *out, size_t cap)
{
    if (out == NULL || cap == 0u) {
        return 0;
    }

    out[0] = CTAP2_OK;

    cbor_w_t w;
    cbor_w_init(&w, out + 1, cap - 1u);

    /*
     * Ordre canonique des cles — RFC 8949 §4.2.1, exige par CTAP2 : pour un
     * type d'argument identique (ici, des entiers non signes tenant tous
     * sur un octet), l'ordre canonique EST l'ordre croissant de la valeur
     * numerique. cbor_enc.c ne voit jamais cette map dans son ensemble, il
     * ecrit seulement les paires dans l'ordre ou on les lui donne (voir
     * cbor_enc.h) : GARANTIR cet ordre est donc enterement a la charge de
     * CE fichier, pas de l'encodeur.
     *
     * Pour cette map precise, 1 (versions) < 3 (aaguid) < 4 (options) <
     * 5 (maxMsgSize) est deja croissant dans l'ordre "naturel" de lecture
     * de la specification CTAP2 — mais c'est une coincidence de cette
     * liste de champs precise, pas une propriete garantie par construction
     * : rien n'empeche un futur champ (extensions, pinUvAuthProtocols...)
     * de casser cet ordre si on l'ajoutait a la fin sans y penser. NE PAS
     * reordonner ces quatre lignes pour "regrouper logiquement" (par
     * exemple options juste apres versions) : ce serait syntaxiquement
     * valide et rendrait la reponse non conforme, refusee par tout client
     * qui verifie le canonique — echec silencieux, loin d'ici. Voir
     * test_ctap2.c:test_get_info_key_order_is_canonical, qui lit les
     * octets produits pour verifier cette propriete plutot que de faire
     * confiance a ce commentaire.
     */
    cbor_map(&w, 4);

    /* Cle 1 : versions supportees. U2F_V2 (CTAP1/U2F) et FIDO_2_0 (CTAP2) —
     * la carte annonce les deux, meme si seul authenticatorGetInfo est
     * cable a ce stade du plan (U2F/MSG suit dans une tache ulterieure). */
    cbor_uint(&w, 1);
    cbor_array(&w, 2);
    cbor_text(&w, "U2F_V2");
    cbor_text(&w, "FIDO_2_0");

    /* Cle 3 : aaguid, 16 octets fixes — voir le commentaire au-dessus de
     * s_aaguid. */
    cbor_uint(&w, 3);
    cbor_bytes(&w, s_aaguid, sizeof s_aaguid);

    /*
     * Cle 4 : options. Dans la sous-map, "rk" precede "up" : RFC 8949, deux
     * cles TEXTE de meme longueur d'encodage s'ordonnent par comparaison
     * d'octets, et 'r' (0x72) < 'u' (0x75) — la encore, l'ordre "alphabetique
     * naturel" coincide avec le canonique pour CES DEUX noms precis, par
     * chance et non par construction (ce ne serait plus vrai avec un futur
     * "uv" a inserer : 'u'=0x75 < ... il faudrait le placer AVANT "up" et
     * verifier a nouveau). clientPin et uv sont ABSENTS de cette sous-map :
     * la spec retenue pour cette carte est SANS PIN (voir CLAUDE.md,
     * contraintes-globales.md) ; une option absente vaut "non supportee"
     * pour un client CTAP2, ce qui est exactement vrai ici. Les annoncer a
     * `false` explicitement serait fonctionnellement equivalent mais
     * ajouterait des octets pour rien ; les annoncer a `true` — l'erreur
     * qu'on prend soin d'eviter ici — ferait echouer un client PLUS TARD,
     * au moment ou il tenterait reellement un flux avec PIN, pas plus tot.
     */
    cbor_uint(&w, 4);
    cbor_map(&w, 2);
    cbor_text(&w, "rk");
    cbor_bool(&w, true);
    cbor_text(&w, "up");
    cbor_bool(&w, true);

    /* Cle 5 : maxMsgSize — la taille maximale qu'un message CTAP-HID peut
     * atteindre sur ce transport (voir ctaphid.h). Annoncer une valeur
     * differente de ce que le transport sait reellement porter tromperait
     * un client sur ce qu'il peut envoyer en une seule requete. */
    cbor_uint(&w, 5);
    cbor_uint(&w, CTAPHID_MAX_PAYLOAD);

    if (w.overflow) {
        return 0;   /* jamais de reponse tronquee — meme discipline que cbor_enc.h */
    }

    return 1u + w.len;
}
