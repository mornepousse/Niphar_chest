#include "test_framework.h"
#include "ctaphid.h"
#include <string.h>

/* ---- les cinq tests du cahier des charges, repris verbatim ---- */

/* Un message court tient dans un seul paquet d'initialisation. */
static void test_single_packet_message(void)
{
    ctaphid_asm_t a; ctaphid_reset(&a);
    uint8_t p[64] = {0};
    p[0]=0x11; p[1]=0x22; p[2]=0x33; p[3]=0x44;   /* CID */
    p[4]=0x80u | CTAPHID_CMD_PING;
    p[5]=0x00; p[6]=0x03;                          /* BCNT = 3 */
    p[7]='a'; p[8]='b'; p[9]='c';
    ctaphid_msg_t m;
    TEST_ASSERT_EQ(ctaphid_feed(&a,p,&m), CTAPHID_COMPLETE, "un paquet suffit");
    TEST_ASSERT_EQ(m.cid, 0x11223344u, "le canal est conserve");
    TEST_ASSERT_EQ(m.cmd, CTAPHID_CMD_PING, "la commande est conservee");
    TEST_ASSERT_EQ(m.len, 3, "la longueur annoncee est respectee");
    TEST_ASSERT(memcmp(m.data, "abc", 3) == 0, "la charge utile est intacte");
}

/* Une longueur annoncee superieure au maximum doit etre refusee AVANT toute
 * ecriture dans le tampon : c'est le debordement classique contre un
 * authentificateur. */
static void test_oversized_length_is_refused(void)
{
    ctaphid_asm_t a; ctaphid_reset(&a);
    uint8_t p[64] = {0};
    p[0]=1; p[4]=0x80u|CTAPHID_CMD_PING;
    p[5]=0xFF; p[6]=0xFF;                          /* 65535 > 7609 */
    ctaphid_msg_t m;
    TEST_ASSERT_EQ(ctaphid_feed(&a,p,&m), CTAPHID_ERROR, "longueur aberrante refusee");
    TEST_ASSERT_EQ(m.err_code, CTAPHID_ERR_INVALID_LEN, "et signalee comme telle");
}

/* Un numero de sequence hors ordre invalide la transaction. Sans ce controle,
 * un hote pourrait reordonner la charge utile d'un message signe. */
static void test_out_of_order_sequence_is_refused(void)
{
    ctaphid_asm_t a; ctaphid_reset(&a);
    uint8_t init[64] = {0};
    init[0]=1; init[4]=0x80u|CTAPHID_CMD_PING; init[5]=0x01; init[6]=0x00; /* 256 */
    ctaphid_msg_t m;
    TEST_ASSERT_EQ(ctaphid_feed(&a,init,&m), CTAPHID_IN_PROGRESS, "message entame");
    uint8_t cont[64] = {0};
    cont[0]=1; cont[4]=0x01;                        /* SEQ 1 au lieu de 0 */
    TEST_ASSERT_EQ(ctaphid_feed(&a,cont,&m), CTAPHID_ERROR, "sequence hors ordre refusee");
    TEST_ASSERT_EQ(m.err_code, CTAPHID_ERR_INVALID_SEQ, "signalee INVALID_SEQ");
}

/* Un canal different pendant une transaction en cours est occupe, pas ignore :
 * sinon deux hotes se marcheraient dessus en silence. */
static void test_other_channel_during_transaction_is_busy(void)
{
    ctaphid_asm_t a; ctaphid_reset(&a);
    uint8_t init[64] = {0};
    init[0]=1; init[4]=0x80u|CTAPHID_CMD_PING; init[5]=0x01; init[6]=0x00;
    ctaphid_msg_t m;
    (void)ctaphid_feed(&a,init,&m);
    uint8_t other[64] = {0};
    /* CID au dernier octet (poids faible) : test_single_packet_message,
     * repris verbatim ci-dessus, prouve sans ambiguite que le CID se lit en
     * gros-boutiste (m.cid==0x11223344 pour p[0..3]=0x11,0x22,0x33,0x44).
     * Le cahier des charges plaçait le 2 en other[0] ; a cet octet, il pese
     * 2<<24 et ne peut jamais valoir "2". Corrige ici pour que l'assertion
     * "err_cid == 2u" ci-dessous porte reellement sur le canal fautif. */
    other[3]=2; other[4]=0x80u|CTAPHID_CMD_PING; other[6]=0x01;
    TEST_ASSERT_EQ(ctaphid_feed(&a,other,&m), CTAPHID_ERROR, "autre canal refuse");
    TEST_ASSERT_EQ(m.err_code, CTAPHID_ERR_CHANNEL_BUSY, "signale CHANNEL_BUSY");
    TEST_ASSERT_EQ(m.err_cid, 2u, "l'erreur part sur LE canal fautif, pas sur le notre");
}

/* Le canal 0 est interdit par la specification, le canal de diffusion ne
 * s'alloue jamais. Trois valeurs comparees, pas une a elle-meme. */
static void test_cid_allocation_skips_reserved(void)
{
    TEST_ASSERT(ctaphid_next_cid(0) != 0, "0 n'est jamais alloue");
    TEST_ASSERT(ctaphid_next_cid(0) != CTAPHID_CID_BROADCAST, "diffusion jamais allouee");
    TEST_ASSERT(ctaphid_next_cid(CTAPHID_CID_BROADCAST - 1u) != CTAPHID_CID_BROADCAST,
                "on saute la diffusion en arrivant dessus");
    TEST_ASSERT(ctaphid_next_cid(5) != ctaphid_next_cid(6),
                "deux canaux successifs different");
}

/* test_cid_allocation_skips_reserved ci-dessus n'atteint le canal de
 * diffusion QUE par le dessous (CTAPHID_CID_BROADCAST - 1u -> BROADCAST) :
 * il n'exerce jamais le cas ou `last_cid` EST DEJA la diffusion elle-meme,
 * le seul qui fait deborder `next` a 0 (CTAPHID_CID_BROADCAST + 1 == 0 en
 * arithmetique 32 bits). Sans ce test, un debordement non rattrape
 * allouerait le canal interdit 0. */
static void test_cid_allocation_from_broadcast_never_wraps_to_zero(void)
{
    TEST_ASSERT(ctaphid_next_cid(CTAPHID_CID_BROADCAST) != 0,
                "partir de la diffusion elle-meme ne deborde jamais vers 0");
}

/* ---- couverture complementaire : les quatre points ou l'ordre porte la
 * securite, au-dela de ce que les cinq tests ci-dessus exercent. ---- */

/* La borne CTAPHID_MAX_PAYLOAD doit etre INCLUSIVE : une longueur qui vaut
 * exactement le maximum est acceptee, une longueur d'un seul octet de plus
 * est refusee. Aucun des cinq tests precedents n'approche cette limite
 * (celui d'oversized utilise 65535, tres loin au-dessus) : sans ce test, un
 * `>` mute en `>=` sur le controle de taille resterait invisible. Deux
 * verdicts distincts, compares l'un a l'autre. */
static void test_max_payload_boundary(void)
{
    ctaphid_asm_t a; ctaphid_reset(&a);
    uint8_t p_max[64] = {0};
    p_max[0]=3; p_max[4]=0x80u|CTAPHID_CMD_PING;
    p_max[5] = (uint8_t)(CTAPHID_MAX_PAYLOAD >> 8);
    p_max[6] = (uint8_t)(CTAPHID_MAX_PAYLOAD & 0xFFu);
    ctaphid_msg_t m;
    ctaphid_result_t r_max = ctaphid_feed(&a, p_max, &m);
    TEST_ASSERT(r_max != CTAPHID_ERROR, "longueur exactement au maximum acceptee");

    ctaphid_asm_t b; ctaphid_reset(&b);
    uint8_t p_over[64] = {0};
    p_over[0]=3; p_over[4]=0x80u|CTAPHID_CMD_PING;
    uint16_t over = (uint16_t)(CTAPHID_MAX_PAYLOAD + 1u);
    p_over[5] = (uint8_t)(over >> 8);
    p_over[6] = (uint8_t)(over & 0xFFu);
    ctaphid_result_t r_over = ctaphid_feed(&b, p_over, &m);
    TEST_ASSERT_EQ(r_over, CTAPHID_ERROR, "un seul octet de plus que le maximum est refuse");
    TEST_ASSERT_EQ(m.err_code, CTAPHID_ERR_INVALID_LEN, "signalee INVALID_LEN");
    TEST_ASSERT(r_max != r_over, "les deux verdicts different (comparaison deux a deux)");
}

/* L'autre borne : BCNT=0 sur un paquet INIT. Rien ne l'exercait jusqu'ici.
 * Le comportement attendu est un COMPLETE immediat avec len==0 (aucune
 * continuation a attendre pour un message vide) — compare a un cas voisin
 * DISTINCT (BCNT=1, len==1) pour que ce test ne puisse pas passer par
 * accident si l'implementation ignorait BCNT et renvoyait toujours 0. */
static void test_zero_length_message_completes_immediately(void)
{
    ctaphid_asm_t a; ctaphid_reset(&a);
    uint8_t p_zero[64] = {0};
    p_zero[0]=4; p_zero[4]=0x80u|CTAPHID_CMD_PING;
    p_zero[5]=0x00; p_zero[6]=0x00;                 /* BCNT = 0 */
    ctaphid_msg_t m;
    ctaphid_result_t r_zero = ctaphid_feed(&a, p_zero, &m);
    TEST_ASSERT_EQ(r_zero, CTAPHID_COMPLETE, "BCNT=0 termine immediatement, rien a attendre");
    TEST_ASSERT_EQ(m.len, 0, "longueur annoncee (0) respectee");

    ctaphid_asm_t b; ctaphid_reset(&b);
    uint8_t p_one[64] = {0};
    p_one[0]=4; p_one[4]=0x80u|CTAPHID_CMD_PING;
    p_one[5]=0x00; p_one[6]=0x01;                   /* BCNT = 1 */
    p_one[7]='z';
    ctaphid_result_t r_one = ctaphid_feed(&b, p_one, &m);
    TEST_ASSERT_EQ(r_one, CTAPHID_COMPLETE, "BCNT=1 termine aussi immediatement");
    TEST_ASSERT_EQ(m.len, 1, "longueur annoncee (1) respectee, differente du cas BCNT=0");
    TEST_ASSERT(r_zero == r_one, "meme verdict (COMPLETE) sur les deux tailles limites");
}

/* Reassemblage sur trois paquets (INIT + deux CONT), avec un dernier
 * paquet qui n'a besoin que d'une partie de sa charge utile : c'est
 * precisement le paquet ou le bornage got+n<=expected doit agir, sinon des
 * octets au-dela de la longueur annoncee entreraient dans le tampon. Motif
 * deterministe base sur l'index (pas une valeur remarquable de
 * l'implementation : ni 57, ni 59, ni un multiple rond). */
static void test_multi_packet_reassembly(void)
{
    ctaphid_asm_t a; ctaphid_reset(&a);
    uint8_t payload[150];
    for (int i = 0; i < 150; i++) payload[i] = (uint8_t)(i * 7 + 11);

    uint8_t init[64] = {0};
    init[0]=0xAA; init[1]=0xBB; init[2]=0xCC; init[3]=0xDD;
    init[4] = 0x80u | CTAPHID_CMD_MSG;
    init[5] = 0x00; init[6] = 150;                 /* BCNT = 150 */
    memcpy(init + 7, payload, 57);
    ctaphid_msg_t m;
    TEST_ASSERT_EQ(ctaphid_feed(&a, init, &m), CTAPHID_IN_PROGRESS,
                "57 octets sur 150 : encore incomplet");

    uint8_t cont0[64] = {0};
    cont0[0]=0xAA; cont0[1]=0xBB; cont0[2]=0xCC; cont0[3]=0xDD;
    cont0[4] = 0x00;                                /* SEQ 0 */
    memcpy(cont0 + 5, payload + 57, 59);
    TEST_ASSERT_EQ(ctaphid_feed(&a, cont0, &m), CTAPHID_IN_PROGRESS,
                "116 octets sur 150 : encore incomplet");

    uint8_t cont1[64] = {0};
    cont1[0]=0xAA; cont1[1]=0xBB; cont1[2]=0xCC; cont1[3]=0xDD;
    cont1[4] = 0x01;                                /* SEQ 1 */
    memcpy(cont1 + 5, payload + 116, 150 - 116);    /* 34 octets seulement */
    ctaphid_result_t r3 = ctaphid_feed(&a, cont1, &m);
    TEST_ASSERT_EQ(r3, CTAPHID_COMPLETE, "les 34 derniers octets achevent le message");
    /* `m.data` ne pointe dans a->buf QUE si r3 vaut bien CTAPHID_COMPLETE —
     * sinon (mutation cassant ce chemin), memcmp lirait un pointeur non
     * pertinent. La garde evite qu'un simple defaut de code de retour ne se
     * transforme en plantage du binaire de tests entier. */
    if (r3 == CTAPHID_COMPLETE) {
        TEST_ASSERT_EQ(m.cid, 0xAABBCCDDu, "canal conserve sur tout le reassemblage");
        TEST_ASSERT_EQ(m.cmd, CTAPHID_CMD_MSG, "commande conservee");
        TEST_ASSERT_EQ(m.len, 150, "longueur totale respectee");
        TEST_ASSERT(memcmp(m.data, payload, 150) == 0,
                    "charge utile intacte sur les trois paquets");
    }
}

/* Un paquet de continuation sans initialisation prealable n'a aucun
 * contexte : SEQ vaut delibrement 0 (la valeur de next_seq juste apres
 * ctaphid_reset) pour que ce test exerce vraiment le controle "a->cmd==0",
 * et non le controle de sequence hors-ordre — avec un SEQ different de 0,
 * le rejet passerait par l'autre chemin et laisserait ce controle-ci non
 * exerce. */
static void test_continuation_without_init_is_refused(void)
{
    ctaphid_asm_t a; ctaphid_reset(&a);
    uint8_t cont[64] = {0};
    cont[0]=0x01; cont[1]=0x02; cont[2]=0x03; cont[3]=0x04;
    cont[4] = 0x00;                                 /* SEQ 0, voir commentaire */
    ctaphid_msg_t m;
    TEST_ASSERT_EQ(ctaphid_feed(&a, cont, &m), CTAPHID_ERROR,
                "continuation sans init refusee");
    TEST_ASSERT_EQ(m.err_code, CTAPHID_ERR_INVALID_SEQ, "signalee INVALID_SEQ");
    TEST_ASSERT_EQ(m.err_cid, 0x01020304u,
                "l'erreur part sur le canal du paquet fautif");
}

/* Meme discipline que pour les paquets INIT : une continuation d'un AUTRE
 * canal pendant une transaction en cours est occupee, pas ignoree. SEQ vaut
 * ici la valeur attendue par la session en cours (0, juste apres l'INIT)
 * pour isoler le controle de canal : avec un SEQ different, le controle de
 * sequence rejetterait seul, et ce test ne prouverait rien sur le controle
 * de canal. */
static void test_other_channel_continuation_during_transaction_is_busy(void)
{
    ctaphid_asm_t a; ctaphid_reset(&a);
    uint8_t init[64] = {0};
    init[0]=7; init[4]=0x80u|CTAPHID_CMD_PING; init[5]=0x01; init[6]=0x00; /* 256 */
    ctaphid_msg_t m;
    (void)ctaphid_feed(&a, init, &m);
    uint8_t cont_other[64] = {0};
    /* Meme correction de position qu'au-dessus : le CID gros-boutiste se lit
     * sur les quatre octets, l'octet de poids faible est le dernier. */
    cont_other[3]=9; cont_other[4]=0x00;            /* SEQ attendu, mauvais canal */
    TEST_ASSERT_EQ(ctaphid_feed(&a, cont_other, &m), CTAPHID_ERROR,
                "continuation d'un autre canal refusee");
    TEST_ASSERT_EQ(m.err_code, CTAPHID_ERR_CHANNEL_BUSY, "signalee CHANNEL_BUSY");
    TEST_ASSERT_EQ(m.err_cid, 9u,
                "l'erreur part sur le canal fautif (9), pas sur celui en cours (7)");
}

/* La specification exige qu'un INIT sur le canal DEJA en cours resynchronise
 * la transaction au lieu de l'echouer : un hote qui a perdu le fil doit
 * pouvoir repartir a zero sur son propre canal. Le second message (commande
 * et longueur differentes du premier) doit etre celui livre, pas un melange
 * avec le premier message abandonne. */
static void test_init_on_own_channel_resyncs(void)
{
    ctaphid_asm_t a; ctaphid_reset(&a);
    uint8_t first[64] = {0};
    first[0]=0x10; first[1]=0x20; first[2]=0x30; first[3]=0x40;
    first[4] = 0x80u | CTAPHID_CMD_PING;
    first[5] = 0x00; first[6] = 0x64;               /* BCNT = 100, incomplet */
    ctaphid_msg_t m;
    TEST_ASSERT_EQ(ctaphid_feed(&a, first, &m), CTAPHID_IN_PROGRESS,
                "premier message entame, pas termine");

    uint8_t second[64] = {0};
    second[0]=0x10; second[1]=0x20; second[2]=0x30; second[3]=0x40;
    second[4] = 0x80u | CTAPHID_CMD_CBOR;
    second[5] = 0x00; second[6] = 0x05;             /* BCNT = 5, tient dans le paquet */
    second[7]='x'; second[8]='y'; second[9]='z'; second[10]='!'; second[11]='?';
    ctaphid_result_t r2 = ctaphid_feed(&a, second, &m);
    TEST_ASSERT_EQ(r2, CTAPHID_COMPLETE, "resynchronisation acceptee, pas d'erreur");
    /* Meme garde que dans test_multi_packet_reassembly : ne lire m.data que
     * si le code de retour promet qu'il est valide. */
    if (r2 == CTAPHID_COMPLETE) {
        TEST_ASSERT_EQ(m.cmd, CTAPHID_CMD_CBOR, "la commande vient du second INIT, pas du premier");
        TEST_ASSERT_EQ(m.len, 5, "la longueur vient du second INIT, pas du premier (100)");
        TEST_ASSERT(memcmp(m.data, "xyz!?", 5) == 0,
                    "la charge utile est celle du second message, pas un melange");
    }
}

void test_ctaphid(void)
{
    TEST_SUITE("ctaphid trames");
    TEST_RUN(test_single_packet_message);
    TEST_RUN(test_oversized_length_is_refused);
    TEST_RUN(test_out_of_order_sequence_is_refused);
    TEST_RUN(test_other_channel_during_transaction_is_busy);
    TEST_RUN(test_cid_allocation_skips_reserved);
    TEST_RUN(test_cid_allocation_from_broadcast_never_wraps_to_zero);
    TEST_RUN(test_max_payload_boundary);
    TEST_RUN(test_zero_length_message_completes_immediately);
    TEST_RUN(test_multi_packet_reassembly);
    TEST_RUN(test_continuation_without_init_is_refused);
    TEST_RUN(test_other_channel_continuation_during_transaction_is_busy);
    TEST_RUN(test_init_on_own_channel_resyncs);
}
