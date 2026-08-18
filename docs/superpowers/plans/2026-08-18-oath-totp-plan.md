# Applet OATH/TOTP — plan d'implémentation

> **Pour les agents :** SOUS-COMPÉTENCE REQUISE — utiliser
> `superpowers:subagent-driven-development` (recommandé) ou
> `superpowers:executing-plans` pour exécuter ce plan tâche par tâche.
> Les étapes utilisent la syntaxe case à cocher (`- [ ]`).

**But :** doter la clé Niphar d'un applet OATH parlant le protocole YKOATH sur
CCID, pour que `ykman oath accounts code <compte>` rende un code TOTP après un
appui physique.

**Architecture :** un sixième mode USB (`usb mode oath`) expose une interface
CCID portant un seul applet. Le gros du travail est de la logique pure —
analyse TLV, troncature RFC 4226, assainissement du nom, magasin de secrets —
donc testable sur l'hôte. La plomberie CCID et l'attente de l'appui réutilisent
`ccid.c`, qui émet déjà les trames d'extension de temps nécessaires.

**Pile :** ESP-IDF 5.5.2, mbedtls (SHA-1/SHA-256), harnais de tests hôte CMake.

**Spec :** [`docs/superpowers/specs/2026-08-18-oath-totp-design.md`](../specs/2026-08-18-oath-totp-design.md)

## Contraintes globales

Elles s'appliquent à **chaque** tâche, sans être répétées ensuite.

- **Ne jamais réaffecter GPIO24/25** (USB-Serial-JTAG : seul chemin de flash).
- **Ne jamais entrer en deep-sleep permanent.**
- `usb_mode_set` et `sec_gate_console_confirm` n'apparaissent que dans
  `usb_mode.{c,h}`, `console.c`, `sec_gate.{c,h}` — **commentaires compris**
  (garde-fou 4 de `scripts/fast.sh` est un `grep`).
- Commentaires en **français**. Ils disent *pourquoi*, pas *quoi*.
- Seule la logique pure entre dans `test/` : aucun appel ESP-IDF, sinon la
  compilation hôte casse.
- Chaque tâche finit sur `./scripts/check.sh --fast --force` **vert**, constaté.
- **Norme TDD** : test écrit d'abord, **rouge constaté**, vert après. Puis
  **mutation obligatoire** — introduire un bug qui devrait faire échouer le
  test, vérifier le rouge, revenir. Un test qui ne mord pas est une décoration.
- **Le harnais de mutation doit vérifier que la compilation a réussi** avant de
  conclure : sinon l'ancien binaire tourne et une mutation qui casse le build
  passe pour « ne mord pas » (documenté dans `b5284bb`).
- `.tripwire-testcount` est mis à jour à chaque tâche qui ajoute des assertions.
- Toutes les valeurs protocolaires viennent de `yubikit/oath.py` de **ykman
  5.9.1**. Ne pas les « corriger » de mémoire.

---

### Tâche 1 : `sec_store` v2 — 16 slots, noms longs

**Fichiers :**
- Modifier : `main/security/sec_store.h`
- Modifier : `main/security/sec_store.c`
- Modifier : `test/test_sec_store.c`

**Interfaces :**
- Produit : `SEC_N_SLOTS` = 16, `SEC_LABEL_LEN` = 64, `SEC_SECRET_MAX` = 64 ;
  `sec_slot_t` avec un champ `digits` ; `bool sec_store_set_slot(uint8_t idx,
  uint8_t type, const char *label, const uint8_t *secret, uint8_t secret_len)`
  (signature inchangée) ; nouveau `bool sec_store_set_digits(uint8_t idx,
  uint8_t digits)` et `uint8_t sec_store_digits(uint8_t idx)`.

Le magasin est **inerte** aujourd'hui : `sec_store_set_slot()` n'a aucun
appelant et `sec_store_init()` n'est jamais appelé. Élargir ne peut donc rien
perdre. Le blob NVS version 1 sera refusé sur sa taille au chargement.

- [ ] **Étape 1 : écrire le test qui échoue**

Dans `test/test_sec_store.c`, ajouter :

```c
/* Seize slots, pas quatre : douze comptes TOTP plus de la marge. Le test
 * compare la borne HAUTE valide et la première INVALIDE — vérifier seulement
 * que le slot 15 marche laisserait passer un magasin de 64 slots. */
static void test_seize_slots_et_pas_un_de_plus(void)
{
    const uint8_t key[4] = { 1, 2, 3, 4 };
    TEST_ASSERT(sec_store_set_slot(15, SEC_SLOT_HMAC_SHA1, "dernier", key, 4),
                "le slot 15 existe");
    TEST_ASSERT(!sec_store_set_slot(16, SEC_SLOT_HMAC_SHA1, "trop", key, 4),
                "le slot 16 n'existe pas");
}

/* Un nom YKOATH s'ecrit « Issuer:compte@domaine » : seize caracteres ne
 * suffisaient pas. Le test verifie qu'un nom long survit ENTIER, et qu'un nom
 * trop long est tronque avec son terminateur — pas qu'il deborde. */
static void test_nom_long_conserve(void)
{
    const uint8_t key[4] = { 1, 2, 3, 4 };
    const char *nom = "GitHub:mae.pugin@exemple-tres-long.org";
    TEST_ASSERT(sec_store_set_slot(0, SEC_SLOT_HMAC_SHA1, nom, key, 4),
                "un nom de 38 caracteres est accepte");
    TEST_ASSERT(strcmp(sec_store_label(0), nom) == 0,
                "le nom long est conserve entier");

    char trop_long[128];
    memset(trop_long, 'x', sizeof(trop_long));
    trop_long[sizeof(trop_long) - 1] = '\0';
    TEST_ASSERT(sec_store_set_slot(1, SEC_SLOT_HMAC_SHA1, trop_long, key, 4),
                "un nom trop long est accepte, tronque");
    TEST_ASSERT(strlen(sec_store_label(1)) == SEC_LABEL_LEN - 1,
                "tronque a SEC_LABEL_LEN-1, terminateur compris");
}

/* Le nombre de chiffres est porte par le slot : six ou huit selon le compte.
 * Compare deux slots ENTRE EUX — verifier qu'un slot rend 6 ne prouve pas que
 * la valeur est lue du slot plutot que d'une constante. */
static void test_digits_par_slot(void)
{
    const uint8_t key[4] = { 1, 2, 3, 4 };
    sec_store_set_slot(0, SEC_SLOT_HMAC_SHA1, "a", key, 4);
    sec_store_set_slot(1, SEC_SLOT_HMAC_SHA1, "b", key, 4);
    TEST_ASSERT(sec_store_set_digits(0, 6), "6 chiffres accepte");
    TEST_ASSERT(sec_store_set_digits(1, 8), "8 chiffres accepte");
    TEST_ASSERT(sec_store_digits(0) != sec_store_digits(1),
                "deux slots portent des valeurs distinctes");
    TEST_ASSERT_EQ(sec_store_digits(0), 6, "slot 0 rend bien 6");
    TEST_ASSERT_EQ(sec_store_digits(1), 8, "slot 1 rend bien 8");
    TEST_ASSERT(!sec_store_set_digits(0, 5), "5 chiffres refuse");
    TEST_ASSERT(!sec_store_set_digits(0, 9), "9 chiffres refuse");
}
```

Puis les enregistrer dans `test_sec_store()` :

```c
    TEST_RUN(test_seize_slots_et_pas_un_de_plus);
    TEST_RUN(test_nom_long_conserve);
    TEST_RUN(test_digits_par_slot);
```

- [ ] **Étape 2 : lancer le test et constater le rouge**

```bash
./scripts/check.sh --fast --force
```
Attendu : ROUGE — `sec_store_set_digits` n'existe pas (erreur de compilation).

- [ ] **Étape 3 : élargir la structure**

Dans `main/security/sec_store.h`, remplacer les trois constantes et la structure :

```c
/* Seize slots : douze comptes TOTP de Mae, plus de la marge. Pas davantage —
 * provisionner pour un besoin qui n'existe pas coute de la NVS et du temps de
 * chargement a chaque entree dans le mode. */
#define SEC_N_SLOTS     16
/* Un nom YKOATH s'ecrit « [periode/][Issuer:]compte » : soixante-quatre octets
 * couvrent « GitHub:mae.pugin@exemple.org » sans troncature. */
#define SEC_LABEL_LEN   64
#define SEC_SECRET_MAX  64

enum { SEC_SLOT_EMPTY = 0, SEC_SLOT_HMAC_SHA1 = 1 };

typedef struct {
    uint8_t type;        /* 0 vide | 0x01 CR-HMAC | octet d'algo YKOATH */
    uint8_t flags;       /* bit0 = appui requis — force a 1 */
    uint8_t secret_len;
    uint8_t digits;      /* 6 ou 8 ; 0 tant que non renseigne */
    char    label[SEC_LABEL_LEN];
    uint8_t secret[SEC_SECRET_MAX];
} sec_slot_t;
```

Ajouter les deux prototypes :

```c
/* Nombre de chiffres du code, porte par le slot. Refuse tout ce qui n'est ni 6
 * ni 8 : RFC 4226 en admet davantage, aucun service de Mae ne s'en sert, et
 * accepter une valeur qu'on ne teste pas revient a la livrer non verifiee. */
bool    sec_store_set_digits(uint8_t idx, uint8_t digits);
uint8_t sec_store_digits(uint8_t idx);
```

- [ ] **Étape 4 : implémenter**

Dans `main/security/sec_store.c`, ajouter :

```c
bool sec_store_set_digits(uint8_t idx, uint8_t digits)
{
    if (idx >= SEC_N_SLOTS) return false;
    if (digits != 6 && digits != 8) return false;
    if (s_slots[idx].type == SEC_SLOT_EMPTY) return false;
    s_slots[idx].digits = digits;
    return sec_store_persist();
}

uint8_t sec_store_digits(uint8_t idx)
{
    return (idx < SEC_N_SLOTS) ? s_slots[idx].digits : 0;
}
```

Et faire passer la version du blob de 1 à 2 dans les deux appels NVS :

```c
    esp_err_t err = nvs_save_blob_with_total(STORAGE_NAMESPACE, "sec_slots", s_slots,
                                             sizeof(s_slots), "sec_slots_ver", 2);
```

```c
    esp_err_t err = nvs_load_blob_with_total(STORAGE_NAMESPACE, "sec_slots", s_slots,
                             sizeof(s_slots), "sec_slots_ver", &ver);
```

- [ ] **Étape 5 : vérifier le vert**

```bash
./scripts/check.sh --fast --force
```
Attendu : VERT.

- [ ] **Étape 6 : mutation — prouver que les tests mordent**

Trois mutations, chacune suivie d'un `git diff` confirmant le retour à l'état
initial. **Vérifier que la compilation réussit avant de conclure quoi que ce
soit** : une mutation qui casse le build fait tourner l'ancien binaire.

| mutation | attendu |
|---|---|
| `SEC_N_SLOTS` à 17 | `test_seize_slots_et_pas_un_de_plus` rouge |
| `sec_store_set_digits` accepte 5 | `test_digits_par_slot` rouge |
| `sec_store_digits` rend toujours 6 | `test_digits_par_slot` rouge sur la comparaison des deux slots |

La troisième est la plus importante : c'est celle qui prouve que la comparaison
croisée sert à quelque chose.

- [ ] **Étape 7 : commit**

```bash
grep -rho TEST_ASSERT test/ --include=test_*.c | wc -l > .tripwire-testcount
git add main/security/sec_store.h main/security/sec_store.c test/test_sec_store.c .tripwire-testcount
git commit -m "oath : sec_store v2 — seize slots, noms de 64 octets, chiffres par slot"
```

---

### Tâche 2 : nom affichable — assainissement et police honnête

**Fichiers :**
- Créer : `main/security/oath_name.h`, `main/security/oath_name.c`
- Créer : `test/test_oath_name.c`
- Modifier : `test/CMakeLists.txt`, `test/test_main.c`
- Modifier : `main/hmi/screen.c` (fonction `glyph_for()`)

**Interfaces :**
- Produit : `void oath_name_display(const char *raw, uint16_t raw_len, char *out,
  uint8_t out_sz)` — `out` reçoit une chaîne terminée par `\0`, toujours écrite
  même si `raw` est vide ou absurde.
- Constante : `OATH_NAME_DISPLAY_MAX` = 12 (dix caractères visibles, un marqueur
  de troncature, un terminateur).

Deux responsabilités **séparées** : `oath_name` décide *quoi* montrer,
`screen.c` garantit que *tout* ce qu'on lui donne se voit. La seconde moitié
corrige un défaut réel de la police — un caractère sans glyphe rend aujourd'hui
un blanc, indiscernable d'une espace.

- [ ] **Étape 1 : écrire le test qui échoue**

Créer `test/test_oath_name.c` :

```c
#include "test_framework.h"
#include "security/oath_name.h"

/* Le nom YKOATH s'ecrit « [periode/][Issuer:]compte ». Ce que Mae reconnait
 * d'un coup d'oeil est l'issuer, pas l'adresse. */
static void test_issuer_extrait(void)
{
    char out[OATH_NAME_DISPLAY_MAX];
    const char *n = "GitHub:mae@exemple.org";
    oath_name_display(n, (uint16_t)strlen(n), out, sizeof(out));
    TEST_ASSERT(strcmp(out, "GITHUB") == 0, "l'issuer seul, en majuscules");

    const char *p = "30/GitLab:mae";
    oath_name_display(p, (uint16_t)strlen(p), out, sizeof(out));
    TEST_ASSERT(strcmp(out, "GITLAB") == 0, "le prefixe de periode est retire");
}

/*
 * LE test de ce fichier. Deux noms DIFFERENTS doivent rester DISTINGUABLES a
 * l'ecran : c'est toute la raison d'afficher le nom. Verifier separement que
 * « a » rend « A » ne prouve rien la-dessus — un assainissement qui rendrait
 * la meme chose pour tout passerait.
 */
static void test_noms_distincts_restent_distincts(void)
{
    const char *paires[][2] = {
        { "GitHub:mae",        "GitHub mae"  },   /* ponctuation vs espace */
        { "Banque",            "Ban\x01que"  },   /* caractere de controle */
        { "MonServiceTresLong1", "MonServiceTresLong2" }, /* divergence tardive */
    };
    for (unsigned i = 0; i < sizeof(paires) / sizeof(paires[0]); i++) {
        char a[OATH_NAME_DISPLAY_MAX], b[OATH_NAME_DISPLAY_MAX];
        oath_name_display(paires[i][0], (uint16_t)strlen(paires[i][0]), a, sizeof(a));
        oath_name_display(paires[i][1], (uint16_t)strlen(paires[i][1]), b, sizeof(b));
        TEST_ASSERT(strcmp(a, b) != 0,
                    "deux noms differents ne doivent pas s'afficher pareil");
    }
}

/* Un caractere sans glyphe devient « ? », jamais un blanc : un nom bricole doit
 * se VOIR, pas se deguiser en nom propre. */
static void test_caractere_indessinable_visible(void)
{
    char out[OATH_NAME_DISPLAY_MAX];
    const char *n = "Ban\x01que";
    oath_name_display(n, (uint16_t)strlen(n), out, sizeof(out));
    TEST_ASSERT(strchr(out, '?') != NULL, "le caractere de controle laisse une trace");
    TEST_ASSERT(strchr(out, '\x01') == NULL, "l'octet brut ne passe pas");
}

/* Troncature marquee : sans marqueur, deux noms qui divergent au-dela du
 * dixieme caractere seraient indiscernables et l'appui redeviendrait aveugle. */
static void test_troncature_marquee(void)
{
    char out[OATH_NAME_DISPLAY_MAX];
    const char *n = "ServiceExtremementLong";
    oath_name_display(n, (uint16_t)strlen(n), out, sizeof(out));
    TEST_ASSERT(strlen(out) == 11, "dix caracteres plus le marqueur");
    TEST_ASSERT(out[10] == '?', "le marqueur de troncature termine la ligne");

    const char *court = "Court";
    oath_name_display(court, (uint16_t)strlen(court), out, sizeof(out));
    TEST_ASSERT(out[strlen(out) - 1] != '?', "un nom court ne porte pas de marqueur");
}

/* Entrees degenerees : la sortie est TOUJOURS une chaine valide. Une fonction
 * d'affichage qui laisse `out` non initialise fait dessiner de la pile. */
static void test_entrees_degenerees(void)
{
    char out[OATH_NAME_DISPLAY_MAX];
    oath_name_display("", 0, out, sizeof(out));
    TEST_ASSERT(out[0] == '\0', "nom vide -> chaine vide, pas de pile dessinee");
    oath_name_display(NULL, 0, out, sizeof(out));
    TEST_ASSERT(out[0] == '\0', "nom absent -> chaine vide");
    oath_name_display(":", 1, out, sizeof(out));
    TEST_ASSERT(out[0] == '\0', "issuer vide -> chaine vide");
}

void test_oath_name(void)
{
    TEST_SUITE("oath_name");
    TEST_RUN(test_issuer_extrait);
    TEST_RUN(test_noms_distincts_restent_distincts);
    TEST_RUN(test_caractere_indessinable_visible);
    TEST_RUN(test_troncature_marquee);
    TEST_RUN(test_entrees_degenerees);
}
```

Déclarer la suite dans `test/test_main.c` (près des autres `extern`) :

```c
extern void test_oath_name(void);
```
et l'appeler dans le corps, après `test_sec_store();` :
```c
    test_oath_name();
```

Ajouter à `test/CMakeLists.txt`, dans `add_executable(test_runner …)` :
```cmake
    test_oath_name.c
    ../main/security/oath_name.c
```

- [ ] **Étape 2 : lancer et constater le rouge**

```bash
./scripts/check.sh --fast --force
```
Attendu : ROUGE — `security/oath_name.h` introuvable.

- [ ] **Étape 3 : écrire l'en-tête**

Créer `main/security/oath_name.h` :

```c
/* main/security/oath_name.h — nom de compte OATH pret a dessiner (pur).
 *
 * Le nom vient de l'HOTE : jusqu'a 64 octets arbitraires, dessines sur l'ecran
 * dont Mae se sert pour decider si elle autorise un code. Il ne peut donc pas
 * aller a l'ecran tel quel — ni pour sa longueur, ni pour son contenu.
 */
#pragma once
#include <stdint.h>

/* Dix caracteres visibles (la largeur reelle en police double hauteur sur
 * 128 px), un marqueur de troncature, un terminateur. */
#define OATH_NAME_DISPLAY_MAX 12

/*
 * Ecrit dans `out` une version affichable de `raw` :
 *   1. l'issuer seul — « 30/GitHub:mae@x.org » -> « GITHUB » — parce que c'est
 *      ce que Mae reconnait, et parce qu'un seul compte par service rend le
 *      reste inutile ;
 *   2. tout caractere non imprimable devient « ? », JAMAIS un blanc : un nom
 *      bricole doit se voir plutot que se deguiser en nom propre ;
 *   3. troncature a dix caracteres avec un marqueur visible, sans quoi deux
 *      comptes divergeant au-dela du dixieme caractere seraient indiscernables.
 *
 * `out` est toujours une chaine terminee, meme si `raw` est NULL ou vide.
 */
void oath_name_display(const char *raw, uint16_t raw_len, char *out, uint8_t out_sz);
```

- [ ] **Étape 4 : implémenter**

Créer `main/security/oath_name.c` :

```c
#include "security/oath_name.h"

#include <string.h>

/* Majuscule ASCII sans <ctype.h> : toupper() depend de la locale, et une
 * locale turque transformerait « i » en « İ ». */
static char majuscule(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
}

/* Imprimable ASCII strict. Tout le reste — controle, UTF-8, octet haut —
 * devient « ? » : voir la regle 2 de l'en-tete. */
static bool imprimable(unsigned char c)
{
    return c >= 0x20 && c <= 0x7E;
}

void oath_name_display(const char *raw, uint16_t raw_len, char *out, uint8_t out_sz)
{
    if (out == NULL || out_sz == 0) return;
    out[0] = '\0';
    if (raw == NULL || raw_len == 0) return;

    /* Prefixe de periode : « 30/GitHub:mae ». Retire s'il est present ET suivi
     * d'un '/' — un nom qui commence par des chiffres sans '/' reste entier. */
    uint16_t i = 0;
    uint16_t chiffres = 0;
    while (chiffres < raw_len && raw[chiffres] >= '0' && raw[chiffres] <= '9') chiffres++;
    if (chiffres > 0 && chiffres < raw_len && raw[chiffres] == '/') i = (uint16_t)(chiffres + 1);

    /* Issuer = jusqu'au premier ':'. Absent -> tout le reste. */
    uint16_t fin = i;
    while (fin < raw_len && raw[fin] != ':') fin++;

    const uint8_t visibles = (uint8_t)(out_sz - 2);   /* place le marqueur et le \0 */
    uint8_t n = 0;
    for (; i < fin && n < visibles; i++, n++) {
        unsigned char c = (unsigned char)raw[i];
        out[n] = imprimable(c) ? majuscule((char)c) : '?';
    }
    if (i < fin) out[n++] = '?';   /* marqueur : il reste du nom non montre */
    out[n] = '\0';
}
```

Ajouter `#include <stdbool.h>` en tête de `oath_name.c` (utilisé par `imprimable`).

- [ ] **Étape 5 : vérifier le vert**

```bash
./scripts/check.sh --fast --force
```
Attendu : VERT.

- [ ] **Étape 6 : rendre la police honnête**

Dans `main/hmi/screen.c`, la fonction `glyph_for()` retombe aujourd'hui sur un
glyphe **vide** pour un caractère situé dans `[0x20, 'z']` mais non dessiné —
toute la ponctuation. Un blanc est indiscernable d'une espace, ce qui recrée à
l'écran exactement l'ambiguïté que `oath_name` vient d'éliminer.

Modifier `glyph_for()` pour qu'un caractère sans glyphe, autre que l'espace,
retombe sur `'?'` :

```c
/* Un caractere DANS l'intervalle mais non dessine (toute la ponctuation ASCII)
 * retombait sur un glyphe vide, indiscernable d'une espace : « GitHub:mae » et
 * « GitHub mae » s'affichaient a l'identique. Sur un ecran qui sert a decider
 * d'autoriser une operation, un caractere qu'on ne sait pas dessiner doit se
 * VOIR. L'espace garde son glyphe vide, qui est le bon. */
```

```c
static const screen_glyph_t *glyph_for(char c)
{
    static const screen_glyph_t vide = {{ 0, 0, 0, 0, 0, 0, 0 }};
    if (c < ' ' || c > 'z') return &s_font['?' - ' '];
    const screen_glyph_t *g = &s_font[c - ' '];
    /* Un caractere DANS l'intervalle mais non dessine (toute la ponctuation
     * ASCII) retombait sur un glyphe vide, indiscernable d'une espace :
     * « GitHub:mae » et « GitHub mae » s'affichaient a l'identique. Sur un
     * ecran qui sert a decider d'autoriser une operation, un caractere qu'on
     * ne sait pas dessiner doit se VOIR. L'espace garde son glyphe vide, qui
     * est le bon. */
    if (c != ' ' && memcmp(g, &vide, sizeof(vide)) == 0)
        return &s_font['?' - ' '];
    return g;
}
```

Adapter au nom et à la forme réels de la fonction existante dans `screen.c` —
le point qui compte est le repli sur `'?'`, pas la signature.

- [ ] **Étape 7 : mutations**

| mutation | attendu |
|---|---|
| `imprimable()` accepte tout | `test_caractere_indessinable_visible` rouge, et la première paire de `test_noms_distincts_restent_distincts` |
| marqueur de troncature retiré | `test_troncature_marquee` rouge, et la troisième paire |
| `oath_name_display` n'écrit pas `out[0]='\0'` en tête | `test_entrees_degenerees` rouge |

- [ ] **Étape 8 : commit**

```bash
grep -rho TEST_ASSERT test/ --include=test_*.c | wc -l > .tripwire-testcount
git add main/security/oath_name.h main/security/oath_name.c main/hmi/screen.c \
        test/test_oath_name.c test/test_main.c test/CMakeLists.txt .tripwire-testcount
git commit -m "oath : nom de compte affichable, et une police qui ne deguise plus"
```

---

### Tâche 3 : troncature RFC 4226 et TLV

**Fichiers :**
- Créer : `main/security/oath_proto.h`, `main/security/oath_proto.c`
- Créer : `test/test_oath_proto.c`
- Modifier : `test/CMakeLists.txt`, `test/test_main.c`

**Interfaces :**
- Produit :
  - `uint32_t oath_dynamic_binary(const uint8_t *hmac, uint8_t hmac_len)` — code
    dynamique 31 bits de la RFC 4226, **sans modulo**.
  - `bool oath_tlv_find(const uint8_t *buf, uint16_t len, uint8_t tag, const
    uint8_t **val, uint16_t *val_len)`.
  - `uint16_t oath_tlv_put(uint8_t *out, uint16_t cap, uint8_t tag, const
    uint8_t *val, uint16_t val_len)` — rend 0 si ça ne tient pas.

**Le point à ne pas manquer :** `ykman` calcule lui-même
`(bytes2int(truncated[1:]) & 0x7FFFFFFF) % 10**digits`
(`_format_code`, `oath.py:259`). **La carte envoie les quatre octets bruts, pas
les six chiffres.** Appliquer le modulo côté carte donnerait des codes faux.

- [ ] **Étape 1 : écrire le test qui échoue**

Créer `test/test_oath_proto.c` :

```c
#include "test_framework.h"
#include "security/oath_proto.h"

/*
 * Vecteur de la RFC 4226, annexe D : secret ASCII « 12345678901234567890 »,
 * compteur 0 -> code 755224. On verifie le code DYNAMIQUE, pas les six
 * chiffres : la carte ne fait pas le modulo, ykman s'en charge.
 * hmac ci-dessous = HMAC-SHA1(secret, 8 octets a zero), constante de la RFC.
 */
static void test_troncature_rfc4226(void)
{
    const uint8_t hmac[20] = {
        0x75,0xa4,0x8a,0x19,0xd4,0xcb,0xe1,0x00,0x64,0x4e,
        0x8a,0xc1,0x39,0x7e,0xea,0x74,0x7a,0x2d,0x33,0xab
    };
    uint32_t dbc = oath_dynamic_binary(hmac, sizeof(hmac));
    TEST_ASSERT_EQ(dbc % 1000000u, 755224u, "vecteur RFC 4226 compteur 0");
    TEST_ASSERT((dbc & 0x80000000u) == 0, "le bit de poids fort est masque");
}

/* L'offset vient du dernier quartet : deux HMAC differant SEULEMENT par ce
 * quartet doivent produire des codes differents. Verifier un seul vecteur ne
 * prouverait pas que l'offset est lu. */
static void test_offset_lu_du_dernier_quartet(void)
{
    uint8_t h[20];
    for (unsigned i = 0; i < sizeof(h); i++) h[i] = (uint8_t)i;
    h[19] = 0x00;
    uint32_t a = oath_dynamic_binary(h, sizeof(h));
    h[19] = 0x05;
    uint32_t b = oath_dynamic_binary(h, sizeof(h));
    TEST_ASSERT(a != b, "changer l'offset change le code");
}

/* Un TLV absent doit se dire absent, pas rendre le TLV voisin. */
static void test_tlv_trouve_et_absent(void)
{
    const uint8_t buf[] = { 0x71, 0x03, 'a','b','c', 0x74, 0x02, 0xAA, 0xBB };
    const uint8_t *v = NULL; uint16_t n = 0;
    TEST_ASSERT(oath_tlv_find(buf, sizeof(buf), 0x74, &v, &n), "0x74 present");
    TEST_ASSERT_EQ(n, 2, "longueur du 0x74");
    TEST_ASSERT(v[0] == 0xAA && v[1] == 0xBB, "valeur du 0x74");
    TEST_ASSERT(!oath_tlv_find(buf, sizeof(buf), 0x79, &v, &n), "0x79 absent");
}

/* Un TLV dont la longueur deborde du tampon est une trame malformee venue de
 * l'hote : elle doit etre refusee, pas lue au-dela. */
static void test_tlv_longueur_qui_deborde(void)
{
    const uint8_t buf[] = { 0x71, 0x40, 'a', 'b' };
    const uint8_t *v = NULL; uint16_t n = 0;
    TEST_ASSERT(!oath_tlv_find(buf, sizeof(buf), 0x71, &v, &n),
                "une longueur qui deborde est refusee");
}

/* Ecriture : refuser plutot que deborder quand la capacite manque. */
static void test_tlv_put_borne(void)
{
    uint8_t out[4];
    const uint8_t v[8] = { 0 };
    TEST_ASSERT_EQ(oath_tlv_put(out, sizeof(out), 0x75, v, 8), 0,
                   "capacite insuffisante -> 0, rien d'ecrit");
    TEST_ASSERT_EQ(oath_tlv_put(out, sizeof(out), 0x75, v, 2), 4,
                   "tag + longueur + 2 octets = 4");
}

void test_oath_proto(void)
{
    TEST_SUITE("oath_proto");
    TEST_RUN(test_troncature_rfc4226);
    TEST_RUN(test_offset_lu_du_dernier_quartet);
    TEST_RUN(test_tlv_trouve_et_absent);
    TEST_RUN(test_tlv_longueur_qui_deborde);
    TEST_RUN(test_tlv_put_borne);
}
```

Enregistrer comme en tâche 2 : `extern void test_oath_proto(void);` et l'appel
dans `test/test_main.c`, `test_oath_proto.c` et
`../main/security/oath_proto.c` dans `test/CMakeLists.txt`.

- [ ] **Étape 2 : lancer et constater le rouge**

```bash
./scripts/check.sh --fast --force
```
Attendu : ROUGE — `security/oath_proto.h` introuvable.

- [ ] **Étape 3 : implémenter**

Créer `main/security/oath_proto.h` :

```c
/* main/security/oath_proto.h — protocole YKOATH (pur, testable hote).
 *
 * Toutes les valeurs viennent de yubikit/oath.py de ykman 5.9.1.
 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

/* Etiquettes TLV YKOATH */
#define OATH_TAG_NAME       0x71
#define OATH_TAG_NAME_LIST  0x72
#define OATH_TAG_KEY        0x73
#define OATH_TAG_CHALLENGE  0x74
#define OATH_TAG_RESPONSE   0x75
#define OATH_TAG_TRUNCATED  0x76
#define OATH_TAG_PROPERTY   0x78
#define OATH_TAG_VERSION    0x79
#define OATH_TAG_TOUCH      0x7C

/*
 * Code dynamique de la RFC 4226 : offset dans le dernier quartet, quatre
 * octets lus a cet offset, bit de poids fort masque.
 *
 * NE FAIT PAS le modulo. ykman calcule lui-meme
 * (bytes2int(valeur) & 0x7FFFFFFF) % 10**chiffres — _format_code(), oath.py.
 * Appliquer le modulo ici rendrait des codes faux.
 */
uint32_t oath_dynamic_binary(const uint8_t *hmac, uint8_t hmac_len);

/* Cherche `tag` dans `buf`. Refuse une longueur qui deborde du tampon : la
 * trame vient de l'hote. */
bool oath_tlv_find(const uint8_t *buf, uint16_t len, uint8_t tag,
                   const uint8_t **val, uint16_t *val_len);

/* Ecrit un TLV. Rend le nombre d'octets ecrits, ou 0 si la capacite manque —
 * jamais une ecriture partielle. */
uint16_t oath_tlv_put(uint8_t *out, uint16_t cap, uint8_t tag,
                      const uint8_t *val, uint16_t val_len);
```

Créer `main/security/oath_proto.c` :

```c
#include "security/oath_proto.h"

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
    while ((uint16_t)(i + 2u) <= len) {
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
```

- [ ] **Étape 4 : vérifier le vert**

```bash
./scripts/check.sh --fast --force
```
Attendu : VERT.

- [ ] **Étape 5 : mutations**

| mutation | attendu |
|---|---|
| `oath_dynamic_binary` applique `% 1000000` | `test_troncature_rfc4226` rouge sur le masque |
| offset forcé à 0 | `test_offset_lu_du_dernier_quartet` rouge |
| `oath_tlv_find` n'écarte pas la longueur qui déborde | `test_tlv_longueur_qui_deborde` rouge |
| `oath_tlv_put` écrit même quand la capacité manque | `test_tlv_put_borne` rouge |

- [ ] **Étape 6 : commit**

```bash
grep -rho TEST_ASSERT test/ --include=test_*.c | wc -l > .tripwire-testcount
git add main/security/oath_proto.h main/security/oath_proto.c test/test_oath_proto.c \
        test/test_main.c test/CMakeLists.txt .tripwire-testcount
git commit -m "oath : troncature RFC 4226 et TLV bornes"
```

---

### Tâche 4 : aiguillage des commandes et découpe des réponses longues

**Fichiers :**
- Modifier : `main/security/oath_proto.h`, `main/security/oath_proto.c`
- Modifier : `test/test_oath_proto.c`

**Interfaces :**
- Consomme : `oath_tlv_find`, `oath_tlv_put`, `oath_dynamic_binary` (tâche 3) ;
  `sec_store_*` (tâche 1).
- Produit : `uint16_t oath_dispatch(const apdu_t *cmd, uint8_t *out, uint16_t
  cap, oath_ctx_t *ctx)` — rend la longueur écrite dans `out`, mot d'état
  inclus. `typedef struct { bool selected; uint16_t pending_off; uint8_t
  pending[OATH_PENDING_MAX]; uint16_t pending_len; } oath_ctx_t;`

L'appui physique n'est **pas** géré ici : `oath_dispatch` est pure et rend, pour
`CALCULATE`, un signal demandant la confirmation. La tâche 6 relie ce signal à
`dongle_confirm()`.

- [ ] **Étape 1 : écrire les tests qui échouent**

Ajouter à `test/test_oath_proto.c` :

```c
#include "security/apdu.h"

/*
 * SELECT et CALCULATE ALL valent TOUS DEUX 0xA4 et ne se distinguent que par
 * P1/P2 (oath.py : select -> P1=04 ; CALCULATE_ALL -> P2=01). Un aiguillage
 * sur le seul INS repondrait un SELECT a une demande de codes. Le test compare
 * les deux reponses ENTRE ELLES : verifier separement que chacune « repond »
 * ne dirait rien sur le fait qu'elles different.
 */
static void test_select_et_calculate_all_ne_se_confondent_pas(void)
{
    oath_ctx_t ctx; memset(&ctx, 0, sizeof(ctx));
    uint8_t sel[8]  = { 0x00, 0xA4, 0x04, 0x00, 0x07,
                        0xA0, 0x00, 0x00 };            /* AID tronque : suffit ici */
    uint8_t all[5]  = { 0x00, 0xA4, 0x00, 0x01, 0x00 };
    uint8_t o1[256], o2[256];
    apdu_t a1, a2;

    TEST_ASSERT(apdu_parse(sel, sizeof(sel), &a1), "SELECT analyse");
    TEST_ASSERT(apdu_parse(all, sizeof(all), &a2), "CALCULATE ALL analyse");
    uint16_t n1 = oath_dispatch(&a1, o1, sizeof(o1), &ctx);
    uint16_t n2 = oath_dispatch(&a2, o2, sizeof(o2), &ctx);

    TEST_ASSERT(n1 != n2 || memcmp(o1, o2, n1) != 0,
                "SELECT et CALCULATE ALL ne rendent pas la meme chose");
}

/* La reponse au SELECT DOIT porter 0x79 (version) : ykman fait
 * data[TAG_VERSION] sans garde et leve une exception s'il manque. */
static void test_select_porte_version_et_sel(void)
{
    oath_ctx_t ctx; memset(&ctx, 0, sizeof(ctx));
    uint8_t sel[8] = { 0x00, 0xA4, 0x04, 0x00, 0x07, 0xA0, 0x00, 0x00 };
    uint8_t out[256]; apdu_t a;
    apdu_parse(sel, sizeof(sel), &a);
    uint16_t n = oath_dispatch(&a, out, sizeof(out), &ctx);

    const uint8_t *v = NULL; uint16_t vl = 0;
    TEST_ASSERT(oath_tlv_find(out, (uint16_t)(n - 2), OATH_TAG_VERSION, &v, &vl),
                "0x79 present, sinon ykman leve une exception");
    TEST_ASSERT(oath_tlv_find(out, (uint16_t)(n - 2), OATH_TAG_NAME, &v, &vl),
                "0x71 present : _get_device_id() en fait un SHA-256");
    TEST_ASSERT(!oath_tlv_find(out, (uint16_t)(n - 2), OATH_TAG_CHALLENGE, &v, &vl),
                "0x74 absent : c'est ce qui signale « pas de mot de passe »");
}

/* Les commandes hors portee se refusent explicitement, jamais en silence. */
static void test_commandes_refusees(void)
{
    oath_ctx_t ctx; memset(&ctx, 0, sizeof(ctx)); ctx.selected = true;
    const uint8_t ins_refuses[] = { 0x03, 0xA3 };
    for (unsigned i = 0; i < sizeof(ins_refuses); i++) {
        uint8_t cmd[5] = { 0x00, ins_refuses[i], 0x00, 0x00, 0x00 };
        uint8_t out[16]; apdu_t a;
        apdu_parse(cmd, sizeof(cmd), &a);
        uint16_t n = oath_dispatch(&a, out, sizeof(out), &ctx);
        TEST_ASSERT_EQ(n, 2, "mot d'etat seul");
        TEST_ASSERT(out[0] == 0x6A && out[1] == 0x81, "6A81 : fonction non supportee");
    }
}

/* Applet non selectionne : toute commande OATH doit etre refusee. */
static void test_refus_si_non_selectionne(void)
{
    oath_ctx_t ctx; memset(&ctx, 0, sizeof(ctx));   /* selected = false */
    uint8_t cmd[5] = { 0x00, 0xA1, 0x00, 0x00, 0x00 };   /* LIST */
    uint8_t out[16]; apdu_t a;
    apdu_parse(cmd, sizeof(cmd), &a);
    uint16_t n = oath_dispatch(&a, out, sizeof(out), &ctx);
    TEST_ASSERT_EQ(n, 2, "mot d'etat seul");
    TEST_ASSERT(out[0] == 0x6A && out[1] == 0x82, "6A82 tant que rien n'est selectionne");
}

/*
 * Douze comptes depassent 255 octets : LIST doit rendre 61xx et le reste par
 * SEND REMAINING. Une implementation qui l'omet marche avec trois comptes et
 * casse avec douze — exactement apres la migration.
 */
static void test_list_douze_comptes_decoupe(void)
{
    oath_ctx_t ctx; memset(&ctx, 0, sizeof(ctx)); ctx.selected = true;
    const uint8_t key[20] = { 0 };
    for (uint8_t i = 0; i < 12; i++) {
        char nom[40];
        snprintf(nom, sizeof(nom), "ServiceNumero%02u:compte@exemple.org", i);
        sec_store_set_slot(i, 0x21, nom, key, sizeof(key));
    }
    uint8_t cmd[5] = { 0x00, 0xA1, 0x00, 0x00, 0x00 };
    uint8_t out[300]; apdu_t a;
    apdu_parse(cmd, sizeof(cmd), &a);
    uint16_t n = oath_dispatch(&a, out, sizeof(out), &ctx);

    TEST_ASSERT(out[n - 2] == 0x61, "61xx : il reste des octets");
    TEST_ASSERT(ctx.pending_len > 0, "le reste est garde pour SEND REMAINING");

    uint8_t suite[5] = { 0x00, 0xA5, 0x00, 0x00, 0x00 };
    apdu_parse(suite, sizeof(suite), &a);
    uint16_t m = oath_dispatch(&a, out, sizeof(out), &ctx);
    TEST_ASSERT(m >= 2, "SEND REMAINING rend quelque chose");
    TEST_ASSERT(out[m - 2] == 0x90 && out[m - 1] == 0x00,
                "la derniere tranche se termine par 9000");
}

/* Un defi de longueur autre que huit est une trame malformee. */
static void test_defi_doit_faire_huit_octets(void)
{
    oath_ctx_t ctx; memset(&ctx, 0, sizeof(ctx)); ctx.selected = true;
    const uint8_t key[20] = { 0 };
    sec_store_set_slot(0, 0x21, "Test:x", key, sizeof(key));

    uint8_t data[] = { OATH_TAG_NAME, 0x06, 'T','e','s','t',':','x',
                       OATH_TAG_CHALLENGE, 0x04, 0,0,0,0 };
    uint8_t cmd[5 + sizeof(data)];
    cmd[0] = 0x00; cmd[1] = 0xA2; cmd[2] = 0x00; cmd[3] = 0x01;
    cmd[4] = (uint8_t)sizeof(data);
    memcpy(&cmd[5], data, sizeof(data));

    uint8_t out[64]; apdu_t a;
    apdu_parse(cmd, sizeof(cmd), &a);
    uint16_t n = oath_dispatch(&a, out, sizeof(out), &ctx);
    TEST_ASSERT_EQ(n, 2, "mot d'etat seul");
    TEST_ASSERT(out[0] == 0x6A && out[1] == 0x80, "6A80 : defi de mauvaise longueur");
}
```

Ajouter les `TEST_RUN` correspondants dans `test_oath_proto()`, et
`#include "security/sec_store.h"` ainsi que `<stdio.h>` en tête du fichier.

- [ ] **Étape 2 : lancer et constater le rouge**

```bash
./scripts/check.sh --fast --force
```
Attendu : ROUGE — `oath_dispatch` et `oath_ctx_t` n'existent pas.

- [ ] **Étape 3 : implémenter l'aiguillage**

Ajouter à `main/security/oath_proto.h` :

```c
#include "security/apdu.h"

/* Taille du tampon de reponse differee. Douze comptes de nom long tiennent
 * largement dedans ; au-dela, LIST rend 6A84 plutot que de tronquer en
 * silence. */
#define OATH_PENDING_MAX 512

/* Signal rendu par oath_dispatch quand la commande exige un appui physique.
 * oath_proto reste pur : c'est mode_oath.c qui appellera dongle_confirm(). */
#define OATH_SW_NEEDS_TOUCH 0x0001u

typedef struct {
    bool     selected;                    /* l'applet a-t-il ete selectionne ? */
    uint8_t  pending[OATH_PENDING_MAX];   /* reste a envoyer par SEND REMAINING */
    uint16_t pending_len;
    uint16_t pending_off;
} oath_ctx_t;

/*
 * Traite une commande APDU. Ecrit la reponse (mot d'etat compris) dans `out` et
 * rend sa longueur. Ne bloque jamais et ne touche a aucun peripherique.
 */
uint16_t oath_dispatch(const apdu_t *cmd, uint8_t *out, uint16_t cap,
                       oath_ctx_t *ctx);
```

```c
/* Mots d'etat, une seule definition pour tout le fichier. */
#define SW_OK              0x9000u
#define SW_COND_NOT_SAT    0x6985u
#define SW_WRONG_DATA      0x6A80u
#define SW_NOT_SUPPORTED   0x6A81u
#define SW_NOT_FOUND       0x6A82u
#define SW_FULL            0x6A84u
#define SW_INS_UNKNOWN     0x6D00u

static uint16_t sw_only(uint8_t *out, uint16_t cap, uint16_t sw)
{
    if (cap < 2) return 0;
    out[0] = (uint8_t)(sw >> 8);
    out[1] = (uint8_t)(sw & 0xFFu);
    return 2;
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
        return sw_only(out, cap, SW_NOT_SUPPORTED);
    case 0xA4u: return oath_do_calculate_all(out, cap, ctx);
    case 0xA2u: return oath_do_calculate(cmd, out, cap, ctx);
    case 0x01u: return oath_do_put(cmd, out, cap);
    case 0x02u: return oath_do_delete(cmd, out, cap);
    case 0x05u: return oath_do_rename(cmd, out, cap);
    case 0x04u: return oath_do_reset(out, cap);
    default:    return sw_only(out, cap, SW_INS_UNKNOWN);
    }
}
```

Les fonctions `oath_do_*` sont statiques et déclarées au-dessus. Leur contenu
suit l'ordre ci-dessous ; chacune tient en une trentaine de lignes.


1. `INS == 0xA4 && P1 == 0x04` → SELECT : `selected = true`, rendre
   `0x79` (version `05 07 01`) + `0x71` (le sel, fourni par le contexte
   d'appel), `9000`. **Ne pas** émettre `0x74`.
2. `!ctx->selected` → `6A82` pour tout le reste.
3. `INS == 0xA5` → SEND REMAINING : tranche suivante de `pending`, `61xx` s'il
   en reste, `9000` sinon.
4. `INS == 0x03 || INS == 0xA3` → `6A81`.
5. `INS == 0xA4 && P2 == 0x01` → CALCULATE ALL : pour chaque slot occupé,
   `0x71` (nom) puis `0x7C` (marqueur tactile, valeur vide) — **jamais** de code.
6. `INS == 0xA2` → CALCULATE : vérifier `0x71` présent et connu (`6A82` sinon),
   `0x74` présent et **long de 8 octets exactement** (`6A80` sinon), puis rendre
   `OATH_SW_NEEDS_TOUCH`.
7. `INS == 0x01` → PUT : refuser un type HOTP (`6A81`), un secret de plus de 64
   octets (`6A80`), un magasin plein (`6A84`) ; forcer le drapeau tactile.
8. `INS == 0x02` / `0x05` / `0x04` → DELETE / RENAME / RESET.
9. sinon → `6D00`.

- [ ] **Étape 4 : vérifier le vert**

```bash
./scripts/check.sh --fast --force
```
Attendu : VERT.

- [ ] **Étape 5 : mutations**

| mutation | attendu |
|---|---|
| aiguiller sur l'INS seul, sans P1/P2 | `test_select_et_calculate_all_ne_se_confondent_pas` rouge |
| retirer le TLV `0x79` de la réponse au SELECT | `test_select_porte_version_et_sel` rouge |
| accepter un défi de longueur quelconque | `test_defi_doit_faire_huit_octets` rouge |
| LIST tronque au lieu de rendre `61xx` | `test_list_douze_comptes_decoupe` rouge |
| retirer la garde `!ctx->selected` | `test_refus_si_non_selectionne` rouge |

- [ ] **Étape 6 : commit**

```bash
grep -rho TEST_ASSERT test/ --include=test_*.c | wc -l > .tripwire-testcount
git add main/security/oath_proto.h main/security/oath_proto.c test/test_oath_proto.c .tripwire-testcount
git commit -m "oath : aiguillage des commandes, decoupe SEND REMAINING, bornes hote"
```

---

### Tâche 5 : la confirmation nomme le compte

**Fichiers :**
- Modifier : `main/security/sec_confirm.h`, `main/security/sec_confirm.c`
- Modifier : `main/hmi/hmi.c`, `main/hmi/screen_view.h`, `main/hmi/screen.c`
- Modifier : `test/test_sec_confirm.c`, `test/test_screen_view.c`
- Modifier : `.tripwire-divergences`

**Interfaces :**
- Produit : `void sec_confirm_arm_named(uint8_t slot, sec_op_t op, const char
  *label, uint32_t now_ms)` ; `const char *sec_confirm_label(void)` — rend une
  chaîne vide, jamais `NULL`. `sec_confirm_arm()` existant conservé, il appelle
  la version nommée avec une étiquette vide.

Le dépôt a **déjà** une divergence assumée du même genre : `sec_confirm_arm()`
avait reçu un paramètre `op` pour que l'écran nomme l'opération. Celle-ci se
déclare pareil, et pour la même raison.

- [ ] **Étape 1 : écrire le test qui échoue**

Dans `test/test_sec_confirm.c` :

```c
/* L'etiquette accompagne l'armement et disparait au desarmement : une etiquette
 * survivante ferait nommer un compte que plus rien n'attend. Compare les deux
 * etats ENTRE EUX plutot que chacun a une constante. */
static void test_etiquette_suit_l_armement(void)
{
    sec_confirm_reset();
    TEST_ASSERT(sec_confirm_label()[0] == '\0', "au repos, pas d'etiquette");
    sec_confirm_arm_named(1, SEC_OP_OATH, "GITHUB", 1000);
    TEST_ASSERT(strcmp(sec_confirm_label(), "GITHUB") == 0, "armee : l'etiquette est la");
    sec_confirm_reset();
    TEST_ASSERT(sec_confirm_label()[0] == '\0', "desarmee : l'etiquette est partie");
}

/* Jamais NULL : screen.c la passe a draw_text_2x_centered() sans garde. */
static void test_etiquette_jamais_nulle(void)
{
    sec_confirm_reset();
    sec_confirm_arm_named(1, SEC_OP_OATH, NULL, 1000);
    TEST_ASSERT(sec_confirm_label() != NULL, "NULL en entree ne ressort pas en NULL");
    TEST_ASSERT(sec_confirm_label()[0] == '\0', "il ressort une chaine vide");
}
```

- [ ] **Étape 2 : constater le rouge**

```bash
./scripts/check.sh --fast --force
```
Attendu : ROUGE — `sec_confirm_arm_named` et `SEC_OP_OATH` n'existent pas.

- [ ] **Étape 3 : implémenter**

- Ajouter `SEC_OP_OATH` à `sec_op_t` (`sec_confirm.h`), **après**
  `SEC_OP_FIDO_AUTH`, pour ne pas décaler les valeurs `0x05`/`0x06` sur
  lesquelles `screen_op_has_deadline()` est testé.
- Ajouter un tampon statique `s_label[OATH_NAME_DISPLAY_MAX]` protégé par le
  même `portMUX` que le reste de l'état. Cela fait inclure
  `security/oath_name.h` par `sec_confirm.c` — dépendance voulue : la taille du
  tampon et la longueur affichable sont **le même nombre**, et les dupliquer les
  ferait diverger. `oath_name.h` est pur, il compile sur l'hôte.
- `screen_view.h` : libellé `"CODE OTP"` pour `SEC_OP_OATH`.
- `hmi.c` : publier l'étiquette dans l'instantané, à côté de `op`.
- `screen.c`, cas `SCREEN_WAIT` : dessiner l'étiquette sous le libellé quand
  elle n'est pas vide.

`screen_op_has_deadline()` n'a **rien à changer** : elle rend `true` par défaut
pour toute opération non-FIDO, et l'échéance est bien réelle ici — `ykman` ne
relance pas.

- [ ] **Étape 4 : vérifier le vert, puis muter**

| mutation | attendu |
|---|---|
| `sec_confirm_reset()` n'efface pas l'étiquette | `test_etiquette_suit_l_armement` rouge |
| `sec_confirm_label()` rend `NULL` sur entrée `NULL` | `test_etiquette_jamais_nulle` rouge |
| `screen_op_has_deadline()` rend `false` pour `SEC_OP_OATH` | `test_screen_layout` rouge (la comparaison croisée existante) |

- [ ] **Étape 5 : déclarer la divergence**

```bash
printf 'main/security/sec_confirm.c\tsec_confirm_arm_named\tl ecran doit nommer le COMPTE OATH demande, pas seulement l operation — sans quoi l appui est un interrupteur de presence et pas un accord ; meme motif que le parametre op deja diverge de KeSp\n' >> .tripwire-divergences
./scripts/check.sh --fast --force
```

Vérifier que l'assertion **mord** : retirer transitoirement le nom de la
fonction du fichier hôte doit rendre le check rouge. Un motif qui apparaît aussi
dans un commentaire ou un message ne prouve rien — le resserrer si c'est le cas.

- [ ] **Étape 6 : commit**

```bash
grep -rho TEST_ASSERT test/ --include=test_*.c | wc -l > .tripwire-testcount
git add main/security/sec_confirm.h main/security/sec_confirm.c main/hmi/hmi.c \
        main/hmi/screen_view.h main/hmi/screen.c test/test_sec_confirm.c \
        test/test_screen_view.c .tripwire-divergences .tripwire-testcount
git commit -m "oath : la confirmation nomme le compte demande"
```

---

### Tâche 6 : sixième mode `usb mode oath`

**Fichiers :**
- Créer : `main/usb/mode_oath.h`, `main/usb/mode_oath.c`
- Modifier : `main/usb/usb_mode.h`, `main/usb/usb_mode.c`,
  `main/usb/usb_mode_cycle.h`, `main/usb/usb_mode_name.c`
- Modifier : `main/console/console.c`, `main/security/ccid.c`
- Modifier : `test/test_usb_mode_cycle.c`, `test/test_screen_view.c`

**Interfaces :**
- Consomme : `oath_dispatch()` (tâche 4), `sec_confirm_arm_named()` (tâche 5).
- Produit : `USB_MODE_OATH` dans `usb_mode_t`, avant `USB_MODE_COUNT`.

Rappel de contrainte : `usb_mode_set` n'apparaît que dans `usb_mode.{c,h}`,
`console.c`, `sec_gate.{c,h}` — **`mode_oath.c` ne doit jamais le nommer**, pas
même en commentaire.

- [ ] **Étape 1 : test du cycle, rouge d'abord**

Dans `test/test_usb_mode_cycle.c`, le cycle passe de trois à quatre crans.
Le test existant sur le nombre de points de l'écran (`screen_cycle_count`) doit
**suivre le cycle**, pas une constante — vérifier qu'il est écrit ainsi ; sinon
c'est lui qu'il faut corriger d'abord.

- [ ] **Étape 2 : constater le rouge**

```bash
./scripts/check.sh --fast --force
```

- [ ] **Étape 3 : implémenter le mode**

`mode_oath.c` reprend la structure de `mode_pgp.c` : installation CCID,
chargement du magasin à **chaque entrée** dans le mode (même raison que pour
PGP : `ccid_drv_init()` réarme l'état en RAM à chaque bascule), boucle
d'aiguillage appelant `oath_dispatch()`, et — sur `OATH_SW_NEEDS_TOUCH` —
`dongle_confirm()` puis calcul du code.

Dans `ccid.c`, élargir `dongle_confirm()` pour porter l'étiquette :

```c
static int dongle_confirm_named(sec_op_t op, const char *label)
```
en gardant `dongle_confirm(op)` comme enveloppe qui passe `NULL`, pour ne pas
toucher au chemin OpenPGP déjà validé sur matériel.

Le sel de huit octets du SELECT : tiré par `esp_fill_random()` à la première
entrée dans le mode, persisté en NVS sous `oath_salt`, relu ensuite. **Stable
d'une session à l'autre** — sinon `ykman` voit un appareil différent à chaque
branchement et refuse les identifiants enregistrés.

- [ ] **Étape 3 bis : emprunter le chemin de démontage, pas le contourner**

`ccid.c` traite déjà le cas d'une bascule de mode **pendant** l'attente de
confirmation : le drapeau `s_shutdown` fait refuser tout de suite, parce que
continuer laisserait tourner une boucle qui poste des trames WTX sur une file
que `tud_deinit()` est en train de détruire.

`dongle_confirm_named()` doit passer par **exactement** ce chemin — c'est-à-dire
partager le corps de `dongle_confirm()`, pas le recopier. Vérifier après coup :

```bash
grep -n 's_shutdown' main/security/ccid.c
```
Attendu : une seule boucle d'attente le teste, pas deux. Deux occurrences dans
deux boucles distinctes signifient que le chemin a été dupliqué, et qu'une des
copies dérivera.

- [ ] **Étape 4 : vérifier le vert et le garde-fou 4**

```bash
./scripts/check.sh --fast --force
grep -rn 'usb_mode_set' main/usb/mode_oath.c && echo "VIOLATION" || echo "ok"
```

- [ ] **Étape 5 : commit**

```bash
git add main/usb/mode_oath.h main/usb/mode_oath.c main/usb/usb_mode.h \
        main/usb/usb_mode.c main/usb/usb_mode_cycle.h main/usb/usb_mode_name.c \
        main/console/console.c main/security/ccid.c test/ .tripwire-testcount
git commit -m "oath : sixieme mode USB, applet OATH sur CCID"
```

---

### Tâche 7 : validation sur matériel

**Fichiers :**
- Modifier : `docs/HARDWARE.md`

**Aucun code.** Cette tâche prouve que l'ensemble fonctionne sur la carte, avec
un vrai appui.

Rappels qui ont déjà coûté du temps :

- **Vérifier la carte après flash**, pas seulement la puce : `sec source` doit
  répondre `bouton en facade`.
- Port : `/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_*-if00`,
  **jamais `/dev/ttyACM*`** — la numérotation permute d'un jour à l'autre et
  `ttyACM0` a déjà été le clavier de Mae.
- Un rapport de sous-agent disant « flashé » n'est pas une preuve que la carte
  porte le binaire courant. Reflasher soi-même avant de faire regarder Mae.

- [ ] **Étape 1 : flasher et vérifier la carte**

```bash
source ~/esp/esp-idf/export.sh
idf.py -B build_wt9932_key -DBOARD=wt9932_key -DSDKCONFIG=build_wt9932_key/sdkconfig build
idf.py -B build_wt9932_key -p /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_30:ED:A0:E0:BC:5F-if00 app-flash
```

- [ ] **Étape 2 : provisionner un compte de test**

```bash
nix-shell --run 'ykman oath accounts add Test:mae JBSWY3DPEHPK3PXP'
nix-shell --run 'ykman oath accounts list'
```
Attendu : le compte apparaît.

- [ ] **Étape 3 : demander un code, appuyer**

```bash
nix-shell --run 'ykman oath accounts code Test:mae'
```
Attendu : `ykman` **patiente** (les trames WTX font leur travail), l'écran
affiche `CONFIRMER` / `CODE OTP` / `TEST` avec la barre de décompte, et l'appui
sur CONFIRM produit un code à six chiffres.

- [ ] **Étape 4 : vérifier que le code est le bon**

```bash
nix-shell -p oath-toolkit --run 'oathtool --totp -b JBSWY3DPEHPK3PXP'
```
Attendu : **le même code**, au même instant. C'est la seule étape qui prouve que
la troncature et le compteur de temps sont corrects — un code à six chiffres
bien formé mais faux passerait toutes les étapes précédentes.

- [ ] **Étape 5 : vérifier le comportement sans appui**

Relancer la commande et **ne pas appuyer**. Attendu : `ykman` rend une erreur au
bout de quinze secondes, la barre se vide **une fois** et ne repart pas — c'est
la différence avec U2F, dont le réarmement a fait retirer la barre.

- [ ] **Étape 6 : les douze comptes**

Migrer les douze comptes depuis Proton, un par un. Puis :

```bash
nix-shell --run 'ykman oath accounts list'
```
Attendu : **les douze**, sans troncature ni disparition — c'est le test réel de
`SEND REMAINING`, celui qu'aucun test hôte ne remplace.

- [ ] **Étape 7 : consigner et commiter**

Ajouter à `docs/HARDWARE.md` une section de validation datée, sur le modèle de
« Validation OpenPGP CCID », citant les commandes et leurs sorties réelles.

```bash
git add docs/HARDWARE.md
git commit -m "materiel : validation de l'applet OATH sur wt9932_key"
```

---

## Ce que ce plan ne fait pas

- **HOTP** : refusé (`6A81`). Compteur persistant et écriture NVS par code
  produit, pour un besoin qui n'existe pas.
- **Mot de passe** (`SET CODE` / `VALIDATE`) : refusé. Décision 3 de la spec.
- **SHA-512** : refusé. Rien ne l'utilise dans les douze comptes.
- **Import automatique depuis Proton** : la migration est manuelle et le reste.
