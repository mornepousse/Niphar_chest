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
#include "storage/sd_card.h"
#include "usb/usb_mode.h"

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
/* Ce que la découpe des écritures suppose de la carte de registres.          */
/* ------------------------------------------------------------------------- */

/*
 * Le bloc se publie en DEUX plages, jamais en une : 0x00..0x0B (les champs du
 * coffre, couverts par le CRC) puis 0x0D..0x0F (l'octet réservé et le CRC).
 * L'octet 0x0C est sauté parce qu'il appartient au maître — l'écrire effacerait
 * un appui déjà posé par le S3 et pas encore lu.
 *
 * Ces trois assertions verrouillent l'arithmétique de cette découpe. Si un jour
 * la carte des registres bouge dans link_proto.h, le build casse ici plutôt
 * que de produire un firmware qui écrase silencieusement les confirmations.
 */
_Static_assert(LINK_REG_CRC_SPAN == LINK_REG_USER_CONFIRM,
               "la zone du coffre doit s'arreter exactement ou commence l'octet du maitre");
_Static_assert(LINK_REG_RESERVED == LINK_REG_USER_CONFIRM + 1,
               "la seconde plage publiee doit reprendre juste apres l'octet du maitre");
_Static_assert(LINK_REG_SIZE <= SOC_SPI_MAXIMUM_BUFFER_SIZE,
               "le bloc de registres depasse le tampon partage du peripherique");

/*
 * L'application écrit ce tampon PAR MOTS de 32 bits, le maître le lit et
 * l'écrit PAR OCTETS (spi_slave_hd.rst, « Writing/Reading Shared Registers »).
 * Une écriture qui ne remplit pas un mot entier passe donc par un
 * lire-modifier-écrire (hal/esp32p4/include/hal/spi_ll.h,
 * spi_ll_write_buffer_byte).
 *
 * La première plage fait exactement trois mots : aucune relecture, donc aucune
 * fenêtre. La seconde (0x0D..0x0F) partage son mot avec l'octet du maître et
 * passe forcément par un lire-modifier-écrire — c'est la seule fenêtre du
 * module, et elle est documentée à publish() plus bas.
 */
_Static_assert(LINK_REG_MAGIC % 4 == 0 && LINK_REG_CRC_SPAN % 4 == 0,
               "la zone du coffre doit couvrir des mots entiers du tampon partage");

/* ------------------------------------------------------------------------- */
/* État du module.                                                            */
/* ------------------------------------------------------------------------- */

static bool     s_up;             /* le pilote est installé */
static bool     s_ready;          /* app_main() est allé au bout */
static uint32_t s_confirm_count;  /* appuis relayés à sec_confirm */

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
 * ce qui change — voir publish(). L'octet 0x0C y est tenu à zéro : il n'est
 * jamais publié, donc jamais comparé. */
static uint8_t s_published[LINK_REG_SIZE];
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
    /* Plage 1 — les champs du coffre, exactement trois mots du tampon partagé,
     * donc écrits d'un bloc sans relecture. */
    if (!s_published_valid
        || memcmp(&s_published[LINK_REG_MAGIC], &regs[LINK_REG_MAGIC], LINK_REG_CRC_SPAN) != 0) {
        spi_slave_hd_write_buffer(LINK_HOST, LINK_REG_MAGIC, &regs[LINK_REG_MAGIC],
                                  LINK_REG_CRC_SPAN);
    }

    /*
     * Plage 2 — l'octet réservé et le CRC. Elle partage son mot de 32 bits avec
     * l'octet du maître (0x0C), donc le pilote la pose par lire-modifier-écrire :
     * il relit le mot, y remet nos trois octets, et réécrit le tout. L'octet du
     * maître survit dans le cas ordinaire, puisqu'il est relu juste avant.
     *
     * Reste une fenêtre de quelques cycles : un appui écrit par le S3 ENTRE la
     * relecture et la réécriture est perdu. On ne peut pas la fermer d'ici — le
     * maître écrit par le matériel, sans nous demander la main — mais on la
     * réduit à sa cause : cette plage n'est réécrite que si son contenu change,
     * c'est-à-dire seulement quand l'état du coffre change, pas à chaque tour de
     * boucle. Et l'appui perdu n'est pas silencieux pour le maître : le compteur
     * de confirmations (0x08-0x0B) ne bouge pas, ce qui lui dit de réessayer.
     *
     * La vraie correction serait de sortir le CRC du mot du maître, dans la
     * carte des registres — donc dans link_proto, et donc dans le contrat à
     * écrire pour KeSp. Pas ici.
     */
    if (!s_published_valid
        || memcmp(&s_published[LINK_REG_RESERVED], &regs[LINK_REG_RESERVED],
                  LINK_REG_SIZE - LINK_REG_RESERVED) != 0) {
        spi_slave_hd_write_buffer(LINK_HOST, LINK_REG_RESERVED, &regs[LINK_REG_RESERVED],
                                  LINK_REG_SIZE - LINK_REG_RESERVED);
    }

    memcpy(s_published, regs, LINK_REG_SIZE);
    /* Jamais publié, donc jamais comparé : le tenir à zéro évite qu'une valeur
     * du maître entre dans la décision de réécrire. */
    s_published[LINK_REG_USER_CONFIRM] = 0x00;
    s_published_valid = true;
}

/* Sérialise l'état réel du coffre. La composition des bits appartient à
 * link_proto ; ici on ne fait que constater. */
static void pack_current(uint8_t *regs, uint16_t pending_op)
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
    if (usb_mode_is_known() && usb_mode_get() != USB_MODE_NONE) {
        state |= LINK_STATE_USB_MOUNTED;
    }
    if (s_ready) {
        state |= LINK_STATE_READY;
    }

    const link_status_t status = {
        .version       = LINK_PROTO_VERSION,
        .state         = state,
        .pending_op    = pending_op,
        .confirm_count = s_confirm_count,
    };

    memset(regs, 0, LINK_REG_SIZE);
    link_proto_pack_status(regs, &status);
}

/* ------------------------------------------------------------------------- */
/* La boucle.                                                                 */
/* ------------------------------------------------------------------------- */

/* Relit l'octet du maître, le reprend, et relaie un appui réel. */
static void drain_user_confirm(uint32_t t)
{
    uint8_t confirm = 0x00;
    spi_slave_hd_read_buffer(LINK_HOST, LINK_REG_USER_CONFIRM, &confirm, 1);
    if (confirm == 0x00) {
        return;
    }

    /*
     * Reprendre l'octet quoi qu'il vaille, et AVANT de statuer dessus : laissé
     * en place, il serait relu à chaque tour et rejouerait le même appui sur
     * toutes les opérations armées ensuite. Un geste, une autorisation.
     */
    uint8_t cleared = 0x00;
    spi_slave_hd_write_buffer(LINK_HOST, LINK_REG_USER_CONFIRM, &cleared, 1);

    if (confirm != LINK_USER_CONFIRM_MAGIC) {
        /*
         * LINK_USER_CONFIRM_MAGIC (0x5A) est choisi pour que du bruit sur le
         * bus ne le produise pas : ni 0x00 ni 0xFF, les deux valeurs d'une
         * ligne flottante, et pas davantage un 1 qu'un bit parasite ferait
         * apparaître. Tout le reste se jette — et se journalise, parce qu'un
         * octet inattendu ici veut dire soit un maître qui n'a pas le bon
         * protocole, soit un bus qui se dégrade, et les deux méritent d'être
         * vus.
         */
        ESP_LOGW(TAG, "octet de confirmation inattendu 0x%02X — ignoré", confirm);
        return;
    }

    /* Une écriture du maître prouve à elle seule qu'il est là. */
    s_master_seen = true;

    /*
     * sec_confirm décide, pas nous : hors d'une opération armée, cet appel n'a
     * aucun effet — un appui hors contexte n'est pas une erreur. Le compteur,
     * lui, compte ce qui a été RELAYÉ, pas ce qui a été accordé : c'est ce qui
     * permet au maître de distinguer « mon écriture est arrivée » de « elle a
     * servi ».
     */
    sec_confirm_authorize(t);
    s_confirm_count++;
}

static void link_task(void *arg)
{
    (void)arg;

    for (;;) {
        const uint32_t t = now_ms();

        drain_user_confirm(t);

        /*
         * Un seul accesseur pour l'état ET l'opération — jamais peek() suivi
         * d'une lecture séparée : sec_confirm.h l'exige, et la raison y est
         * détaillée. L'étiquette ne nous concerne pas (elle est faite pour un
         * écran, pas pour un bus), d'où le NULL.
         */
        sec_op_t op = SEC_OP_UNKNOWN;
        const sec_confirm_state_t st = sec_confirm_peek_labeled(t, &op, NULL);
        const bool pending = (st == SEC_CONFIRM_PENDING);

        uint8_t regs[LINK_REG_SIZE];
        pack_current(regs, pending ? (uint16_t)op : 0);
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
    pack_current(regs, 0);
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
