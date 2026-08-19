/* main/security/oath_name.h — nom de compte OATH pret a dessiner (pur).
 *
 * Le nom vient de l'HOTE : jusqu'a 64 octets arbitraires, dessines sur l'ecran
 * dont Mae se sert pour decider si elle autorise un code. Il ne peut donc pas
 * aller a l'ecran tel quel — ni pour sa longueur, ni pour son contenu.
 */
#pragma once
#include <stdint.h>

/*
 * Budget d'affichage : VINGT-ET-UN caracteres dessines, plus le terminateur.
 *
 * Le chiffre vient de la police REELLE de cette ligne, mesuree dans
 * hmi/screen.c : l'etiquette de compte est dessinee par draw_text_centered(),
 * donc en police SIMPLE hauteur, dont la cellule fait SCREEN_CHAR_PX = 6 px
 * (hmi/screen_layout.h). Sur les 128 px de la dalle : 128 / 6 = 21 caracteres,
 * soit 126 px. Un vingt-deuxieme ferait 132 px — screen_center_x() rend alors
 * 0 (voir sa garde), le texte part de la gauche et fb_set_pixel() coupe a
 * droite : le dernier caractere serait AMPUTE, et c'est precisement celui qui
 * porte le marqueur de troncature.
 *
 * CE QUI ETAIT FAUX : cette constante valait 12, justifiee par « la largeur
 * reelle en police double hauteur ». La police double hauteur
 * (draw_text_2x_centered) ne sert qu'au LIBELLE D'OPERATION — « CODE OTP »,
 * « RESET OATH » — jamais au nom de compte. La contrainte etait donc reelle,
 * mais appliquee a la mauvaise ligne : le budget etait divise par deux, et
 * c'est ce qui rendait la troncature si agressive.
 *
 * Le marqueur de troncature et l'empreinte se prennent DANS ces vingt-et-un,
 * pas au-dela : voir oath_name_display().
 */
#define OATH_NAME_DISPLAY_MAX 22

/* Taille minimale de `out_sz` en dessous de laquelle `oath_name_display` ne
 * garantit que la chaine vide : il faut au moins l'empreinte, le marqueur de
 * troncature et le terminateur pour qu'une troncature ait un sens sans
 * deborder. Voir la garde en tete de la fonction. */
#define OATH_NAME_OUT_SZ_MIN 3

/*
 * Ecrit dans `out` une version affichable de `raw` :
 *   1. le nom COMPLET, en majuscules, prive du seul prefixe de periode —
 *      « 30/GitHub:mae@x.org » -> « GITHUB:MAE@X.ORG ». Le « 30/ » est du
 *      protocole (convention YKOATH), pas du sens : il ne dit rien a qui
 *      regarde l'ecran. Tout le reste est garde.
 *
 *      CE QUI ETAIT FAUX : cette regle ne gardait que l'emetteur (« GITHUB »),
 *      au motif qu'« un seul compte par service rend le reste inutile ».
 *      L'hypothese est dementie par la proprietaire : OVH et Ankama auront
 *      chacun un compte perso ET un compte pro. « OVH:perso » et « OVH:pro »
 *      rendaient tous deux « OVH », donc l'ecran ne pouvait plus dire lequel
 *      etait vise — et l'appui redevenait un interrupteur de presence pour
 *      ces comptes-la, ce que la decision 4 de la spec existe pour empecher.
 *      Voir test_noms_distincts_restent_distincts, qui porte la paire.
 *   2. tout caractere non imprimable devient « ? », JAMAIS un blanc : un nom
 *      bricole doit se voir plutot que se deguiser en nom propre ;
 *   3. troncature a OATH_NAME_DISPLAY_MAX - 1 caracteres avec un marqueur
 *      visible, complete par une empreinte du nom entier en avant-dernier
 *      caractere visible plutot qu'un caractere litteral de plus — sans quoi
 *      deux comptes divergeant au-dela de la coupe, ou confondus par
 *      l'assainissement du point 2, seraient indiscernables.
 *
 * `out` est toujours une chaine terminee, meme si `raw` est NULL ou vide, ou
 * si `out_sz` est sous OATH_NAME_OUT_SZ_MIN (auquel cas `out` recoit la
 * chaine vide : pas assez de place pour autre chose sans deborder).
 *
 * CE QUE L'EMPREINTE N'EST PAS : une protection contre un hote malveillant.
 * L'algorithme est public et deterministe — un hote qui choisit le nom peut,
 * en au plus 36 essais, faire coincider prefixe assaini ET empreinte avec un
 * compte legitime qu'il vise. Elle protege de la confusion ACCIDENTELLE entre
 * deux noms proches, pas d'une usurpation deliberee.
 */
void oath_name_display(const char *raw, uint16_t raw_len, char *out, uint8_t out_sz);

/*
 * Taille du tampon a passer a oath_reset_label() : « 255 COMPTES » plus son
 * terminateur, soit la plus longue forme qu'un uint8_t puisse produire. Le
 * magasin s'arrete a SEC_N_SLOTS comptes, donc dix caracteres en pratique —
 * mais dimensionner sur le type plutot que sur la constante du jour evite
 * qu'un agrandissement du magasin ne fasse deborder ici en silence.
 */
#define OATH_RESET_LABEL_MAX 12

/*
 * Ecrit dans `out` l'etiquette d'un RESET OATH : combien de comptes l'appui
 * va detruire — « 12 COMPTES », « 1 COMPTE ».
 *
 * POURQUOI ICI, et pas formatee sur place dans le mode USB : cette chaine
 * traverse ensuite oath_name_display() comme n'importe quelle etiquette
 * venue de l'hote (sec_confirm_arm_named() l'y passe sans distinction
 * d'origine). Elle est donc soumise au retrait d'un prefixe numerique suivi
 * de '/', a l'assainissement des caracteres non imprimables, et a la
 * troncature. Une forme mal choisie ressortirait amputee ou vide, et l'ecran
 * annoncerait un effacement total SANS dire combien de comptes partent.
 * Cette contrainte se PROUVE — test_reset_label_traverse_l_affichage_intact,
 * qui fait le passage bout en bout — ce qui exige que le formatage soit de
 * la logique pure, donc ici plutot que dans usb/mode_oath.c.
 *
 * `out` est toujours une chaine terminee. Si `out_sz` ne suffit pas, `out`
 * recoit la chaine VIDE : jamais un nombre ampute, qui ferait autoriser
 * l'effacement de seize comptes en croyant en effacer un.
 */
void oath_reset_label(uint8_t count, char *out, uint8_t out_sz);
