# Écran OLED — affinage de l'UX

Addendum à [`2026-08-17-ecran-oled-carte-cle-design.md`](2026-08-17-ecran-oled-carte-cle-design.md).
Écrit après que Mae a vu les écrans de la tâche 4 sur la dalle réelle.

## Le constat, dans ses mots

> « actuellement ux c'est juste du texte en haut a gauche »
> « par exemple "au repos" il y a rien sur l'ecran juste "au repos" dans le coins en tout petit »
> « quand je navigue l'ecran est utiliser 10% »

Ce n'est pas une question de goût : **90 % d'une dalle de 128×64 est vide**, et
les trois informations affichées (titre, ligne, barre) ont toutes la même taille.
Rien ne dit ce qui compte.

Diagnostic précis de ce que fait `render_frame()` (`main/hmi/screen.c:218`) :

| symptôme | cause dans le code |
|---|---|
| tout en haut à gauche | `draw_text(title_x, 4)`, `draw_text(2, 24)` — coordonnées fixes, aucun centrage |
| tout de la même taille | une seule police, 5×7 dans une cellule 6×8 (`s_font`, `screen.c:60`) |
| écran vide au repos | `SCREEN_IDLE` n'a qu'un titre et pas de `line` |
| version du splash coupée | `draw_text(SCREEN_LOGO_WIDTH + 4, 32, NIPHAR_VERSION)` à x=68 : 60 px restants = 10 caractères, or `git describe` rend `6da0d70-dirty` (13) faute de tag |

Le dernier point n'est **pas** un défaut mémoire : `fb_set_pixel()`
(`screen.c:151`) borne les quatre côtés et le tronquage est explicitement voulu
pour l'animation de glissement. C'est un défaut de disposition.

## Le principe directeur

**Une seule information domine par écran, et elle occupe la dalle.** Tout le
reste est du contexte, en petit, sur les bords. C'est ce qui distingue un
appareil d'une console de debug.

## Les cinq changements, par ordre d'effet

### 1. Police double hauteur pour la ligne qui compte

12×16 par **doublement de pixels** depuis la police 5×7 existante — aucune donnée
nouvelle en flash, une trentaine de lignes. Rend 10 caractères par ligne
(`SIGNATURE` = 9, `DECHIFFRER` = 10, `AUTH` = 4 : ça passe).

Pourquoi le doublement plutôt qu'une vraie police 12×16 : une table de glyphes
supplémentaire coûterait ~2 Kio, et à cette taille sur un OLED de 0,96" la
grossièreté du doublement est à peine perceptible. Si ça se voit à l'œil, on
achètera la vraie police plus tard — mais on ne paie pas d'avance.

### 2. Bandeau inversé pleine largeur

Blanc sur noir, 128 px de large, 10 px de haut. Sur un monochrome c'est
l'élément qui a le plus de poids visuel, et il occupe la largeur — ce que rien
ne fait aujourd'hui. Coût : inverser un octet du tampon.

**Il porte le mode, pas l'état.** `AU REPOS` ne dit rien ; savoir si la carte
PGP est exposée est justement l'information qui compte. Donc :
`RIEN EXPOSE` / `DISQUE` / `CARTE PGP` / `CLE OTP`.

### 3. Quatre points de cycle

`○ ● ○ ○` — quel mode sur les quatre. C'est le **seul ajout qui apporte une
information que Mae n'a pas** : la LED donne une couleur qu'il faut mémoriser
(sa question « je suis en bleu mais fait quoi ? » vient de là), les points
disent où on est **et combien d'appuis pour aller où on veut**.

### 4. Secondes restantes en chiffre, sous la barre

La barre dit qu'il reste du temps, le chiffre dit combien. Deux générations de
clés ont été perdues sur des expirations invisibles — ce n'est pas décoratif.

### 5. Coche et croix dessinées pour le verdict

Deux bitmaps de ~32 octets à la place de `ACCORDE` / `REFUSE` en petit. Se
lisent sans lire.

## Les quatre écrans affinés

```
AU REPOS                      CONFIRMER ?
┌─────────────────────┐      ┌─────────────────────┐
│    ▄▄▀▀███▀▀▄▄      │      │▓▓▓▓▓ CONFIRMER ▓▓▓▓▓│
│   ██  ▄▄▄▄▄  ██     │      │                     │
│   ██  ▀▀▀▀▀  ██     │      │  ███ ██ ███ █  ███  │
│    ▀▀▄▄███▄▄▀▀      │      │  ██  ██ ██ ███ ██   │  12×16, centré
│                     │      │  ███ ██ ███ █  ███  │
│▓▓▓▓ RIEN EXPOSE ▓▓▓▓│      │▇▇▇▇▇▇▇▇▇▇▇░░░░░░░░░░│  pleine largeur
└─────────────────────┘      │        11 s         │
                             └─────────────────────┘

VERDICT                       BASCULE DE MODE
┌─────────────────────┐      ┌─────────────────────┐
│                     │      │ MODE                │
│         ▄▄██        │      │                     │
│       ▄███▀         │      │  ███  ███  ███      │
│  ▄██▄███▀           │      │  ██   ██   ██       │  glisse
│   ▀███▀             │      │  ███  ███  ███      │
│                     │      │                     │
│▓▓▓▓▓ ACCORDE ▓▓▓▓▓▓▓│      │   ○  ●  ○  ○        │
└─────────────────────┘      └─────────────────────┘
```

Le repos porte le logo Niphargus (décision de Mae : « au repos met le logo
niphar plutot »), déjà en flash depuis la tâche 6 (`screen_logo.h`, 512 o).

## La rémanence, tranchée

Un OLED qui affiche 1425 pixels allumés en permanence **brûle** : le logo se
graverait dans la dalle en quelques semaines d'usage.

Trois mesures, cumulées :
1. Le logo **dérive** de ±4 px — `screen_shift_px()` existe déjà (tâche 3,
   `screen_anim.h`), il suffit de l'appliquer au repos et non seulement au texte.
2. L'écran **s'éteint** au bout d'une minute d'inactivité (commande SSD1306
   `0xAE`), et se rallume au premier appui ou au premier événement.
3. Aucun élément inversé n'est permanent : le bandeau n'apparaît qu'avec le logo,
   qui dérive.

Sans le point 2, le point 1 ne fait que répartir la brûlure sur 8 px de plus.

## Ce qui est pur et donc testé d'abord

Ces fonctions ne connaissent aucune géométrie d'écran et vont dans `test/` :

- `screen_text_px(const char *s)` — largeur d'un texte, pour centrer
- `screen_center_x(uint16_t width_px, uint16_t text_px)` — origine centrée, jamais négative
- `screen_mode_index(usb_mode_t)` → 0..3, et `screen_mode_count()` — les points
- `screen_seconds_left(armed_at_ms, now_ms)` — le chiffre, arrondi **vers le haut**
  (afficher « 0 s » alors qu'il reste 900 ms mentirait dans le sens dangereux)
- `screen_verdict_glyph(led_event_t)` → coche / croix / rien
- `screen_blank_after_ms(last_activity_ms, now_ms)` — l'extinction

Le tracé (`draw_*`, `render_*`) reste dans `screen.c` : ce sont des pixels, pas
des décisions.

## Ce que ça ne fait pas

**Pas de menu navigable.** Mae a explicitement écarté cette lecture (« non
affiné »). Un menu capable de modifier l'état de la clé serait une nouvelle
surface d'attaque physique, et ce n'est pas ce qui est demandé.
