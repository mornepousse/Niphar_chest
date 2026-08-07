# Portage de la pile de sécurité — OpenPGP CCID et CR-HMAC OTP-HID

Design validé le 2026-08-07. Troisième incrément du firmware du coffre.

## 1. Problème

Le coffre doit devenir un token OpenPGP et une clé de sécurité. Cette pile
existe déjà, écrite et éprouvée dans `KeSp_firmware` : ~2500 lignes dont
l'OpenPGP CCID a été validé sur matériel avec gpg 2.4.9. La réécrire serait une
régression de sécurité gratuite.

Deux choses empêchent aujourd'hui d'aller au bout, et cette spec les traite
comme des contraintes plutôt que de les ignorer :

- **La confirmation physique n'a pas de source.** Toute la solidité du modèle
  KeSp tient à `sec_confirm` : un appui réel sur une touche, qu'un malware hôte
  ne peut pas fabriquer. Sur le coffre, cet appui arrivera par le lien SPI —
  reporté à la carte révisée. Sans substitut, rien n'est exécutable.
- **Le seul matériel qui existe est le kit de dev**, et son firmware ne doit
  jamais servir de base à celui du coffre par accident.

### Correction d'une confusion, qui a manqué changer la portée

FIDO/CTAP **n'existe pas** dans `KeSp_firmware`. Sa « clé de sécurité » est un
CR-HMAC compatible YubiKey sur HID, et FIDO2 y est explicitement noté « Phase 3
— out of scope ». Ce qui se porte est donc **CCID + OTP-HID** ; un vrai CTAP2
serait du développement neuf, à décider séparément.

## 2. Une fonction à la fois, commandée par le clavier

**Le coffre n'expose jamais plus d'une fonction USB.** C'est la devise du
projet — « plein de choses, une à la fois » — et c'est une décision de sécurité
autant que d'ergonomie : une clé PGP ne doit pas être présente sur le bus parce
qu'on voulait un disque.

Quatre modes, exclusifs :

| mode | ce que l'hôte voit |
|---|---|
| `NONE` | rien — état au démarrage |
| `STORAGE` | le disque (MSC) |
| `PGP` | la carte OpenPGP (CCID) |
| `OTP` | la clé CR-HMAC (HID) |

**Le sélecteur est le S3.** Le coffre vit dans la moitié gauche du clavier :
partout où tu emportes le clavier, le S3 est là et alimenté, donc capable de
commander. Sur le kit de dev, faute de S3, une commande console tient ce rôle —
verrouillée par construction comme la béquille de confirmation (§5).

**Au démarrage, rien.** Une fonction ne s'expose que sur ordre explicite, et
l'ordre ne survit pas au débranchement. Un coffre branché sur une machine
inconnue ne présente donc aucune surface tant que tu n'as rien demandé.

Deux conséquences, dont une gênante :

- **Changer de mode impose une ré-énumération.** Les interfaces d'un
  périphérique USB configuré ne se modifient pas : il faut quitter le bus et y
  revenir avec d'autres descripteurs. L'hôte verra un débranchement suivi d'un
  rebranchement à chaque bascule. C'est imposé par l'USB, pas choisi.
- **Sur `niphar_chest`, tant que le lien SPI n'existe pas, rien ne peut être
  sélectionné** — le firmware du coffre sera inerte côté USB. C'est cohérent
  avec le reste : refuser plutôt que d'accorder par défaut.

Le budget d'endpoints du P4 (16 contre 4 sur le S3) cesse d'être un argument de
conception, puisqu'on n'expose jamais plus d'un jeu à la fois. Il reste une
marge confortable, rien de plus.

## 3. Portée

Dans la portée : le portage des deux volets, la bascule de mode USB et son
sélecteur, une source de confirmation utilisable sur le kit, et le portage des
tests hôte existants.

Hors portée : le transport SPI du lien (sa propre spec, en attente de la carte),
un vrai CTAP2, et la décision sur le stockage des clés (§8).

## 4. Ce qui se copie, et ce qui ne se copie pas

Le couplage de la pile à `KeSp_firmware` est minuscule : elle n'emprunte à
`keyboard_config.h` qu'un `STORAGE_NAMESPACE "storage"`, et `nvs_utils` se
déclare lui-même réutilisable sans dépendance projet.

```
main/sys/
├── nvs_utils.{c,h}       verbatim de KeSp
└── cr_crc16.{c,h}        déplacé depuis main/link/

main/security/
├── apdu.{c,h}            \
├── ccid.{c,h}             |
├── openpgp_card.{c,h}     |
├── openpgp_crypto.{c,h}   |  copiés, quasi verbatim
├── openpgp_do.{c,h}       |
├── otp_hid.{c,h}          |
├── otp_proto.{c,h}        |
├── cr_hmac.{c,h}          |
├── sec_confirm.{c,h}     /
├── sec_store.{c,h}       /
├── sec_config.h          NOUVEAU — remplace keyboard_config.h
└── sec_gate.{c,h}        NOUVEAU — source de confirmation
```

**`cr_crc16` remonte dans `main/sys/`.** Il avait été placé dans `main/link/` le
2026-08-07 sans savoir que la pile CCID s'en servirait aussi. Deux copies
divergentes entre volets seraient exactement le bug silencieux que son propre
commentaire prétend éviter.

**Règle de portage :** modifier le moins possible. Un fichier copié qui diverge
de son original perd le bénéfice d'avoir été éprouvé, et les corrections
futures de KeSp ne s'y reporteront plus. Toute divergence délibérée se
documente en tête de fichier.

## 5. `sec_gate` — la seule pièce vraiment neuve

`sec_confirm` reste inchangé : il arme, il expire, il consomme. Ce qui manque
est la source de l'appui, et elle dépend de la carte.

| carte | source | état |
|---|---|---|
| `niphar_chest` | le lien SPI | pas encore écrit — les opérations expirent |
| `jc_devkit` | commande console `sec confirm` | utilisable immédiatement |

**La béquille ne doit pas pouvoir partir en production**, et une confirmation
automatique est indistinguable, à l'usage, d'un dispositif qui fonctionne. Elle
est donc verrouillée par construction, sur le modèle du garde Secure Boot :

- la variante console n'est compilée que si `BOARD_LINK_AVAILABLE == 0` ;
- un `#error` interdit de l'activer sur une carte qui a le lien ;
- `scripts/fast.sh` échoue si la variante console apparaît dans un build
  `niphar_chest`.

Sur le coffre avant que le lien existe, les opérations expireront au bout de
15 s. C'est le comportement honnête : refuser plutôt que d'accorder.

## 6. USB à mode unique

Chaque mode a son propre jeu de descripteurs, et un seul est monté à la fois.
Les endpoints ne sont donc jamais en concurrence : chaque mode repart de la
même paire.

| mode | interface | OUT | IN |
|---|---|---|---|
| `STORAGE` | MSC | `0x01` | `0x81` |
| `PGP` | CCID | `0x01` | `0x81` |
| `OTP` | HID | `0x01` | `0x81` |
| `NONE` | — | — | — |

Un nouveau module **`usb_mode`** possède la pile USB : il installe TinyUSB avec
les descripteurs du mode demandé, et bascule en détachant puis réinstallant.
`usb_device.c` cesse d'être le propriétaire du périphérique pour devenir la
mécanique que `usb_mode` pilote.

`ccid.c` s'enregistre auprès de TinyUSB par `usbd_app_driver_get_cb`, un
mécanisme de TinyUSB **brut** — donc compatible avec l'architecture retenue au
socle, où `esp_tinyusb` a été écarté. Ce n'était pas prévu : le wrapper aurait
ici encore posé problème.

**Point délicat.** `usbd_app_driver_get_cb` est résolu à l'édition de liens,
pas à l'exécution : la classe CCID est présente dans le binaire quel que soit le
mode. Ce qui change d'un mode à l'autre, ce sont les **descripteurs** — donc ce
que l'hôte voit. Une classe compilée mais non décrite n'est jamais atteinte,
puisque aucune interface ne lui est associée. C'est correct, mais ça mérite
d'être su : le code de la carte OpenPGP est là même en mode `STORAGE`.

## 7. Tests

Le portage amène la plus grosse quantité de logique pure du projet à ce jour :
`apdu`, `otp_proto`, `cr_hmac`, `cr_crc16`, `sec_confirm`. **KeSp a déjà des
tests hôte pour chacun** — ils sont portés avec le code, sans les réécrire.

Le parsing d'APDU est la nouvelle surface d'attaque : des octets arbitraires
venant de l'hôte, interprétés comme des commandes. Il doit être couvert avant
d'être branché.

Sur le kit, bout en bout : `gpg --card-status`, génération de clé, signature
d'un commit, authentification SSH — avec `sec confirm` en guise de touche. Et
KeePassXC pour l'OTP, qui demande le patch de VID documenté chez KeSp.

## 8. Réserves, explicitement non résolues ici

**L'OTP-HID n'a jamais tourné sur matériel**, même chez KeSp — leur propre doc
le dit. Le kit permettra de le valider pour la première fois, ce que le S3 ne
pouvait pas faire faute d'endpoints. Tant que ce n'est pas fait, il reste du
code neuf déguisé en code éprouvé.

**Le stockage des clés n'est pas tranché.** L'audit du 2026-08-07 a montré que
l'hôte peut forcer le mode download par les lignes de contrôle de la CDC
(`docs/HARDWARE.md`), donc dumper la flash et la NVS. L'hypothèse héritée de
KeSp — clés en clair acceptables parce que l'extraction demande un accès
physique — ne tient plus telle quelle.

Sur le kit, sans enjeu : clés jetables sur une carte de développement. Mais
**ce portage ne doit pas décider par défaut** ce qu'on fera sur le coffre. La
décision (ouvrir JP1/JP2, brûler les eFuses, ou accepter le risque) se prend
avant que le coffre porte une vraie clé, pas au moment où le code existe déjà.

**Le kit reste un kit** : aucune option Secure Boot ni Flash Encryption dans
les defaults partagés. Le garde-fou existe déjà dans `scripts/fast.sh`.

## 9. Décisions et alternatives écartées

| Décision | Alternative écartée | Raison |
|---|---|---|
| Quatre modes exclusifs, un seul monté | composite MSC + CCID + HID permanent | « une à la fois » est la devise du projet et une décision de sécurité : une clé PGP ne doit pas être sur le bus parce qu'on voulait un disque |
| Aucune fonction au démarrage | dernier mode mémorisé en NVS | un coffre branché sur une machine inconnue ne présente aucune surface tant que rien n'a été demandé |
| CCID **et** OTP-HID portés | CCID seul d'abord | ils sont deux modes distincts, et l'OTP n'a jamais pu être validé ailleurs faute d'endpoints |
| Confirmation par la console, kit seulement | confirmation automatique sous Kconfig | rien n'empêcherait de la laisser active ; un dispositif qui s'auto-confirme se comporte comme un dispositif qui marche |
| Copie quasi verbatim | réécriture adaptée au coffre | le code est éprouvé sur matériel ; diverger coûte le bénéfice et coupe les corrections futures |
| `cr_crc16` dans `main/sys/` | une copie par volet | deux CRC divergents seraient un bug silencieux à l'interface, ce que son commentaire prétend justement éviter |
