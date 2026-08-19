# Applet OATH/TOTP — remplacer Proton Authenticator

*Spec de conception, 2026-08-18. Cible : `wt9932_key`, applicable au coffre.*

## Pourquoi

Mae détient douze comptes TOTP dans Proton Authenticator. L'objectif est de les
porter sur la clé Niphar, pour que le second facteur vive dans le même objet que
les clés OpenPGP et FIDO2 — un objet qu'on débranche.

Proton reste la sauvegarde. La migration est **manuelle**, compte par compte :
douze secrets recopiés à la main, sans outil d'import à écrire ni fichier
déchiffré à faire transiter.

## Décisions prises

Six arbitrages, tranchés avec Mae avant conception. Ils contraignent tout ce
qui suit.

| # | Décision | Conséquence directe |
|---|---|---|
| 1 | **Modèle YKOATH strict** — le compteur de temps vient de l'hôte | La clé n'a pas d'horloge et n'en veut pas. Elle ne sait jamais quelle heure il est. `ykman` est requis au quotidien. |
| 2 | **Appui obligatoire sur chaque code** | `CALCULATE ALL` ne peut rendre aucun code : il répond `TAG_TOUCH` pour chaque compte. `ykman oath accounts code` sans argument n'affiche que `[Requires Touch]`. |
| 3 | **Pas de mot de passe** | Pas de `SET CODE` / `VALIDATE`. La liste des comptes et leur modification sont lisibles par tout logiciel de la machine hôte ; les secrets, jamais. |
| 4 | **L'écran nomme le compte demandé** | Le nom vient de l'hôte : il doit être assaini avant d'être dessiné. Sans cela l'appui est un simple interrupteur de présence, pas un accord sur un compte. |
| 5 | **Sixième mode `usb mode oath`** | Un applet par mode, conforme à « plein de choses, une à la fois ». Pas de second applet greffé sur le mode PGP. |
| 6 | **Magasin unique élargi** (`sec_store`) plutôt qu'un `oath_store` dédié | Un seul endroit où regarder ce que la clé détient. Coût nul : voir « État des lieux ». |

## État des lieux — ce sur quoi on s'appuie

Trois faits mesurés dans le dépôt avant conception, dont deux changent le plan.

**`cr_hmac_sha1()` existe** (`security/cr_hmac.c`) et c'est exactement la
primitive de TOTP (RFC 6238 par défaut). mbedtls est disponible pour SHA-256.

**`dongle_confirm()` existe** (`security/ccid.c:431`) : il arme `sec_confirm`,
sonde toutes les 20 ms, et **émet une trame d'extension de temps CCID (WTX)
toutes les 1,5 s** pour que l'hôte patiente au lieu d'abandonner. C'est
précisément ce dont `ykman` a besoin — voir « Attente de l'appui » — et c'est
déjà éprouvé sur matériel avec `gpg`. Aucune machine à états à inventer.

**`sec_store` est inerte.** `sec_store_set_slot()` n'a aucun appelant dans tout
le dépôt, et `sec_store_init()` n'est jamais appelé non plus. Le magasin n'est
donc ni chargé ni écrit, jamais. Deux conséquences :

- élargir la structure ne migre rien et ne peut rien perdre — la décision 6 est
  gratuite ;
- **le mode OTP CR-HMAC ne peut pas fonctionner aujourd'hui**, puisqu'il lit des
  slots qu'aucun chemin ne remplit. Défaut préexistant, indépendant de ce
  travail ; l'applet OATH lui apporte au passage le chemin de provisionnement
  qui lui manquait.

## Composants

| fichier | rôle | pur ? |
|---|---|---|
| `security/oath_proto.{c,h}` | analyse des commandes YKOATH, troncature RFC 4226, sérialisation des réponses, découpe `SEND REMAINING` | **oui** |
| `security/oath_name.{c,h}` | assainissement du nom venu de l'hôte avant l'écran | **oui** |
| `security/sec_store.{c,h}` | élargi : 16 slots, nom 64 o, secret 64 o ; blob NVS version 2 | oui |
| `security/ccid.c` | `dongle_confirm()` élargi pour porter une étiquette | non |
| `usb/mode_oath.{c,h}` | plomberie CCID, sur le modèle de `mode_pgp.c` | non |
| `usb/usb_mode.{c,h}` | `USB_MODE_OATH` dans l'énumération et le cycle | non |
| `hmi/screen_view.h` | `SEC_OP_OATH` et son libellé | oui |

L'essentiel du travail est en logique pure, donc sous TDD et vérifiable sans
matériel. Ce n'est pas un hasard : c'est la contrainte du harnais hôte qui force
ce découpage, et elle tombe bien ici.

## Surface protocolaire

AID : `A0 00 00 05 27 21 01`. Toutes les valeurs ci-dessous sont relevées dans
`yubikit/oath.py` de **ykman 5.9.1**, pas reconstituées de mémoire.

### Commandes implémentées

| INS | commande | comportement |
|---|---|---|
| `A4` P1=04 | SELECT — sélection de l'applet | rend `TAG_VERSION` + `TAG_NAME` (sel) |
| `01` | PUT | ajoute un compte ; drapeau tactile **forcé** quel que soit ce que demande l'hôte |
| `02` | DELETE | supprime un compte |
| `05` | RENAME | renomme un compte |
| `04` | RESET | efface tous les comptes OATH |
| `A1` | LIST | liste les noms et leur type |
| `A2` | CALCULATE | **arme la confirmation**, puis rend le code tronqué |
| `A4` P2=01 | CALCULATE ALL | rend `TAG_TOUCH` pour chaque compte, jamais de code |
| `A5` | SEND REMAINING | suite d'une réponse tronquée |

**Collision d'octet d'instruction, à ne pas manquer.** `SELECT` (ISO 7816) et
`CALCULATE ALL` valent **tous deux `A4`**. Ils ne se distinguent que par leurs
paramètres : `ykman` envoie `CLA=00 INS=A4 P1=04 P2=00` pour la sélection et
`CLA=00 INS=A4 P1=00 P2=01` pour le calcul global (`send_apdu(0,
INS_CALCULATE_ALL, 0, 1, …)` dans `oath.py`). Un aiguillage sur le seul INS
répondrait une réponse de SELECT à une demande de codes.

### Commandes refusées

| INS | commande | réponse | pourquoi |
|---|---|---|---|
| `03` | SET CODE | `6A81` | décision 3 : pas de mot de passe |
| `A3` | VALIDATE | `6A81` | sans mot de passe, rien à valider |
| — | PUT d'un compte HOTP | `6A81` | voir « Portée » |
| — | PUT d'un compte SHA-256 | `6A81` | voir « Portée » — divergence déclarée |

### Réponse au SELECT

`ykman` fait `data[TAG_VERSION]` sans garde : **l'absence de `TAG_VERSION`
(`0x79`) lève une exception côté hôte**, ce n'est pas optionnel. Et
`_get_device_id(salt)` calcule un SHA-256 du champ `TAG_NAME` (`0x71`), donc son
absence plante aussi. La réponse porte donc les deux :

- `TAG_VERSION` : trois octets. On annonce **5.7.1**, une version qui sélectionne
  les chemins modernes de `ykman` (au-dessus de 3.0.0, elle évite un contournement
  hérité du YubiKey NEO). C'est une annonce de compatibilité protocolaire, pas
  une prétention d'être un YubiKey ; à documenter comme telle.
- `TAG_NAME` : un sel de huit octets, **aléatoire, tiré une fois et persisté en
  NVS**. Stable d'une session à l'autre, sinon `ykman` verrait un appareil
  différent à chaque branchement. Tiré au sort plutôt que dérivé de l'adresse
  MAC : l'identifiant d'appareil publié à l'hôte n'a pas à révéler un
  identifiant matériel.
- `TAG_CHALLENGE` : **absent**, ce qui signale « pas de mot de passe ».

### Découpe des réponses longues

`LIST` sur douze comptes dépasse les 255 octets d'une réponse APDU courte.
`SEND REMAINING` (`A5`) est donc **obligatoire**, pas optionnel : `ykman`
l'appelle automatiquement tant que le mot d'état vaut `61xx`. Une implémentation
qui l'omet marche avec trois comptes et casse avec douze — le genre de défaut
qui ne se voit qu'après la migration.

## Attente de l'appui

`ykman.calculate()` envoie l'APDU et déballe la réponse. **Aucune boucle de
relance, aucune gestion du toucher dans la bibliothèque.** La carte doit donc
tenir la commande ouverte jusqu'à l'appui — c'est l'inverse d'U2F, où le client
relance toutes les ~117 ms.

```
ykman                          clé
  │  SELECT AID A0000005272101 →
  │                            ← TAG_VERSION 5.7.1, TAG_NAME <sel>
  │  CALCULATE (nom, défi 8 o) →
  │                              dongle_confirm(SEC_OP_OATH, "GITHUB")
  │                              écran : CONFIRMER / CODE OTP / GITHUB + barre
  │  ← WTX toutes les 1,5 s ─────  (ykman patiente, pas d'erreur)
  │                              ┌ appui   → HMAC, troncature RFC 4226
  │                              └ 15 s    → 0x6985
  │  ← TAG_TRUNCATED / 6985 ─────
```

Le défi de huit octets est le compteur de temps calculé par `ykman`.

**L'échéance de quinze secondes est ici réelle**, puisque `ykman` ne relance
pas — contrairement à U2F, dont le réarmement à chaque tentative a fait retirer
la barre de décompte le 2026-08-18. `screen_op_has_deadline()` rend `true` par
défaut pour toute opération non-FIDO : OATH hérite donc de la barre sans une
ligne supplémentaire, et pour la bonne raison.

## Stockage

```c
#define SEC_N_SLOTS     16
#define SEC_LABEL_LEN   64
#define SEC_SECRET_MAX  64

typedef struct {
    uint8_t type;        /* 0 vide | 0x01 CR-HMAC | octet d'algo YKOATH */
    uint8_t flags;       /* bit0 = appui requis — forcé à 1 */
    uint8_t secret_len;
    uint8_t digits;      /* 6 ou 8 — remplace `reserved` */
    char    label[SEC_LABEL_LEN];
    uint8_t secret[SEC_SECRET_MAX];
} sec_slot_t;
```

2112 octets de blob NVS, version **2**. Le champ `type` porte directement
l'octet d'algorithme YKOATH — quartet haut `0x20` pour TOTP, quartet bas `0x01`
SHA-1 / `0x02` SHA-256 — qui ne peut jamais valoir `0x01` seul : aucune collision
avec les slots CR-HMAC existants. Le blob version 1 est refusé sur sa taille au
chargement, sans perte puisque rien n'y a jamais été écrit.

Seize slots pour douze comptes : de la marge, sans provisionner pour un besoin
qui n'existe pas.

## Portée

**Dedans** : TOTP **SHA-1**, six ou huit chiffres, période portée par le nom
(`30/Issuer:compte`, convention YKOATH).

**Dehors, et refusé explicitement** :

- **SHA-256** (`6A81`). **Cette ligne disait le contraire jusqu'au 2026-08-19**
  — la section annonçait « TOTP SHA-1 **et SHA-256** » dedans, alors que
  `oath_do_put()` refuse tout ce qui n'est pas `OATH_ALGO_TOTP_SHA1` (`0x21`)
  depuis le premier jour. Le choix est maintenu : implémenter un algorithme
  qu'**aucun compte réel n'exerce** serait spéculatif, et un HMAC-SHA-256 que
  rien ne teste rendrait des codes parfaitement formés et faux, pour toujours,
  sans qu'aucune erreur ne le dise. Un refus franc vaut mieux qu'un mensonge
  silencieux. **On le fera si l'export Proton en contient** — c'est la
  migration des douze comptes qui tranchera, pas une supposition.
  Le défaut n'était pas le refus mais son absence de déclaration : il est
  désormais inscrit dans `.tripwire-divergences`, qui rendra rouge la
  disparition silencieuse de la garde.
- **HOTP** (`6A81`). Il exige un compteur persistant incrémenté à chaque usage :
  une machine à états et une écriture NVS par code produit, pour un besoin que
  Mae n'a pas. Si l'export Proton contient du HOTP, on le verra à la migration.
- **SHA-512** (`6A81`). Rien ne l'utilise dans les douze comptes ; l'ajouter
  coûte peu mais ne se teste sur rien.
- **Mot de passe** — décision 3.

## Assainissement du nom

Le nom vient de l'hôte : jusqu'à 64 octets arbitraires, dessinés sur un écran
dont Mae se sert pour décider. Trois règles, toutes en logique pure et testables.

1. **Garder le nom complet, sans le préfixe de période.**
   `30/GitHub:mae@ex.org` → `GITHUB:MAE@EX.ORG`. Le `30/` est de la convention
   YKOATH, pas du sens : il ne dit rien à qui regarde l'écran. Tout le reste
   est gardé.

   > **Amendée le 2026-08-19 — la version précédente était fausse.** Elle
   > disait : « Garder l'issuer. `GitHub:mae@exemple.org` → `GITHUB`. C'est la
   > partie que Mae reconnaît ; **le compte importe peu quand on n'en a qu'un
   > par service.** » La prémisse en gras est démentie par la propriétaire :
   > OVH et Ankama auront chacun **un compte perso et un compte pro**.
   > `OVH:perso` et `OVH:pro` rendaient donc tous deux `OVH` — strictement
   > indiscernables sur l'écran qui sert à décider. Pour ces comptes-là,
   > l'appui redevenait un interrupteur de présence, et la décision 4 ne
   > protégeait plus rien. La paire est au test central
   > `test_noms_distincts_restent_distincts`, constatée rouge avant la
   > correction.
2. **Tout caractère sans glyphe devient `?`, jamais un blanc.** La police de
   `screen.c` définit `A-Z`, `a-z`, `0-9` et `?` — **aucune ponctuation** — et
   son repli actuel rend un glyphe vide pour un caractère non dessiné situé dans
   l'intervalle. Sans cette règle, `GitHub:mae` et `GitHub mae` s'afficheraient
   **identiquement** : exactement l'attaque que la décision 4 vise à empêcher.
3. **Tronquer à vingt-et-un caractères avec un marqueur visible.** Sans
   marqueur, deux comptes dont les noms divergent après la coupe seraient
   indiscernables, et l'appui redeviendrait aveugle. L'avant-dernier caractère
   dessiné porte une empreinte du nom entier, pour la même raison.

   > **Corrigée le 2026-08-19 — c'était une erreur de police.** La version
   > précédente disait « tronquer à **dix** caractères. Dix est la largeur
   > réelle en police **double hauteur** sur 128 px ». La mesure était juste,
   > mais elle ne s'applique pas à cette ligne : dans `main/hmi/screen.c`,
   > l'étiquette de compte est dessinée par `draw_text_centered()` — police
   > **simple** hauteur, `SCREEN_CHAR_PX` = 6 px, donc 128 / 6 = **21**
   > caractères. Seul le libellé d'opération (`CODE OTP`, `RESET OATH`) passe
   > par `draw_text_2x_centered()` et subit la contrainte des dix. Le budget
   > d'affichage du nom était donc divisé par deux sans raison, ce qui rendait
   > la troncature bien plus agressive qu'il ne fallait — et pesait
   > directement sur la règle 1. `OATH_NAME_DISPLAY_MAX` vaut désormais 22
   > (21 dessinés + terminateur) ; un vingt-deuxième caractère ferait 132 px,
   > `screen_center_x()` collerait à gauche et `fb_set_pixel()` amputerait le
   > dernier glyphe — celui qui porte le marqueur. Vérifié par
   > `test_le_nom_de_compte_tient_en_police_simple`.

La règle 2 corrige un défaut de la police qui touchera aussi l'affichage du site
pour les passkeys FIDO2 (tâche #42) : le corriger ici le corrige pour les deux.

## Erreurs

| situation | réponse |
|---|---|
| applet non sélectionné | `6A82` |
| commande inconnue | `6D00` |
| PUT au-delà de seize comptes | `6A84` (mémoire pleine) |
| nom inconnu sur CALCULATE / DELETE / RENAME | `6A82` |
| défi de longueur ≠ 8 | `6A80` |
| appui non donné en quinze secondes | `6985` |
| secret plus long que 64 octets | `6A80` |

Toute donnée venue de l'hôte est bornée avant usage : longueur de nom, longueur
de secret, longueur de défi, nombre de TLV. C'est la surface d'attaque de ce
mode, et elle passera par `niphar-security-auditor` avant fusion.

## Tests

Norme TDD du projet : test écrit d'abord, rouge constaté, vert après, et
**mutation obligatoire** pour prouver qu'il mord.

- `oath_proto` — troncature RFC 4226 sur les vecteurs de la RFC ; découpe
  `SEND REMAINING` avec douze comptes ; refus des commandes hors portée ; bornes
  sur chaque longueur venue de l'hôte.
- `oath_name` — issuer extrait, caractères sans glyphe rendus visibles,
  troncature marquée. **Piège à éviter** : comparer un nom à une constante ne
  prouve rien sur le fait que deux noms *différents* restent *distinguables*.
  Les assertions comparent des paires de noms entre elles, comme
  `screen_op_has_deadline()` compare les deux familles d'opérations.
- `sec_store` v2 — bornes de slot, refus des longueurs excessives, invariant
  « aucun secret ne sort par une fonction publique autre que
  `sec_store_get_secret()` ».

Validation matérielle, en fin de parcours : `ykman oath accounts add` pour un
compte de test, puis `ykman oath accounts code <nom>` avec appui réel, code
vérifié contre `oathtool` sur le même secret et le même instant.

## Risques

**Le seul risque protocolaire sérieux est levé** : on a lu la source de `ykman`
plutôt que de supposer, et le mécanisme d'attente (WTX) existe déjà et tourne.

Reste à surveiller :

- **La durée de vie de l'attente.** `dongle_confirm()` bloque la tâche CCID
  jusqu'à quinze secondes. Le chemin de démontage USB pendant l'attente est déjà
  traité dans `ccid.c` (`s_shutdown`) ; il faut vérifier qu'OATH emprunte bien ce
  même chemin et ne le contourne pas.
- **La migration des douze comptes.** Elle passe par `ykman oath accounts add`,
  donc par `PUT` : un défaut de bornage sur `PUT` se manifesterait pendant la
  migration, avec les vrais secrets en main. Les tests de bornes doivent être
  verts *avant* le premier compte réel.
