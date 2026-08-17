# Écran OLED de la carte-clé — plan d'implémentation

> **Pour les exécutants agentiques :** SOUS-SKILL REQUISE — utiliser
> `superpowers:subagent-driven-development` (recommandé) ou
> `superpowers:executing-plans` pour dérouler ce plan tâche par tâche. Les
> étapes utilisent la syntaxe à cases (`- [ ]`).

**But :** donner à la carte-clé un écran qui dit **ce qu'on est en train
d'autoriser**, pour que l'appui sur le bouton cesse d'être aveugle.

**Architecture :** une tâche d'affichage séparée de la tâche IHM, alimentée par
un instantané publié sous verrou. Toute la décision — quel écran, quels textes,
quelle proportion de barre, quelle phase d'animation — vit dans des en-têtes
purs testés sur l'hôte ; `screen.c` ne fait que piloter l'I²C et peindre.

**Pile technique :** ESP-IDF 5.5, C11, pilote SSD1306 écrit directement sur
`driver/i2c_master` (pas de composant tiers — même choix que TinyUSB brut),
harnais de tests hôte CMake existant.

**Spec :** [`docs/superpowers/specs/2026-08-17-ecran-oled-carte-cle-design.md`](../specs/2026-08-17-ecran-oled-carte-cle-design.md)

## Contraintes globales

- **GPIO24, GPIO25, GPIO35 ne sont jamais assignés** hors d'un en-tête de carte
  (`scripts/fast.sh` garde-fou n°1). L'écran est sur **IO53 (SCL)** et
  **IO54 (SDA)**, déclarés dans `boards/wt9932_key/board.h`.
- **Aucun `esp_deep_sleep_start`** (garde-fou n°2).
- **`usb_mode_set` et `sec_gate_console_confirm` sont confinés** à
  `usb_mode.{c,h}`, `console.c`, `sec_gate.{c,h}` — **commentaires compris**, le
  garde-fou n°4 est un grep. Trois implémenteurs y sont tombés sur la branche
  précédente.
- **Seule la logique pure entre dans `test/`** : aucun appel ESP-IDF. Les
  en-têtes partagés utilisent `#ifdef TEST_HOST typedef int esp_err_t;`
  (modèle : `main/usb/usb_mode.h`).
- **TDD** : test écrit d'abord, rouge constaté, implémentation, puis **preuve que
  le test mord** (mutation → rouge → retour). Rapporter les tests rouges **et
  les verts** : c'est le contraste qui prouve que la mutation éprouve ce qu'on
  croit.
- **Ne jamais écrire `.tripwire-testcount` à la main** (550 actuellement) :
  `./scripts/check.sh` le relève seul, on committe le fichier tel qu'il l'écrit.
- **Committer AVANT toute mutation** : un fichier neuf n'est pas restauré par
  `git checkout`, un fichier suivi non committé est restauré *trop loin*.
- **Ne jamais attendre une notification de tâche de fond** : elle remonte au
  coordinateur, pas à l'implémenteur. Lancer les vérifications au premier plan.
- Messages de commit en français, corps expliquant le *pourquoi*, pieds
  `Co-Authored-By` et `Claude-Session` habituels.

## Structure des fichiers

| fichier | responsabilité |
|---|---|
| `main/security/sec_confirm.{c,h}` | *(modifié)* `sec_op_t`, l'op passée à `arm()`, lecture sans effet de bord |
| `main/security/openpgp_card.{c,h}` | *(modifié)* le hook `confirm` transporte l'op ; trois sites d'appel |
| `main/security/ccid.c`, `otp_hid.c` | *(modifiés)* passent leur op |
| `main/hmi/screen_view.h` | *(créé)* **pur** — état → quel écran, quels textes |
| `main/hmi/screen_anim.h` | *(créé)* **pur** — barre, décalage anti-marquage, glissement |
| `main/hmi/screen.{c,h}` | *(créés)* pilote SSD1306, tâche d'affichage |
| `main/hmi/hmi.{c,h}` | *(modifiés)* publient l'instantané sous verrou |
| `boards/wt9932_key/board.h` | *(modifié)* brochage I²C |

---

### Tâche 1 : `sec_op_t` — dire *quelle* opération est armée

Sans ça, l'écran ne peut afficher que « une opération attend », ce qui ne vaut
guère mieux que la LED. C'est la tâche qui rend le reste utile.

**Fichiers :**
- Modifier : `main/security/sec_confirm.h`, `main/security/sec_confirm.c`
- Modifier : `main/security/openpgp_card.h` (le hook), `main/security/openpgp_card.c` (trois sites)
- Modifier : `main/security/ccid.c`, `main/security/otp_hid.c`
- Modifier : `test/test_sec_confirm.c`

**Interfaces :**
- Produit : `sec_op_t` et `sec_op_t sec_confirm_armed_op(void)`, consommés par la tâche 2.

- [ ] **Étape 1 : écrire les tests qui échouent**

Ajouter à `test/test_sec_confirm.c` :

```c
/* L'ecran doit nommer ce qu'il fait confirmer. Un numero de slot ne le permet
 * pas : toutes les operations CCID partagent le meme slot. */
static void test_armed_op_is_reported(void)
{
    sec_confirm_reset();
    sec_confirm_arm(0xF0u, SEC_OP_SIGN, 1000);
    TEST_ASSERT_EQ(sec_confirm_armed_op(), SEC_OP_SIGN, "l'operation armee est rendue");
}

static void test_armed_op_survives_peek(void)
{
    sec_confirm_reset();
    sec_confirm_arm(0xF0u, SEC_OP_DECRYPT, 1000);
    (void)sec_confirm_peek(1100);
    TEST_ASSERT_EQ(sec_confirm_armed_op(), SEC_OP_DECRYPT,
                   "peek ne detruit pas l'operation");
}

/* Rien d'arme : l'ecran ne doit pas afficher l'operation PRECEDENTE, sinon il
 * ment sur ce qui se passe. */
static void test_reset_clears_the_op(void)
{
    sec_confirm_reset();
    sec_confirm_arm(0xF0u, SEC_OP_AUTH, 1000);
    sec_confirm_reset();
    TEST_ASSERT_EQ(sec_confirm_armed_op(), SEC_OP_UNKNOWN,
                   "apres reset, aucune operation n'est armee");
}

/* Deux armements successifs : c'est le dernier qui compte. */
static void test_rearm_replaces_the_op(void)
{
    sec_confirm_reset();
    sec_confirm_arm(0xF0u, SEC_OP_SIGN, 1000);
    sec_confirm_arm(0xF0u, SEC_OP_OTP, 2000);
    TEST_ASSERT_EQ(sec_confirm_armed_op(), SEC_OP_OTP, "le dernier armement gagne");
}
```

Les appeler par `TEST_RUN` dans la fonction de suite du fichier.

- [ ] **Étape 2 : constater le rouge**

```bash
cmake -S test -B test/build && cmake --build test/build
```
Attendu : ÉCHEC de compilation — `SEC_OP_SIGN` inconnu, `sec_confirm_arm` prend
deux arguments.

- [ ] **Étape 3 : écrire l'implémentation**

Dans `main/security/sec_confirm.h`, avant les déclarations :

```c
/*
 * Ce qu'une confirmation autorise. Un CODE, pas une chaine : sec_confirm garde
 * la porte, il n'a a porter ni texte, ni allocation, ni borne de longueur a
 * defendre. La traduction en libelle affichable est une fonction pure de
 * hmi/screen_view.h, donc testable — et un futur FIDO2 ajoutera ses codes ici
 * sans toucher a ce module.
 *
 * Necessaire parce que le slot ne porte pas le sens : toutes les operations
 * CCID partagent CCID_CONFIRM_SLOT.
 */
typedef enum {
    SEC_OP_UNKNOWN = 0,   /* rien d'arme, ou origine inconnue */
    SEC_OP_SIGN,          /* PSO:CDS — signature OpenPGP */
    SEC_OP_DECRYPT,       /* PSO:DEC */
    SEC_OP_AUTH,          /* INTERNAL AUTHENTICATE */
    SEC_OP_OTP,           /* defi/reponse CR-HMAC */
} sec_op_t;
```

Changer la signature et ajouter l'accesseur :

```c
void sec_confirm_arm(uint8_t slot, sec_op_t op, uint32_t now_ms);

/* L'operation actuellement armee, ou SEC_OP_UNKNOWN. Lecture SANS effet de
 * bord, comme peek() : pour l'affichage seulement. */
sec_op_t sec_confirm_armed_op(void);
```

Dans `main/security/sec_confirm.c` : ajouter `static sec_op_t s_op = SEC_OP_UNKNOWN;`
aux statiques, l'écrire dans `arm()`, le remettre à `SEC_OP_UNKNOWN` dans
`reset()`, et implémenter `sec_confirm_armed_op()` en simple lecture.

**Mettre à jour le commentaire `CONCURRENCY MODEL`** en tête du fichier : `s_op`
est un quatrième champ scalaire, écrit par `arm()` et lu par la tâche
d'affichage. Le raisonnement existant sur la lecture déchirée `(s_state,
s_armed_ms)` s'étend à lui — dire lequel des deux cas bénins s'applique. Ne pas
laisser ce paragraphe décrire trois champs quand il y en a quatre.

- [ ] **Étape 4 : plomber les appelants**

Dans `main/security/openpgp_card.h`, le hook `confirm` prend l'opération :

```c
    int (*confirm)(sec_op_t op);
```

Dans `main/security/openpgp_card.c`, les trois portes UIF passent leur code —
les DO le disent déjà :

| ligne | DO | opération |
|---|---|---|
| ~1059 | `0x00D7` | `SEC_OP_DECRYPT` |
| ~1101 | `0x00D6` | `SEC_OP_SIGN` |
| ~1158 | `0x00D8` | `SEC_OP_AUTH` |

soit `int cs = s_hooks->confirm(SEC_OP_DECRYPT);` et ainsi de suite.

Dans `main/security/ccid.c`, `dongle_confirm()` prend le paramètre et le passe
à `sec_confirm_arm(CCID_CONFIRM_SLOT, op, now)`.

Dans `main/security/otp_hid.c:85`, `sec_confirm_arm((uint8_t)idx, SEC_OP_OTP, now_ms())`.

- [ ] **Étape 5 : constater le vert**

```bash
cmake --build test/build && ./test/build/test_runner
```
Attendu : quatre `OK` de plus, `0 échecs`.

- [ ] **Étape 6 : prouver que les tests mordent**

Remplacer temporairement le corps de `sec_confirm_armed_op()` par
`return SEC_OP_SIGN;`. Attendu : `test_armed_op_survives_peek`,
`test_reset_clears_the_op` et `test_rearm_replaces_the_op` rougissent,
`test_armed_op_is_reported` reste **vert** (il attend justement `SEC_OP_SIGN`).
C'est ce contraste qui montre qu'un seul de ces tests ne suffirait pas.
Restaurer avec `git checkout main/security/sec_confirm.c`, vérifier le vert.

- [ ] **Étape 7 : vérifier et commiter**

```bash
source ~/esp/esp-idf/export.sh && ./scripts/check.sh
```
Attendu : vert sur les trois cartes. Committer, `.tripwire-testcount` inclus tel
que le script l'a écrit.

Ajouter une ligne à `.tripwire-divergences` : `sec_confirm_arm` diverge de
`KeSp_firmware` par un paramètre — divergence assumée, motif et raison.

---

### Tâche 2 : `screen_view.h` — quel écran, quels textes

**Fichiers :**
- Créer : `main/hmi/screen_view.h`, `test/test_screen_view.c`
- Modifier : `test/CMakeLists.txt`, `test/test_main.c`

**Interfaces :**
- Consomme : `sec_op_t` (tâche 1), `usb_mode_t`, `led_event_t`.
- Produit : `screen_kind_t`, `screen_view_t`, `screen_view_of()` — consommés par les tâches 3 et 4.

- [ ] **Étape 1 : écrire les tests qui échouent**

Créer `test/test_screen_view.c` :

```c
/* L'ecran est la SEULE chose qui dit POUR QUOI on appuie. Un libelle faux ou
 * partage entre deux operations est pire qu'une absence d'ecran : il fait
 * confirmer en croyant savoir. */
#include "test_framework.h"
#include <string.h>

#include "usb/usb_mode.h"
#include "sec_confirm.h"
#include "hmi/led_state.h"
#include "hmi/screen_view.h"

/* Totalite : aucun etat ne laisse l'ecran indefini. */
static void test_every_state_has_a_screen(void)
{
    for (int m = 0; m <= USB_MODE_COUNT; m++) {
        screen_view_t v = screen_view_of((usb_mode_t)m, false, SEC_OP_UNKNOWN,
                                         LED_EVENT_NONE);
        TEST_ASSERT(v.kind == SCREEN_IDLE || v.kind == SCREEN_VERDICT
                    || v.kind == SCREEN_WAIT || v.kind == SCREEN_SWITCH,
                    "chaque etat donne un ecran connu");
        TEST_ASSERT(v.title != NULL && v.line != NULL, "aucun texte n'est NULL");
    }
}

/* Chaque operation a son libelle, et deux operations n'en partagent jamais un —
 * sinon l'ecran ne distingue pas une signature d'un dechiffrement. */
static void test_every_op_has_a_distinct_label(void)
{
    const sec_op_t ops[] = { SEC_OP_SIGN, SEC_OP_DECRYPT, SEC_OP_AUTH, SEC_OP_OTP };
    const unsigned n = sizeof(ops) / sizeof(ops[0]);
    for (unsigned i = 0; i < n; i++) {
        const char *a = screen_view_of(USB_MODE_PGP, true, ops[i], LED_EVENT_NONE).line;
        TEST_ASSERT(a != NULL && a[0] != '\0', "chaque operation a un libelle non vide");
        for (unsigned j = i + 1; j < n; j++) {
            const char *b = screen_view_of(USB_MODE_PGP, true, ops[j], LED_EVENT_NONE).line;
            TEST_ASSERT(strcmp(a, b) != 0, "deux operations ne partagent pas un libelle");
        }
    }
}

/* Une operation inconnue ne doit pas etre presentee comme une operation connue. */
static void test_unknown_op_is_not_mistaken_for_a_known_one(void)
{
    const char *u = screen_view_of(USB_MODE_PGP, true, SEC_OP_UNKNOWN, LED_EVENT_NONE).line;
    const char *s = screen_view_of(USB_MODE_PGP, true, SEC_OP_SIGN, LED_EVENT_NONE).line;
    TEST_ASSERT(strcmp(u, s) != 0, "l'operation inconnue a son propre libelle");
}

/* L'attente prime sur la bascule : c'est le seul ecran qui reclame une action. */
static void test_wait_beats_switch(void)
{
    screen_view_t v = screen_view_of(USB_MODE_PGP, true, SEC_OP_SIGN, LED_EVENT_MODE);
    TEST_ASSERT_EQ(v.kind, SCREEN_WAIT, "l'attente prime sur la bascule");
}

/* Le verdict prime sur la bascule : il est fugace et c'est ce qu'on cherche a lire. */
static void test_verdict_beats_switch(void)
{
    screen_view_t v = screen_view_of(USB_MODE_PGP, false, SEC_OP_UNKNOWN, LED_EVENT_GRANTED);
    TEST_ASSERT_EQ(v.kind, SCREEN_VERDICT, "le verdict prime sur la bascule");
}

/* Accord et refus doivent etre distinguables en toutes lettres. */
static void test_granted_and_refused_read_differently(void)
{
    const char *g = screen_view_of(USB_MODE_PGP, false, SEC_OP_UNKNOWN, LED_EVENT_GRANTED).title;
    const char *r = screen_view_of(USB_MODE_PGP, false, SEC_OP_UNKNOWN, LED_EVENT_REFUSED).title;
    TEST_ASSERT(strcmp(g, r) != 0, "accord et refus ne s'ecrivent pas pareil");
}

/* Au repos, l'ecran nomme le mode — c'est ce qui leve l'ambiguite du rouge. */
static void test_idle_names_the_mode(void)
{
    const char *p = screen_view_of(USB_MODE_PGP, false, SEC_OP_UNKNOWN, LED_EVENT_NONE).title;
    const char *o = screen_view_of(USB_MODE_OTP, false, SEC_OP_UNKNOWN, LED_EVENT_NONE).title;
    TEST_ASSERT(strcmp(p, o) != 0, "deux modes ne portent pas le meme nom");
}

void test_screen_view(void)
{
    TEST_SUITE("screen_view");
    TEST_RUN(test_every_state_has_a_screen);
    TEST_RUN(test_every_op_has_a_distinct_label);
    TEST_RUN(test_unknown_op_is_not_mistaken_for_a_known_one);
    TEST_RUN(test_wait_beats_switch);
    TEST_RUN(test_verdict_beats_switch);
    TEST_RUN(test_granted_and_refused_read_differently);
    TEST_RUN(test_idle_names_the_mode);
}
```

Déclarer et appeler `test_screen_view` dans `test/test_main.c`, ajouter le
fichier à `add_executable` dans `test/CMakeLists.txt`, et ajouter
`../main/security/sec_confirm.c` s'il n'y est pas déjà.

- [ ] **Étape 2 : constater le rouge**

Attendu : `hmi/screen_view.h: No such file or directory`.

- [ ] **Étape 3 : écrire l'implémentation**

Créer `main/hmi/screen_view.h` :

```c
#pragma once

/*
 * screen_view — quel ecran l'utilisateur voit, et avec quels mots.
 *
 * Pur : le pilote SSD1306 et la tache d'affichage vivent dans hmi/screen.c.
 * Ce fichier ne fait que decider.
 *
 * C'est le module qui porte le seul gain de securite de l'ecran : dire POUR
 * QUOI on appuie. Un bouton sans ecran prouve qu'un humain a touche, jamais
 * son consentement a CETTE operation-la — un hote peut demander une signature
 * pendant que l'utilisateur croit en confirmer une autre. D'ou l'exigence que
 * deux operations ne partagent jamais un libelle, verifiee par un test.
 */

#include <stdbool.h>

#include "sec_confirm.h"
#include "usb/usb_mode.h"
#include "hmi/led_state.h"

typedef enum {
    SCREEN_IDLE = 0,   /* le mode, au repos */
    SCREEN_WAIT,       /* une operation attend un appui */
    SCREEN_VERDICT,    /* accorde / refuse, fugace */
    SCREEN_SWITCH,     /* bascule de mode, fugace */
} screen_kind_t;

typedef struct {
    screen_kind_t kind;
    const char   *title;   /* jamais NULL */
    const char   *line;    /* jamais NULL ; le libelle de l'operation en attente */
} screen_view_t;

static inline const char *screen_op_label(sec_op_t op)
{
    switch (op) {
    case SEC_OP_SIGN:    return "Signature OpenPGP";
    case SEC_OP_DECRYPT: return "Dechiffrement";
    case SEC_OP_AUTH:    return "Authentification";
    case SEC_OP_OTP:     return "Cle OTP";
    /* Une origine inconnue se dit, elle ne s'habille pas du libelle d'une
     * operation connue : mieux vaut « operation inconnue » qu'un mensonge
     * plausible. */
    default:             return "Operation inconnue";
    }
}

static inline const char *screen_mode_name(usb_mode_t mode)
{
    switch (mode) {
    case USB_MODE_PGP:     return "OpenPGP";
    case USB_MODE_OTP:     return "Cle OTP";
    case USB_MODE_STORAGE: return "Disque";
    default:               return "Au repos";
    }
}

static inline screen_view_t screen_view_of(usb_mode_t mode,
                                           bool confirm_pending,
                                           sec_op_t op,
                                           led_event_t event)
{
    screen_view_t v;

    /* L'attente d'abord : c'est le seul ecran qui reclame une action de
     * l'utilisateur, et le rater lui coute une operation. */
    if (confirm_pending) {
        v.kind  = SCREEN_WAIT;
        v.title = "CONFIRMER ?";
        v.line  = screen_op_label(op);
        return v;
    }

    if (event == LED_EVENT_GRANTED || event == LED_EVENT_REFUSED) {
        v.kind  = SCREEN_VERDICT;
        v.title = (event == LED_EVENT_GRANTED) ? "ACCORDE" : "REFUSE";
        v.line  = screen_mode_name(mode);
        return v;
    }

    if (event == LED_EVENT_MODE) {
        v.kind  = SCREEN_SWITCH;
        v.title = screen_mode_name(mode);
        v.line  = "";
        return v;
    }

    v.kind  = SCREEN_IDLE;
    v.title = screen_mode_name(mode);
    v.line  = "";
    return v;
}
```

- [ ] **Étape 4 : constater le vert**

Attendu : sept `OK` dans la suite `screen_view`, `0 échecs`.

- [ ] **Étape 5 : prouver que les tests mordent**

Remplacer temporairement le `default:` de `screen_op_label()` par
`return "Signature OpenPGP";`. Attendu : `test_unknown_op_is_not_mistaken_for_a_known_one`
et `test_every_op_has_a_distinct_label` rougissent, les cinq autres restent
verts. Restaurer à la main (fichier neuf, `git checkout` ne le ramènera pas),
vérifier le vert.

- [ ] **Étape 6 : vérifier et commiter**

`./scripts/check.sh --fast`, puis committer avec `.tripwire-testcount`.

---

### Tâche 3 : `screen_anim.h` — la barre, le décalage, le glissement

**Fichiers :**
- Créer : `main/hmi/screen_anim.h`, `test/test_screen_anim.c`
- Modifier : `test/CMakeLists.txt`, `test/test_main.c`

**Interfaces :**
- Produit : `screen_bar_permille()`, `screen_shift_px()`, `screen_slide_permille()` — consommés par la tâche 4.

- [ ] **Étape 1 : écrire les tests qui échouent**

Créer `test/test_screen_anim.c` :

```c
/* La barre de decompte est la seule animation qui n'est pas decorative : elle
 * rend visibles les 15 s de SEC_CONFIRM_TIMEOUT_MS, aujourd'hui muettes. Deux
 * generations de cles ont echoue le 2026-08-17 sur des expirations que rien
 * n'annoncait. */
#include "test_framework.h"

#include "sec_confirm.h"
#include "hmi/screen_anim.h"

static void test_bar_is_full_at_arming(void)
{
    TEST_ASSERT_EQ(screen_bar_permille(1000, 1000), 1000, "pleine a l'armement");
}

static void test_bar_is_empty_at_deadline(void)
{
    TEST_ASSERT_EQ(screen_bar_permille(1000, 1000 + SEC_CONFIRM_TIMEOUT_MS), 0,
                   "vide a l'echeance");
}

static void test_bar_is_half_at_half_time(void)
{
    const uint32_t half = SEC_CONFIRM_TIMEOUT_MS / 2u;
    const uint16_t p = screen_bar_permille(1000, 1000 + half);
    TEST_ASSERT(p > 480 && p < 520, "environ la moitie a mi-parcours");
}

/* Jamais hors bornes : une barre negative ou au-dela de 100 % se dessine
 * n'importe ou et masque le reste de l'ecran. */
static void test_bar_never_leaves_its_bounds(void)
{
    for (uint32_t d = 0; d <= SEC_CONFIRM_TIMEOUT_MS + 5000u; d += 250u) {
        const uint16_t p = screen_bar_permille(1000, 1000 + d);
        TEST_ASSERT(p <= 1000, "jamais au-dela du plein");
    }
}

/* Le compteur d'ESP-IDF repasse par zero apres ~49 jours ; une cle peut rester
 * branchee plus longtemps. */
static void test_bar_survives_millisecond_wraparound(void)
{
    const uint32_t armed = 0xFFFFF000u;   /* non aligne sur rien */
    TEST_ASSERT_EQ(screen_bar_permille(armed, armed), 1000, "pleine a l'armement");
    TEST_ASSERT_EQ(screen_bar_permille(armed, armed + SEC_CONFIRM_TIMEOUT_MS), 0,
                   "vide a l'echeance, a cheval sur le repassage a zero");
    const uint16_t p = screen_bar_permille(armed, armed + SEC_CONFIRM_TIMEOUT_MS / 2u);
    TEST_ASSERT(p > 480 && p < 520, "et juste au milieu");
}

/* Le decalage anti-marquage ne doit jamais pousser le contenu hors de l'ecran. */
static void test_shift_stays_within_bounds(void)
{
    for (uint32_t t = 0; t < 3600u * 1000u; t += 7919u) {
        const uint8_t s = screen_shift_px(t);
        TEST_ASSERT(s <= SCREEN_SHIFT_MAX_PX, "le decalage reste borne");
    }
}

/* Il doit bouger, sinon il ne sert a rien. */
static void test_shift_actually_moves(void)
{
    bool seen_other = false;
    const uint8_t first = screen_shift_px(0);
    for (uint32_t t = 0; t < 3600u * 1000u; t += 60u * 1000u) {
        if (screen_shift_px(t) != first) { seen_other = true; break; }
    }
    TEST_ASSERT(seen_other, "le decalage change au fil du temps");
}

static void test_slide_runs_from_zero_to_full(void)
{
    TEST_ASSERT_EQ(screen_slide_permille(500, 500), 0, "commence a zero");
    TEST_ASSERT_EQ(screen_slide_permille(500, 500 + SCREEN_SLIDE_MS), 1000, "finit au plein");
    for (uint32_t d = 0; d <= SCREEN_SLIDE_MS + 2000u; d += 25u) {
        TEST_ASSERT(screen_slide_permille(500, 500 + d) <= 1000, "jamais au-dela");
    }
}

void test_screen_anim(void)
{
    TEST_SUITE("screen_anim");
    TEST_RUN(test_bar_is_full_at_arming);
    TEST_RUN(test_bar_is_empty_at_deadline);
    TEST_RUN(test_bar_is_half_at_half_time);
    TEST_RUN(test_bar_never_leaves_its_bounds);
    TEST_RUN(test_bar_survives_millisecond_wraparound);
    TEST_RUN(test_shift_stays_within_bounds);
    TEST_RUN(test_shift_actually_moves);
    TEST_RUN(test_slide_runs_from_zero_to_full);
}
```

- [ ] **Étape 2 : constater le rouge**

Attendu : `hmi/screen_anim.h: No such file or directory`.

- [ ] **Étape 3 : écrire l'implémentation**

Créer `main/hmi/screen_anim.h` :

```c
#pragma once

/*
 * screen_anim — les trois animations de l'ecran, en logique pure.
 *
 * Une seule n'est pas decorative : la barre de decompte. Elle rend visibles les
 * quinze secondes de SEC_CONFIRM_TIMEOUT_MS, qui ne se voient nulle part
 * ailleurs — le 2026-08-17, deux generations de cles ont echoue sur des
 * expirations que rien n'annoncait.
 *
 * Toute l'arithmetique de temps est en uint32_t non signe : la difference reste
 * juste au repassage a zero du compteur de millisecondes, qui survient apres
 * ~49 jours et qu'une cle branchee en permanence atteindra.
 *
 * Les proportions sont en pour mille et non en pixels : le module ne connait
 * pas la geometrie de l'ecran, c'est screen.c qui la connait.
 */

#include <stdbool.h>
#include <stdint.h>

#include "sec_confirm.h"

/* Decalage anti-marquage : les OLED gardent une trace permanente d'un contenu
 * statique, et cette cle peut rester branchee des journees. */
#define SCREEN_SHIFT_MAX_PX   4u
#define SCREEN_SHIFT_PERIOD_MS (60u * 1000u)   /* un pas par minute */
#define SCREEN_SLIDE_MS       400u

/* Proportion restante de la duree de confirmation, en pour mille : 1000 a
 * l'armement, 0 a l'echeance. Bornee : une barre hors bornes se dessinerait
 * n'importe ou. */
static inline uint16_t screen_bar_permille(uint32_t armed_at_ms, uint32_t now_ms)
{
    const uint32_t elapsed = now_ms - armed_at_ms;   /* juste au repassage a zero */
    if (elapsed >= SEC_CONFIRM_TIMEOUT_MS) return 0;
    return (uint16_t)(((uint64_t)(SEC_CONFIRM_TIMEOUT_MS - elapsed) * 1000u)
                      / SEC_CONFIRM_TIMEOUT_MS);
}

/* Decalage vertical courant, en pixels, borne a SCREEN_SHIFT_MAX_PX. */
static inline uint8_t screen_shift_px(uint32_t now_ms)
{
    return (uint8_t)((now_ms / SCREEN_SHIFT_PERIOD_MS) % (SCREEN_SHIFT_MAX_PX + 1u));
}

/* Avancement du glissement de bascule, en pour mille. */
static inline uint16_t screen_slide_permille(uint32_t started_ms, uint32_t now_ms)
{
    const uint32_t elapsed = now_ms - started_ms;
    if (elapsed >= SCREEN_SLIDE_MS) return 1000;
    return (uint16_t)(((uint64_t)elapsed * 1000u) / SCREEN_SLIDE_MS);
}
```

- [ ] **Étape 4 : constater le vert**

Attendu : huit `OK` dans la suite `screen_anim`.

- [ ] **Étape 5 : prouver que les tests mordent**

Deux mutations **séparées**, chacune rapportée avec ses rouges et ses verts :

1. Dans `screen_bar_permille()`, retirer la garde `if (elapsed >= …) return 0;`.
   Attendu : `test_bar_is_empty_at_deadline` et `test_bar_never_leaves_its_bounds`
   rougissent.
2. Dans `screen_shift_px()`, retourner `0` en dur. Attendu :
   `test_shift_actually_moves` rougit **seul** — `test_shift_stays_within_bounds`
   reste vert, ce qui montre qu'une borne sans mouvement passerait sans le
   second test.

Restaurer à la main entre les deux (fichier neuf), vérifier le vert.

- [ ] **Étape 6 : vérifier et commiter**

---

### Tâche 4 : `screen.c` — le pilote et la tâche d'affichage

Livrable vérifiable seul : l'écran s'allume et affiche le mode, sans qu'aucun
appui ne soit encore raté.

**Fichiers :**
- Créer : `main/hmi/screen.h`, `main/hmi/screen.c`
- Modifier : `main/hmi/hmi.h`, `main/hmi/hmi.c` (publication de l'instantané)
- Modifier : `boards/wt9932_key/board.h`, `main/board_common.h` (assertions)
- Modifier : `main/CMakeLists.txt`, `main/main.c`

**Interfaces :**
- Consomme : `screen_view_of()` (tâche 2), `screen_bar_permille()`/`screen_shift_px()`/`screen_slide_permille()` (tâche 3), `sec_confirm_armed_op()` (tâche 1).
- Produit : `esp_err_t screen_init(void)`, `hmi_snapshot_t`, `void hmi_snapshot(hmi_snapshot_t *out)`.

- [ ] **Étape 1 : déclarer le brochage**

Dans `boards/wt9932_key/board.h`, à côté des boutons :

```c
/* ------------------------------------------------------------------------- */
/* Ecran OLED SSD1306, I2C.                                                   */
/* ------------------------------------------------------------------------- */

/*
 * IO53/IO54 ne sont pas des pins de strapping et n'ont aucune restriction : la
 * colonne « Comments » de la table GPIO du P4 est vide pour les deux. Elles
 * portent des fonctions analogiques (ADC2_CH4/CH5, comparateur canal 1) dont ce
 * projet n'a aucun usage — meme famille qu'IO51, deja prise par la LED.
 *
 * Cablage utilisateur, sorties sur J6. Les pull-ups sont ceux du module SSD1306.
 */
#define BOARD_OLED_SCL        GPIO_NUM_53
#define BOARD_OLED_SDA        GPIO_NUM_54
#define BOARD_OLED_ADDR       0x3Cu
#define BOARD_OLED_WIDTH      128u
#define BOARD_OLED_HEIGHT     64u
```

Dans `main/board_common.h`, à côté des assertions des boutons :

```c
#if defined(BOARD_OLED_SCL)
_Static_assert(!BOARD_PIN_IS_RESERVED(BOARD_OLED_SCL),
               "BOARD_OLED_SCL empiete sur un pin reserve");
_Static_assert(!BOARD_PIN_IS_RESERVED(BOARD_OLED_SDA),
               "BOARD_OLED_SDA empiete sur un pin reserve");
_Static_assert(BOARD_OLED_SCL != BOARD_OLED_SDA,
               "SCL et SDA sur la meme broche");
#endif
```

- [ ] **Étape 2 : publier l'instantané depuis `hmi.c`**

Dans `main/hmi/hmi.h` :

```c
/*
 * Ce que la tache d'affichage a besoin de savoir. Publie par la tache IHM sous
 * verrou, lu par la tache ecran : deux contextes, donc un instantane copie d'un
 * coup plutot que quatre champs lus separement — une lecture dechiree
 * afficherait une operation avec l'echeance d'une autre.
 *
 * Le verrou ne protege QUE la copie de cette structure. Il n'est jamais tenu
 * pendant un transfert I2C.
 */
typedef struct {
    usb_mode_t  mode;
    bool        confirm_pending;
    uint32_t    armed_at_ms;
    sec_op_t    op;
    led_event_t event;
    uint32_t    event_at_ms;
} hmi_snapshot_t;

void hmi_snapshot(hmi_snapshot_t *out);
```

Dans `main/hmi/hmi.c` : un `hmi_snapshot_t` statique et un mutex statique
(modèle `sd_card.c`), écrits en fin de chaque tick de `hmi_task`, lus par
`hmi_snapshot()`. `op` vient de `sec_confirm_armed_op()`.

**Mettre à jour le commentaire de tête de `hmi.h`** : il énumère ce que `hmi.c`
décide encore ; la publication de l'instantané en fait partie.

- [ ] **Étape 3 : écrire le pilote et la tâche**

Créer `main/hmi/screen.h` (déclaration de `screen_init()`, avec le même contrat
que `hmi_init()` : une erreur n'est pas fatale, la clé reste utilisable), puis
`main/hmi/screen.c` :

- initialisation `i2c_master` sur `BOARD_OLED_SCL`/`BOARD_OLED_SDA` à 400 kHz ;
- séquence d'initialisation SSD1306 (`0xAE` display off, `0x20 0x00` mode
  horizontal, `0xA1` remap segment, `0xC8` scan inversé, `0x8D 0x14` charge
  pump, `0xAF` display on) ;
- un tampon de trame de `BOARD_OLED_WIDTH * BOARD_OLED_HEIGHT / 8` octets ;
- une police 6×8 minimale (chiffres, majuscules, minuscules, espace, `?`) ;
- `xTaskCreate` d'une tâche `screen` à 3072 octets, priorité **4** — une de
  moins que la tâche IHM, qui doit garder la main sur l'échantillonnage ;
- boucle : `hmi_snapshot()`, `screen_view_of()`, calcul des animations, dessin
  dans le tampon, envoi I²C, `vTaskDelay(pdMS_TO_TICKS(50))` (20 images/s).

**Aucune décision dans ce fichier** : quel écran, quel texte, quelle proportion —
tout vient des trois en-têtes purs. Il choisit seulement *où* poser les pixels.

Une erreur I²C se journalise et l'image est sautée ; jamais de blocage.

- [ ] **Étape 4 : brancher**

Dans `main/CMakeLists.txt`, ajouter `"hmi/screen.c"` à `srcs` et
`esp_driver_i2c` à `REQUIRES`. Dans `main/main.c`, appeler `screen_init()`
après `hmi_init()`, avec le même traitement d'erreur non fatal.

`screen.c` doit être un no-op complet sur une carte sans écran : entourer son
contenu de `#if defined(BOARD_OLED_SCL)` / `#else` (implémentation vide) /
`#endif`, comme `hmi.c` le fait pour les boutons.

- [ ] **Étape 5 : vérifier la compilation sur les trois cartes**

```bash
source ~/esp/esp-idf/export.sh && ./scripts/check.sh
```
Puis vérifier que rien d'I²C n'entre dans le firmware du coffre :

```bash
nm build_niphar_chest/esp-idf/main/CMakeFiles/__idf_main.dir/hmi/screen.c.obj | grep -c i2c
```
Attendu : `0`.

- [ ] **Étape 6 : commiter**

---

### Tâche 5 : validation sur la carte, et les documents

**Fichiers :**
- Modifier : `docs/HARDWARE.md`

⚠️ **Le port, jamais en glob** — `/dev/ttyACM0` est le clavier de la
propriétaire :
```
/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_30:ED:A0:E0:BC:5F-if00
```
Confirmer la puce par `chip_id` (`MAC: 30:ed:a0:e0:bc:5f`) avant tout flash.

- [ ] **Étape 1 : flasher et dérouler le protocole**

Noter chaque résultat, y compris les échecs, et **dire pour chacun s'il vient
d'un appui réel ou d'un déclenchement par la console** :

1. démarrage : l'écran s'allume, affiche « Au repos », la clé reste muette sur l'USB ;
2. appui MODE : l'écran affiche « OpenPGP », la LED passe au bleu ;
3. `gpg` demande une signature : l'écran affiche **« CONFIRMER ? / Signature OpenPGP »** et la barre se vide ;
4. appui CONFIRM pendant la barre : « ACCORDE », la signature aboutit ;
5. ne pas appuyer : la barre se vide entièrement, « REFUSE », `gpg` échoue sur `6985` ;
6. **la barre continue de se vider pendant que le firmware calcule** — c'est la raison d'être de la tâche séparée, donc le point à vérifier en priorité ;
7. aucun appui raté pendant les animations : enchaîner dix bascules de mode et vérifier que les dix sont prises.

- [ ] **Étape 2 : consigner sans embellir**

Ajouter à `docs/HARDWARE.md` une sous-section datée, sur le modèle des
précédentes. Y écrire aussi ce qui **n'a pas** été validé. Si un résultat
surprend et que la cause n'est pas établie, **écrire que la cause n'est pas
établie** — ne pas inventer de mécanisme.

- [ ] **Étape 3 : `./scripts/check.sh` complet, puis commiter**

---

## Auto-relecture

**Couverture de la spec.** Code d'opération → tâche 1 ; contenu des écrans →
tâche 2 ; barre, décalage anti-marquage, glissement → tâche 3 ; tâche séparée,
pilote, instantané sous verrou, brochage et assertions → tâche 4 ; validation
matérielle et documents → tâche 5. La répartition des rôles LED/écran est
réalisée par construction : la tâche 4 ne touche pas à `led_state.h`.

**Cohérence des noms.** `sec_op_t` et `sec_confirm_armed_op()` sont produits en
tâche 1 et consommés en 2 et 4. `screen_view_of()` produit en 2, consommé en 4.
`screen_bar_permille()`, `screen_shift_px()`, `screen_slide_permille()` produits
en 3, consommés en 4. `hmi_snapshot_t`/`hmi_snapshot()` produits et consommés en
tâche 4. Aucun nom ne diverge entre sa définition et son usage.

**Ce que le plan ne couvre pas**, et c'est voulu : le nom du site demandeur pour
FIDO2 (pas de FIDO2), et le TOTP (fonction distincte, bloquée par l'absence
d'horloge).
