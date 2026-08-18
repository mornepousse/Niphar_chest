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

    /*
     * Cle 1 : versions supportees. SEUL "U2F_V2" est annonce.
     *
     * Ruling du coordinateur (2026-08-18, revue de cette meme tache) : la
     * reponse doit dire ce que la cle saura faire A LA FIN DE CE PLAN, rien
     * de plus. "FIDO_2_0" impliquerait un support CTAP2 complet — au moins
     * une commande de creance (authenticatorMakeCredential,
     * authenticatorGetAssertion) — or AUCUNE des deux n'est cablee, ni ne
     * le sera dans ce plan : le decodeur CBOR general est explicitement
     * reporte au plan 2 (contraintes-globales.md, ecart assume 2). Annoncer
     * "FIDO_2_0" inviterait un navigateur a tenter une passkey CTAP2, qui
     * echouerait tard et de façon peu informative (CTAP2_ERR_INVALID_
     * COMMAND depuis mode_fido.c). "U2F_V2" restera vrai a la fin de la
     * branche : la tache 7 cable CTAPHID_MSG et U2F (REGISTER/AUTHENTICATE/
     * VERSION), et la porte de presence existe deja et fonctionne.
     */
    cbor_uint(&w, 1);
    cbor_array(&w, 1);
    cbor_text(&w, "U2F_V2");

    /* Cle 3 : aaguid, 16 octets fixes — voir le commentaire au-dessus de
     * s_aaguid. */
    cbor_uint(&w, 3);
    cbor_bytes(&w, s_aaguid, sizeof s_aaguid);

    /*
     * Cle 4 : options. SEUL "up" (user presence) est annonce.
     *
     * Meme ruling que pour "FIDO_2_0" ci-dessus : "rk" (resident keys,
     * identifiants residents) suppose un magasin de creances — il n'en
     * existe aucun dans ce plan, la gestion des creances residentes etant
     * elle aussi reportee au plan 2 avec le decodeur CBOR general. Annoncer
     * "rk": true ferait echouer plus tard, obscurement, un client qui
     * tenterait un enregistrement resident. "up" reste annonce et VRAI :
     * la porte de presence existe deja et fonctionne independamment du
     * decodeur CBOR — voir sec_gate.{c,h}.
     *
     * clientPin et uv restent ABSENTS de cette sous-map (inchangé) : la
     * spec retenue pour cette carte est SANS PIN (voir CLAUDE.md,
     * contraintes-globales.md) ; une option absente vaut "non supportee"
     * pour un client CTAP2, ce qui est exactement vrai ici. Les annoncer a
     * `false` explicitement serait fonctionnellement equivalent mais
     * ajouterait des octets pour rien ; les annoncer a `true` — l'erreur
     * qu'on prend soin d'eviter ici — ferait echouer un client PLUS TARD,
     * au moment ou il tenterait reellement un flux avec PIN, pas plus tot.
     *
     * N'y ayant plus qu'UNE seule cle dans cette sous-map, la question de
     * l'ordre canonique entre "rk" et "up" ne se pose plus — a reconsiderer
     * si "rk" ou "uv" revient un jour (voir le commentaire au sommet de ce
     * fichier sur la garantie d'ordre a la charge de cet appelant).
     */
    cbor_uint(&w, 4);
    cbor_map(&w, 1);
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
