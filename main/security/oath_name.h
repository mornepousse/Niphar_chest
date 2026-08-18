/* main/security/oath_name.h — nom de compte OATH pret a dessiner (pur).
 *
 * Le nom vient de l'HOTE : jusqu'a 64 octets arbitraires, dessines sur l'ecran
 * dont Mae se sert pour decider si elle autorise un code. Il ne peut donc pas
 * aller a l'ecran tel quel — ni pour sa longueur, ni pour son contenu.
 */
#pragma once
#include <stdint.h>

/* Dix caracteres visibles (la largeur reelle en police double hauteur sur
 * 128 px), un marqueur de troncature, un terminateur. */
#define OATH_NAME_DISPLAY_MAX 12

/*
 * Ecrit dans `out` une version affichable de `raw` :
 *   1. l'issuer seul — « 30/GitHub:mae@x.org » -> « GITHUB » — parce que c'est
 *      ce que Mae reconnait, et parce qu'un seul compte par service rend le
 *      reste inutile ;
 *   2. tout caractere non imprimable devient « ? », JAMAIS un blanc : un nom
 *      bricole doit se voir plutot que se deguiser en nom propre ;
 *   3. troncature a dix caracteres avec un marqueur visible, sans quoi deux
 *      comptes divergeant au-dela du dixieme caractere seraient indiscernables.
 *
 * `out` est toujours une chaine terminee, meme si `raw` est NULL ou vide.
 */
void oath_name_display(const char *raw, uint16_t raw_len, char *out, uint8_t out_sz);
