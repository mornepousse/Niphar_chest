---
name: ecran-oled-sec-op-concurrency-gap
description: sec_confirm.c — gap peek()+armed_op() fermé par sec_confirm_peek_labeled() (d2f0ad0), sur-affirmation du CONCURRENCY MODEL corrigée (b7f24da), et consommateur réel (screen.c, tâche 4, c0ef7ea) audité — POINT DÉFINITIVEMENT CLOS, aucune ré-ouverture attendue sauf ajout d'un futur appelant direct de sec_confirm.
metadata:
  type: project
---

Audité initialement le 2026-08-17 (commits 66cea42, ea8b125) : `sec_confirm_armed_op()`
et `sec_confirm_peek()` étaient deux accesseurs séparés, non synchronisés — un
`reset()+arm()` intercalé pouvait faire lire à l'appelant l'état d'une opération
avec le libellé d'une autre. Signalé Important.

**Re-revue le 2026-08-17 (commit d2f0ad0)** : correctif appliqué et vérifié.
`sec_confirm_armed_op()` supprimée (aucun appelant compilé restant — grep
confirmé sur `main/` et `test/`), remplacée par l'unique accesseur
`sec_confirm_state_t sec_confirm_peek_labeled(uint32_t now_ms, sec_op_t *out_op)`
(`main/security/sec_confirm.c:195-207`) qui lit `s_op` puis `s_state`/`s_armed_ms`
dans le MÊME appel. `sec_confirm_poll()` remet aussi `s_op = SEC_OP_UNKNOWN` sur
ses deux branches consommantes (AUTHORIZED, TIMEDOUT), symétrique avec `reset()`.

**Ce qui reste vrai malgré le fix** : `s_op` et `s_state` restent deux `load`
non-atomiques distincts À L'INTÉRIEUR de `peek_labeled()` — la fenêtre de course
est réduite (de « un cycle de scheduler arbitraire entre deux appels API » à
« quelques cycles CPU entre deux instructions adjacentes ») mais PAS fermée.
Le commentaire CONCURRENCY MODEL (`sec_confirm.c:93-96`) justifie ce résiduel
par « pas de point d'appel ou de cession entre les deux lectures » — argument
qui suppose un ordonnancement coopératif. Sous FreeRTOS préemptif par tick
(`configUSE_PREEMPTION=1`, défaut ESP-IDF), une préemption PEUT survenir entre
deux instructions quelconques, pas seulement à un point de cession explicite.
Le mécanisme invoqué n'est donc pas exact — ce qui borne réellement le risque,
c'est le petit nombre de cycles, pas l'absence de "computation".

Le commentaire range aussi ce résiduel dans « la MÊME classe bénigne que
`s_armed_ms` » — imprécis : le cas `s_armed_ms` est fail-safe par construction
(un TIMEDOUT halluciné ne fait que refuser plus tôt), alors qu'un résiduel de
tear sur `s_op` peut en principe reproduire — à probabilité désormais infime —
exactement la mal-attribution humaine (confirmer A en accordant B) que toute
la fonctionnalité écran existe pour empêcher. Le progrès pratique est réel et
suffisant pour ne pas bloquer ; l'imprécision est dans la JUSTIFICATION écrite,
pas dans le code.

**Re-revue le 2026-08-17 (commit b7f24da) — POINT CLOS.** Les deux
sur-affirmations relevées ci-dessus sont corrigées dans le même bullet
(`main/security/sec_confirm.c:81-134`) :
1. Le texte n'invoque plus un point de cession coopératif. Il pose
   explicitement que FreeRTOS ordonnance en préemptif
   (`configUSE_PREEMPTION=1`) et qu'un tick peut préempter entre deux
   instructions quelconques ; ce qui borne le résiduel, c'est le nombre de
   cycles CPU dans la fenêtre (quelques cycles vs. potentiellement des
   millisecondes entre deux appels API séparés), pas l'absence de
   "computation". Vrai et vérifié contre le code : `sec_confirm_peek_labeled()`
   (`sec_confirm.c:223-235`) lit `s_op` puis calcule le timeout sur `s_state`/
   `s_armed_ms` en une poignée d'instructions adjacentes, sans yield ni appel
   entre les deux — l'argument "quelques cycles" correspond au code réel.
2. Le résiduel n'est plus rangé dans la « même classe bénigne » que
   `s_armed_ms` sans nuance : le texte distingue maintenant explicitement
   fail-safe-par-construction (`s_armed_ms`, ne peut que refuser plus tôt)
   vs. mal-attribution-possible-à-probabilité-infime (`s_op`, peut en
   principe afficher le libellé de A pendant que l'état montré appartient à
   B) — tout en maintenant, correctement, qu'aucun GRANT erroné n'en résulte
   puisque `authorize()`/`poll()` ne dépendent que de `s_state`, jamais de
   `s_op`. Lu en entier comme un futur auditeur : l'image transmise est
   juste — ni rassurante à tort (le résiduel et sa catégorie de risque sont
   nommés), ni alarmante à tort (la borne "GRANT toujours correct" est
   maintenue et justifiée). Décision de ne pas ajouter de section critique
   (portMUX casserait la pureté testée sur l'hôte) explicitement assumée et
   correctement motivée dans le texte, pas laissée implicite.

**Gap de test comblé** : `test/test_sec_confirm.c` ajoute
`test_peek_labeled_tolerates_null_out_op()` qui appelle
`sec_confirm_peek_labeled(1000, NULL)` avec une opération armée. Mutation
(retrait de `if (out_op)` à `sec_confirm.c:229`) prouvée mordante par
l'implémenteur : SIGSEGV (exit 139), au point d'exécution du nouveau test —
pas une découverte a posteriori, une preuve délibérée. Voir la note
méthodologique ci-dessous sur la valeur de cette forme de preuve.

**Note méthodologique — crash comme preuve de morsure** : le harnais hôte
(`test/test_main.c`) est un seul process, un seul `main()`, qui enchaîne
toutes les suites séquentiellement (`test_sec_confirm` est la 3e sur 15) sans
fork ni isolation par test. Un SIGSEGV pendant la vérification transitoire du
mordant (norme TDD du CLAUDE.md : bug transitoire → rouge → revert) est une
preuve valable et sans ambiguïté pour CET usage ponctuel — le process sort en
échec, personne ne peut lire ça comme un vert accidentel. Mais ce n'est pas
une preuve de la même qualité qu'un `TEST_ASSERT` propre : si la même
régression était réintroduite par accident plus tard (pas par un
implémenteur qui sait qu'il mute), le crash tue le process AVANT les 12
suites suivantes et avant la ligne de résumé (`test_main.c:43-45`) — `check.sh`
verrait bien un échec (exit non nul), mais sans le compte `N assertions OK,
M échecs` habituel, donc un diagnostic plus pauvre sur ce run précis. Rien à
corriger ici : le garde `if (out_op)` est resté en place dans le code
committé, donc en usage normal ce test ne crashe jamais. Le point à retenir
pour de futurs tests de ce genre : un déréférencement NULL est une preuve de
morsure acceptable en vérification transitoire ponctuelle, mais n'aspire pas
à remplacer une assertion lisible comme filet de régression permanent — ici
ce n'était pas un choix, c'est la nature même du bug (NULL deref) qui impose
le crash comme signal.

**Point vérifié et clos** : `.superpowers/sdd/2026-08-17-ecran-oled-carte-cle/
tache-4-brief.md` référence déjà `sec_confirm_peek_labeled()` (pas l'ancienne
`armed_op()`) — la rupture d'interface signalée dans le rapport de tâche 1 est
résolue, plus de blocage pour l'implémenteur de la tâche 4.

**Tâche 4 auditée le 2026-08-17 (commit c0ef7ea, `main/hmi/screen.c` + `hmi.c`
publie `hmi_snapshot_t`) — le consommateur réel existe, et il est PLUS sûr que
ce que la spec de conception prévoyait.** La spec
(`docs/superpowers/specs/2026-08-17-ecran-oled-carte-cle-design.md:129-133`)
anticipait que la tâche écran appellerait `sec_confirm_peek()` directement,
« comme la tâche IHM » — ce qui aurait ajouté un vrai 4e contexte à
`sec_confirm.c`. L'implémentation a dévié en mieux : `screen.c`/`screen_task`
n'appelle JAMAIS aucune fonction de `sec_confirm` (grep confirmé sur
`main/hmi/screen.c` — zéro occurrence de `sec_confirm_`). Il lit uniquement
`hmi_snapshot()`, qui recopie sous verrou (`s_snap_lock`,
`xSemaphoreCreateMutexStatic`, modèle `sd_card.c`) un instantané que
`hmi_task` a déjà rempli à partir d'un SEUL appel à
`sec_confirm_peek_labeled()` par tick. Donc :
- `sec_confirm_poll()` n'est appelé nulle part dans le chemin écran (ni
  ailleurs de nouveau — seuls `ccid_worker`/`otp_hid.c` le font, inchangé).
- Le nombre de contextes qui touchent l'API de `sec_confirm.c` n'a PAS
  augmenté avec cette tâche : `hmi_task` reste le seul « troisième contexte »
  du raisonnement CONCURRENCY MODEL. Ce raisonnement décrit donc toujours la
  réalité sans modification nécessaire — il n'y avait rien à étendre.
- La copie sous verrou de `hmi_snapshot_t` (6 champs scalaires, struct
  assignment complète sous mutex des deux côtés) est atomique vis-à-vis du
  lecteur : pas de lecture déchirée possible entre `mode`/`op`/`armed_at_ms`/
  `event_at_ms`. Le verrou n'est jamais tenu pendant un transfert I2C (I2C vit
  entièrement dans `screen_task`, après relâchement du verrou pris seulement
  dans `hmi_snapshot()`).
- Séparation de tâches vérifiée efficace : `screen_task` priorité 4,
  `hmi_task` priorité 5 (FreeRTOS : priorité numérique plus haute = priorité
  plus haute), donc `hmi_task` préempte toujours `screen_task` — l'échantillon
  bouton à 5 ms n'est jamais retardé par les 25-30 ms de flush I2C. Vérifié
  aussi côté build coffre : `nm` sur l'objet `screen.c` du firmware
  `niphar_chest` donne 0 symbole i2c et 4 octets de `.text` — la branche
  `#else` (no-op) est bien celle qui compile sur la carte sans écran,
  reconfirmé indépendamment de l'implémenteur.

**Constat qualité mineur, sans risque de sécurité** : `screen.c:652-655`
calcule lui-même l'interpolation pour mille→pixel du glissement de bascule
(`title_x += (1000-pm)*travel/1000`) — arithmétique correcte (vérifiée à la
main : pm=0 → hors-champ à droite, pm=1000 → position finale x=2) mais non
extraite dans `screen_anim.h`, donc non testée sur l'hôte. C'est la même
CATÉGORIE de risque que « ce qui n'est pas extrait n'est pas testé, et ce qui
n'est pas testé dérive » citée dans la spec de conception comme leçon de la
branche précédente (`hmi.c` avait une phase absolue d'alternance non testée
qui produisait un vrai défaut d'affichage) — mais ici la fonction est sans
état (recalculée à chaque frame depuis `pm`, pas de phase accumulée dans le
temps), donc structurellement moins exposée au type de dérive qui avait mordu
avant. Pas bloquant ; à surveiller si `screen.c` accumule d'autres calculs de
ce genre.

**Anomalie matérielle notée, non tranchée** : bascule spontanée `usb_mode`
PGP↔OTP sans appui, observée sur la carte `wt9932_key` après ce flash. Le
chemin bouton n'est touché par aucun diff de cette tâche (grep confirmé), donc
cause logicielle exclue côté ce diff. Cette tâche AJOUTE cependant un bus I2C
qui bascule à 400 kHz pendant 25-30 ms toutes les 50 ms sur IO53/54, à
proximité (au sens numérotation GPIO, pas nécessairement physique) des
boutons IO32/33 — et remplace les pull-ups internes par des externes 4,7 kΩ,
ce qui change les temps de montée/descente du bus. Sur un prototype câblé à
la main sans condensateur anti-rebond, c'est un facteur plausible de
couplage EMI qui n'existait pas avant cette tâche. Hypothèse non tranchable
depuis le firmware seul — recommandé : isoler en testant bouton MODE avec
l'écran physiquement débranché.

Voir aussi [[audit-baseline-2026-08-07]] pour la baseline générale du projet.

---

**Addendum 2026-08-17 (commit `b5284bb`) — la condition de réouverture énoncée
ci-dessus a été déclenchée, et vérifiée.**

Ce fichier disait : « aucune ré-ouverture attendue sauf ajout d'un futur
appelant direct de `sec_confirm` ». La correction du Ruling 28 fait exactement
cela — `sec_confirm_authorize()` prend désormais un horodatage d'appui et **lit
`s_armed_ms`**, champ qu'il ne touchait pas.

Analyse, pour ne pas laisser la question ouverte :

- `s_armed_ms` est un mot aligné : lecture atomique sur les deux cœurs HP du P4,
  donc jamais déchirée. Le raisonnement du CONCURRENCY MODEL sur l'atomicité
  s'applique tel quel.
- Le pire cas est une valeur **périmée**, pas déchirée : `authorize()` pourrait
  lire l'ancien `s_armed_ms` pendant qu'`arm()` écrit ses quatre champs. C'est la
  même fenêtre que `peek()` tolère déjà.
- **Et ce pire cas est fail-safe par construction** : un `s_armed_ms` périmé rend
  la différence non signée plus grande, donc fait tomber l'appui hors fenêtre,
  donc **refuse**. Toute incertitude sur l'horodatage refuse ; aucune ne peut
  accorder. C'est la bonne direction d'erreur pour une porte de présence.
- `authorize()` reste appelé depuis la tâche IHM et le REPL, comme avant. Aucun
  contexte nouveau, donc aucune nouvelle course.

Le CONCURRENCY MODEL de `main/security/sec_confirm.c` porte ce paragraphe.

**Nouveau point de vigilance pour le futur FIDO** : la spec retenue
(`docs/superpowers/specs/2026-08-17-fido2-webauthn-design.md`) est **sans PIN**.
Cette porte y sera donc la seule défense, et `ctap2.c` deviendra un appelant
direct de plus. À réauditer à ce moment-là, sans attendre.
