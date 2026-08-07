# Portage CCID + OTP-HID — plan d'implémentation

> **Pour les workers agentiques :** SOUS-SKILL REQUISE — utiliser
> `superpowers:subagent-driven-development` (recommandé) ou
> `superpowers:executing-plans` pour dérouler ce plan tâche par tâche. Les
> étapes utilisent la syntaxe case à cocher (`- [ ]`).

**But :** porter la pile OpenPGP CCID et CR-HMAC OTP-HID de `KeSp_firmware`
vers le coffre, en composite avec le MSC existant, avec une source de
confirmation utilisable sur le kit de dev et impossible à embarquer sur le
coffre.

**Architecture :** copie quasi verbatim de `~/Documents/GitHub/KeSp_firmware/main/security/`
et de `main/sys/nvs_utils.*`, plus deux modules neufs — `sec_config.h` qui
remplace l'unique emprunt au clavier, et `sec_gate` qui fournit l'appui de
confirmation. Les tests hôte existants sont portés avec le code.

**Pile technique :** ESP-IDF 5.5.2, TinyUSB brut (`espressif/tinyusb`), mbedtls
(ECDH/ECDSA/ECP/MD), NVS, harnais de tests hôte CMake déjà en place.

Conception : `docs/superpowers/specs/2026-08-07-portage-ccid-otp-design.md`.

## Contraintes globales

- **Modifier le moins possible les fichiers copiés.** Un fichier qui diverge de
  son original perd le bénéfice d'avoir été éprouvé sur matériel, et les
  correctifs futurs de KeSp ne s'y reporteront plus. Toute divergence délibérée
  se documente en tête de fichier, en disant pourquoi.
- **Style d'include :** les fichiers copiés s'incluent à plat (`#include "apdu.h"`).
  On ajoute `security` et `sys` à `INCLUDE_DIRS` plutôt que de réécrire les
  includes. Deux styles coexisteront donc dans le projet — le style qualifié
  (`#include "storage/sd_card.h"`) pour le code du coffre, le style plat pour le
  code porté. C'est assumé : la seule alternative diverge de l'amont.
- **`GPIO24/25` et `GPIO35` restent interdits** hors des en-têtes de carte, et
  **aucun `esp_deep_sleep_start`**. `scripts/fast.sh` le vérifie.
- **Aucune option Secure Boot ni Flash Encryption** dans les defaults partagés.
  `scripts/fast.sh` le vérifie. Le kit reste un kit.
- **Après chaque tâche :** `./scripts/check.sh` doit être vert, et le ratchet
  `.tripwire-testcount` doit avoir monté quand la tâche apporte des tests.
- Commentaires et messages de commit en français, comme le reste du dépôt.
- Source de vérité pour les copies :
  `KESP=~/Documents/GitHub/KeSp_firmware`

---

### Tâche 1 : socle partagé — `main/sys/`, CRC déplacé, faux NVS

**Fichiers :**
- Créer : `main/sys/nvs_utils.c`, `main/sys/nvs_utils.h`
- Déplacer : `main/link/cr_crc16.{c,h}` → `main/sys/cr_crc16.{c,h}`
- Créer : `test/nvs_fake.c`, `test/nvs_fake.h`, `test/nvs.h`, `test/nvs_flash.h`
- Créer : `test/test_cr_crc16.c`
- Modifier : `main/CMakeLists.txt`, `test/CMakeLists.txt`, `test/test_main.c`,
  `main/link/link_proto.c` (include du CRC)

**Interfaces :**
- Consomme : rien.
- Produit : `uint16_t cr_crc16(const uint8_t *data, uint16_t len)` accessible
  par `#include "cr_crc16.h"` ; `esp_err_t nvs_save_blob_with_total(const char
  *ns, const char *blob_key, const void *blob, size_t blob_size, const char
  *total_key, uint32_t total)` et `esp_err_t nvs_load_blob_with_total(const char
  *ns, const char *blob_key, void *blob, size_t blob_size, const char
  *total_key, uint32_t *total)` par `#include "nvs_utils.h"`.

- [ ] **Étape 1 : déplacer le CRC et copier le socle**

```bash
KESP=~/Documents/GitHub/KeSp_firmware
mkdir -p main/sys
git mv main/link/cr_crc16.c main/sys/cr_crc16.c
git mv main/link/cr_crc16.h main/sys/cr_crc16.h
cp "$KESP/main/sys/nvs_utils.c" "$KESP/main/sys/nvs_utils.h" main/sys/
cp "$KESP/test/nvs_fake.c" "$KESP/test/nvs_fake.h" \
   "$KESP/test/nvs.h" "$KESP/test/nvs_flash.h" test/
cp "$KESP/test/test_cr_crc16.c" test/
```

- [ ] **Étape 2 : corriger l'include du CRC dans `link_proto.c`**

Remplacer `#include "link/cr_crc16.h"` par `#include "cr_crc16.h"`.
Dans `main/sys/cr_crc16.c`, remplacer `#include "link/cr_crc16.h"` par
`#include "cr_crc16.h"`.

Ajouter en tête de `main/sys/cr_crc16.h`, sous le commentaire existant :

```c
/* Déplacé de main/link/ vers main/sys/ le 2026-08-07 : la pile CCID portée
 * depuis KeSp s'en sert aussi, et deux copies divergentes entre volets
 * seraient exactement le bug silencieux que ce fichier prétend éviter. */
```

- [ ] **Étape 3 : ouvrir les chemins d'include du firmware**

Dans `main/CMakeLists.txt`, remplacer la ligne `INCLUDE_DIRS "${BOARD_DIR}" "."` par :

```cmake
    # "sys" et "security" à plat : les fichiers portés depuis KeSp s'incluent
    # ainsi, et les laisser verbatim vaut mieux que réécrire leurs includes à
    # chaque synchronisation amont.
    INCLUDE_DIRS "${BOARD_DIR}" "." "sys" "security"
```

- [ ] **Étape 4 : câbler les tests**

Dans `test/CMakeLists.txt`, ajouter aux sources de `test_runner` :

```cmake
    test_cr_crc16.c
    nvs_fake.c
    ../main/sys/cr_crc16.c
```

et remplacer `../main/link/cr_crc16.c` (qui n'existe plus).

Dans `test/test_main.c`, ajouter la déclaration et l'appel :

```c
extern void test_cr_crc16(void);
```
```c
    test_cr_crc16();
```

- [ ] **Étape 5 : vérifier**

Run : `./scripts/check.sh --fast --force`
Attendu : VERT, et le compte de tests passe de 84 à 86.

- [ ] **Étape 6 : commit**

```bash
git add -A
git commit -m "socle : nvs_utils et cr_crc16 partagés, faux NVS pour les tests"
```

---

### Tâche 2 : `sec_confirm` — le verrou, et ses tests

**Fichiers :**
- Créer : `main/security/sec_confirm.c`, `main/security/sec_confirm.h`
- Créer : `test/test_sec_confirm.c`
- Modifier : `main/CMakeLists.txt`, `test/CMakeLists.txt`, `test/test_main.c`

**Interfaces :**
- Consomme : rien.
- Produit : `void sec_confirm_reset(void)`, `void sec_confirm_arm(uint8_t slot,
  uint32_t now_ms)`, `void sec_confirm_authorize(void)`,
  `sec_confirm_state_t sec_confirm_poll(uint32_t now_ms, uint8_t *out_slot)`.
  États : `SEC_CONFIRM_IDLE`, `SEC_CONFIRM_PENDING`, `SEC_CONFIRM_AUTHORIZED`,
  `SEC_CONFIRM_TIMEDOUT`. Délai : `SEC_CONFIRM_TIMEOUT_MS` (15000).

- [ ] **Étape 1 : copier le module et ses tests**

```bash
KESP=~/Documents/GitHub/KeSp_firmware
mkdir -p main/security
cp "$KESP/main/security/sec_confirm.c" "$KESP/main/security/sec_confirm.h" main/security/
cp "$KESP/test/test_sec_confirm.c" test/
```

Aucune modification des fichiers copiés : `sec_confirm` est pur, sans NVS ni
matériel, et son en-tête documente déjà son modèle de concurrence.

- [ ] **Étape 2 : lancer les tests avant de câbler quoi que ce soit**

Ajouter dans `test/CMakeLists.txt` : `test_sec_confirm.c` et
`../main/security/sec_confirm.c`. Dans `test/test_main.c` : la déclaration
`extern void test_sec_confirm(void);` et l'appel `test_sec_confirm();`.

Run : `cmake --build test/build && ./test/build/test_runner`
Attendu : PASS, 11 assertions de plus.

- [ ] **Étape 3 : prouver que les tests mordent**

Dans `main/security/sec_confirm.c`, dans `sec_confirm_authorize()`, supprimer
temporairement la condition qui exige l'état `PENDING` — la fonction doit
devenir un `AUTHORIZED` inconditionnel.

Run : `./test/build/test_runner`
Attendu : ÉCHEC sur `authorize w/o arm = no-op`.

Rétablir le fichier :

```bash
git checkout main/security/sec_confirm.c 2>/dev/null || cp ~/Documents/GitHub/KeSp_firmware/main/security/sec_confirm.c main/security/
```

Run : `./test/build/test_runner`
Attendu : PASS.

- [ ] **Étape 4 : ajouter au firmware**

Dans `main/CMakeLists.txt`, ajouter `"security/sec_confirm.c"` aux `srcs`.

Run : `./scripts/check.sh --fast --force`
Attendu : VERT, compte de tests à 97.

- [ ] **Étape 5 : commit**

```bash
git add -A
git commit -m "sécurité : sec_confirm porté depuis KeSp, avec ses tests"
```

---

### Tâche 3 : `sec_store` — les emplacements de secrets

**Fichiers :**
- Créer : `main/security/sec_store.c`, `main/security/sec_store.h`,
  `main/security/sec_config.h`
- Créer : `test/test_sec_store.c`
- Modifier : `main/CMakeLists.txt`, `test/CMakeLists.txt`, `test/test_main.c`

**Interfaces :**
- Consomme : `nvs_save_blob_with_total`, `nvs_load_blob_with_total` (tâche 1).
- Produit : `void sec_store_init(void)`, `bool sec_store_set_slot(uint8_t idx,
  uint8_t type, const char *label, const uint8_t *secret, uint8_t secret_len)`,
  `bool sec_store_clear_slot(uint8_t idx)`, `uint8_t sec_store_count(void)`.
  Constantes : `SEC_N_SLOTS` (4), `SEC_LABEL_LEN` (16), `SEC_SECRET_MAX` (64).

- [ ] **Étape 1 : copier, et créer le seul fichier qui remplace le clavier**

```bash
KESP=~/Documents/GitHub/KeSp_firmware
cp "$KESP/main/security/sec_store.c" "$KESP/main/security/sec_store.h" main/security/
cp "$KESP/test/test_sec_store.c" test/
```

Créer `main/security/sec_config.h` :

```c
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
```

- [ ] **Étape 2 : la seule divergence de cette tâche**

Dans `main/security/sec_store.c`, remplacer `#include "keyboard_config.h"` par
`#include "sec_config.h"`, et remplacer le commentaire de fin de ligne par :

```c
#include "sec_config.h"   /* STORAGE_NAMESPACE — divergence assumée vs KeSp */
```

- [ ] **Étape 3 : câbler les tests et vérifier**

`test/CMakeLists.txt` : ajouter `test_sec_store.c` et
`../main/security/sec_store.c`. `test/test_main.c` : déclaration et appel de
`test_sec_store`.

Run : `cmake --build test/build && ./test/build/test_runner`
Attendu : PASS, 18 assertions de plus (115 au total).

- [ ] **Étape 4 : ajouter au firmware et vérifier**

`main/CMakeLists.txt` : ajouter `"security/sec_store.c"` aux `srcs` et `nvs_flash`
aux `REQUIRES`.

Run : `./scripts/check.sh --fast --force`
Attendu : VERT.

- [ ] **Étape 5 : commit**

```bash
git add -A
git commit -m "sécurité : sec_store porté, sec_config remplace keyboard_config"
```

---

### Tâche 4 : `apdu` — la nouvelle surface d'attaque

**Fichiers :**
- Créer : `main/security/apdu.c`, `main/security/apdu.h`
- Créer : `test/test_apdu.c`
- Modifier : `main/CMakeLists.txt`, `test/CMakeLists.txt`, `test/test_main.c`

**Interfaces :**
- Consomme : rien.
- Produit : les fonctions de parsing déclarées dans `main/security/apdu.h`,
  telles que copiées — les lire avant d'écrire le code appelant.

C'est le module qui interprète des octets arbitraires venant de l'hôte comme
des commandes. Ses 29 assertions sont la raison pour laquelle il est porté avant
tout ce qui s'en sert.

- [ ] **Étape 1 : copier**

```bash
KESP=~/Documents/GitHub/KeSp_firmware
cp "$KESP/main/security/apdu.c" "$KESP/main/security/apdu.h" main/security/
cp "$KESP/test/test_apdu.c" test/
```

- [ ] **Étape 2 : câbler les tests**

`test/CMakeLists.txt` : `test_apdu.c` et `../main/security/apdu.c`.
`test/test_main.c` : déclaration et appel de `test_apdu`.

Run : `cmake --build test/build && ./test/build/test_runner`
Attendu : PASS, 29 assertions de plus (144 au total).

- [ ] **Étape 3 : prouver que les tests mordent**

Dans `main/security/apdu.c`, ligne 37, neutraliser le contrôle de troncature :

```c
    if (0) return false; /* BUG TRANSITOIRE — était : (uint32_t)len < data_end */
```

C'est le contrôle qui empêche d'interpréter une APDU tronquée comme complète —
donc de lire au-delà du tampon fourni par l'hôte.

Run : `./test/build/test_runner`
Attendu : ÉCHEC sur au moins une assertion de `test_apdu`.

Rétablir :

```bash
cp ~/Documents/GitHub/KeSp_firmware/main/security/apdu.c main/security/
```

- [ ] **Étape 4 : ajouter au firmware et vérifier**

`main/CMakeLists.txt` : ajouter `"security/apdu.c"`.

Run : `./scripts/check.sh --fast --force`
Attendu : VERT.

- [ ] **Étape 5 : commit**

```bash
git add -A
git commit -m "sécurité : apdu porté — le parsing que l'hôte pilote"
```

---

### Tâche 5 : `otp_proto` et `cr_hmac`

**Fichiers :**
- Créer : `main/security/otp_proto.c`, `main/security/otp_proto.h`,
  `main/security/cr_hmac.c`, `main/security/cr_hmac.h`
- Créer : `test/test_otp_proto.c`
- Modifier : `main/CMakeLists.txt`, `test/CMakeLists.txt`, `test/test_main.c`

**Interfaces :**
- Consomme : `cr_crc16` (tâche 1).
- Produit : les fonctions déclarées dans `otp_proto.h` et `cr_hmac.h`, telles
  que copiées.

- [ ] **Étape 1 : copier**

```bash
KESP=~/Documents/GitHub/KeSp_firmware
cp "$KESP/main/security/otp_proto.c" "$KESP/main/security/otp_proto.h" \
   "$KESP/main/security/cr_hmac.c" "$KESP/main/security/cr_hmac.h" main/security/
cp "$KESP/test/test_otp_proto.c" test/
```

- [ ] **Étape 2 : câbler, vérifier, commiter**

`test/CMakeLists.txt` : `test_otp_proto.c` et `../main/security/otp_proto.c`.
`test/test_main.c` : déclaration et appel de `test_otp_proto`.
`main/CMakeLists.txt` : `"security/otp_proto.c"` et `"security/cr_hmac.c"`,
plus `mbedtls` aux `REQUIRES`.

Run : `./scripts/check.sh --fast --force`
Attendu : VERT, 5 assertions de plus (149 au total).

```bash
git add -A
git commit -m "sécurité : otp_proto et cr_hmac portés"
```

---

### Tâche 6 : `openpgp_do` — les objets de données de la carte

**Fichiers :**
- Créer : `main/security/openpgp_do.c`, `main/security/openpgp_do.h`
- Créer : `test/test_openpgp_do.c`
- Modifier : `main/CMakeLists.txt`, `test/CMakeLists.txt`, `test/test_main.c`

**Interfaces :**
- Consomme : `nvs_save_blob_with_total`, `nvs_load_blob_with_total` (tâche 1),
  `STORAGE_NAMESPACE` (tâche 3).
- Produit : les fonctions déclarées dans `openpgp_do.h`.

- [ ] **Étape 1 : copier et corriger l'unique include**

```bash
KESP=~/Documents/GitHub/KeSp_firmware
cp "$KESP/main/security/openpgp_do.c" "$KESP/main/security/openpgp_do.h" main/security/
cp "$KESP/test/test_openpgp_do.c" test/
```

Dans `main/security/openpgp_do.c`, remplacer `#include "keyboard_config.h"` par :

```c
#include "sec_config.h"   /* STORAGE_NAMESPACE — divergence assumée vs KeSp */
```

- [ ] **Étape 2 : câbler, vérifier, commiter**

`test/CMakeLists.txt` : `test_openpgp_do.c` et `../main/security/openpgp_do.c`.
`test/test_main.c` : déclaration et appel. `main/CMakeLists.txt` :
`"security/openpgp_do.c"`.

Run : `./scripts/check.sh --fast --force`
Attendu : VERT, 12 assertions de plus (161 au total).

```bash
git add -A
git commit -m "sécurité : openpgp_do porté"
```

---

### Tâche 7 : `openpgp_card` et `openpgp_crypto` — le cœur

**Fichiers :**
- Créer : `main/security/openpgp_card.c`, `main/security/openpgp_card.h`,
  `main/security/openpgp_crypto.c`, `main/security/openpgp_crypto.h`
- Créer : `test/test_openpgp_card.c`
- Modifier : `main/CMakeLists.txt`, `test/CMakeLists.txt`, `test/test_main.c`,
  `sdkconfig.defaults`

**Interfaces :**
- Consomme : `apdu` (tâche 4), `openpgp_do` (tâche 6), `sec_confirm` (tâche 2),
  `STORAGE_NAMESPACE` (tâche 3).
- Produit : les fonctions déclarées dans `openpgp_card.h` et
  `openpgp_crypto.h`, dont le point d'entrée de traitement d'APDU qu'utilisera
  `ccid.c` en tâche 9.

254 assertions : c'est le module le mieux couvert de tout le portage, et le plus
gros. Il vient en une seule tâche parce que `openpgp_card` et `openpgp_crypto`
ne se compilent pas l'un sans l'autre.

- [ ] **Étape 1 : copier et corriger l'unique include**

```bash
KESP=~/Documents/GitHub/KeSp_firmware
cp "$KESP/main/security/openpgp_card.c" "$KESP/main/security/openpgp_card.h" \
   "$KESP/main/security/openpgp_crypto.c" "$KESP/main/security/openpgp_crypto.h" main/security/
cp "$KESP/test/test_openpgp_card.c" test/
```

Dans `main/security/openpgp_card.c`, remplacer `#include "keyboard_config.h"` par :

```c
#include "sec_config.h"   /* STORAGE_NAMESPACE — divergence assumée vs KeSp */
```

- [ ] **Étape 2 : activer les courbes elliptiques**

Ajouter à `sdkconfig.defaults` :

```
# --- Cryptographie (pile OpenPGP portée de KeSp) --------------------------
# ECC uniquement : les clés de la carte sont sur courbes elliptiques, et
# activer RSA gonflerait le binaire pour rien.
CONFIG_MBEDTLS_ECDH_C=y
CONFIG_MBEDTLS_ECDSA_C=y
CONFIG_MBEDTLS_ECP_DP_SECP256R1_ENABLED=y
CONFIG_MBEDTLS_ECP_DP_CURVE25519_ENABLED=y
```

- [ ] **Étape 3 : câbler les tests**

`test/CMakeLists.txt` : `test_openpgp_card.c` et `../main/security/openpgp_card.c`.
`test/test_main.c` : déclaration et appel de `test_openpgp_card`.

Run : `cmake --build test/build && ./test/build/test_runner`
Attendu : PASS, 254 assertions de plus (415 au total).

Si des symboles de `openpgp_crypto.c` manquent à l'édition de liens hôte,
ajouter `../main/security/openpgp_crypto.c` aux sources de test — mais **ne pas
l'ajouter s'il n'est pas nécessaire** : il dépend de mbedtls, indisponible sur
l'hôte, et un test qui tire mbedtls n'est plus un test de logique pure.

- [ ] **Étape 4 : ajouter au firmware et vérifier**

`main/CMakeLists.txt` : `"security/openpgp_card.c"` et
`"security/openpgp_crypto.c"`.

Run : `./scripts/check.sh` (complet — le binaire grossit, il faut voir les deux
builds passer et la place restante en flash)
Attendu : VERT.

- [ ] **Étape 5 : commit**

```bash
git add -A
git commit -m "sécurité : openpgp_card et openpgp_crypto portés, ECC activé"
```

---

### Tâche 8 : `sec_gate` — la source de confirmation, verrouillée par construction

**Fichiers :**
- Créer : `main/security/sec_gate.c`, `main/security/sec_gate.h`
- Modifier : `main/console/console.c`, `main/CMakeLists.txt`, `scripts/fast.sh`
- Test : `test/test_sec_gate.c`

**Interfaces :**
- Consomme : `sec_confirm_authorize()` (tâche 2).
- Produit : `esp_err_t sec_gate_init(void)` et `const char *sec_gate_source(void)`
  — cette dernière rend une chaîne lisible pour la console, jamais NULL.

C'est la seule pièce vraiment neuve du portage, et celle qui porte le risque :
une béquille de développement qui partirait en production serait
indistinguable, à l'usage, d'un dispositif qui fonctionne.

- [ ] **Étape 1 : écrire le test d'abord**

Créer `test/test_sec_gate.c` :

```c
/* La béquille de confirmation ne doit exister que sur une carte SANS lien.
 * Ce test ne vérifie pas du code : il vérifie une décision de compilation,
 * et c'est justement ce qui peut se perdre en silence. */
#include "test_framework.h"

#include "sec_confirm.h"

/* Rejoue ce que fera sec_gate côté kit : un appui simulé n'accorde rien s'il
 * n'y a pas d'opération armée. La béquille ne doit pas être plus permissive
 * que la vraie touche. */
static void test_stub_is_not_more_permissive_than_a_key(void)
{
    sec_confirm_reset();
    sec_confirm_authorize();
    TEST_ASSERT_EQ(sec_confirm_poll(0, NULL), SEC_CONFIRM_IDLE,
                   "confirmation hors contexte sans effet");

    sec_confirm_arm(1, 1000);
    sec_confirm_authorize();
    uint8_t slot = 0xFF;
    TEST_ASSERT_EQ(sec_confirm_poll(1100, &slot), SEC_CONFIRM_AUTHORIZED,
                   "confirmation après armement accordée");
    TEST_ASSERT_EQ(slot, 1, "emplacement conservé");
}

/* Une confirmation ne sert qu'une fois : rejouer l'appui ne doit pas
 * ré-autoriser une seconde opération. */
static void test_confirmation_is_single_use(void)
{
    sec_confirm_reset();
    sec_confirm_arm(2, 1000);
    sec_confirm_authorize();
    TEST_ASSERT_EQ(sec_confirm_poll(1100, NULL), SEC_CONFIRM_AUTHORIZED, "premier usage");
    TEST_ASSERT_EQ(sec_confirm_poll(1200, NULL), SEC_CONFIRM_IDLE, "consommée");
}

void test_sec_gate(void)
{
    TEST_SUITE("sec_gate");
    TEST_RUN(test_stub_is_not_more_permissive_than_a_key);
    TEST_RUN(test_confirmation_is_single_use);
}
```

`test/CMakeLists.txt` : ajouter `test_sec_gate.c`. `test/test_main.c` :
déclaration et appel.

- [ ] **Étape 2 : lancer les tests**

Run : `cmake --build test/build && ./test/build/test_runner`
Attendu : PASS (le module testé, `sec_confirm`, est déjà là depuis la tâche 2 ;
ce test verrouille le contrat que `sec_gate` ne doit pas contourner).

- [ ] **Étape 3 : écrire `sec_gate.h`**

```c
#pragma once

/*
 * Source de l'appui de confirmation.
 *
 * sec_confirm arme et expire ; il ne sait pas d'où vient l'appui. Ce module le
 * lui fournit, et son implémentation dépend de la carte :
 *
 *   niphar_chest  le lien SPI avec le clavier — pas encore écrit, donc toute
 *                 opération expire au bout de SEC_CONFIRM_TIMEOUT_MS. Refuser
 *                 est le comportement honnête.
 *   jc_devkit     une commande console, pour éprouver la pile sur le kit.
 *
 * La variante console n'est PAS une option de configuration : elle est liée à
 * l'absence de lien, donc absente du firmware du coffre par construction. Une
 * confirmation qu'on peut accorder sans geste physique est indistinguable,
 * à l'usage, d'un dispositif qui fonctionne — c'est exactement ce qu'il ne
 * faut pas rendre possible.
 */

#include "esp_err.h"

esp_err_t sec_gate_init(void);

/* Chaîne lisible décrivant d'où viendra l'appui. Jamais NULL. */
const char *sec_gate_source(void);
```

- [ ] **Étape 4 : écrire `sec_gate.c`**

```c
#include "sec_gate.h"

#include "esp_log.h"

#include "board.h"
#include "sec_confirm.h"

static const char *TAG = "sec_gate";

#if BOARD_LINK_AVAILABLE

esp_err_t sec_gate_init(void)
{
    /* La source réelle est le lien SPI. Tant qu'il n'est pas écrit, aucune
     * confirmation n'est accordée et les opérations expirent. */
    ESP_LOGW(TAG, "lien S3 pas encore implémenté — toute confirmation expirera");
    return ESP_OK;
}

const char *sec_gate_source(void)
{
    return "lien S3 (non implémenté)";
}

#else /* !BOARD_LINK_AVAILABLE */

esp_err_t sec_gate_init(void)
{
    ESP_LOGW(TAG, "carte sans lien : confirmation par la console « sec confirm »");
    ESP_LOGW(TAG, "béquille de développement — aucune valeur de sécurité");
    return ESP_OK;
}

const char *sec_gate_source(void)
{
    return "console (béquille de développement)";
}

/* Appelée par la commande console. Ne fait que relayer : c'est sec_confirm qui
 * décide, et il refuse hors d'une opération armée. */
void sec_gate_console_confirm(void)
{
    sec_confirm_authorize();
}

#endif /* BOARD_LINK_AVAILABLE */
```

Ajouter à `sec_gate.h`, avant la dernière ligne :

```c
#include "board.h"
#if !BOARD_LINK_AVAILABLE
/* Béquille de développement — n'existe que sur une carte sans lien. */
void sec_gate_console_confirm(void);
#endif
```

- [ ] **Étape 5 : la commande console**

Dans `main/console/console.c`, ajouter l'include `#include "sec_gate.h"` et la
commande :

```c
static int cmd_sec(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage : sec confirm | sec source\n");
        return 1;
    }

    if (strcmp(argv[1], "source") == 0) {
        printf("confirmation : %s\n", sec_gate_source());
        return 0;
    }

#if !BOARD_LINK_AVAILABLE
    if (strcmp(argv[1], "confirm") == 0) {
        sec_gate_console_confirm();
        printf("appui simulé — sans effet s'il n'y a pas d'opération armée.\n");
        return 0;
    }
#endif

    printf("sous-commande inconnue : %s\n", argv[1]);
    return 1;
}
```

L'enregistrer à côté des autres, dans `console_start()` :

```c
    const esp_console_cmd_t sec_cmd = {
        .command = "sec",
        .help = "Sécurité : « sec source », et « sec confirm » sur carte sans lien",
        .hint = "confirm|source",
        .func = &cmd_sec,
    };
    err = esp_console_cmd_register(&sec_cmd);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "commande sec : %s", esp_err_to_name(err));
        return err;
    }
```

- [ ] **Étape 6 : le garde-fou dans `scripts/fast.sh`**

Ajouter, avant le point de sortie unique des garde-fous :

```bash
# --- Garde-fou 4 : la béquille de confirmation ne part pas en production ----
# sec_gate_console_confirm n'existe que sur une carte sans lien. Si ce symbole
# apparaît hors d'un bloc conditionné à BOARD_LINK_AVAILABLE, la béquille
# pourrait se retrouver dans le firmware du coffre — indistinguable, à l'usage,
# d'un dispositif qui fonctionne.
if grep -rn 'sec_gate_console_confirm' main/ --include='*.c' --include='*.h' \
        | grep -vE '^main/security/sec_gate\.(c|h):' \
        | grep -vE '^main/console/console\.c:'; then
    echo "ERREUR : la béquille de confirmation est référencée hors des deux"
    echo "         fichiers qui la conditionnent à BOARD_LINK_AVAILABLE."
    fail=1
fi
```

- [ ] **Étape 7 : vérifier que le garde mord**

Ajouter temporairement dans `main/main.c` un appel `sec_gate_console_confirm();`.

Run : `./scripts/fast.sh > /dev/null 2>&1; echo "rc=$?"`
Attendu : `rc=1`.

Retirer l'appel, puis :

Run : `./scripts/fast.sh > /dev/null 2>&1; echo "rc=$?"`
Attendu : `rc=0`.

- [ ] **Étape 8 : vérifier que le coffre ne compile pas la béquille**

`main/CMakeLists.txt` : ajouter `"security/sec_gate.c"` aux `srcs`.
`main/main.c` : appeler `sec_gate_init()` juste avant `console_start()`.

Run : `./scripts/check.sh --force`
Attendu : VERT, les deux cartes.

Run : `grep -c sec_gate_console_confirm build_niphar_chest/compile_commands.json || true`
Puis vérifier dans le binaire :
`nm build_niphar_chest/niphar_chest.elf | grep -c sec_gate_console_confirm`
Attendu : `0` — le symbole est absent du firmware du coffre.

- [ ] **Étape 9 : commit**

```bash
git add -A
git commit -m "sécurité : sec_gate, la béquille qui ne peut pas partir en prod"
```

---

### Tâche 9 : CCID sur l'USB — composite MSC + CCID

**Fichiers :**
- Créer : `main/security/ccid.c`, `main/security/ccid.h`
- Modifier : `main/tusb_config.h`, `main/usb/usb_device.c`, `main/CMakeLists.txt`

**Interfaces :**
- Consomme : `openpgp_card` (tâche 7), `sec_confirm` (tâche 2).
- Produit : `usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count)`
  — surcharge forte d'un symbole faible de TinyUSB. C'est ainsi que la classe
  CCID s'enregistre, sans que `usb_device.c` ait à la connaître.

- [ ] **Étape 1 : copier**

```bash
KESP=~/Documents/GitHub/KeSp_firmware
cp "$KESP/main/security/ccid.c" "$KESP/main/security/ccid.h" main/security/
```

- [ ] **Étape 2 : les descripteurs**

Dans `main/usb/usb_device.c`, étendre l'énumération d'interfaces :

```c
enum {
    ITF_NUM_MSC = 0,
    ITF_NUM_CCID,
    ITF_NUM_TOTAL,
};
```

Ajouter les endpoints CCID sous ceux du MSC :

```c
#define EPNUM_CCID_OUT 0x02
#define EPNUM_CCID_IN  0x82
```

TinyUSB n'a **pas** de macro pour la classe CCID : KeSp a écrit la sienne.
Copier verbatim le bloc `KeSp_firmware/main/comm/usb/usb_hid.c:120-162` — c'est
`KASE_CCID_DESC` (le descripteur fonctionnel de 54 octets, CCID Rev 1.1 §5.1),
`TUD_CCID_DESC_LEN` et `KASE_CCID_ITF_DESC(_itfnum, _stridx, _epout, _epin)` —
dans un nouveau fichier `main/security/ccid_desc.h`, avec un `#pragma once` et
une note de provenance. Le mettre là plutôt que dans `usb_device.c` garde le
descripteur avec le code de la classe qu'il décrit.

Renommer le préfixe `KASE_` en `NIPHAR_` **est une divergence à ne pas faire** :
elle n'apporte rien et casserait la comparaison avec l'amont.

Puis dans `usb_device.c`, inclure `ccid_desc.h` et ajouter aux deux
configurations, après la ligne `TUD_MSC_DESCRIPTOR(...)` :

```c
    KASE_CCID_ITF_DESC(ITF_NUM_CCID, STRID_CCID, EPNUM_CCID_OUT, EPNUM_CCID_IN),
```

Corriger la longueur totale :

```c
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN + TUD_CCID_DESC_LEN)
```

Ajouter `STRID_CCID` à l'énumération des chaînes, avant `STRID_COUNT`, et
`[STRID_CCID] = "Coffre OpenPGP"` au tableau `s_strings`.

- [ ] **Étape 3 : forcer l'édition de liens de la classe**

`ccid.c` définit `usbd_app_driver_get_cb` en surcharge forte d'un symbole faible.
Si rien du fichier n'est référencé, l'éditeur de liens peut l'écarter et la
classe disparaît en silence. KeSp règle ça dans `usb_hid.c:370` par une
référence explicite ; reproduire le même mécanisme dans `usb_device.c`, avec un
commentaire disant pourquoi.

- [ ] **Étape 4 : vérifier sur matériel**

Run : `./scripts/check.sh --force`
Attendu : VERT.

```bash
PORT=$(ls /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_*-if00 | head -1)
idf.py -B build_jc_devkit -p "$PORT" app-flash
```

Run : `lsusb -v -d 303a:4021 2>/dev/null | grep -E "bInterfaceClass|bNumInterfaces"`
Attendu : deux interfaces, dont une de classe `11` (Chip/SmartCard).

Run : `lsblk | grep -i niphar`
Attendu : le disque est **toujours** là — le MSC ne doit pas avoir régressé.

- [ ] **Étape 5 : commit**

```bash
git add -A
git commit -m "usb : CCID en composite avec le MSC"
```

---

### Tâche 10 : OTP-HID — la troisième interface

**Fichiers :**
- Créer : `main/security/otp_hid.c`, `main/security/otp_hid.h`
- Modifier : `main/tusb_config.h`, `main/usb/usb_device.c`, `main/CMakeLists.txt`

**Interfaces :**
- Consomme : `otp_proto`, `cr_hmac` (tâche 5), `sec_store` (tâche 3),
  `sec_confirm` (tâche 2).
- Produit : les callbacks HID de TinyUSB (`tud_hid_get_report_cb`,
  `tud_hid_set_report_cb`) et le descripteur de rapport que `usb_device.c`
  expose.

C'est ici que le coffre fait ce qu'aucune carte de KeSp ne peut : CCID et
OTP-HID en même temps. Sur le S3 les deux débordaient le budget de 4 endpoints
IN ; le P4 en a seize.

- [ ] **Étape 1 : copier et activer HID**

```bash
KESP=~/Documents/GitHub/KeSp_firmware
cp "$KESP/main/security/otp_hid.c" "$KESP/main/security/otp_hid.h" main/security/
```

Dans `main/tusb_config.h`, remplacer `#define CFG_TUD_HID 0` par :

```c
#define CFG_TUD_HID                 1
#define CFG_TUD_HID_EP_BUFSIZE      64
```

- [ ] **Étape 2 : les descripteurs**

Ajouter `ITF_NUM_HID` à l'énumération, avant `ITF_NUM_TOTAL`, et :

```c
#define EPNUM_HID_OUT 0x03
#define EPNUM_HID_IN  0x83
```

Contrairement au CCID, TinyUSB fournit la macro. Ajouter aux deux
configurations :

```c
    TUD_HID_DESCRIPTOR(ITF_NUM_HID, STRID_HID, HID_ITF_PROTOCOL_NONE,
                       sizeof(desc_hid_report), EPNUM_HID_IN, 64, 10),
```

`HID_ITF_PROTOCOL_NONE` et non `_KEYBOARD` : le coffre n'est pas un clavier, et
l'annoncer comme tel ferait que l'hôte lui envoie des rapports de LED.

Corriger la longueur totale :

```c
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN \
                          + TUD_CCID_DESC_LEN + TUD_HID_DESC_LEN)
```

Le descripteur de rapport `desc_hid_report` et le callback
`tud_hid_descriptor_report_cb` viennent de
`KeSp_firmware/main/comm/usb/usb_hid.c:237` — copier la définition du tableau et
le callback, en ne gardant que la branche OTP (le clavier n'existe pas ici).
Ajouter `STRID_HID` à l'énumération et `[STRID_HID] = "Coffre OTP"` au tableau
des chaînes.

- [ ] **Étape 3 : rendre chaque interface débrayable**

La mutuelle exclusion de KeSp n'a plus lieu d'être — le budget d'endpoints du P4
les accepte toutes —, mais un coffre qui n'expose que le disque doit rester
possible : moins d'interfaces, moins de surface offerte à l'hôte.

Créer `main/Kconfig.projbuild` :

```
menu "Coffre Niphar"

config NIPHAR_SEC_CCID
    bool "Carte OpenPGP sur CCID"
    default y
    help
        Expose une interface CCID (classe 0x0B) portant la carte OpenPGP.
        Le budget d'endpoints du P4 permet de la cumuler avec l'OTP-HID et
        le stockage de masse — ce que l'ESP32-S3 ne pouvait pas.

config NIPHAR_SEC_OTP_HID
    bool "Clé CR-HMAC sur HID (compatible YubiKey)"
    default y
    help
        Expose une interface HID portant le challenge-response HMAC-SHA1.
        Jamais validée sur matériel, y compris chez KeSp_firmware.

endmenu
```

Conditionner les deux blocs de descripteurs, les sources dans
`main/CMakeLists.txt` et `CFG_TUD_HID` dans `main/tusb_config.h` à ces options.
`CONFIG_TOTAL_LEN` et `ITF_NUM_*` doivent se recalculer correctement dans les
quatre combinaisons.

- [ ] **Étape 4 : vérifier les combinaisons**

Run, pour chacune des quatre combinaisons (les deux options à `y`/`n`) :
`idf.py -B build_jc_devkit -DBOARD=jc_devkit -DSDKCONFIG=build_jc_devkit/sdkconfig build`
Attendu : compile dans les quatre cas.

Le cas « les deux à `n` » doit produire exactement le firmware d'avant ce
portage — le disque seul.

- [ ] **Étape 5 : vérifier sur matériel**

Run : `./scripts/check.sh --force` puis flasher le kit (les deux options à `y`).

Run : `lsusb -v -d 303a:4021 2>/dev/null | grep -cE "bInterfaceClass"`
Attendu : `3` — stockage de masse, carte à puce, HID.

- [ ] **Étape 6 : le disque n'a pas régressé**

Run : `lsblk | grep -i niphar`
Attendu : le disque est toujours là.

- [ ] **Étape 7 : commit**

```bash
git add -A
git commit -m "usb : OTP-HID en troisième interface, ce que le S3 ne pouvait pas"
```

---

### Tâche 11 : validation bout en bout sur le kit

**Fichiers :**
- Modifier : `docs/HARDWARE.md`, `README.md`, `CLAUDE.md`

Aucun code. Cette tâche existe parce qu'un portage qui compile n'est pas un
portage qui marche, et que c'est la première fois que ces deux interfaces
tournent ensemble où que ce soit.

- [ ] **Étape 1 : la carte OpenPGP répond**

```bash
gpg --card-status
```
Attendu : la carte est vue, avec son numéro de série.

- [ ] **Étape 2 : une opération qui exige la confirmation**

Générer une clé de test, puis signer. Au moment où gpg attend, taper `sec confirm`
sur la console du coffre.

Attendu : sans `sec confirm`, l'opération échoue après 15 s. Avec, elle aboutit.
**Les deux moitiés de ce test comptent** : une confirmation qui n'est pas
nécessaire ne prouve rien.

- [ ] **Étape 3 : le disque survit**

Pendant que gpg dialogue avec la carte, monter le disque et lire un fichier.
Attendu : les deux fonctionnent simultanément.

- [ ] **Étape 4 : consigner les résultats**

Mettre à jour `README.md` (case « Intégration PGP/FIDO ») et ajouter à
`docs/HARDWARE.md` une note sur ce qui a été validé, avec la date. Corriger le
terme « FIDO » du README : ce qui existe est OpenPGP CCID et CR-HMAC OTP-HID.

- [ ] **Étape 5 : commit**

```bash
git add -A
git commit -m "docs : portage sécurité validé sur le kit"
```

---

## Ce que ce plan ne fait pas

- **Le lien SPI** reste non implémenté : sur `niphar_chest`, `sec_gate` refuse
  toute confirmation. C'est voulu, et c'est le comportement honnête.
- **Le stockage des clés n'est pas tranché.** Le code écrira des secrets en NVS,
  et l'audit du 2026-08-07 a montré que l'hôte peut forcer le mode download donc
  dumper la flash. Sur le kit, sans enjeu. **Avant que le coffre porte une vraie
  clé**, il faut décider : ouvrir JP1/JP2, brûler les eFuses, ou accepter le
  risque explicitement. Ce plan ne décide pas à la place de cette conversation.
- **L'OTP-HID sera validé pour la première fois** ici. Chez KeSp il n'a jamais
  tourné sur matériel faute d'endpoints. Le traiter comme du code neuf, pas
  comme du code éprouvé.
- **CTAP2 / FIDO2** n'existe nulle part et n'est pas dans ce plan.
