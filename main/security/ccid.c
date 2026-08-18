/* main/security/ccid.c — TinyUSB CCID class driver (dongle, Phase 1)
 *
 * Phase 1 behaviour:
 *   - PC_to_RDR_IccPowerOn / IccPowerOff / GetSlotStatus are handled inline
 *     (instant; run in tud_task context).
 *   - PC_to_RDR_XfrBlock is offloaded to a dedicated FreeRTOS worker task so
 *     that PSO:CDS + UIF can block up to SEC_CONFIRM_TIMEOUT_MS waiting for a
 *     physical keypress without stalling the USB task (and thereby CDC + HID).
 *     While the worker waits for the touch, the host is kept alive with CCID
 *     time-extension frames (RDR_to_PC_DataBlock bStatus=0x80, bError=0x02).
 *
 * Implements USB CCID Rev 1.1 §6:
 *   - claims the CCID interface (class 0x0B, interface 4 of the dongle
 *     composite descriptor built in usb_hid.c)
 *   - opens the bulk IN/OUT endpoint pair
 *   - reassembles a CCID message (10-byte header + abData) on bulk OUT
 *   - dispatches by bMessageType and queues an RDR_to_PC response on bulk IN
 *
 * One message in flight at a time (bMaxCCIDBusySlots = 1).
 * All buffers static (no malloc). bSlot and bSeq are always echoed.
 *
 * NOTE: openpgp_card_apdu() mutates applet statics (retries, verified flags,
 * s_key).  It is called exclusively from the ccid_worker task — safe for the
 * current single-consumer design.  If openpgp_card_factory_reset() is ever
 * called concurrently (e.g. from an admin CDC command), a mutex shared by
 * both callers will be required.
 *
 * Compiled for the dongle role only (gated in main/CMakeLists.txt).
 *
 * DIVERGENCE (Niphar_chest task 10): KeSp builds against espressif/esp_tinyusb
 * ^2.0.1, whose vendored TinyUSB predates the `is_isr` parameter added to
 * usbd_edpt_xfer(). Niphar_chest pulls raw espressif/tinyusb >=0.17.0~2 (see
 * main/idf_component.yml), where usbd_edpt_xfer() takes a fifth bool
 * argument. Every call site below passes `false` — all of them run on the
 * tud_task, never from an ISR, matching TinyUSB's own class drivers (e.g.
 * msc_device.c).
 *
 * DIVERGENCE (Niphar_chest task 12) : s_out_buf/s_in_buf/s_wtx_buf gagnent
 * CFG_TUSB_MEM_ALIGN (voir tusb_config.h), absent de l'original. KeSp cible
 * l'ESP32-S3 ; le coffre est un ESP32-P4, dont le contrôleur USB DMA exige
 * des tampons alignés sur la ligne de cache (esp_cache_msync, 64 o) pour que
 * le CPU et le DMA voient la même donnée. Sans cet attribut, le compilateur
 * plaçait ces tampons statiques à une adresse quelconque ; esp_cache_msync
 * échouait silencieusement à chaque transfert bulk (log "cache:
 * esp_cache_msync ... not aligned"), et l'hôte recevait des réponses CCID
 * corrompues (vues sur matériel : scdaemon lisait un octet de type de
 * message à 0x00 au lieu de la vraie réponse). Trouvé en validant `gpg
 * --card-status` (tâche 12) — le mode PGP énumérait correctement mais aucun
 * échange APDU n'aboutissait jamais avant ce correctif.
 *
 * DIVERGENCE (Niphar_chest, revue finale — BLOQUANT 1) : ccid_drv_deinit()
 * n'est plus un no-op, et ccid_shutdown() est ajouté. C'est un BUG DE L'AMONT
 * révélé par notre usage, pas un choix de portage : chez KeSp, TinyUSB est
 * installé une fois et jamais démonté, donc un .deinit vide est inoffensif.
 * Le coffre, lui, désinstalle et réinstalle TinyUSB à chaque bascule de mode
 * (usb_mode.c). Or tud_deinit() détruit la file de tud_task
 * (usbd.c: osal_queue_delete(_usbd_q); _usbd_q = NULL), pendant que la tâche
 * ccid_worker — créée une seule fois et jamais détruite — peut être bloquée
 * jusqu'à 15 s dans dongle_confirm(), à poster un WTX toutes les 1,5 s via
 * usbd_defer_func(). osal_queue_send() sur une file NULL fait échouer un
 * configASSERT() de FreeRTOS, et un callback qui passerait quand même
 * écrirait sur un endpoint périmé, réattribué au MSC ou au HID après
 * réinstallation. Trois pièces au correctif :
 *   1. ccid_shutdown(), appelé par mode_pgp_stop() AVANT usb_device_uninstall(),
 *      pose s_shutdown puis attend que le worker redevienne inactif ;
 *   2. ccid_defer(), seule porte vers usbd_defer_func(), sérialise le test de
 *      s_shutdown et l'envoi sous s_defer_lock — sans ce verrou le test et
 *      l'appel seraient séparés par une fenêtre où la file peut disparaître.
 *      Zéroer s_ep_in ne suffit PAS : l'appel de dongle_confirm() n'est
 *      conditionné par rien, et le crash est dans osal_queue_send(), en amont
 *      de tout usage d'endpoint ;
 *   3. ccid_drv_deinit() fait désormais ce que fait ccid_drv_reset() (endpoints
 *      et état à zéro), pour que le garde `s_ep_in == 0` des deux callbacks
 *      cesse d'être un faux filet après un démontage.
 * À REMONTER À L'AMONT KeSp : le .deinit no-op est un piège dormant tant que
 * personne ne démonte la pile.
 *
 * DIVERGENCE (Niphar_chest, revue finale — BLOQUANT 2) : le seuil de ZLP de
 * ccid_drv_xfer() ne compare plus à 64 en dur mais à s_ep_in_mps, le
 * wMaxPacketSize réellement négocié, relevé dans le descripteur d'endpoint à
 * l'ouverture. Bug de l'amont lui aussi : chez KeSp le dongle n'énumère qu'en
 * pleine vitesse, où 64 EST le MPS, donc la constante était juste par
 * accident. Le coffre négocie la haute vitesse et mode_pgp.c déclare 512 —
 * une réponse de 64, 128, 192 ou 256 octets est alors un paquet COURT que le
 * code prenait pour un multiple exact du MPS : il mettait une ZLP en file et
 * sortait sans réarmer OUT ni remettre s_busy/s_final_queued, figeant le pipe
 * CCID jusqu'à la bascule de mode suivante. L'hôte choisit ces longueurs
 * (total = 10 + réponse APDU, donc 54/118/182/246 octets), et les DO OpenPGP
 * inscriptibles — URL, login data, DO privés — lui permettent de les écrire
 * puis de les relire : déni de service déclenchable depuis l'hôte.
 * À REMONTER À L'AMONT KeSp, avec le wMaxPacketSize haute vitesse (tâche 12).
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "device/usbd_pvt.h"   /* usbd_class_driver_t, usbd_*, tu_desc_* */
#include "ccid.h"
#include "ccid_zlp.h"
#include "openpgp_card.h"
#include "openpgp_crypto.h"
#include "sec_confirm.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "CCID";

/* ------------------------------------------------------------------ */
/* CCID message types (USB CCID Rev 1.1 §6)                            */
/* ------------------------------------------------------------------ */
#define PC_TO_RDR_ICC_POWER_ON    0x62
#define PC_TO_RDR_ICC_POWER_OFF   0x63
#define PC_TO_RDR_GET_SLOT_STATUS 0x65
#define PC_TO_RDR_XFR_BLOCK       0x6F

#define RDR_TO_PC_DATA_BLOCK      0x80
#define RDR_TO_PC_SLOT_STATUS     0x81

#define CCID_HDR_LEN              10

/* bStatus: bmICCStatus(bits1:0)=0 present+active, bmCommandStatus(bits7:6)=0 ok */
#define CCID_STATUS_OK           0x00
#define CCID_ERROR_NONE          0x00

/* sec_confirm slot ID reserved for OpenPGP UIF.
 * sec_store uses slots 0-3; 0xF0 is a dedicated sentinel for the PGP touch gate.
 *
 * INVARIANT (Phase-2 audit): the THREE UIF-gated ops (PSO:CDS D6, PSO:DECIPHER
 * D7, INTERNAL AUTHENTICATE D8) all share this ONE slot, and that is safe
 * because openpgp_card_apdu() runs only on the single ccid_worker task and
 * ccid_dispatch() serialises XfrBlocks via s_busy + the binary semaphore
 * (bMaxCCIDBusySlots=1).  While dongle_confirm() blocks the worker polling for
 * a touch, no second UIF op can arm sec_confirm, and sec_confirm_poll() consumes
 * the AUTHORIZED state before returning — so a DECIPHER touch can never
 * authorise a CDS.  If a SECOND consumer of openpgp_card_apdu() is ever added
 * (e.g. a concurrent CCID worker or an admin CDC command), this single-slot
 * design MUST be revisited with a per-op slot distinction. */
#define CCID_CONFIRM_SLOT        0xF0u

/* How often to fire a WTX frame while the worker waits for a UIF touch. */
#define CCID_WTX_PERIOD_MS       1500u

/* Message buffer: 10-byte header + abData. Sized to comfortably hold a
 * short-APDU exchange (dwMaxCCIDMessageLength = 271 in the descriptor) plus
 * margin. No malloc; static buffers only. */
#define CCID_BUF_SZ              512

/* ------------------------------------------------------------------ */
/* ATR for IccPowerOn (OpenPGP-style T=1).                             */
/* The ATR advertises T=1 + T=15 global bytes, so a TCK check byte is  */
/* MANDATORY (ISO 7816-3): TCK = XOR of all bytes from T0 to the last  */
/* historical byte. Here XOR(DA..00) = 0xCD. Without it scdaemon       */
/* rejects the card with "update_param_by_atr failed: -1".             */
/* ------------------------------------------------------------------ */
static const uint8_t s_atr[] = {
    0x3B,0xDA,0x18,0xFF,0x81,0xB1,0xFE,0x75,0x1F,0x03,
    0x00,0x31,0x84,0x73,0x80,0x01,0x80,0x05,0x90,0x00,
    /* historical byte [17]=LCS=0x05 (operational) — required for gpg factory-reset;
     * mirrors FACTORY_HIST in openpgp_card.c. TCK recomputed: 0xCD ^ 0x00 ^ 0x05 = 0xC8. */
    0xC8   /* TCK = XOR(T0..last historical) */
};

/* ------------------------------------------------------------------ */
/* Endpoint addresses                                                   */
/* ------------------------------------------------------------------ */
static uint8_t s_ep_out;   /* bulk OUT endpoint addr (host -> device) */
static uint8_t s_ep_in;    /* bulk IN  endpoint addr (device -> host) */

/* wMaxPacketSize du point IN, relevé dans le descripteur que TinyUSB vient de
 * parser — donc celui de la vitesse RÉELLEMENT négociée (64 en pleine vitesse,
 * 512 en haute vitesse ; voir mode_pgp.c). 0 tant qu'aucune interface n'est
 * ouverte. C'est la seule référence honnête pour décider d'une ZLP : une
 * constante en dur est fausse à l'une des deux vitesses. Voir la divergence
 * BLOQUANT 2 en tête de fichier. */
static uint16_t s_ep_in_mps;

/* ------------------------------------------------------------------ */
/* APDU buffers (static, no malloc)                                    */
/* ------------------------------------------------------------------ */
/* CFG_TUSB_MEM_ALIGN (tusb_config.h) : aligne sur la ligne de cache — requis
 * sur ESP32-P4 pour que esp_cache_msync() reste cohérent CPU/DMA sur ces
 * tampons passés directement à usbd_edpt_xfer(). Voir la divergence tâche 12
 * ci-dessus. */
CFG_TUSB_MEM_ALIGN static uint8_t s_out_buf[CCID_BUF_SZ];   /* incoming CCID message (OUT)  */
CFG_TUSB_MEM_ALIGN static uint8_t s_in_buf[CCID_BUF_SZ];    /* outgoing CCID response (IN)  */

/* Separate buffer for WTX frames so they never clobber s_in_buf while
 * the worker is building the final APDU response. */
CFG_TUSB_MEM_ALIGN static uint8_t s_wtx_buf[CCID_HDR_LEN];

/* ------------------------------------------------------------------ */
/* Worker-task state                                                    */
/* ------------------------------------------------------------------ */
static TaskHandle_t      s_worker;
static SemaphoreHandle_t s_msg_ready;     /* binary: one XfrBlock pending    */
static volatile bool     s_busy;          /* true while XfrBlock is in flight */

/* Démontage de la pile USB en cours : plus aucun callback ne doit être posté
 * sur la file de tud_task, et dongle_confirm() doit rendre la main tout de
 * suite au lieu d'attendre ses 15 s. Voir la divergence BLOQUANT 1. */
static volatile bool     s_shutdown;

/* Sérialise « tester s_shutdown » et « poster le callback » vis-à-vis de
 * ccid_shutdown(), qui pose le drapeau sous ce même verrou. Créé en même temps
 * que le worker ; NULL avant ccid_init(). */
static SemaphoreHandle_t s_defer_lock;

/* true tant que le worker est DANS ccid_process_xfrblock() (applet + attente de
 * confirmation incluses). Lu par ccid_shutdown() pour attendre qu'il ressorte. */
static volatile bool     s_worker_active;

/*
 * Qui repond aux XfrBlock. NULL = l'applet OpenPGP, le DEFAUT — c'est le
 * chemin valide sur materiel, et il doit rester celui qu'on obtient quand
 * personne n'a rien pose. Voir ccid_set_applet() dans ccid.h.
 *
 * volatile : pose par la tache qui bascule le mode (usb_mode.c), lu par le
 * worker CCID. La bascule n'a lieu qu'apres ccid_shutdown(), qui attend que
 * le worker soit au repos — il n'y a donc jamais d'ecriture concurrente d'une
 * commande en vol.
 */
static ccid_applet_fn_t volatile s_applet;

/* XfrBlock context saved by ccid_dispatch for the worker to echo back. */
static uint8_t           s_cur_slot;
static uint8_t           s_cur_seq;
static uint8_t           s_rhport;
static volatile uint16_t s_resp_len;      /* final response length in s_in_buf */

/* Set to true immediately before queueing a final IN response (from either
 * the inline path or ccid_send_final_cb).  Read and cleared in xfer_cb(IN).
 * Both the set and the clear happen in tud_task context — no additional
 * locking needed.  When false, an IN completion is a WTX acknowledgement
 * and OUT must NOT be re-primed yet. */
static volatile bool     s_final_queued;

/* ------------------------------------------------------------------ */
/* Response builders (write into s_in_buf, return total byte count)    */
/* ------------------------------------------------------------------ */

/* Fill the 10-byte RDR_to_PC header. Assumes abData (data_len bytes) is
 * already present at s_in_buf[CCID_HDR_LEN]. */
static uint16_t ccid_fill_header(uint8_t msg_type, uint8_t slot, uint8_t seq,
                                 uint16_t data_len)
{
    s_in_buf[0] = msg_type;
    s_in_buf[1] = (uint8_t)(data_len & 0xFF);
    s_in_buf[2] = (uint8_t)((data_len >> 8) & 0xFF);
    s_in_buf[3] = 0x00;          /* dwLength is LE; payload < 64KB here */
    s_in_buf[4] = 0x00;
    s_in_buf[5] = slot;          /* echo bSlot */
    s_in_buf[6] = seq;           /* echo bSeq  */
    s_in_buf[7] = CCID_STATUS_OK;
    s_in_buf[8] = CCID_ERROR_NONE;
    s_in_buf[9] = 0x00;          /* bChainParameter / bClockStatus */
    return (uint16_t)(CCID_HDR_LEN + data_len);
}

static uint16_t ccid_build_datablock(uint8_t slot, uint8_t seq,
                                     const uint8_t *data, uint16_t data_len)
{
    if (data_len > CCID_BUF_SZ - CCID_HDR_LEN)
        data_len = CCID_BUF_SZ - CCID_HDR_LEN;
    if (data_len && data)
        memcpy(&s_in_buf[CCID_HDR_LEN], data, data_len);
    return ccid_fill_header(RDR_TO_PC_DATA_BLOCK, slot, seq, data_len);
}

static uint16_t ccid_build_slotstatus(uint8_t slot, uint8_t seq)
{
    return ccid_fill_header(RDR_TO_PC_SLOT_STATUS, slot, seq, 0);
}

/* ------------------------------------------------------------------ */
/* USB-task-side callbacks (scheduled via usbd_defer_func)             */
/* These run on the tud_task; only they may call usbd_edpt_xfer.       */
/* ------------------------------------------------------------------ */

/* Unique porte vers usbd_defer_func() (divergence BLOQUANT 1).
 *
 * Le verrou n'est pas décoratif : ccid_shutdown() pose s_shutdown en le
 * tenant, et il tourne AVANT tusb_deinit(). Donc de deux choses l'une — soit
 * le callback est posté avant, et _usbd_q existe encore (il sera exécuté, ou
 * détruit avec la file, les deux sont sûrs) ; soit s_shutdown est déjà vrai et
 * rien n'est posté. Un simple `if (!s_shutdown) usbd_defer_func(...)` laisserait
 * entre les deux une fenêtre où osal_queue_send() écrit dans _usbd_q == NULL,
 * ce qui fait tomber un configASSERT() de FreeRTOS.
 *
 * Appelé aussi bien depuis ccid_worker que depuis tud_task (le ré-armement de
 * ccid_send_final_cb) : ccid_shutdown() ne fait qu'écrire un booléen sous ce
 * verrou, sans jamais attendre quoi que ce soit en le tenant, donc pas
 * d'interblocage possible. */
static void ccid_defer(osal_task_func_t func, void *param)
{
    if (s_defer_lock == NULL) return;
    xSemaphoreTake(s_defer_lock, portMAX_DELAY);
    if (!s_shutdown)
        usbd_defer_func(func, param, false);
    xSemaphoreGive(s_defer_lock);
}

/* Send a WTX (time extension) frame to keep scdaemon waiting.
 * Silently skipped if the IN endpoint is already busy (e.g. a previous
 * WTX or the final response is still in flight). */
static void ccid_send_wtx_cb(void *param)
{
    (void)param;
    /* USB reset (ccid_drv_reset) zeroes s_ep_in while the worker may still be
     * blocked in dongle_confirm() mid-UIF-wait. Never issue a bulk xfer on
     * EP0 (the control endpoint): WTX is moot until re-enumeration re-opens
     * the interface. (Phase-2 pentest: USB-reset-during-UIF, ccid.c medium.) */
    if (s_ep_in == 0) return;
    if (usbd_edpt_busy(s_rhport, s_ep_in)) return;
    s_wtx_buf[0] = RDR_TO_PC_DATA_BLOCK;
    s_wtx_buf[1] = 0x00;   /* dwLength = 0, LE */
    s_wtx_buf[2] = 0x00;
    s_wtx_buf[3] = 0x00;
    s_wtx_buf[4] = 0x00;
    s_wtx_buf[5] = s_cur_slot;
    s_wtx_buf[6] = s_cur_seq;
    s_wtx_buf[7] = 0x80;   /* bmCommandStatus=10b → time extension; bmICCStatus=00b */
    s_wtx_buf[8] = 0x02;   /* bError: BWT multiplier 2 */
    s_wtx_buf[9] = 0x00;
    if (!usbd_edpt_xfer(s_rhport, s_ep_in, s_wtx_buf, CCID_HDR_LEN, false))
        ESP_LOGW(TAG, "WTX send failed");
}

/* Send the final APDU response built by the worker (s_in_buf[0..s_resp_len)).
 * Retries on the next tud_task tick if a WTX frame is still in flight — this
 * handles the rare race where a WTX was queued just before dongle_confirm()
 * returned and both arrive in the defer queue before the WTX transfer drains. */
static void ccid_send_final_cb(void *param)
{
    uintptr_t retries = (uintptr_t)param;
    /* Interface was reset mid-operation (s_ep_in zeroed by ccid_drv_reset):
     * drop the now-stale response instead of issuing a bulk xfer on EP0 or
     * spinning 200 retries on a dead endpoint. State is already cleared by the
     * reset; re-enumeration re-primes OUT for a fresh command flow.
     * (Phase-2 pentest: USB-reset-during-UIF; also unwedges the abandon path.) */
    if (s_ep_in == 0) {
        s_final_queued = false;
        s_busy         = false;
        return;
    }
    if (usbd_edpt_busy(s_rhport, s_ep_in)) {
        if (retries < 200)
            ccid_defer(ccid_send_final_cb, (void *)(retries + 1));
        else
            ESP_LOGE(TAG, "final send abandoned after 200 retries");
        return;
    }
    s_final_queued = true;
    if (!usbd_edpt_xfer(s_rhport, s_ep_in, s_in_buf, s_resp_len, false))
        ESP_LOGE(TAG, "final IN queue failed");
}

/* ------------------------------------------------------------------ */
/* CCID worker task                                                    */
/* ------------------------------------------------------------------ */

/* Process one pending PC_to_RDR_XfrBlock: read the clamped dwLength that
 * ccid_dispatch wrote back into s_out_buf[1..4], run the applet, build the
 * RDR_to_PC_DataBlock header, and queue the response.  Extracted from the
 * worker loop (behaviour-preserving) so the security-relevant body can be
 * driven synchronously by the host CCID fuzzer (test/fuzz/fuzz_ccid.c). */
static void ccid_process_xfrblock(void)
{
    /* dwLength was clamped by ccid_dispatch and written back into
     * s_out_buf[1..4] (bytes 3+4 zeroed) before the semaphore was given. */
    uint32_t dwLength = (uint32_t)s_out_buf[1]
                      | ((uint32_t)s_out_buf[2] << 8);

    /* Lu UNE fois dans une locale : relire s_applet entre le test et l'appel
     * laisserait, en theorie, deux applets se partager une meme commande. */
    const ccid_applet_fn_t applet = s_applet;
    uint16_t apdu_n = (applet != NULL)
        ? applet(&s_out_buf[CCID_HDR_LEN], (uint16_t)dwLength,
                 &s_in_buf[CCID_HDR_LEN], CCID_BUF_SZ - CCID_HDR_LEN)
        : openpgp_card_apdu(
                          &s_out_buf[CCID_HDR_LEN], (uint16_t)dwLength,
                          &s_in_buf[CCID_HDR_LEN], CCID_BUF_SZ - CCID_HDR_LEN);

    if (apdu_n >= 2) {
        ESP_LOGD(TAG, "APDU in=%u out=%u SW=%02x%02x",
                 (unsigned)dwLength, (unsigned)apdu_n,
                 s_in_buf[CCID_HDR_LEN + apdu_n - 2],
                 s_in_buf[CCID_HDR_LEN + apdu_n - 1]);
    }

    /* One-shot stack high-watermark log (mbedTLS ECDSA P-256 ~2-3 KB). */
    static bool s_hwm_logged;
    if (!s_hwm_logged) {
        s_hwm_logged = true;
        ESP_LOGD(TAG, "worker stack HWM: %u",
                 (unsigned)uxTaskGetStackHighWaterMark(NULL));
    }

    s_resp_len = ccid_fill_header(RDR_TO_PC_DATA_BLOCK,
                                  s_cur_slot, s_cur_seq, apdu_n);

    /* s_in_buf and s_resp_len are written by the worker BEFORE usbd_defer_func().
     * The FreeRTOS queue inside usbd_defer_func provides a full memory barrier
     * (SMP spinlock), guaranteeing visibility from tud_task. Do not bypass it.
     *
     * ccid_defer() plutôt que usbd_defer_func() : si le démontage a commencé, la
     * réponse n'a plus de destinataire et la file n'a peut-être plus d'existence
     * (divergence BLOQUANT 1). On libère alors s_busy nous-mêmes, sinon le
     * prochain passage en mode PGP repartirait avec un slot occupé. */
    if (s_shutdown) {
        s_busy = false;
        return;
    }
    ccid_defer(ccid_send_final_cb, NULL);
}

/* Runs indefinitely; woken by a binary semaphore each time a
 * PC_to_RDR_XfrBlock arrives.  Owns s_in_buf while s_busy is true. */
static void ccid_worker(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_msg_ready, portMAX_DELAY);
        /* Encadre TOUT le traitement : c'est ce que ccid_shutdown() attend
         * avant de laisser démonter TinyUSB. */
        s_worker_active = true;
        ccid_process_xfrblock();
        s_worker_active = false;
    }
}

/* ------------------------------------------------------------------ */
/* Real applet hooks (Phase 1)                                          */
/* ------------------------------------------------------------------ */

static bool dongle_sign(const uint8_t d[32],
                        const uint8_t *hash, uint16_t n,
                        uint8_t *out, uint16_t *out_n)
{
    if (!openpgp_crypto_p256_sign(d, hash, n, out)) return false;
    *out_n = 64;
    return true;
}

/* UIF gate — runs on the ccid_worker task (NOT tud_task).
 * Arms sec_confirm, then polls every 20 ms.  While waiting, fires a CCID
 * time-extension (WTX) frame every CCID_WTX_PERIOD_MS so scdaemon does not
 * time out.  Returns 1 if authorised by touch, 2 if denied / timed out. */
static int dongle_confirm_named(sec_op_t op, const char *label)
{
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    const uint32_t deadline = now + SEC_CONFIRM_TIMEOUT_MS;
    sec_confirm_arm_named(CCID_CONFIRM_SLOT, op, label, now);
    uint32_t last_wtx = now;
    uint8_t  slot     = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(20));

        /* Démontage de la pile USB pendant l'attente de confirmation : refuser
         * tout de suite. Attendre les 15 s restantes ferait patienter d'autant
         * la bascule de mode, et surtout laisserait tourner une boucle qui
         * poste des WTX sur une file que tud_deinit() est en train de détruire
         * (divergence BLOQUANT 1). Refuser est le comportement honnête : la
         * confirmation physique n'a pas eu lieu. */
        if (s_shutdown) {
            sec_confirm_reset();
            return 2;
        }

        now = (uint32_t)(esp_timer_get_time() / 1000);
        if ((int32_t)(now - deadline) >= 0) {
            /* Discard any button press that raced the outer deadline so a stale
             * AUTHORIZED state cannot linger until the next arm (Phase-2 audit). */
            sec_confirm_reset();
            return 2;   /* unconditional termination */
        }

        sec_confirm_state_t st = sec_confirm_poll(now, &slot);
        if (st == SEC_CONFIRM_AUTHORIZED)
            return (slot == CCID_CONFIRM_SLOT) ? 1 : 2;
        if (st == SEC_CONFIRM_TIMEDOUT)
            return 2;

        if (now - last_wtx >= CCID_WTX_PERIOD_MS) {
            ccid_defer(ccid_send_wtx_cb, NULL);
            last_wtx = now;
        }
    }
}

/*
 * Enveloppe du chemin OpenPGP, validee sur materiel : elle passe NULL, donc
 * l'ecran n'affiche que le libelle d'operation, exactement comme avant. Le
 * corps est PARTAGE et non recopie — c'est lui qui teste s_shutdown, et une
 * seconde boucle d'attente finirait par en diverger (voir la divergence
 * BLOQUANT 1 en tete de fichier).
 */
static int dongle_confirm(sec_op_t op)
{
    return dongle_confirm_named(op, NULL);
}

/* Meme corps, ouvert aux modes qui ont un compte a nommer (usb/mode_oath.c). */
int ccid_confirm_named(sec_op_t op, const char *label)
{
    return dongle_confirm_named(op, label);
}

/* Derive the public key for READ PUBLIC KEY (INS 0x47 P1=0x81) — gpg keytocard
 * reads the card's public key here to build the card-backed secret stub.
 * P-256 (ECDSA) -> 65 B uncompressed point; X25519 (ECDH) -> 32 B LE u. */
static bool dongle_pubkey(uint8_t algo, const uint8_t d[32],
                          uint8_t *out, uint16_t *out_n)
{
    if (algo == PGP_ALGO_ECDH) {
        if (!openpgp_crypto_x25519_pubkey(d, out)) return false;
        *out_n = 32;
        return true;
    }
    if (!openpgp_crypto_p256_pubkey(d, out)) return false;
    *out_n = 65;
    return true;
}

static bool dongle_ecdh(const uint8_t d[32], const uint8_t *peer, uint16_t peer_n,
                        uint8_t *out, uint16_t *out_n)
{
    if (peer_n != 32) return false;
    if (!openpgp_crypto_x25519_ecdh(d, peer, out)) return false;
    *out_n = 32;
    return true;
}

static bool dongle_genkey(uint8_t algo, uint8_t d_out[32])
{
    return openpgp_crypto_genkey(algo, d_out);
}

static const openpgp_card_hooks_t s_dongle_hooks = {
    .sign    = dongle_sign,
    .confirm = dongle_confirm,
    .pubkey  = dongle_pubkey,
    .ecdh    = dongle_ecdh,
    .genkey  = dongle_genkey,   /* used from Task 5; harmless before */
};

/* ------------------------------------------------------------------ */
/* Message dispatch (runs in tud_task context via ccid_drv_xfer)       */
/* ------------------------------------------------------------------ */
static void ccid_dispatch(uint8_t rhport, uint32_t xferred)
{
    /* Re-prime OUT and bail on a runt frame (no full header).
     * Guard s_busy: if a worker is live, OUT should not be primed
     * (the runt should never happen in that state, but be safe). */
    if (xferred < CCID_HDR_LEN) {
        if (!s_busy) {
            if (!usbd_edpt_xfer(rhport, s_ep_out, s_out_buf, sizeof(s_out_buf), false))
                ESP_LOGE(TAG, "OUT re-prime failed — CCID pipe wedged");
        }
        return;
    }

    uint8_t  msg_type = s_out_buf[0];
    uint32_t dwLength = (uint32_t)s_out_buf[1]
                      | ((uint32_t)s_out_buf[2] << 8)
                      | ((uint32_t)s_out_buf[3] << 16)
                      | ((uint32_t)s_out_buf[4] << 24);
    uint8_t  bSlot = s_out_buf[5];
    uint8_t  bSeq  = s_out_buf[6];

    ESP_LOGD(TAG, "CCID msg type=0x%02x seq=%u dwLen=%u",
             msg_type, bSeq, (unsigned)dwLength);

    /* Clamp the declared payload to what actually arrived. */
    uint32_t avail = xferred - CCID_HDR_LEN;
    if (dwLength > avail) dwLength = avail;

    uint16_t resp_len;
    switch (msg_type) {
    case PC_TO_RDR_ICC_POWER_ON:
        resp_len = ccid_build_datablock(bSlot, bSeq, s_atr, sizeof(s_atr));
        break;

    case PC_TO_RDR_ICC_POWER_OFF:
    case PC_TO_RDR_GET_SLOT_STATUS:
        resp_len = ccid_build_slotstatus(bSlot, bSeq);
        break;

    case PC_TO_RDR_XFR_BLOCK:
        /* Write the clamped dwLength back into s_out_buf so the worker reads
         * the safe value (bytes 3+4 zeroed; max apdu < 64 KB). */
        s_out_buf[1] = (uint8_t)(dwLength & 0xFF);
        s_out_buf[2] = (uint8_t)((dwLength >> 8) & 0xFF);
        s_out_buf[3] = 0x00;
        s_out_buf[4] = 0x00;
        s_cur_slot = bSlot;
        s_cur_seq  = bSeq;
        s_rhport   = rhport;
        s_busy     = true;
        xSemaphoreGive(s_msg_ready);
        return;   /* worker owns s_in_buf, IN endpoint, and OUT re-prime */

    default:
        /* Unhandled message: answer with a slot status so the host is
         * not left waiting. */
        resp_len = ccid_build_slotstatus(bSlot, bSeq);
        break;
    }

    /* Queue the inline response.  Set s_final_queued so xfer_cb knows this
     * IN completion is a final (not WTX) transfer and must re-prime OUT. */
    s_final_queued = true;
    if (!usbd_edpt_xfer(rhport, s_ep_in, s_in_buf, resp_len, false))
        ESP_LOGE(TAG, "IN queue failed");
}

/* ------------------------------------------------------------------ */
/* Class driver callbacks                                              */
/* ------------------------------------------------------------------ */
static void ccid_drv_init(void)
{
    s_ep_out    = 0;
    s_ep_in     = 0;
    s_ep_in_mps = 0;
    /* Nouvelle pile USB installée : la fenêtre de démontage est refermée, les
     * callbacks peuvent de nouveau être postés (divergence BLOQUANT 1). */
    s_shutdown  = false;
    /* Et l'aiguillage repart sur son défaut. Un applet posé par le mode
     * précédent qui survivrait ici répondrait à la place du suivant : c'est
     * au mode qui vient d'être installé de se déclarer, jamais à l'absence
     * de nettoyage de décider. Sûr ici, et seulement ici : ce callback tourne
     * dans usb_device_install(), donc AVANT le mode_*_start() qui pose
     * l'applet. */
    s_applet    = NULL;
    /* Wire the real Phase-1 hooks (P-256 sign + sec_confirm UIF gate). */
    openpgp_card_init(&s_dongle_hooks);
}

/* Appelé par tud_deinit() juste avant la destruction de la file de tud_task.
 * L'amont KeSp renvoie true sans rien faire — inoffensif chez eux, qui ne
 * démontent jamais ; ici ça laissait s_ep_in pointer un endpoint périmé, que
 * la réinstallation réattribue au MSC ou au HID. Même remise à zéro que
 * ccid_drv_reset(). Voir la divergence BLOQUANT 1 en tête de fichier. */
static bool ccid_drv_deinit(void)
{
    s_ep_out       = 0;
    s_ep_in        = 0;
    s_ep_in_mps    = 0;
    s_busy         = false;
    s_final_queued = false;
    return true;
}

static void ccid_drv_reset(uint8_t rhport)
{
    (void)rhport;
    s_ep_out       = 0;
    s_ep_in        = 0;
    s_ep_in_mps    = 0;
    s_busy         = false;
    s_final_queued = false;
}

static uint16_t ccid_drv_open(uint8_t rhport,
                              tusb_desc_interface_t const *desc_intf,
                              uint16_t max_len)
{
    (void)max_len;
    TU_VERIFY(desc_intf->bInterfaceClass == TUSB_CLASS_SMART_CARD, 0);

    uint8_t const *p_desc = tu_desc_next(desc_intf);  /* past 9-byte interface */

    /* Skip the CCID functional (class) descriptor — type 0x21 — and any other
     * non-endpoint descriptors until the first endpoint. */
    while (tu_desc_type(p_desc) != TUSB_DESC_ENDPOINT)
        p_desc = tu_desc_next(p_desc);

    uint8_t const *p_ep = p_desc;   /* premier des deux descripteurs d'endpoint */
    uint8_t ep_out = 0, ep_in = 0;
    TU_ASSERT(usbd_open_edpt_pair(rhport, p_desc, 2, TUSB_XFER_BULK,
                                  &ep_out, &ep_in), 0);
    s_ep_out = ep_out;
    s_ep_in  = ep_in;

    /* Relève le wMaxPacketSize du point IN dans le descripteur que TinyUSB
     * vient de nous passer : c'est celui de la configuration rendue par
     * tud_descriptor_configuration_cb(), donc celui de la vitesse réellement
     * négociée. Seule référence valable pour décider d'une ZLP — voir la
     * divergence BLOQUANT 2 en tête de fichier. */
    s_ep_in_mps = 0;
    for (int i = 0; i < 2; i++) {
        tusb_desc_endpoint_t const *ep = (tusb_desc_endpoint_t const *)p_ep;
        if (ep->bEndpointAddress == s_ep_in)
            s_ep_in_mps = tu_edpt_packet_size(ep);
        p_ep = tu_desc_next(p_ep);
    }
    TU_ASSERT(s_ep_in_mps != 0, 0);
    ESP_LOGI(TAG, "interface ouverte, MPS IN = %u o", (unsigned)s_ep_in_mps);

    /* Prime the first bulk-OUT read. */
    TU_ASSERT(usbd_edpt_xfer(rhport, s_ep_out, s_out_buf, sizeof(s_out_buf), false), 0);

    /* Bytes consumed = interface(9) + CCID class desc(54) + 2*EP(7) = 77.
     * Advance past the two endpoint descriptors and return the delta. */
    p_desc = tu_desc_next(p_desc);   /* past bulk OUT EP */
    p_desc = tu_desc_next(p_desc);   /* past bulk IN  EP */
    return (uint16_t)((uintptr_t)p_desc - (uintptr_t)desc_intf);
}

/* No class-specific control requests are needed; STALL them. */
static bool ccid_drv_control_xfer(uint8_t rhport, uint8_t stage,
                                  tusb_control_request_t const *request)
{
    (void)rhport; (void)stage; (void)request;
    return false;
}

static bool ccid_drv_xfer(uint8_t rhport, uint8_t ep_addr,
                          xfer_result_t result, uint32_t xferred_bytes)
{
    if (result != XFER_RESULT_SUCCESS)
        ESP_LOGW(TAG, "xfer ep=0x%02x result=%d xferred=%u",
                 ep_addr, result, (unsigned)xferred_bytes);

    if (ep_addr == s_ep_out) {
        /* A full CCID message arrived: dispatch it. */
        ccid_dispatch(rhport, xferred_bytes);
        return true;
    }
    if (ep_addr == s_ep_in) {
        if (s_final_queued) {
            /* USB bulk spec §5.8.3: a transfer whose byte count is an exact
             * multiple of MPS does not carry an implicit "end-of-transfer"
             * marker.  The host's libusb bulk read (length = dwMaxCCIDMessageLength
             * = 271) only terminates on a short packet OR a ZLP.  If we just
             * delivered N×MPS bytes with no short last packet, send a ZLP now
             * before clearing state and re-priming OUT.  xferred_bytes == 0
             * means the ZLP itself just completed — fall through to re-prime.
             *
             * La décision est dans ccid_zlp.h — seul morceau de logique pure du
             * fichier, donc le seul testable sur l'hôte (test/test_ccid_zlp.c).
             * Elle compare au MPS RÉELLEMENT négocié, PAS à une constante : à
             * 512 (haute vitesse), 64/128/192/256 sont des paquets COURTS, et
             * les traiter comme des multiples du MPS figeait le pipe. Voir la
             * divergence BLOQUANT 2 en tête de fichier. */
            if (ccid_needs_zlp(xferred_bytes, s_ep_in_mps)) {
                if (!usbd_edpt_xfer(rhport, s_ep_in, NULL, 0, false)) {
                    ESP_LOGE(TAG, "ZLP send failed — aborting, re-priming OUT");
                    /* ZLP failed: clear state and recover so OUT is not wedged. */
                    s_final_queued = false;
                    s_busy = false;
                    usbd_edpt_xfer(rhport, s_ep_out, s_out_buf, sizeof(s_out_buf), false);
                }
                /* Wait for ZLP IN completion (next xfer_cb with xferred_bytes==0). */
                return true;
            }
            /* Final APDU response delivered (short last packet, or ZLP just done).
             * Clear busy state and re-prime OUT so the next command can arrive. */
            s_final_queued = false;
            s_busy = false;
            if (!usbd_edpt_xfer(rhport, s_ep_out, s_out_buf, sizeof(s_out_buf), false))
                ESP_LOGE(TAG, "OUT re-prime failed — CCID pipe wedged");
        }
        /* else: a WTX frame was acknowledged by the host — nothing to do;
         * wait for the next WTX or the final response callback. */
        return true;
    }
    return false;
}

static const usbd_class_driver_t ccid_driver = {
    .name            = "CCID",
    .init            = ccid_drv_init,
    .deinit          = ccid_drv_deinit,
    .reset           = ccid_drv_reset,
    .open            = ccid_drv_open,
    .control_xfer_cb = ccid_drv_control_xfer,
    .xfer_cb         = ccid_drv_xfer,
    .xfer_isr        = NULL,
    .sof             = NULL,
};

/* TinyUSB picks up application class drivers through this weak hook (default
 * empty in usbd.c). esp_tinyusb does not override it, so we provide it. */
usbd_class_driver_t const *usbd_app_driver_get_cb(uint8_t *driver_count)
{
    *driver_count = 1;
    return &ccid_driver;
}

/* MUST be called from force-linked code (kase_tinyusb_init).
 *
 * usbd_app_driver_get_cb above is a *strong* override of a *weak* default in
 * libtinyusb.a. But ccid.c exports no other referenced symbol, so without this
 * call the linker never pulls ccid.c.obj from libmain.a — the weak (empty)
 * default wins, no CCID app driver is registered, and SET_CONFIGURATION asserts
 * (process_set_config: no driver claims the CCID interface). This reference
 * forces the object in so our strong symbol takes effect. Do not remove.
 *
 * Phase 1 additions: creates the binary semaphore and worker task (once), and
 * runs the crypto self-test (log-only — CCID is not disabled on failure). */
void ccid_init(void)
{
    static bool s_init_done = false;
    if (s_init_done) return;
    s_init_done = true;

    bool ok = openpgp_crypto_selftest();
    openpgp_card_set_crypto_health(ok);
    if (ok) {
        ESP_LOGI(TAG, "crypto selftest: PASS");
    } else {
        ESP_LOGE(TAG, "crypto selftest: FAIL — cryptographic operations disabled (SW 6581)");
    }

    s_msg_ready = xSemaphoreCreateBinary();
    configASSERT(s_msg_ready);

    /* Créé AVANT le worker : ccid_defer() le déréférence dès le premier
     * message, et un ccid_shutdown() très précoce doit déjà pouvoir le
     * prendre. */
    s_defer_lock = xSemaphoreCreateMutex();
    configASSERT(s_defer_lock);

    BaseType_t rc = xTaskCreate(ccid_worker, "ccid", 6144, NULL, 5, &s_worker);
    configASSERT(rc == pdPASS);

    ESP_LOGI(TAG, "CCID class driver registered (worker task running)");
}

/* Combien de temps ccid_shutdown() laisse au worker pour ressortir. Large :
 * dongle_confirm() rend la main en moins de 20 ms une fois s_shutdown posé,
 * et le pire cas réel est une signature ECDSA en cours (quelques dizaines de
 * ms). Au-delà, on journalise et on continue — refuser la bascule laisserait
 * le coffre coincé dans un mode dont le worker ne sort pas. */
#define CCID_SHUTDOWN_WAIT_MS   2000u
#define CCID_SHUTDOWN_POLL_MS     10u

void ccid_set_applet(ccid_applet_fn_t applet)
{
    s_applet = applet;
}

void ccid_shutdown(void)
{
    if (s_defer_lock == NULL) return;   /* ccid_init() jamais appelé */

    /* 1. Fermer la porte. Sous le verrou : un ccid_defer() concurrent est soit
     *    déjà passé (et la file existe encore, on est avant tusb_deinit()),
     *    soit il verra le drapeau. */
    xSemaphoreTake(s_defer_lock, portMAX_DELAY);
    s_shutdown = true;
    xSemaphoreGive(s_defer_lock);

    /* 2. Attendre que le worker ressorte de ccid_process_xfrblock(), et que
     *    plus aucun message ne soit en attente de traitement. On regarde aussi
     *    le compte du sémaphore : un XfrBlock donné par tud_task juste avant
     *    n'a pas encore mis s_worker_active à true.
     *
     *    Reste une fenêtre de quelques instructions entre le take du sémaphore
     *    et la pose de s_worker_active. Elle est SANS conséquence : le verrou
     *    de ccid_defer() garantit déjà qu'aucun callback ne part après ce
     *    point, et le worker ne toucherait alors qu'à de la RAM applicative. */
    uint32_t waited = 0;
    while ((s_worker_active || uxSemaphoreGetCount(s_msg_ready) != 0)
           && waited < CCID_SHUTDOWN_WAIT_MS) {
        vTaskDelay(pdMS_TO_TICKS(CCID_SHUTDOWN_POLL_MS));
        waited += CCID_SHUTDOWN_POLL_MS;
    }
    if (s_worker_active)
        ESP_LOGE(TAG, "worker toujours actif après %u ms — démontage quand même",
                 (unsigned)CCID_SHUTDOWN_WAIT_MS);

    /* 3. L'état de transfert ne survit pas au mode. ccid_drv_deinit() le
     *    refera, mais il ne tourne que si TinyUSB était bien initialisé. */
    s_busy         = false;
    s_final_queued = false;
    ESP_LOGI(TAG, "worker CCID au repos (attente %u ms)", (unsigned)waited);
}

/* ------------------------------------------------------------------ */
/* Host pentest accessors — ONLY compiled for test/fuzz/fuzz_ccid.c.   */
/* Gated behind CCID_HOST_FUZZ; never built on target (the dongle      */
/* CMakeLists defines no such symbol).  These expose the static driver */
/* statics + entry points so the host harness can drive ccid_dispatch  */
/* / ccid_drv_xfer synchronously and let ASan/UBSan instrument the     */
/* static buffers s_out_buf / s_in_buf.                                */
/* ------------------------------------------------------------------ */
#ifdef CCID_HOST_FUZZ
uint8_t  *ccid_host_out_buf(void)  { return s_out_buf; }
uint8_t  *ccid_host_in_buf(void)   { return s_in_buf; }
uint32_t  ccid_host_buf_sz(void)   { return (uint32_t)CCID_BUF_SZ; }
void      ccid_host_set_eps(uint8_t out, uint8_t in) { s_ep_out = out; s_ep_in = in; }
void      ccid_host_reset_state(void) { s_busy = false; s_final_queued = false; }
void      ccid_host_dispatch(uint8_t rhport, uint32_t xferred)
              { ccid_dispatch(rhport, xferred); }
bool      ccid_host_drv_xfer(uint8_t rhport, uint8_t ep_addr,
                             xfer_result_t result, uint32_t xferred_bytes)
              { return ccid_drv_xfer(rhport, ep_addr, result, xferred_bytes); }
/* Called by the host xSemaphoreGive() shim to run the extracted worker body. */
void      ccid_host_run_worker(void) { ccid_process_xfrblock(); }
#endif /* CCID_HOST_FUZZ */
