# FIDO2 / WebAuthn sur la carte-clé — conception

Ajouter un authentificateur FIDO2 (CTAP2) et U2F (CTAP1) au firmware
`Niphar_chest`, sur la carte `wt9932_key`.

Voisin : [`2026-08-16-carte-cle-wt9932-design.md`](2026-08-16-carte-cle-wt9932-design.md)
pour les boutons, la LED et la porte de présence ; le contrat matériel est dans
[`docs/HARDWARE.md`](../../HARDWARE.md).

## Ce qu'on construit, et pourquoi

Deux usages retenus par la propriétaire :

- **Passkeys** — connexion sans mot de passe ni nom d'utilisateur, ce qui exige
  CTAP2 avec identifiants résidents (*discoverable credentials*).
- **Second facteur** sur les sites qui ne connaissent qu'U2F.

Le **PIN (clientPIN) est dans le périmètre**. Il a d'abord été écarté par erreur
de sélection puis explicitement redemandé : l'échelonnement qui suit est un
**ordre de construction**, pas une réduction de périmètre.

## Hors périmètre, et pourquoi

| écarté | raison |
|---|---|
| extension `hmac-secret` | ni la connexion locale Linux ni LUKS n'ont été retenus ; c'est la seule chose qui les exigeait |
| attestation « basic » avec certificat | un certificat auto-signé n'est chaîné à aucune autorité : il ferait *ressembler* la clé à une clé du commerce sans rien apporter. Attestation **`none`** |
| protocole PIN/UV v2 | la v1 est universellement supportée ; la v2 sert surtout aux permissions par commande, inutiles sans `hmac-secret` |
| compteur de signature | mis à **0**, ce que la spécification autorise pour un authentificateur qui ne l'implémente pas. Il sert à détecter le clonage — or les clés dérivent d'un eFuse illisible, donc la clé n'est pas clonable. Le garder coûterait une écriture flash à *chaque* authentification |
| gestion d'identifiants (`credentialManagement`) | à reconsidérer une fois les passkeys en service ; sans elle on efface par `authenticatorReset` |

## Architecture

```
       hôte (navigateur, libfido2)
                    │  rapports HID de 64 octets
                    ▼
        usb/mode_fido.c            page d'usage 0xF1D0, colle TinyUSB
                    │
                    ▼
     security/ctaphid.c    ◄── PUR
                    │
                    ├──────────────► security/u2f.c        (CTAP1, via apdu.c)
                    ▼
        security/ctap2.c            getInfo, makeCredential, getAssertion,
                    │               reset, clientPIN
        ┌───────────┼────────────┬───────────────┐
        ▼           ▼            ▼               ▼
   cbor.c      ctap_pin.c   fido_cred.c    openpgp_crypto.c
   ◄── PUR     ◄── PUR      magasin        ES256 — existe déjà
                            résident
                    │
                    ▼
        security/sec_confirm.c      la porte de présence
```

Le découpage reproduit celui, éprouvé, du CCID/OpenPGP : transport → trames
pures → dispatch → crypto.

### Ce qui est pur, et pourquoi ça n'est pas un détail

| module | ce qu'il décide | pourquoi il doit être pur |
|---|---|---|
| `ctaphid.c` | canaux, paquets init/cont, réassemblage, longueurs | **il lit des octets fournis par l'hôte** : surface d'attaque, au même titre qu'`apdu.c` |
| `cbor.c` | décodage et encodage canonique | idem ; le CBOR mal formé est le vecteur classique contre les authentificateurs |
| `ctap_pin.c` | compteurs de tentatives, verrouillage | **un bug y est un trou de sécurité**, pas un défaut cosmétique |
| assainissement du domaine | ce qui s'affiche à l'écran | du texte hôte rendu à un humain pour une décision de sécurité |

`openpgp_crypto.c:37` fait déjà de l'ECDSA sur `MBEDTLS_ECP_DP_SECP256R1`,
c'est-à-dire ES256 : réutilisé tel quel, déjà validé sur matériel.

## Les clés : dérivées, jamais stockées

```
d   = HMAC(K_maître, 0x01 ‖ nonce ‖ rp_id_hash)     clé privée du credential
tag = HMAC(K_maître, 0x02 ‖ nonce ‖ rp_id_hash)     son authentificateur

credential_id = nonce(16) ‖ tag(16)                  32 octets
```

À l'authentification on recalcule `tag` depuis le nonce présenté et le hash du
domaine ; s'il ne correspond pas, l'identifiant n'est pas le nôtre et on refuse.
**Les identifiants non résidents ne coûtent donc aucun stockage** et leur nombre
est illimité.

**Les octets de préfixe `0x01`/`0x02` sont une séparation de domaine, et leur
absence serait une vulnérabilité** : sans eux, l'authentificateur du credential
*serait* sa clé privée, et le publier dans le credential ID reviendrait à
publier la clé. Ce n'est pas une précaution de style.

### Où vit `K_maître`

Périphérique HMAC du P4, clé dans un bloc eFuse en lecture protégée.

> *The 256-bit HMAC key is stored in an eFuse key block and can be set as
> read-protected, i.e., the key is not accessible from outside the HMAC
> accelerator.*
> — **ESP32-P4 TRM, ch. 28 « HMAC Accelerator », p. 1526**

**Mode *upstream* obligatoire.** La même page précise que le résultat n'est
accessible au logiciel qu'en *upstream* ; le mode *downstream* n'a que deux
usages câblés (réactiver le JTAG, déchiffrer les paramètres du périphérique de
signature RSA) et ne sert pas à une dérivation arbitraire.

**Limite honnête, à ne pas cacher** : `K_maître` n'est jamais extractible et un
dump de flash ne rend rien — mais **la clé privée dérivée transite par la RAM**.
Une faille *logicielle* (un bug du parseur CBOR, typiquement) pourrait
l'exfiltrer. Ce n'est pas un défaut de conception mais la limite du silicium :
aucun mode du P4 ne permet de signer sans que le scalaire existe en clair. C'est
la justification directe du fait que `ctaphid.c` et `cbor.c` soient purs et
testés.

Le P4 offre six blocs de clés (`EFUSE_BLK_KEY0..KEY5`). **Deux blocs distincts**
— un pour la dérivation FIDO, un pour le chiffrement NVS. Pas de partage :
réutiliser une clé pour deux usages est précisément ce que le champ *key
purpose* de l'eFuse existe pour empêcher.

**Griller un eFuse est irréversible.** D'où un cinquième axe de carte,
`BOARD_FIDO_KEY_SOURCE`, à côté des quatre existants :

| carte | mode FIDO | `K_maître` | présence |
|---|---|---|---|
| `wt9932_key` | oui | eFuse en lecture protégée, périphérique HMAC | bouton CONFIRM |
| `jc_devkit` | oui | **NVS en clair** — *« le devkit reste un devkit »* | console (`sec confirm`) |
| `niphar_chest` | oui | eFuse en lecture protégée | lien S3 |

Les trois cartes portent FIDO — contrairement à l'écran, qui n'existe que sur la
carte-clé. Le devkit en a besoin pour développer, et le coffre est la cible
finale du projet. Mais **le devkit ne grille jamais d'eFuse** : son `K_maître`
vit en NVS en clair, ce qui est sans valeur de sécurité et parfaitement assumé —
c'est un kit de développement, ses clés FIDO sont jetables.

Conséquence à ne pas manquer : un identifiant créé sur le devkit **ne
fonctionnera jamais** sur la carte-clé, et réciproquement. Les clés dérivent de
`K_maître` ; deux cartes, deux maîtres, deux univers d'identifiants. C'est le
comportement voulu, pas un défaut à corriger.

## Stockage des identifiants résidents

Un enregistrement par identifiant résident, uniquement pour pouvoir les
**énumérer** — c'est l'énumération qui fait le sans-mot-de-passe. Environ
260 octets : hash du domaine, credential ID, *user handle*, nom du domaine et de
l'utilisateur (pour l'écran), drapeaux.

- Partition `fido`, 64 Ko, à l'offset `0x620000`. `partitions.csv` déclare
  explicitement que tout au-delà est libre et qu'y ajouter une partition **ne
  déplace aucun offset existant** : aucun effacement de flash.
- Plafond à **64 identifiants** (une Yubikey 5 en tient 25 à 100).
- **En NVS et non en flash brute** : `nvs_utils.c` existe, et on hérite de
  l'écriture atomique et du nivellement d'usure sans écrire de pilote.
- **NVS chiffré par le schéma HMAC** (`NVS_SEC_KEY_PROTECT_USING_HMAC`) : un bloc
  eFuse porte la clé dont dérivent les clés de chiffrement, **sans exiger le
  chiffrement de flash**. Cela protège les *métadonnées* — quels sites, quels
  comptes — qui seraient sinon en clair dans un dump. C'est une propriété de vie
  privée distincte de la protection des clés.

## clientPIN

Protocole PIN/UV **v1**. Accord de clés ECDH P-256, `sharedSecret = SHA-256(Z.x)`,
PIN chiffré en AES-256-CBC, `pinToken` de 32 octets valable jusqu'à la coupure
d'alimentation, commandes suivantes authentifiées par HMAC de ce jeton.
`MBEDTLS_ECDH_C`, `MBEDTLS_AES_C` et SHA-256 matériel sont déjà activés.

### La machine à états de verrouillage — le cœur testable

```
tentatives_persistantes = 8      survit à la coupure d'alimentation
tentatives_depuis_boot  = 3      remis à 3 à chaque démarrage

avant toute comparaison :   décrémenter les persistantes ET LES ÉCRIRE EN FLASH
    si elles atteignent 0   → PIN_BLOCKED, définitif jusqu'à authenticatorReset
comparaison réussie :       remettre à 8 et à 3
comparaison échouée :       décrémenter celles depuis boot
    si elles atteignent 0   → PIN_AUTH_BLOCKED, coupure d'alimentation requise
    sinon                   → PIN_INVALID
après chaque échec :        régénérer la clé d'accord ECDH
```

**L'écriture en flash *avant* la comparaison est la règle qui porte tout.**
Décrémenter après offrirait un nombre illimité d'essais à qui coupe le courant
pendant la comparaison. **Aucun test fonctionnel ne verrait la différence** : les
deux versions rendent le même résultat quand on ne coupe rien. Le test doit donc
modéliser explicitement la coupure.

La régénération de la clé ECDH après chaque échec force un aller-retour ECDH
complet par tentative — le forçage brutal reste lent même si les compteurs
étaient contournés.

**Ce que le PIN protège** : une clé volée. **Ce qu'il ne protège pas** : un hôte
compromis, qui peut réutiliser le jeton jusqu'au débranchement. C'est la garantie
du standard, ni plus.

## L'écran

Sur `getAssertion`, l'écran affiche **le domaine auquel on s'authentifie**, en
police double hauteur. Propriété anti-hameçonnage réelle : le navigateur a
vérifié que l'origine correspond au domaine, et l'afficher sur un écran que la
page web ne contrôle pas laisse remarquer une anomalie.

**Ce texte vient de l'hôte**, donc il s'assainit avant d'être rendu : imprimables
ASCII seulement, troncature **explicite et visible**. Rendre tel quel une chaîne
hôte sur un écran servant à une décision de sécurité serait une faute. Logique
pure, donc testée.

Le cinquième mode ajoute un cinquième point de cycle. `screen_mode_count()` rend
4 en dur et `test_mode_count_is_four` l'épingle comme **décision de disposition**
et non comme l'énumération : ce test doit rougir, c'est son rôle.

## Prérequis, avant la première ligne de FIDO

1. **`sec_confirm`, Ruling 28.** `authorize()` ne vérifie pas que l'appui est
   postérieur à l'armement courant : un appui tardif peut autoriser l'opération
   *suivante*. Théorique sur OpenPGP ; réel avec un navigateur qui enchaîne des
   `getAssertion`. Brancher FIDO sur une porte défectueuse et le découvrir en
   phase 6 coûterait bien plus cher.
2. **Clôturer `ecran-oled`.** Le Critique de sa revue finale : la validation
   matérielle de l'écran et sa consignation dans `docs/HARDWARE.md`, dont le
   résidu électrique déjà mesuré (*« des adresses parasites persistent même avec
   les pull-ups, la marge est faible »*), qui n'est aujourd'hui dans aucun
   fichier committé.

## Phases

| # | contenu | ce qui marche à la fin |
|---|---|---|
| **0** | prérequis ci-dessus | on cesse de construire sur une porte défectueuse |
| **1** | `ctaphid.c` (pur), `mode_fido.c`, cinquième mode | l'hôte voit un périphérique FIDO ; `INIT` et `PING` répondent |
| **2** | `cbor.c` (pur) + `authenticatorGetInfo` | `fido2-token -I` décrit la clé |
| **3** | `u2f.c`, dérivation HMAC, écran de confirmation | **utilisable** : second facteur sur GitHub, Google, GitLab |
| **4** | `ctap2.c` : `makeCredential`/`getAssertion`, non résidents | WebAuthn avec `userVerification: preferred` |
| **5** | `fido_cred.c`, partition `fido`, NVS chiffré | **passkeys** |
| **6** | `ctap_pin.c`, verrouillage, affichage assaini du domaine | passkeys avec `userVerification: required` |

**U2F avant CTAP2**, bien que CTAP2 soit le but : U2F exerce *toute* la chaîne —
transport, présence, dérivation, signature, écran — avec le plus petit jeu de
commandes. Si quelque chose casse en phase 3, la cause est dans un de cinq
maillons ; en sautant à CTAP2 elle serait aussi dans le CBOR et le dispatch. Et
la phase 3 est déjà un produit utilisable, pas une étape morte.

**Le PIN en dernier** parce que les phases 4 et 5 posent les commandes qu'il vient
authentifier.

### Découpage en plans d'implémentation

**Cette spec ne donne pas un seul plan mais au moins deux.** Les six phases
représentent l'équivalent du portage OpenPGP entier ; les tenir dans un seul plan
produirait un document que personne ne relit et dont les dernières tâches seraient
écrites contre un code qui aura changé.

- **Plan 1 — phases 0 à 3.** Se termine sur un produit utilisable : un second
  facteur qui marche sur GitHub. C'est là qu'on apprend si le transport, la
  dérivation par eFuse et l'écran tiennent sur matériel.
- **Plan 2 — phases 4 à 6.** Écrit *après* le retour du plan 1, parce que ce
  retour changera des choses.

Seul le plan 1 est à écrire maintenant.

## Tests

La norme TDD du projet s'applique intégralement : test d'abord, rouge avant vert,
et **mutation transitoire pour prouver que le test mord**.

- Les quatre modules purs vont dans `test/`, compilés sur l'hôte.
- La machine à états du PIN se teste avec un **modèle explicite de coupure
  d'alimentation** — sans quoi la règle « écrire avant comparer » n'est pas
  couverte.
- Le parseur CBOR se teste sur des entrées **malformées** autant que valides :
  longueurs mentant sur le contenu, imbrication profonde, chaînes non terminées,
  entiers hors bornes.

**`libfido2` doit être installé** (`pacman -S libfido2`) : les phases 1 et 2 n'ont
aucun flux navigateur pour les tester. Et il vaut mieux que mon propre client de
test pour une raison de fond — **un client que j'écris partage mes propres
mélectures de la spécification**. `libfido2` est une implémentation
indépendante : c'est ce qui en fait un oracle plutôt qu'un miroir. Firefox et
Chromium (présents) couvrent les phases 4 à 6 via `webauthn.io`.

L'agent `niphar-security-auditor` est requis sur `ctaphid.c`, `cbor.c` et
`ctap_pin.c` : ce sont des gestionnaires d'entrée externe, exactement son
domaine.

## Ordre de grandeur

Environ 1900 lignes de firmware et autant de tests, dont ~900 pour le seul PIN.
Comparable au portage OpenPGP (3884 lignes dans `main/security/`). **Ce n'est pas
un après-midi**, et le découpage en phases existe pour que chacune soit livrable
plutôt que pour donner l'illusion d'avancer.

## Risques

| risque | traitement |
|---|---|
| bug dans le parseur CBOR ou CTAP-HID | logique pure, testée, mutation, audit sécurité — c'est le poste le plus exposé |
| eFuse grillé par erreur | axe de carte : le devkit n'en grille jamais ; la procédure de gravure est manuelle et documentée, pas automatique |
| bascules de mode spontanées (défaut ouvert) | observé deux fois, toujours trois bascules en moins de deux secondes. **Non diagnostiqué.** Un mode FIDO actif qui disparaît en pleine authentification serait très visible — à résoudre avant la phase 3 |
| usure flash du compteur PIN | 8 écritures maximum entre deux succès, sur une partition NVS avec nivellement : négligeable |
