---
name: ctaphid-audit-2026-08-17
description: Audit de main/security/ctaphid.c (commit e407061, tâche 2 FIDO2) — bornage vérifié ligne à ligne, mutant équivalent confirmé par ASan, gap sur out non-zéroé si a/pkt sont NULL
metadata:
  type: project
---

Premier module qui touche des octets fournis par l'hôte pour la pile FIDO2
(`main/security/ctaphid.c`, réassemblage de trames CTAP-HID 64 octets). Audit
du commit `e407061` sur `fido2-u2f`, en réponse à un brief de relecture
combinée conformité + sécurité.

**Verdict** : les trois bornages du cahier des charges sont bien en place et
dans le bon ordre (`ctaphid.c:69` longueur avant écriture, `ctaphid.c:97`
continuation sans init refusée, `ctaphid.c:114-116` `got+n<=expected` via
`min(room, remaining)` — pas de check naïf `got+n<=expected` donc pas de
risque de dépassement d'entier sur l'addition). Build ASan+UBSan clean sur le
module isolé.

**Gap trouvé (Important)** : `ctaphid_feed` fait `memset(out, 0, sizeof(*out))`
juste après le garde `if (!a || !pkt || !out) return CTAPHID_ERROR;`
(`ctaphid.c:45` puis `:51`). Si `a` ou `pkt` est NULL mais `out` valide, la
fonction retourne CTAPHID_ERROR **sans jamais toucher `out`** — contredit
l'invariant documenté juste en dessous ("out part a zero avant toute autre
chose"). Risque limité (a/pkt viennent d'appelants internes, pas de l'hôte
directement) mais c'est exactement la classe de bug qui a déjà fait planter
le binaire de test dans ce même commit (pointeur `m.data` non initialisé,
corrigé par l'implémenteur). À vérifier si corrigé à la tâche 3 (le
dispatcher CTAP-HID, futur appelant réel).

**Mutant équivalent vérifié empiriquement, pas juste lu** : l'implémenteur
affirmait que retirer le bornage `got+n<=expected` sur les paquets CONT est
un mutant équivalent (buf dimensionné exactement au pire cas 57+128*59, et
`next_seq` sur 7 bits interdit plus de 128 CONT). J'ai reproduit la mutation
dans une copie scratch, rebuild avec `-fsanitize=address,undefined -O0`, et
poussé un harnais dédié au pire cas EXACT (INIT BCNT=7609 + 128 CONT pleins)
— zéro overflow, zéro UB. La preuve mathématique de l'implémenteur est
correcte ET confirmée par instrumentation, pas seulement par les tests
existants qui ne l'auraient pas forcément détecté. Le bornage reste en place
dans le code comme défense en profondeur légitime (si `ctaphid_asm_t` change
un jour de dimensionnement) — recommandation : le garder.

**Pattern récurrent du projet, encore une fois confirmé** : le brief
(`tache-2-brief.md`) contenait lui-même un bug de test (`other[0]=2` au lieu
de `other[3]=2` pour un CID gros-boutiste) — pas un bug de l'implémenteur.
Le CID se lit `pkt[0]<<24 | pkt[1]<<16 | pkt[2]<<8 | pkt[3]` (gros-boutiste,
octet de poids fort en premier), prouvé par `test_single_packet_message`.
Un cahier des charges peut lui-même contenir le "test aveugle" que la
consigne du projet demande de traquer — ne pas supposer que le brief est
la référence infaillible, le vérifier contre le reste du code testé.

**Méthode utile pour un futur audit de framing binaire** : quand un rapport
affirme un mutant équivalent basé sur un raisonnement de dimensionnement
(buffer exactement au pire cas), le vérifier avec un harnais ASan/UBSan
poussé au pire cas exact plutôt que de se fier aux seuls tests unitaires
existants — les tests peuvent ne jamais exercer le pire cas alors qu'ASan
le détecterait immédiatement s'il y avait un vrai dépassement.

Voir aussi [[audit-baseline-2026-08-07]] pour la baseline MSC/USB, et
[[ecran-oled-sec-op-concurrency-gap]] pour le gap de concurrence encore
ouvert côté HMI (sans rapport avec ce module).
