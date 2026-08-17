# Écran OLED de la carte-clé — spécification de conception

*2026-08-17*

## Problème

La carte-clé sait dire **qu'elle attend un doigt** : sa LED alterne entre la
couleur du mode et le rouge, et l'utilisateur appuie. Validé sur matériel le
2026-08-17 — sans appui, la carte renvoie `6985` et l'opération est refusée ;
avec appui, elle passe.

Elle ne sait pas dire **pour quoi**.

C'est la limite structurelle de toute clé sans affichage. Un hôte malveillant
peut demander une signature pendant que l'utilisateur croit en confirmer une
autre : l'appui est donné de bonne foi, sur une opération que personne n'a
choisie. Le bouton prouve la présence d'un humain, jamais son consentement à
*cette* opération.

L'écran répare exactement ça. Il transforme un accusé de présence en
consentement éclairé, et c'est ce qui rendra un futur FIDO2 défendable : CTAP
exige un test de présence, mais une clé qui ne montre pas le site demandeur ne
protège pas contre un hôte qui substitue la demande.

Deux gains secondaires, réels mais moindres : l'état de la clé devient lisible
sans connaître le code couleur, et le décompte de quinze secondes cesse d'être
invisible — deux générations de clés ont échoué le 2026-08-17 sur des
expirations que rien n'annonçait.

## Portée

**Dans la portée** : le pilote SSD1306, une tâche d'affichage, la logique pure
qui décide du contenu et des animations, et l'ajout d'un **code d'opération** à
`sec_confirm` pour que l'écran sache nommer ce qu'il fait confirmer.

**Hors portée**, et délibérément :

- **le nom du site demandeur** — il n'y a pas de FIDO2. Le libellé d'attente est
  une chaîne calculée, donc la place existe ; on ne construit rien de plus pour
  un consommateur qui n'existe pas ;
- **les codes TOTP** — ce n'est pas de l'affichage mais une fonction nouvelle,
  et elle bute sur un obstacle matériel traité plus bas ;
- **la LED** — elle reste, inchangée. Voir « Répartition des rôles ».

## Matériel

| élément | fait |
|---|---|
| Écran | SSD1306, 128×64 monochrome, I²C |
| SCL | **IO53** |
| SDA | **IO54** |

Ces deux broches sont sûres : ni pin de strapping, ni restriction. Elles portent
des fonctions analogiques (`ADC2_CH4`/`CH5`, comparateur analogique canal 1) dont
ce projet n'a aucun usage — même famille qu'IO51, déjà prise par la LED WS2812
sans conséquence.

> `GPIO53 | ADC2_CH4, ANA_CMPR_CH1 reference voltage | |`
> `GPIO54 | ADC2_CH5, ANA_CMPR_CH1 input (non-inverting) | |`
> — *ESP-IDF Programming Guide*, « GPIO & RTC GPIO — ESP32-P4 », § GPIO Summary
> (colonne *Comments* vide = aucune restriction)

**À vérifier au banc avant d'alimenter** : les pull-ups I²C. Beaucoup de modules
SSD1306 les embarquent ; en ajouter en plus met des résistances en parallèle. Et
le module doit être alimenté en **3,3 V** — un SSD1306 en 5 V avec des pull-ups
vers 5 V renverrait du 5 V sur des broches qui ne le tolèrent pas.

## La contrainte qui gouverne l'architecture

**Une image pleine bloque 25 à 30 ms.** 128×64 monochrome = 1024 octets ; à
400 kHz, bits d'acquittement compris, c'est ~23 ms de bus, ~30 ms en pratique.

Or la tâche IHM **échantillonne les boutons toutes les 5 ms**, et l'anti-rebond
(`main/hmi/button_debounce.h`) suppose un échantillonnage régulier. Un
rafraîchissement bloquant mangerait six périodes d'échantillonnage — donc des
appuis ratés sur le contact qui porte la présence physique. Ce n'est pas un
inconfort : c'est le mécanisme de sécurité validé la veille.

**Décision : une tâche d'affichage séparée.** Les boutons gardent leur cadence,
l'écran vit à la sienne.

La raison décisive n'est pas la performance mais la barre de décompte : elle doit
continuer à se vider **pendant** que le firmware calcule une signature. Dans une
boucle unique, l'affichage se figerait exactement à l'instant où l'utilisateur
regarde l'écran pour savoir combien de temps il lui reste.

Deux approches ont été écartées :

- **rafraîchissements partiels dans la tâche IHM** (une bande de 8 lignes par
  tick, ~3 ms) — élégant, sans tâche ni état partagé, mais l'affichage reste
  couplé à la boucle des boutons et se fige quand elle se fige ;
- **accepter la gigue d'échantillonnage** — payer la sécurité en confort.

## Architecture

### Découpage

| fichier | nature | responsabilité |
|---|---|---|
| `main/hmi/screen_view.h` | **pur** | `(mode, attente, échéance, verdict, t) →` quel écran, quels textes |
| `main/hmi/screen_anim.h` | **pur** | décalage anti-marquage, remplissage de la barre, avancement du glissement |
| `main/hmi/screen.c` / `.h` | matériel | pilote SSD1306, tâche d'affichage, I²C |

Même règle que la LED, et pour la même raison : **aucune décision dans
`screen.c`**. Il reçoit une description de ce qu'il faut peindre et il peint.
Quel écran, quelle proportion de barre, quelle phase d'animation — tout est pur
et testé sur l'hôte.

La leçon vient de la branche précédente : `hmi.c` avait promis « aucune
décision » et en contenait quatre, dont une — la phase absolue de l'alternance —
qui produisait un vrai défaut d'affichage. Ce qui n'est pas extrait n'est pas
testé, et ce qui n'est pas testé dérive.

### État partagé entre les deux tâches

La tâche IHM publie un état minuscule que la tâche écran lit :

```c
typedef struct {
    usb_mode_t   mode;
    bool         confirm_pending;
    uint32_t     armed_ms;      /* pour la barre : début de l'attente */
    sec_op_t     op;            /* ce qui est armé — voir plus bas */
    led_event_t  verdict;
    uint32_t     verdict_ms;
} hmi_state_t;
```

**Un troisième contexte apparaît donc**, et le projet a une règle à ce sujet : le
raisonnement de concurrence en tête de `main/security/sec_confirm.c` énumère les
contextes d'appel et en tire une conclusion de sûreté. Il devra être étendu — et
la tâche écran ne doit **jamais** appeler `sec_confirm_poll()`, qui consomme
l'autorisation. Elle lit `sec_confirm_peek()`, comme la tâche IHM.

### Le code d'opération

L'écran d'attente doit nommer ce qu'il fait confirmer. Or `sec_confirm_arm()` ne
reçoit aujourd'hui qu'un numéro de slot — un entier sans description.

**Décision : un code d'opération, pas une chaîne.**

```c
typedef enum {
    SEC_OP_UNKNOWN = 0,
    SEC_OP_SIGN,        /* signature OpenPGP */
    SEC_OP_DECRYPT,
    SEC_OP_AUTH,
    SEC_OP_OTP,
} sec_op_t;
```

`sec_confirm` reste numérique — ni texte, ni allocation, ni borne de longueur à
défendre dans le module qui garde la porte. La traduction code → libellé est une
fonction **pure** de `screen_view.h`, donc testable, et FIDO2 y ajoutera ses
propres codes sans toucher au module de sécurité.

C'est une modification d'un module déjà audité et porté depuis `KeSp_firmware` :
elle doit rester minimale et être déclarée comme divergence.

## Les écrans

```
AU REPOS                    ATTENTE DE CONFIRMATION
┌─────────────────────┐     ┌─────────────────────┐
│                     │     │  CONFIRMER ?        │
│      OpenPGP        │     │                     │
│   ───────────────   │     │  Signature OpenPGP  │
│    prête · 3 sign.  │     │                     │
│                     │     │  ███████████░░░░░░  │
└─────────────────────┘     └─────────────────────┘
  décalé toutes les minutes   la barre se vide en 15 s

VERDICT (600 ms)            BASCULE DE MODE (400 ms)
┌─────────────────────┐     ┌─────────────────────┐
│                     │     │  OpenPGP  →         │
│       ACCORDÉ       │     │        → Clé OTP    │
│          ✓          │     │                     │
│                     │     │   (glissement)      │
└─────────────────────┘     └─────────────────────┘
```

**Au repos, l'écran affiche le mode en permanence, décalé de quelques pixels
toutes les minutes.** Les OLED marquent : un contenu statique laisse une trace
permanente, et cette clé peut rester branchée des journées. Le décalage coûte une
fonction pure et supprime le problème.

**La barre de décompte est la seule animation qui n'est pas décorative** : elle
rend visible les quinze secondes de `SEC_CONFIRM_TIMEOUT_MS`, aujourd'hui
totalement muettes.

## Répartition des rôles avec la LED

La LED **reste**, et ce n'est pas une redondance : elle se voit du coin de l'œil,
de loin, sans lire. L'écran se lit, mais suppose qu'on le regarde.

| | LED | écran |
|---|---|---|
| appeler l'attention | ✅ alternance vive | ✗ |
| dire le mode | couleur | nom en toutes lettres |
| dire **pour quoi** | ✗ | ✅ |
| dire le temps restant | ✗ | ✅ barre |
| verdict | flash | texte |

L'ambiguïté assumée du rouge — « j'attends » et « refusé » distingués par la
durée — est **levée par l'écran** : le texte dit lequel des deux. La LED garde
son rôle d'alerte périphérique, l'écran porte le sens.

## Gestion des absences

| situation | comportement |
|---|---|
| écran absent ou I²C muet | `screen_init()` journalise et rend une erreur ; la tâche n'est pas créée ; **la clé reste pleinement utilisable**, la LED continue seule. Même contrat que `hmi_init()` aujourd'hui (`main/main.c`) |
| erreur I²C en cours de route | on journalise, on saute l'image, on retente au tick suivant ; jamais de blocage d'une opération |
| verdict arrivant pendant une bascule | le verdict prime — c'est fugace et c'est ce que l'utilisateur cherche à lire |
| armement pendant une animation de bascule | l'attente prime — elle est le seul écran qui réclame une action |

## Vérification

**Sur l'hôte, tests écrits avant l'implémentation :**

- `test_screen_view.c` — totalité du mapping (aucun état sans écran) ; chaque
  code d'opération a un libellé, et deux codes distincts n'en partagent jamais un ;
  l'attente prime sur la bascule, le verdict prime sur la bascule.
- `test_screen_anim.c` — la barre est pleine à l'armement et vide à l'échéance,
  jamais négative ni au-delà de 100 % ; le calcul survit au repassage à zéro du
  compteur de millisecondes (`uint32_t`, ~49 jours) ; le décalage anti-marquage
  reste dans les bornes de l'écran pour tout instant.

Chaque test doit **mordre** : mutation introduite, rouge constaté, retour.

**Sur la carte** :

- démarrage : l'écran s'allume, affiche le mode, la clé reste muette sur l'USB ;
- `gpg` demande une signature : l'écran affiche « Signature OpenPGP » et la barre
  se vide ; l'appui l'interrompt et affiche « ACCORDÉ » ;
- ne pas appuyer : la barre se vide entièrement, l'écran affiche le refus, `gpg`
  échoue sur `6985` ;
- **la barre continue de se vider pendant que le firmware calcule** — c'est la
  raison d'être de la tâche séparée, donc le point à vérifier en priorité ;
- l'échantillonnage des boutons reste franc : aucun appui raté pendant les
  animations.

## Ce que cette spec ne résout pas

- **Le TOTP.** Sous-système distinct, et il bute sur un obstacle matériel : la
  carte n'a pas de pile, donc perd l'heure à chaque débranchement, alors que le
  TOTP exige ±30 s. Il faudrait resynchroniser depuis l'hôte — et un hôte
  malveillant pourrait alors mentir sur la date et faire produire les codes de
  n'importe quel instant futur, sur un appareil dont tout l'intérêt est de ne pas
  faire confiance à l'hôte. À trancher avant d'écrire quoi que ce soit.
- **FIDO2.** L'écran est conçu pour l'accueillir, rien de plus.
- **La substitution de demande par un hôte malveillant** n'est réduite que si
  l'utilisateur *lit* l'écran. Un écran qu'on ignore ne protège pas mieux qu'une
  LED. C'est une limite inhérente, pas un défaut d'implémentation.

## Risques

| risque | traitement |
|---|---|
| Pull-ups I²C en double sur le module | à vérifier au banc avant d'alimenter |
| Marquage de la dalle | décalage périodique, fonction pure testée |
| La tâche écran fait dériver l'échantillonnage des boutons | tâches séparées ; à vérifier au banc, pas seulement en théorie |
| Un troisième contexte lit l'état de `sec_confirm` | raisonnement de concurrence à étendre ; `peek()` seulement, jamais `poll()` |
| Modification d'un module de sécurité audité | changement minimal (un énum, un paramètre), divergence déclarée |
