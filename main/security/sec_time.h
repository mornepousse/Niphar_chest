#pragma once

/*
 * sec_time — l'heure murale du coffre, POSEE et jamais devinee.
 *
 * Logique pure : le temps monotone est passe en parametre, jamais lu depuis
 * esp_timer. C'est ce qui rend ce module testable sur l'hote, et c'est la meme
 * convention que sec_confirm.
 *
 * POURQUOI CE MODULE EXISTE. Un TOTP vaut HMAC(secret, floor(unix / 30)) :
 * sans heure, pas de code. Or le coffre n'a AUCUNE horloge — ni RTC, ni pile —
 * et la moitie gauche du clavier n'en a pas davantage (oscillateur RC interne,
 * derive en minutes par jour). L'heure vient donc de l'hote, par
 * `niphar-oath set-time` sur le canal CCID que le client parle deja, et ce
 * module l'avance sur le temps monotone.
 *
 * CE QUI REND L'INVALIDATION GRATUITE : le coffre n'existe que branche et
 * redemarre au debranchement. Son heure s'efface donc toute seule. Il n'y a
 * aucun drapeau a tenir, aucune heure perimee possible — contrairement a une
 * conception ou le clavier l'aurait detenue, puisque lui survit au
 * debranchement sur batterie.
 *
 * L'heure survit en revanche a une bascule de mode USB : le coffre ne redemarre
 * pas, donc ce module vit HORS de l'applet OATH, a cote de sec_confirm et non
 * dedans.
 *
 * LA LIMITE, A DIRE PLUTOT QU'A TAIRE : un hote qui ment sur l'heure fait
 * calculer les codes d'une autre fenetre. Il n'en apprend rien — ils ne
 * s'affichent que sur l'ecran du clavier et ne repassent jamais par l'hote — et
 * au pire un code faux s'affiche, que le service refuse. C'est inherent a la
 * classe d'appareil : un hote menteur ne se distingue pas d'une semaine
 * debranchee.
 */

#include <stdbool.h>
#include <stdint.h>

/* Pas de la fenetre TOTP, en secondes (RFC 6238 : 30 par defaut). */
#define SEC_TIME_TOTP_STEP  30u

/*
 * Plancher de plausibilite : 2024-01-01T00:00:00Z.
 *
 * Il n'est pas la pour valider une date, mais pour attraper une heure qui
 * n'en est pas une — un zero, une variable non initialisee, une epoque de
 * compilation. Une heure devinee produirait des codes faux PRESENTES COMME
 * JUSTES, ce qui est pire que pas de code : avec « NO TIME » a l'ecran, la
 * proprietaire sait quoi faire.
 */
#define SEC_TIME_MIN_PLAUSIBLE  1704067200u

/*
 * Oublie l'heure.
 *
 * PAS appele au demarrage, et ce n'est pas un oubli : les statiques de ce
 * module sont a zero par construction, donc le coffre demarre deja sans heure.
 * Un appel explicite au boot donnerait l'illusion que l'invalidation depend de
 * lui — alors qu'elle depend du REDEMARRAGE, qui est ce qui rend l'heure du
 * coffre auto-invalidante (voir l'en-tete de ce fichier).
 *
 * Existe pour les tests, qui ont besoin de repartir d'un etat connu.
 */
void sec_time_reset(void);

/*
 * Pose l'heure murale. `now_ms` est le temps monotone au moment de la pose.
 *
 * Rend false — et ne retient RIEN — si `unix_s` est sous le plancher de
 * plausibilite. Une seconde pose remplace la premiere : l'hote peut corriger sa
 * propre derive sans qu'on redemarre le coffre.
 */
bool sec_time_set(uint64_t unix_s, uint32_t now_ms);

/* Vrai si une heure a ete posee depuis le dernier reset. */
bool sec_time_is_valid(void);

/*
 * Rend l'heure murale courante. Rend false — et ne touche PAS `out_unix` — si
 * aucune heure n'a ete posee.
 *
 * L'ecart se calcule en arithmetique non signee sur 32 bits, donc le
 * rebouclage du compteur de millisecondes (~49,7 jours) est traverse
 * correctement. Le coffre ne devrait jamais y arriver — il n'existe que
 * branche — mais « ne devrait jamais » n'est pas une garantie, et une
 * soustraction naive rendrait une heure de quarante-neuf jours dans le passe
 * sans que rien ne le dise.
 */
bool sec_time_now(uint32_t now_ms, uint64_t *out_unix);

/* Numero de fenetre TOTP : le defi que le HMAC consomme. */
uint64_t sec_time_totp_counter(uint64_t unix_s);

/*
 * Secondes restantes dans la fenetre courante, dans 1..SEC_TIME_TOTP_STEP.
 *
 * JAMAIS ZERO : un decompte a zero afficherait un code deja perime comme s'il
 * restait a l'utiliser. A la derniere seconde il vaut 1, puis la fenetre
 * suivante commence a 30.
 */
uint8_t sec_time_window_remaining(uint64_t unix_s);
