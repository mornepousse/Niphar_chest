# Lien v3 — le clavier nomme, navigue, et affiche les codes

**État : CONCEPTION ARRÊTÉE, en relecture chez KeSp. Rien n'est implémenté.**
Deux réponses de KeSp et une décision de Mae manquent — elles sont marquées
« OUVERT » ci-dessous. Mae a demandé qu'on conçoive tout avant de publier une
seule fois : KeSp a déjà repris deux fois la carte des registres.

## Ce qu'on veut

Trois besoins, empilés, du plus ancien au plus récent :

1. **Nommer le compte visé.** Sur la clé autonome, l'écran est celui du coffre et
   affiche « CODE OTP GITHUB ». Sur le coffre, l'écran est celui du CLAVIER, qui
   ne reçoit qu'un code d'opération : la propriétaire approuve un TYPE, jamais un
   COMPTE. La décision 4 de la spec OATH existe précisément pour l'empêcher, et
   elle n'est tenue que sur la clé.
2. **Afficher le code TOTP et son décompte** sur l'écran du clavier.
   D'abord une question d'accessibilité : lire le code sur le clavier plutôt que
   de dépendre de ce que l'hôte veut bien montrer.
3. **Naviguer dans les comptes** depuis le clavier.

Le scénario qui a tranché le point 1 : elle demande le code de GITHUB, un hôte
malveillant en demande un pour BANQUE dans la même seconde. L'instance (v2)
empêche l'appui de glisser de l'une à l'autre ; elle ne dit pas laquelle est
affichée. Les deux blocs sont indiscernables à l'écran.

## Deux contraintes dures, mesurées

- **`SOC_SPI_MAXIMUM_BUFFER_SIZE = 64`** sur l'ESP32-P4
  (`components/soc/esp32p4/include/soc/soc_caps.h:574`). Les registres partagés
  font 64 octets, et une carte v3 portant un libellé utile les occupe TOUS. Il
  n'y a pas la place pour la navigation, un code et un décompte.
- **Le coffre n'a aucune horloge.** Ni RTC, ni pile. Il ne connaît que son temps
  monotone depuis le démarrage. Un TOTP vaut `HMAC(secret, floor(unix/30))` :
  sans heure, pas de code. Aujourd'hui l'heure vient de l'hôte, dans le défi que
  `CALCULATE` transporte — et c'est pour ça que le code sort vers l'hôte.

## Acquis (ne dépendent d'aucune réponse)

- **Le libellé sur le fil est de l'ASCII imprimable STRICT, par construction.**
  `main/security/oath_name.c:15` n'accepte que `0x20`–`0x7E`, met en majuscules,
  et remplace tout le reste — contrôle, octet haut, UTF-8 multi-octets — par
  « ? » AVANT le fil. Conséquence pour KeSp : leur police UNSCII suffit, leur
  substitution `?` ne se déclenchera jamais. À écrire au contrat ET à mettre sous
  test, pour que ça reste vrai.
- **Le coffre ne produit que 21 caractères aujourd'hui**
  (`OATH_NAME_DISPLAY_MAX`), parce que c'est la largeur de son propre OLED.
  L'écran de KeSp en affiche ~48 (68×68 px, UNSCII 8, 6 lignes de 8). Le champ
  se dimensionne sur LEUR écran ; l'élargissement côté coffre sera un changement
  interne, sans nouvelle version de protocole.
- **Piège connu de l'élargissement** : `oath_name_display()` tronque à `out_sz`
  avec une EMPREINTE, pour que deux comptes divergeant au-delà de la coupe
  restent distinguables. Produire un libellé long pour le lien et le laisser
  tronquer à 21 par l'OLED perdrait l'empreinte sur l'écran du coffre. Les deux
  largeurs doivent chacune passer par `oath_name_display()`, jamais par une
  troncature en aval.
- **Le canal DMA existe.** `SOC_SPI_SUPPORT_SLAVE_HD_VER2 = 1` ; le pilote expose
  `spi_slave_hd_queue_trans()` (mode segment), de taille libre et indépendant des
  registres partagés. Le lien est déjà en `SPI_DMA_CH_AUTO`.

## TRANCHÉ 1 — l'heure vient de l'hôte, une fois par session USB

**La gauche n'a pas d'horloge murale digne de confiance** (KeSp) : pas de quartz
32 kHz, RTC du S3 sur RC interne (dérive au pour-cent, des minutes par jour en
veille), pas de pile de sauvegarde, pas de RTC dans le nRF24.

**Mais la veille n'arrive jamais quand le coffre tourne.** Le coffre n'existe
que branché en USB, et branché, la gauche ne dort pas (veto USB) : elle compte
alors sur son quartz principal 40 MHz, soit **moins de 2 s de dérive par jour**,
très en dessous de la fenêtre TOTP de 30 s.

Donc : l'hôte pose l'heure **une fois par branchement**, la gauche l'entretient
en monotone tant que l'USB tient, et un drapeau « heure valide pour cette
session » retombe au débranchement. Sans heure valide, **le clavier n'affiche
aucun code et le dit** (« NO TIME ») — jamais un code faux présenté comme juste.

Précision attendue : ±1 s au réglage, plus < 2 s/jour.

**Côté hôte : `niphar-oath set-time`** (décision de Mae). Pas de service
système, pas de règle udev : la commande vit dans le client TUI qui existe déjà
et qui est compatible lecteur d'écran.

**L'HEURE VIT DANS LE COFFRE, PAS DANS LE CLAVIER.** C'était incohérent dans la
première version de ce document, et KeSp l'a relevé : `niphar-oath` parle au
COFFRE en CCID. Faire détenir l'heure au clavier aurait exigé une commande CDC
de plus chez eux **et** un client hôte bilingue. L'heure part donc là où le
client sait déjà parler :

- `niphar-oath set-time` envoie l'heure au coffre par le canal CCID existant
  (commande d'extension de l'applet OATH — hors du jeu YKOATH, que `ykman`
  ignorera simplement) ;
- le coffre l'entretient en monotone sur `esp_timer`. **Il n'existe que branché
  et redémarre au débranchement : son heure s'efface donc toute seule.** C'est
  une invalidation par construction, pas un drapeau à tenir — aucune heure
  périmée n'est possible ;
- l'heure survit à une bascule de mode USB (le coffre ne redémarre pas), donc
  elle vit hors de l'applet, à côté de `sec_confirm` et non dedans ;
- le coffre publie **« heure valide », bit 3 de `0x05`**. Le clavier affiche
  « NO TIME » quand il vaut 0 et ne demande aucun code ;
- le champ heure de la requête DMA disparaît.

`set-time` n'exige **aucun appui** : il ne révèle rien. Conséquence de flot à
connaître : le coffre doit être en mode OATH pour recevoir l'heure, puisque
c'est le CCID qui la porte.

**Limite à écrire au contrat** : un hôte qui ment sur l'heure fait calculer les
codes d'une autre fenêtre. Il n'en apprend rien — ils ne s'affichent que sur
l'écran du clavier et ne repassent jamais par l'hôte. Au pire un code faux
s'affiche et le service le refuse. C'est inhérent à la classe d'appareil, comme
déjà tranché pour TOTP : un hôte menteur ne se distingue pas d'une semaine
débranchée.

## TRANCHÉ 2 — registres pour l'état, DMA pour la navigation

KeSp confirme que leur maître émet les commandes DMA du protocole esclave-HD
(WRDMA `0x03`, RDDMA `0x04`), `max_transfer_sz` effectif **5120 octets**.
Contraintes de leur côté, qui commandent la forme : toute transaction passe sous
leur verrou radio, donc **segments courts** ; tampons DMA statiques en RAM
interne ; et **ils ne doivent jamais lire un segment qui n'est pas en file** —
d'où un signal « données prêtes, longueur N » dans les registres.

## TRANCHÉ 3 — naviguer est libre, révéler exige l'appui

Décision de Mae. La liste des NOMS défile sans rien approuver ; **le code
n'apparaît qu'après un appui, et pour ce compte-là**.

Ce que ça préserve : aujourd'hui, quelqu'un qui passe cinq secondes devant le
clavier n'obtient rien. Avec une navigation qui révélerait les codes, un coup
d'œil les donnerait tous. Ici, un coup d'œil donne la liste des noms — ce qu'un
`LIST` YKOATH donne déjà à l'hôte — et aucun code.

Et ça ne coûte rien de plus : sortir un code exige **déjà** un appui
(`SEC_OP_OATH_CODE`). On réutilise la machinerie existante telle quelle, au lieu
d'en créer une seconde qui en divergerait.

## La carte v3

### Registres partagés (64 o) — l'état, lisible à tout instant

```
0x00-0x03  magique « NIPH »          coffre
0x04       version = 3               coffre
0x05       bits d'etat               coffre
0x06-0x07  operation en attente      coffre
0x08-0x0B  confirmations consommees  coffre
0x0C       numero d'instance         coffre
0x0D       mode USB actif            coffre
0x0E       longueur du libelle       coffre   (0..34)
0x0F       nombre de comptes vises   coffre   (0 si rien d'arme, 1, ou N pour RESET)
0x10       type du segment en file   coffre   (0 = rien en file)
0x11       numero du segment         coffre   (incremente a chaque mise en file)
0x12-0x13  longueur du segment       coffre   (petit-boutiste)
0x14-0x35  libelle, 34 o ASCII       coffre   (non termine par zero)
0x36-0x37  CRC16 sur 0x00..0x35      coffre   (petit-boutiste)
0x38       confirmation 0x5A         maitre
0x39       echo du numero d'instance maitre
0x3A       mode USB demande          maitre
0x3B       reserve                   maitre
0x3C       numero de requete         maitre   (sonnette : incremente = requete prete)
0x3D-0x3F  reserve                   maitre
```

Invariants de la v2, tous tenus : coffre `0x00`–`0x37` (56 o, **14 mots**
pleins alignés), maître `0x38`–`0x3F` (8 o, **2 mots**), **aucun mot partagé**,
CRC dernier champ du coffre, étendue **contiguë** depuis zéro. Total 64 =
`SOC_SPI_MAXIMUM_BUFFER_SIZE` du P4.

**LA SONNETTE EST DANS LE SECOND MOT DU MAÎTRE, ET CE N'EST PAS ESTHÉTIQUE.**
Relevé par KeSp, et confirmé par le code : `link_spi.c:365` appelle
`spi_slave_hd_write_buffer(LINK_HOST, LINK_REG_USER_CONFIRM, &cleared, 1)` pour
effacer l'octet de confirmation consommé — et le pilote écrit **par mots de 32
bits**. Cette écriture d'un octet est donc une lecture-modification-écriture sur
tout le premier mot du maître. Une sonnette logée dedans serait écrasée quand
elle tombe dans ces quelques cycles, et **la requête ne serait jamais servie, en
silence**. C'est la fenêtre que le §5 du contrat décrit déjà pour l'octet de
mode — sauf que le mode se relit et se réécrit, alors qu'une sonnette ne se
relit pas. Elle vit donc en `0x3C`, dans le mot que le coffre n'écrit jamais.

**Le libellé passe de 42 à 34 octets** par rapport à ce que j'avais annoncé :
les deux mots du maître et les trois octets de signalisation DMA les prennent.
34 reste très au-dessus des 21 que le coffre produit aujourd'hui, et couvre
« GITHUB:MAE@EXAMPLE » et ses cousins.

### Requête (WRDMA, 16 o fixes, maître → coffre)

```
0x00      commande        (0x01 LIST, 0x02 CODE)
0x01      argument        (LIST : premier index ; CODE : index du compte)
0x02-0x0D reserve
0x0E-0x0F CRC16 sur 0x00..0x0D
```

Seize octets, multiple de quatre — le pilote tronque une réception qui ne l'est
pas.

**`LIST` est paginé depuis le premier jour**, sur la suggestion de KeSp : son
argument est un premier index (`0` = depuis le début), la réponse est bornée à
512 octets et porte un drapeau « suite ». Douze comptes tiennent aujourd'hui en
une seule réponse ; la pagination ne coûte rien maintenant et évite une v4 le
jour où quelqu'un en a quarante.

### Réponse (RDDMA, longueur annoncée en `0x12-0x13`)

`LIST` — la liste des comptes, noms seuls, aucun code :
```
0x00      nombre de comptes
puis, par compte : index (1 o), longueur (1 o), nom (n o, ASCII imprimable)
puis CRC16 sur tout ce qui precede
```

`CODE` — **uniquement après un appui accordé** :
```
0x00      index du compte
0x01      nombre de chiffres (6 ou 8)
0x02-0x09 code en ASCII, complete a gauche par des zeros
0x0A      secondes restantes dans la fenetre
0x0B      reserve
0x0C-0x0D CRC16 sur 0x00..0x0B
```

### Ce que le mode segment exige des deux côtés

Deux obligations, à écrire au contrat parce qu'elles ne se devinent pas au banc.

**Le coffre garde EN PERMANENCE une réception en file** (`spi_slave_hd_queue_trans`
sur `SPI_SLAVE_CHAN_RX`). Sans réception armée, un WRDMA du maître est **perdu
sans erreur** — ni de son côté, ni du nôtre. La tâche du lien réarme donc
immédiatement après chaque segment consommé.

**Les commandes de fin de segment**, relevées dans l'en-tête et non de mémoire
(`components/hal/esp32p4/include/hal/spi_ll.h:81-90`). En mode une ligne, l'octet
de fil vaut la commande de base :

| commande | octet | quand |
|---|---|---|
| `RDBUF`  | `0x02` | lire les registres partagés |
| `WRBUF`  | `0x01` | écrire les registres partagés |
| `WRDMA`  | `0x03` | envoyer le segment de requête |
| `WR_END` | `0x07` | **après** le WRDMA — clôt le segment en écriture |
| `RDDMA`  | `0x04` | lire le segment de réponse |
| `INT0`   | `0x08` | **après** le RDDMA — clôt le segment en lecture |

Références maître : `essl_spi_wrdma_done()` émet `SPI_CMD_HD_WR_END`,
`essl_spi_rddma_done()` émet `SPI_CMD_HD_INT0`
(`components/driver/test_apps/components/esp_serial_slave_link/essl_spi.c`).

### L'index peut changer entre LIST et CODE, et c'est sans danger

Un compte peut être ajouté ou effacé par l'hôte entre le moment où le clavier
lit la liste et celui où il demande un code : l'index qu'il envoie ne désigne
alors plus le même compte.

**Ce qui rend ça inoffensif, c'est le libellé, pas l'index.** Ce que la
propriétaire voit avant d'appuyer est le nom que le COFFRE a publié en `0x14` au
moment de l'armement, jamais la copie que le clavier garde de sa liste. Si
l'index a glissé, elle voit le nom du compte réellement visé et n'appuie pas.
C'est exactement la décision 4, et c'est pour ça qu'elle vaut la peine.

**Engagement du clavier** (KeSp) : afficher TOUJOURS le libellé du registre
pendant l'invite, jamais le nom tiré de son propre `LIST`.

### Les engagements d'IHM du clavier

Ils appartiennent au contrat autant que les octets — le coffre ne peut pas les
tenir à leur place :

- un code ne s'affiche **jamais** sans appui préalable ;
- il disparaît à la fin de sa fenêtre, ou dès qu'on navigue ;
- il n'est **jamais** rafraîchi automatiquement : un nouveau code demande un
  nouvel appui ;
- la liste des noms défile librement.

### Le flot d'un code, bout à bout

1. Le maître écrit sa requête `CODE(index)` en WRDMA, clôt par `WR_END`, puis
   incrémente la sonnette `0x3C`.
2. Le coffre voit la sonnette, **arme `sec_confirm`** avec le libellé du compte :
   `pending_op` = `SEC_OP_OATH_CODE`, instance publiée, libellé publié en
   `0x14`. Rien n'est calculé, rien n'est mis en file.
3. Le clavier affiche le nom et attend l'appui.
4. La propriétaire appuie ; le maître écrit `0x5A` en `0x38` **et l'instance
   lue** en `0x39` — exactement le protocole de confirmation de la v2, inchangé.
5. Le coffre vérifie les deux, calcule le HMAC, met le segment en file, publie
   `type`, `numero` et `longueur`.
6. Le maître voit le numéro changer, lit le segment en RDDMA, clôt par `INT0`,
   et affiche.

**Sans l'étape 4, l'étape 5 n'arrive jamais** : aucun code n'est mis en file
sans appui accordé. Le segment se consomme une fois et le coffre l'efface —
une confirmation ne sert pas deux fois, comme pour le chemin CCID.

## Ce qu'on ne refera pas

KeSp a repris la carte des registres deux fois (instance en v2, mode actif
ajouté après publication). La v3 ne se publie qu'une fois : disposition
complète, vecteurs régénérés en exécutant le code, et le commit du coffre
flashé communiqué en même temps — c'est leur demande, et elle est justifiée
(« avant d'accuser le fil, on vérifie quel commit tourne »).
