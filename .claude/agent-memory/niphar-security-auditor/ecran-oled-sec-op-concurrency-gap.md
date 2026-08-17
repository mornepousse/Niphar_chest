---
name: ecran-oled-sec-op-concurrency-gap
description: sec_confirm.c — le gap peek()+armed_op() séparés a été fermé par sec_confirm_peek_labeled() (commit d2f0ad0, tâche 1 addendum) ; résiduel narrow documenté mais le commentaire surclaims un mécanisme de préemption. Relire avant l'audit de la tâche 2 (hmi_task/screen.c, consommateur réel pas encore écrit).
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

**Recommandation actionable avant que la tâche 2 (hmi_task/screen.c) ne
consomme `peek_labeled()`** : reformuler le bullet `sec_confirm_peek_labeled()`
du CONCURRENCY MODEL pour (1) justifier la fenêtre par le nombre de cycles
entre les deux loads, pas par l'absence de point de cession, et (2) ne pas
qualifier le résiduel de « bénin, même classe » sans nuancer la différence
fail-safe vs. mal-attribution-possible-mais-improbable.

**Gap de couverture de test mineur, non bloquant** : `out_op == NULL`
(chemin documenté supporté, `sec_confirm.h:56`) n'est exercé par aucun test.
Si le garde `if (out_op)` disparaissait, rien ne rougirait — seul un futur
appelant NULL crasherait en prod. Aucun appelant actuel ne passe NULL.

**Point vérifié et clos** : `.superpowers/sdd/2026-08-17-ecran-oled-carte-cle/
tache-4-brief.md` référence déjà `sec_confirm_peek_labeled()` (pas l'ancienne
`armed_op()`) — la rupture d'interface signalée dans le rapport de tâche 1 est
résolue, plus de blocage pour l'implémenteur de la tâche 4.

Voir aussi [[audit-baseline-2026-08-07]] pour la baseline générale du projet.
