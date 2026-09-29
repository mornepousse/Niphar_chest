#include "link/link_spi.h"

#include "board.h"

#if BOARD_LINK_AVAILABLE

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_slave_hd.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"

#include "link/link_proto.h"
#include "sec_confirm.h"
#include "sec_time.h"
#include "storage/sd_card.h"
#include "usb/usb_mode.h"
#include "usb/usb_mode_wire.h"

static const char *TAG = "link";

/*
 * SPI2 et pas SPI3 : c'est le seul hôte dont le quatuor CS/MOSI/CLK/MISO tombe
 * sur les broches 7/8/9/10 du P4 par l'IOMUX (soc/spi_pins.h:29-32), et le
 * pilote rebascule TOUT le bus sur la matrice GPIO dès qu'un seul signal en
 * sort. Sur un faisceau entre deux cartes, le retard d'entrée que la matrice
 * ajoute à MISO n'est pas de trop.
 */
#define LINK_HOST           SPI2_HOST

/*
 * 20 ms. Le lien ne transporte qu'un appui humain : la latence perçue est celle
 * du doigt, pas celle du bus, et sec_confirm laisse SEC_CONFIRM_TIMEOUT_MS
 * (15 s) pour répondre. Descendre plus bas ne gagnerait rien de mesurable et
 * ferait tourner une tâche pour rien sur une carte qui sert aussi du MSC.
 */
#define LINK_TICK_MS        20
#define LINK_TASK_STACK     3072
/*
 * Sous hmi (5) et usb (5) : ce lien est un supplément, il ne doit jamais
 * retarder ni l'IHM ni l'énumération USB. Le pilote SPI répond de toute façon
 * au maître par le matériel, sans passer par cette tâche.
 */
#define LINK_TASK_PRIO      4

/* ------------------------------------------------------------------------- */
/* Ce que la publication des registres suppose de leur carte.                 */
/* ------------------------------------------------------------------------- */

/*
 * Le bloc se publie en UNE SEULE plage : 0x00..0x0F, toute la zone du coffre.
 *
 * Ça n'a pas toujours été possible. La toute première carte logeait le CRC du
 * coffre et l'octet du maître dans le MÊME mot de 32 bits, ce qui obligeait à
 * publier en deux morceaux et à sauter l'octet du maître — et le second
 * morceau, incomplet, passait par un lire-modifier-écrire pendant lequel un
 * appui du S3 pouvait être perdu. link_proto.h a séparé les deux extrémités
 * dans des mots distincts ; la découpe n'a plus de raison d'être.
 *
 * Le CRC est revenu en 0x0E à la v2 et ce n'est PAS un retour en arrière : le
 * défaut d'alors n'était pas son offset mais le MOT qu'il partageait. Le maître
 * vit maintenant en 0x10-0x13, le coffre possède 0x00-0x0F en entier, et les
 * assertions ci-dessous continuent de le prouver à chaque build.
 *
 * L'application écrit ce tampon PAR MOTS de 32 bits, le maître le lit et
 * l'écrit PAR OCTETS (spi_slave_hd.rst, « Writing/Reading Shared Registers ») :
 * une écriture qui ne remplit pas un mot entier passe par un
 * lire-modifier-écrire (hal/esp32p4/include/hal/spi_ll.h,
 * spi_ll_write_buffer_byte). La zone du coffre fait exactement quatre mots
 * pleins et commence sur une frontière de mot — donc aucune relecture, donc
 * aucune fenêtre.
 *
 * Ces assertions verrouillent cet argument. Si un jour la carte des registres
 * bouge dans link_proto.h, le build casse ici plutôt que de produire un
 * firmware qui écrase silencieusement les confirmations.
 */
_Static_assert(LINK_REG_CHEST_BASE + LINK_REG_CHEST_LEN == LINK_REG_MASTER_BASE,
               "la zone du coffre doit s'arreter exactement ou commence celle du maitre");
_Static_assert(LINK_REG_MASTER_BASE + LINK_REG_MASTER_LEN == LINK_REG_SIZE,
               "les deux zones doivent couvrir le bloc entier, sans octet orphelin");
_Static_assert(LINK_REG_CHEST_BASE % 4 == 0 && LINK_REG_CHEST_LEN % 4 == 0
                   && LINK_REG_MASTER_BASE % 4 == 0 && LINK_REG_MASTER_LEN % 4 == 0,
               "chaque zone doit couvrir des mots entiers du tampon partage");
_Static_assert(LINK_REG_CRC + 2 <= LINK_REG_CHEST_BASE + LINK_REG_CHEST_LEN
                   && LINK_REG_CRC_SPAN <= LINK_REG_CRC,
               "le CRC doit tenir dans la zone du coffre, apres ce qu'il couvre");
_Static_assert(LINK_REG_SIZE <= SOC_SPI_MAXIMUM_BUFFER_SIZE,
               "le bloc de registres depasse le tampon partage du peripherique");

/* ------------------------------------------------------------------------- */
/* État du module.                                                            */
/* ------------------------------------------------------------------------- */

static bool     s_up;             /* le pilote est installé */
static bool     s_ready;          /* app_main() est allé au bout */
static uint32_t s_confirm_count;  /* appuis relayés à sec_confirm */

/*
 * Dernière valeur de fil du mode RÉELLEMENT APPLIQUÉE — pas la dernière lue.
 *
 * LINK_USB_MODE_NONE au démarrage, et c'est la vérité : le coffre démarre sans
 * rien exposer. C'est ce repère-là qui rend la sélection auto-réparante après un
 * reboot du coffre — le tampon partagé repart à zéro, le maître relit 0x12, le
 * voit différent du mode qu'il veut, le réécrit, et le coffre voit un vrai
 * changement. Un repère sur la dernière valeur LUE aurait dit « inchangé » et
 * laissé le coffre muet jusqu'à ce que la propriétaire change de mode deux fois.
 *
 * Mis à jour SEULEMENT quand la bascule a réussi : une bascule refusée (une
 * autre est déjà en cours, ESP_ERR_INVALID_STATE) est donc retentée au tour
 * suivant, sans que le maître ait rien à détecter.
 */
static uint8_t  s_mode_applied = LINK_USB_MODE_NONE;

/*
 * Dernière valeur de mode dont on s'est plaint. Le protocole veut qu'une valeur
 * inconnue soit refusée ET journalisée ; sans ce souvenir, un maître qui laisse
 * un octet aberrant en 0x12 ferait écrire cinquante lignes par seconde, ce qui
 * revient à ne rien journaliser du tout. Une ligne par valeur distincte.
 */
static uint8_t  s_mode_complained = LINK_USB_MODE_NONE;
static bool     s_mode_complained_valid;

/*
 * Le S3 nous a-t-il parlé au moins une fois ? Posé depuis l'ISR du pilote,
 * lu par la tâche : `volatile` suffit, c'est un booléen d'un seul écrivain
 * qui ne redescend jamais à faux.
 *
 * Sert l'invariant de boards/niphar_chest/board.h : ne JAMAIS asserter IO11
 * avant que le S3 ait parlé. Ça garde la ligne calme pendant le boot du
 * clavier, où elle arrive sur un de ses pins de strapping.
 */
static volatile bool s_master_seen;

/* Dernier bloc réellement poussé dans le tampon partagé, pour ne réécrire que
 * ce qui change — voir publish(). Dimensionné à la SEULE zone du coffre : ce
 * qu'on ne publie pas n'a pas à peser dans la décision de republier. */
static uint8_t s_published[LINK_REG_CHEST_LEN];
static bool    s_published_valid;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* ------------------------------------------------------------------------- */
/* Broches.                                                                   */
/* ------------------------------------------------------------------------- */

/*
 * Remet les cinq broches du lien en entrée, fonction GPIO, SANS pull.
 *
 * Appelée avant l'init (pour partir d'un état connu) et après tout échec.
 * gpio_config() coupe la sortie ET rend la fonction GPIO au pin
 * (esp_driver_gpio/src/gpio.c : gpio_output_disable + gpio_hal_func_sel(…,
 * PIN_FUNC_GPIO)), ce que gpio_set_direction() seul ne ferait pas : après un
 * échec tardif de spi_slave_hd_init(), MISO est encore sur l'IOMUX du
 * périphérique — et spi_slave_hd_deinit() ne libère PAS le brochage
 * (esp_driver_spi/src/gpspi/spi_slave_hd.c : aucun appel à
 * spicommon_bus_free_io_cfg), donc personne d'autre ne le ferait.
 *
 * Sans pull, délibérément : gpio_reset_pin() arme un pull-up « pour raisons
 * d'économie », ce qui n'a pas sa place sur trois lignes que le clavier pilote.
 */
static void link_pins_release(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << BOARD_LINK_MOSI)
                      | (1ULL << BOARD_LINK_SCK)
                      | (1ULL << BOARD_LINK_MISO)
                      | (1ULL << BOARD_LINK_CS)
                      | (1ULL << BOARD_LINK_IRQ),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    (void)gpio_config(&cfg);
}

/*
 * Pose la ligne d'interruption à son niveau de repos.
 *
 * ACTIVE À L'ÉTAT HAUT, avec un pull-down côté S3 — l'inverse de la convention
 * habituelle, et l'inverse du seul autre signal d'interruption du clavier (la
 * nRF24, active BAS avec pull-up). Écrire celle-ci « par symétrie avec le
 * voisin » produirait un lien qui ne réveille jamais, sans un seul message
 * d'erreur. La raison de ce choix est dans boards/niphar_chest/board.h : le
 * coffre est non alimenté la plupart du temps, cette broche est alors en haute
 * impédance, et un pull-up injecterait du courant dans un rail éteint à travers
 * les diodes de protection du P4.
 *
 * Seule sortie de ce module, et elle n'est partagée avec personne : c'est ce
 * qui la distingue des trois lignes du bus.
 */
static esp_err_t link_irq_configure(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << BOARD_LINK_IRQ,
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    const esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    return gpio_set_level(BOARD_LINK_IRQ, BOARD_LINK_IRQ_ACTIVE_HIGH ? 0 : 1);
}

static void link_irq_set(bool asserted)
{
    (void)gpio_set_level(BOARD_LINK_IRQ,
                         BOARD_LINK_IRQ_ACTIVE_HIGH ? (asserted ? 1 : 0)
                                                    : (asserted ? 0 : 1));
}

/* ------------------------------------------------------------------------- */
/* Le maître nous a parlé.                                                    */
/* ------------------------------------------------------------------------- */

/*
 * Appelée depuis l'ISR du pilote quand le maître lit (SPI_EV_BUF_TX) ou écrit
 * (SPI_EV_BUF_RX) le tampon partagé. Une seule écriture, rien d'autre : le
 * travail se fait dans link_task().
 *
 * IRAM_ATTR, et ce n'est pas de la prudence décorative : CONFIG_SPI_SLAVE_ISR_IN_IRAM
 * vaut y par défaut (esp_driver_spi/Kconfig:53-55), donc l'ISR qui nous appelle
 * tourne cache désactivé. Un rappel resté en flash s'y planterait dès qu'une
 * transaction tombe pendant une écriture de flash — c'est-à-dire pendant un
 * nvs_set, donc pendant précisément les opérations que le lien est censé
 * confirmer.
 */
static IRAM_ATTR bool on_master_touch(void *arg, spi_slave_hd_event_t *event, BaseType_t *awoken)
{
    (void)arg;
    (void)event;
    (void)awoken;
    s_master_seen = true;
    return false;
}

/* ------------------------------------------------------------------------- */
/* Publication du bloc de registres.                                          */
/* ------------------------------------------------------------------------- */

/*
 * `regs` n'est pas const : spi_slave_hd_write_buffer() prend un uint8_t*, et un
 * cast pour retirer le const serait une façon de se mentir sur qui possède ce
 * tampon.
 */
static void publish(uint8_t *regs)
{
    /*
     * Une seule écriture, et c'est tout l'intérêt de la carte des registres
     * actuelle : la zone du coffre fait quatre mots pleins alignés, le pilote
     * les pose sans rien relire, donc rien de ce que le maître aurait écrit ne
     * peut se perdre. Les assertions en tête de fichier tiennent cet argument.
     *
     * Toujours conditionnée au changement : republier à l'identique vingt fois
     * par seconde ferait travailler le bus pour rien.
     */
    if (!s_published_valid
        || memcmp(s_published, &regs[LINK_REG_CHEST_BASE], LINK_REG_CHEST_LEN) != 0) {
        spi_slave_hd_write_buffer(LINK_HOST, LINK_REG_CHEST_BASE,
                                  &regs[LINK_REG_CHEST_BASE], LINK_REG_CHEST_LEN);
    }

    memcpy(s_published, &regs[LINK_REG_CHEST_BASE], LINK_REG_CHEST_LEN);
    s_published_valid = true;
}

/* Sérialise l'état réel du coffre. La composition des bits appartient à
 * link_proto ; ici on ne fait que constater. */
static void pack_current(uint8_t *regs, uint16_t pending_op, uint8_t instance,
                         const char *label, uint8_t op_count)
{
    uint8_t state = 0;

#if BOARD_HAS_SD
    if (sd_present()) {
        state |= LINK_STATE_SD_PRESENT;
    }
#endif
    /* « Monté » au sens de l'hôte : un jeu de descripteurs est installé. Le
     * mode incertain (entre deux bascules) ne compte pas — ce que voit l'hôte
     * n'est alors plus garanti, et l'annoncer au clavier serait mentir. */
    const bool mode_known = usb_mode_is_known();
    if (mode_known && usb_mode_get() != USB_MODE_NONE) {
        state |= LINK_STATE_USB_MOUNTED;
    }

    /* Le mode ACTIF, et la même règle que le bit ci-dessus : un mode incertain
     * ne se publie pas, il se déclare incertain. Le clavier affiche alors sa
     * propre demande en attente, au lieu d'annoncer une arrivée qui n'a pas eu
     * lieu.
     *
     * usb_mode_to_wire() n'avait jusqu'ici aucun appelant dans le firmware :
     * elle existait pour que l'aller-retour des deux numérotations soit
     * testable dans les deux sens. C'est son premier usage réel, et c'est le
     * bon — la traduction passe par le switch sur des noms, jamais par un cast
     * qui ferait exactement ce que les deux numérotations interdisent. */
    const uint8_t mode_wire = mode_known ? usb_mode_to_wire(usb_mode_get())
                                         : LINK_USB_MODE_UNKNOWN;
    if (s_ready) {
        state |= LINK_STATE_READY;
    }
    /* L'heure murale, POSÉE et jamais devinée (sec_time.h). Sans ce bit, le
     * clavier affiche « NO TIME » et ne demande aucun code : un code calculé
     * sans heure serait faux tout en paraissant juste, ce qui est pire que pas
     * de code du tout. */
    if (sec_time_is_valid()) {
        state |= LINK_STATE_TIME_VALID;
    }

    /* Le libellé, borné à ce que le fil porte. Il vient de sec_confirm, qui le
     * tient déjà assaini et tronqué par oath_name_display() — donc de l'ASCII
     * imprimable strict, jamais de l'UTF-8 : le clavier dessine en UNSCII, qui
     * ne saurait pas quoi en faire. */
    link_status_t status = {
        .version       = LINK_PROTO_VERSION,
        .state         = state,
        .pending_op    = pending_op,
        .confirm_count = s_confirm_count,
        .instance      = instance,
        .usb_mode_active = mode_wire,
        .op_count      = op_count,
    };

    /* Tampon de travail, jamais le miroir du tampon partagé : publish() n'en
     * pousse que la zone du coffre, donc mettre à zéro celle du maître ici
     * n'écrit rien chez lui. */
    if (label != NULL) {
        size_t n = strnlen(label, LINK_LABEL_MAX);
        status.label_len = (uint8_t)n;
        memcpy(status.label, label, n);
    }

    memset(regs, 0, LINK_REG_SIZE);
    link_proto_pack_status(regs, &status);
}

/* ------------------------------------------------------------------------- */
/* La boucle.                                                                 */
/* ------------------------------------------------------------------------- */

/*
 * Relit l'octet du maître, le reprend, et relaie un appui réel.
 *
 * C'est le SEUL endroit où le coffre écrit dans le mot du maître, et il faut
 * bien qu'il y en ait un : reprendre l'octet fait partie du protocole, sinon
 * l'appui se rejouerait à chaque tour. Cette écriture d'un octet passe donc par
 * un lire-modifier-écrire de son mot, et un second appui arrivant dans ces
 * quelques cycles serait perdu.
 *
 * Ça n'a rien de la fenêtre qu'on vient de fermer dans publish() : celle-là
 * s'ouvrait à chaque changement d'état du coffre, sans rapport avec un appui.
 * Celle-ci ne s'ouvre qu'immédiatement après un appui déjà reçu, alors que le
 * maître attend justement de voir bouger le compteur de confirmations avant de
 * considérer le sien consommé — un second appui dans cet intervalle serait de
 * toute façon un doublon. Et on ne peut pas la fermer en écrivant le mot entier
 * : les trois octets réservés qui suivent appartiennent au maître, les remettre
 * à zéro d'autorité poserait un piège au premier champ qu'il y mettra.
 */
static void drain_user_confirm(uint32_t t, const link_master_t *m, uint8_t armed)
{
    if (m->confirm == 0x00) {
        return;
    }

    /*
     * Reprendre l'octet quoi qu'il vaille, et AVANT de statuer dessus : laissé
     * en place, il serait relu à chaque tour. Et depuis la v2 ce n'est plus
     * seulement une question de rejeu : un 0x5A dont l'écho ne correspond à
     * rien aujourd'hui correspondrait à une instance FUTURE au bout de quelques
     * armements. Le laisser en place transformerait un appui périmé en appui
     * différé. Un geste, une autorisation.
     */
    uint8_t cleared = 0x00;
    spi_slave_hd_write_buffer(LINK_HOST, LINK_REG_USER_CONFIRM, &cleared, 1);

    /* Une écriture du maître prouve à elle seule qu'il est là, qu'elle soit
     * valable ou non. */
    s_master_seen = true;

    if (link_proto_confirm_accepted(m, armed)) {
        /*
         * sec_confirm décide, pas nous : hors d'une opération armée, cet appel
         * n'a aucun effet — un appui hors contexte n'est pas une erreur. Le
         * compteur, lui, compte ce qui a été RELAYÉ, pas ce qui a été accordé :
         * c'est ce qui permet au maître de distinguer « mon écriture est
         * arrivée » de « elle a servi ».
         */
        sec_confirm_authorize(t);
        s_confirm_count++;
        return;
    }

    /*
     * REFUSÉ, ET SILENCIEUX VERS LE MAÎTRE : le compteur ne bouge pas. C'est
     * tout le signal, et il est suffisant — le maître qui ne voit pas bouger le
     * compteur relit le bloc et réessaie avec l'instance courante. Rien n'est
     * renvoyé, rien n'est codé en erreur : il n'y a pas de canal pour ça dans
     * la plage du coffre, et lui en inventer un aurait demandé un champ que la
     * disposition n'a pas.
     *
     * Le silence est vers le MAÎTRE, pas vers le journal : les deux causes de
     * refus veulent dire des choses différentes, et se taire sur les deux
     * rendrait le lien indébogable.
     */
    if (m->confirm != LINK_USER_CONFIRM_MAGIC) {
        /*
         * LINK_USER_CONFIRM_MAGIC (0x5A) est choisi pour que du bruit sur le
         * bus ne le produise pas : ni 0x00 ni 0xFF, les deux valeurs d'une
         * ligne flottante, et pas davantage un 1 qu'un bit parasite ferait
         * apparaître. Un octet inattendu ici veut dire soit un maître qui n'a
         * pas le bon protocole, soit un bus qui se dégrade, et les deux
         * méritent d'être vus.
         */
        ESP_LOGW(TAG, "octet de confirmation inattendu 0x%02X — ignoré", m->confirm);
        return;
    }

    /*
     * Confirmation parfaitement formée, mais portant l'écho d'une AUTRE
     * instance : c'est exactement le défaut de la v1, et il se journalise en
     * INFO plutôt qu'en WARN parce que ce n'est PAS une anomalie. Une écriture
     * perdue suivie d'une expiration suffit à le produire, et c'est le cas
     * normal que ce refus existe pour traiter.
     */
    ESP_LOGI(TAG, "confirmation pour l'instance %u alors que %u est armée — ignorée",
             m->echo, armed);
}

/*
 * Applique le mode USB demandé par le maître en 0x12.
 *
 * AU CHANGEMENT, pas à chaque lecture : le maître relit et réécrit l'octet à
 * chaque cycle (c'est ce qui rend la sélection auto-réparante), donc rebasculer
 * à chaque tour ré-énumérerait le périphérique vingt fois par seconde.
 *
 * AUCUNE CONFIRMATION PHYSIQUE ICI, et c'est une décision de la propriétaire,
 * pas un oubli : appuyer sur une touche du clavier EST le geste. La
 * conséquence — et sa limite — est écrite dans docs/LINK_CONTRACT.md, section
 * « Sélection du mode ». Résumé : exposer un applet n'est pas autoriser une
 * opération ; tout ce qui sort un secret continue d'exiger l'appui ET l'écho
 * d'instance.
 *
 * usb_mode_apply_wire() peut bloquer jusqu'à une quinzaine de secondes (le
 * temps qu'un worker CCID sorte). La tâche du lien cesse donc de republier
 * pendant une bascule. C'est sans conséquence : le bloc déjà publié reste
 * valide et cohérent, et la bascule purge de toute façon toute confirmation
 * armée au passage — il n'y a rien à drainer pendant ce temps-là.
 */
static void service_mode_request(const link_master_t *m)
{
    switch (link_proto_mode_request(m->usb_mode, s_mode_applied)) {
    case LINK_MODE_REQ_UNCHANGED:
        return;

    case LINK_MODE_REQ_REFUSE:
        if (!s_mode_complained_valid || s_mode_complained != m->usb_mode) {
            ESP_LOGW(TAG, "mode USB demandé 0x%02X : valeur hors contrat — refusée, le coffre reste où il est",
                     m->usb_mode);
            s_mode_complained = m->usb_mode;
            s_mode_complained_valid = true;
        }
        return;

    case LINK_MODE_REQ_APPLY:
        break;
    }

    const esp_err_t err = usb_mode_apply_wire(m->usb_mode);
    if (err == ESP_OK) {
        s_mode_applied = m->usb_mode;
        s_mode_complained_valid = false;
        return;
    }

    /* Pas de mise à jour de s_mode_applied : la demande reste en vigueur et
     * sera retentée au prochain tour. Une seule plainte par valeur, pour la
     * même raison que ci-dessus. */
    if (!s_mode_complained_valid || s_mode_complained != m->usb_mode) {
        ESP_LOGW(TAG, "bascule vers le mode 0x%02X refusée (%s) — nouvelle tentative au prochain tour",
                 m->usb_mode, esp_err_to_name(err));
        s_mode_complained = m->usb_mode;
        s_mode_complained_valid = true;
    }
}

static void link_task(void *arg)
{
    (void)arg;

    for (;;) {
        const uint32_t t = now_ms();

        /*
         * UN SEUL accesseur pour l'état, l'opération ET le numéro d'armement —
         * jamais peek() suivi d'une lecture séparée : sec_confirm.h l'exige, et
         * la v2 en fait une question de justesse et plus seulement d'affichage.
         * Un couple déchiré (l'opération d'un armement, le numéro du suivant)
         * ferait publier au coffre une instance qui ne va pas avec l'opération
         * que le maître va montrer — donc renvoyer un écho valable pour une
         * opération que la propriétaire n'a pas vue. C'est le défaut même que
         * l'instance ferme, réintroduit par le transport.
         *
         * L'ÉTIQUETTE NOUS CONCERNE DEPUIS LA v3, et c'est tout son objet : sur
         * le coffre, l'écran qui montre l'opération est celui du CLAVIER, et
         * sans ce nom la propriétaire approuve un TYPE d'opération, jamais un
         * COMPTE. Elle est lue ICI, dans le MÊME appel que l'opération et le
         * numéro d'armement : les trois forment un groupe indivisible, sans
         * quoi le lien publierait le nom d'un armement avec l'instance d'un
         * autre — exactement le couple déchiré que l'instance ferme.
         */
        sec_op_t op = SEC_OP_UNKNOWN;
        uint32_t arm_seq = 0;
        uint8_t  op_count = 0;
        char     label[OATH_NAME_DISPLAY_MAX] = { 0 };
        const sec_confirm_state_t st = sec_confirm_peek_armed(t, &op, label, &arm_seq, &op_count);
        const bool pending = (st == SEC_CONFIRM_PENDING);

        /*
         * L'instance publiée est l'octet de poids faible du compteur
         * d'armements. Publiée MÊME quand rien n'est armé : elle nomme alors le
         * dernier armement, et c'est ce qui permet à une reprise tardive
         * d'aboutir plutôt que de se faire refuser sans raison lisible.
         */
        const uint8_t instance = (uint8_t)arm_seq;

        /* Rien d'armé : ni nom ni nombre. Publier l'étiquette du dernier
         * armement ferait afficher au clavier un compte que plus rien
         * n'attend — le même défaut que sec_confirm_poll() a corrigé pour son
         * propre écran. L'instance, elle, reste publiée : elle nomme le dernier
         * armement expressément, pour qu'une reprise tardive aboutisse. */
        if (!pending) {
            label[0] = '\0';
            op_count = 0;
        }

        uint8_t regs[LINK_REG_SIZE];
        memset(regs, 0, sizeof(regs));

        /*
         * La plage du maître, lue d'un bloc et pas octet par octet : la
         * confirmation et son écho doivent venir de la MÊME lecture. Deux
         * lectures séparées laisseraient le maître écrire entre les deux, et le
         * coffre apparier un 0x5A avec l'écho du coup suivant.
         */
        spi_slave_hd_read_buffer(LINK_HOST, LINK_REG_MASTER_BASE,
                                 &regs[LINK_REG_MASTER_BASE], LINK_REG_MASTER_LEN);

        link_master_t master;
        if (link_proto_parse_master(regs, LINK_REG_SIZE, &master)) {
            drain_user_confirm(t, &master, instance);
            service_mode_request(&master);
        }

        pack_current(regs, pending ? (uint16_t)op : 0, instance, label, op_count);
        publish(regs);

        /* L'invariant de board.h : rien sur la ligne tant que le S3 n'a pas
         * parlé au moins une fois. */
        link_irq_set(s_master_seen && pending);

        vTaskDelay(pdMS_TO_TICKS(LINK_TICK_MS));
    }
}

/* ------------------------------------------------------------------------- */
/* Installation.                                                              */
/* ------------------------------------------------------------------------- */

esp_err_t link_spi_init(void)
{
    if (s_up) {
        return ESP_OK;
    }

    /*
     * D'abord relâcher : si quoi que ce soit avait laissé une de ces cinq
     * broches en sortie, le bus du clavier serait cloué jusqu'ici. Le coût est
     * nul, le mode de panne qu'il ferme a déjà coûté une heure.
     */
    link_pins_release();

    esp_err_t err = link_irq_configure();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ligne IRQ (GPIO%d) inconfigurable : %s — lien abandonné",
                 BOARD_LINK_IRQ, esp_err_to_name(err));
        link_pins_release();
        return err;
    }

    /*
     * SPICOMMON_BUSFLAG_SLAVE vaut 0 : il se lit comme une intention, pas comme
     * un bit. C'est spi_slave_hd_init() qui impose réellement le rôle esclave.
     *
     * SPICOMMON_BUSFLAG_NATIVE_PINS est une EXIGENCE, pas une préférence : le
     * pilote échoue bruyamment (« not using iomux pins ») si le brochage cesse
     * de tomber sur l'IOMUX de SPI2, au lieu de basculer en silence sur la
     * matrice GPIO et d'ajouter du retard d'entrée sur MISO — un défaut qui ne
     * se verrait qu'au faisceau, sur un banc qu'on n'a pas.
     *
     * Les quatre broches ne sont PAS configurées ici : le pilote s'en charge,
     * et lui seul a le droit de les toucher (voir l'en-tête).
     */
    const spi_bus_config_t bus = {
        .mosi_io_num     = BOARD_LINK_MOSI,
        .miso_io_num     = BOARD_LINK_MISO,
        .sclk_io_num     = BOARD_LINK_SCK,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = 0,   /* canal de données non implémenté dans cet incrément */
        .flags           = SPICOMMON_BUSFLAG_SLAVE | SPICOMMON_BUSFLAG_NATIVE_PINS,
        .intr_flags      = 0,
    };

    /*
     * Mode 0 (CPOL=0, CPHA=0), et c'est le seul acceptable ici : c'est ce qui
     * garde SCK au repos à l'état bas. Sur un bus partagé, un esclave qui
     * exigerait le repos haut imposerait son mode aux deux autres.
     *
     * command/address/dummy à 8 bits : la valeur par défaut du protocole
     * `spi_slave_hd` d'Espressif (docs/en/api-reference/protocols/
     * esp_spi_slave_protocol.rst). Le maître n'existant pas encore, c'est le
     * défaut du côté qui devra s'y conformer, pas une négociation.
     *
     * queue_size 1 : aucune transaction DMA n'est postée dans cet incrément —
     * seuls les registres partagés servent, et le matériel y répond seul, sans
     * tampon préposté. C'est précisément la raison du choix de spi_slave_hd
     * plutôt que de l'esclave classique.
     */
    const spi_slave_hd_slot_config_t slot = {
        .mode         = 0,
        .spics_io_num = BOARD_LINK_CS,
        .flags        = 0,
        .command_bits = 8,
        .address_bits = 8,
        .dummy_bits   = 8,
        .queue_size   = 1,
        .dma_chan     = SPI_DMA_CH_AUTO,  /* le pilote esclave HD l'exige */
        .cb_config    = {
            .cb_buffer_tx = on_master_touch,
            .cb_buffer_rx = on_master_touch,
            .arg          = NULL,
        },
    };

    err = spi_slave_hd_init(LINK_HOST, &bus, &slot);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI2 esclave non installé : %s — pas de lien", esp_err_to_name(err));
        /*
         * spi_slave_hd_init() nettoie ses ressources par spi_slave_hd_deinit(),
         * qui ne rend PAS le brochage. Sans cette ligne, un échec tardif
         * laisserait MISO sur l'IOMUX d'un périphérique éteint : le pire des
         * états possibles pour le clavier.
         */
        link_pins_release();
        return err;
    }

    /*
     * Publier AVANT d'ouvrir la tâche. Sans ça, le tampon partagé rendrait des
     * zéros — que link_proto_is_absent() lit comme « coffre absent ». Ce serait
     * juste par accident, pas par construction : un maître qui interroge dans
     * cette fenêtre doit voir un coffre présent et non prêt, pas un coffre
     * absent.
     */
    uint8_t regs[LINK_REG_SIZE];
    pack_current(regs, 0, 0, NULL, 0);
    publish(regs);

    if (xTaskCreate(link_task, "link", LINK_TASK_STACK, NULL, LINK_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "création de la tâche du lien impossible — pas de lien");
        (void)spi_slave_hd_deinit(LINK_HOST);
        link_pins_release();
        return ESP_ERR_NO_MEM;
    }

    s_up = true;
    ESP_LOGI(TAG, "lien S3 installé : SPI2 esclave mode 0, CS=%d MOSI=%d SCK=%d MISO=%d (IOMUX)",
             BOARD_LINK_CS, BOARD_LINK_MOSI, BOARD_LINK_SCK, BOARD_LINK_MISO);
    ESP_LOGI(TAG, "IRQ GPIO%d au repos (active à l'état %s) — jamais assertée avant que le S3 ait parlé",
             BOARD_LINK_IRQ, BOARD_LINK_IRQ_ACTIVE_HIGH ? "HAUT" : "BAS");
    return ESP_OK;
}

bool link_spi_is_up(void)
{
    return s_up;
}

void link_spi_set_ready(bool ready)
{
    /* La tâche republiera au prochain tour : rien à pousser ici, et un seul
     * écrivain du tampon partagé vaut mieux que deux. */
    s_ready = ready;
}

#else /* !BOARD_LINK_AVAILABLE */

/*
 * Le kit de dev et la carte-clé n'ont pas ce câblage. Des souches plutôt qu'un
 * « #if » chez chaque appelant : main.c et sec_gate.c n'ont pas à connaître le
 * brochage des cartes pour savoir qu'il n'y a pas de lien.
 */

esp_err_t link_spi_init(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

bool link_spi_is_up(void)
{
    return false;
}

void link_spi_set_ready(bool ready)
{
    (void)ready;
}

#endif /* BOARD_LINK_AVAILABLE */
