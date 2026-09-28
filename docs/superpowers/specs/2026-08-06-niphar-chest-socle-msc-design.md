# Niphar chest — firmware foundation and raw MSC

Design validated on 2026-08-06. First increment of the chest firmware.

## 1. Problem

The chest hardware is designed, reviewed and has gone to manufacturing; the
repo contains only `README.md` and `docs/HARDWARE.md`. There is not a single
line of firmware, no build, no tooling.

The chest targets three successive uses — multi-ISO USB stick, PGP/FIDO token,
removable storage — but none is reachable until the two hardware paths that
carry them all are proven: access to the microSD, and enumeration as a
high-speed USB device.

The only hardware available is the **JC-ESP32P4-M3-DEV** kit.

## 2. What the kit proves, and what it does not

The kit is a valid stand-in for this increment. Its microSD is wired
identically to the chest's:

| Signal | Chest (`docs/HARDWARE.md`) | Kit (JCZN BSP) |
|---|---|---|
| CLK | 43 | `BSP_SD_CLK` = 43 |
| CMD | 44 | `BSP_SD_CMD` = 44 |
| D0–D3 | 39, 40, 41, 42 | `BSP_SD_D0..D3` = 39, 40, 41, 42 |

Source: `1-Demo/IDF-DEMO/NoDisplay/common_components/espressif__esp32_p4_function_ev_board/include/bsp/esp32_p4_function_ev_board.h:71-76`,
and the associated `.c` which brings it up in `SDMMC_HOST_SLOT_0`,
`SDMMC_FREQ_HIGHSPEED`, `SDMMC_SLOT_NO_CD` / `SDMMC_SLOT_NO_WP`. Consistent
with the silicon: “card one (SDMMC_HOST_SLOT_0) signals are multiplexed with
GPIO39–GPIO48 … via IO MUX”, *ESP32-P4 Series Datasheet v0.7*, p. 81.

The HS USB device path exists on both: `SOC_USB_OTG_PERIPH_NUM 2`,
`SOC_USB_UTMI_PHY_NUM 1` (`components/soc/esp32p4/include/soc/soc_caps.h:485-489`,
ESP-IDF v5.5.2).

**What the kit does not prove.** It has a CH340C and a BOOTMODE button; the
chest has neither a reset button nor hardware access to download mode. On the
kit, firmware that breaks the USB-Serial-JTAG is repaired in three seconds; on
the chest, it is repaired with a soldering iron. That asymmetry cannot be
tested — it holds by construction (§6).

## 3. Scope

In scope: ESP-IDF project, sector access to the microSD, MSC enumeration
exposing the card raw, debug console, anti-regression tooling.

Out of scope: multi-ISO, PGP/FIDO, P4↔C6 link, OTA. Each will get its own
spec.

## 4. Architecture

Structuring principle: **MSC owns the SD card, exclusively.** As long as the
firmware exposes raw blocks to the host, it does not mount FATFS on its own
side. Two filesystems writing the same medium without coordinating corrupt the
medium — that is not a risk, it is a certainty. `sd_card` serves sectors,
nothing more.

```
main/
├── board.h              pinout + compile-time guardrails
├── main.c               app_main: sd → usb → console
├── storage/sd_card.*    SDMMC slot 0, 4-bit, sector access
├── usb/usb_device.*     esp_tinyusb, HS port, descriptors
├── usb/msc_disk.*       tud_msc_* callbacks → SD sectors
└── console/console.*    esp_console over USB-Serial-JTAG
```

### Boundaries

**`sd_card`** — holds the `sdmmc_card_t*`. Knows nothing of USB or FATFS.

```c
esp_err_t sd_probe(void);                 /* (re)détecte la carte */
bool      sd_present(void);
uint32_t  sd_sector_count(void);
uint32_t  sd_sector_size(void);
esp_err_t sd_read_sectors (void *dst, uint32_t start, uint32_t count);
esp_err_t sd_write_sectors(const void *src, uint32_t start, uint32_t count);
```

No hardware card-detect on the chest: `sd_probe()` is called at boot and can be
re-triggered from the console. The usage rule that was settled — card inserted
and removed with the power off — makes any polling task pointless at this
stage.

**`msc_disk`** — implements the TinyUSB callbacks (`inquiry`,
`test_unit_ready`, `capacity`, `read10`, `write10`, `start_stop`). Knows only
the interface above. Reports *no medium* when `sd_present()` is false, rather
than failing loudly.

**`usb_device`** — esp_tinyusb init on the HS port, descriptors, VID/PID. Knows
nothing about the SD card.

**`console`** — `esp_console` REPL over the USB-Serial-JTAG only. Commands
`sd info`, `sd probe`, `usb status`.

### Data flow

```
USB host ──HS──> TinyUSB ──> msc_disk ──> sd_card ──SDMMC 4-bit──> microSD
USB host ──FS──> USB-Serial-JTAG ──> console ──> sd_card (read-only)
```

The two USB paths are physically distinct (separate controllers, separate hub
ports on the chest). Their independence is a property to be verified, not
assumed: see §7.

### Two known traps

1. **DMA buffers.** `sdmmc_read_sectors()` requires DMA-capable memory; the
   buffers TinyUSB hands to the callbacks are not necessarily so, and
   `read10_cb` can arrive at an offset that is not sector-aligned. `msc_disk`
   goes through a `MALLOC_CAP_DMA` bounce buffer and handles partial offsets
   and lengths.
2. **VID/PID.** The PID must differ from `KeSp_firmware`'s so as not to confuse
   udev rules and host-side clients.

## 5. Error handling

- No card at boot: warning log, `sd_present()` false, the USB device enumerates
  anyway and answers *medium not present*. The chest stays flashable and
  queryable — never a panic at startup over a missing card.
- Sector read or write error: reported as a SCSI error to the host, logged at
  error level. No silent retry that would mask a dying card.
- USB init failure: error log, the console stays alive. The console is the last
  diagnostic resort; nothing must be able to take it down.

## 6. Chest guardrails

The chest has neither a reset button nor a hardware download mode. Two rules
follow, and neither can be verified at runtime on the kit:

- **Never reassign GPIO24/25** (USB-Serial-JTAG). `board.h` carries one
  `_Static_assert` per declared pin; `scripts/check.sh` fails if the code
  reassigns those GPIOs.
- **Never enter permanent deep-sleep.** `scripts/check.sh` fails on the
  appearance of `esp_deep_sleep_start`.

The console is pinned to the USB-Serial-JTAG in `sdkconfig.defaults`: the chest
has no accessible UART.

## 7. Verification

On the kit, console over USB-Serial-JTAG:

- `sd info` → capacity consistent with the card, 4-bit bus, negotiated
  frequency.
- `sd probe` with no card → absence reported cleanly, no panic.

On the host, cable plugged into the HS port:

- `lsblk` shows a disk of the right size.
- Sector 0 readable (`dd` + `xxd`), MBR signature if the card is partitioned.
- Read-only mount, reading a known file, unmount.
- Write: copy a file, `sync`, unmount, remount, re-read — identical content.
- **Coexistence**: while the disk is mounted on the host, the console still
  answers. That test, and only that test, proves the two USB paths are
  independent.

Verification debt accepted, to be cleared when the chest arrives: recovery
behaviour without a button, and the P4↔C6 link.

### Bring-up results (2026-08-06, JC-ESP32P4-M3-DEV kit, SE04G 4 GB card)

Everything is green on the functional side: `INQUIRY` and `READ CAPACITY`
correct as seen from the kernel (`Direct-Access Niphar Coffre microSD`,
3.64 GiB), partitioning and formatting from the host, 64 MiB written then
re-read **after unmounting** with identical SHA-256 digests. The USB → MSC →
SDMMC path is proven in both directions.

The **VDDPST_5** question is settled on the kit: the card is detected through
the *external supply* path, without the internal LDO — consistent with the
chest's hypothesis. It remains to be confirmed on the netlist that the chest
does tie that pin to 3.3 V; the kit only proves the kit.

Throughput:

| | before | after | raw SDMMC |
|---|---|---|---|
| read | 5.8 MiB/s | **9.4 MiB/s** | 18.3 MiB/s |
| write | 2.4 MiB/s | **5.3 MiB/s** | not measured |

The factor was `CFG_TUD_MSC_EP_BUFSIZE`: TinyUSB was asking the card for 4 KiB
blocks, raised to 32 KiB. The initial hypothesis — a bounce buffer that would
dominate — was wrong, and `msc_disk`'s counters showed it: 0 bounced sectors
out of 4650, direct DMA path 100 % of the time.

A factor of two remains on reads (9.4 against 18.3 MiB/s). The cause is
structural: each block is handled **synchronously**, USB waits for SDMMC with
no overlap. Closing that gap requires double buffering — a real rework of
`msc_disk`, to be decided as a separate increment.

For writes, 5.3 MiB/s is plausible for an entry-level SE04G; it is not
demonstrated, for lack of a raw SDMMC write measurement (it would destroy the
card's content). To be revisited on an expendable card.

## 8. Decisions and rejected alternatives

| Decision | Rejected alternative | Reason |
|---|---|---|
| Single build target | two boards `boards/<name>/` | zero pinout delta between kit and chest; two twin `board.h` to keep in sync protect against nothing |
| Hand-written `tud_msc_*` callbacks | `tinyusb_msc_storage` component | the component is built around “one SD = one volume”; switchable-media multi-ISO and the composite CCID+HID require control over the callbacks. Writing the right layer straight away avoids throwing it away |
| Dependency on raw `espressif/tinyusb` | `espressif/esp_tinyusb` wrapper | discovered during implementation, see §9 |

## 9. Correction — why raw TinyUSB

The “hand-written callbacks” decision had been costed assuming we would keep
`espressif/esp_tinyusb` for the PHY, the task and the descriptors. That is
false: the wrapper does not separate the MSC class from its own storage layer.

- `CFG_TUD_MSC` derives from `CONFIG_TINYUSB_MSC_ENABLED`, and that same
  Kconfig decides whether to compile `tinyusb_msc.c`, which defines
  `tud_msc_read10_cb` and friends as **strong symbols** — a direct collision
  with ours.
- Working around it by injecting macros at compile time works right up to link
  time: `tinyusb.c` calls `msc_storage_mount_to_usb()` hard-coded from
  `tud_mount_cb()`. Going further would mean forging the component's internal
  symbols — a price this project must not pay.

The project therefore depends directly on `espressif/tinyusb`, with its own
`main/tusb_config.h` and, in `main/usb/usb_device.c`, the UTMI PHY
initialisation (`usb_new_phy`), a `tud_task()` task and the descriptor
callbacks. Since the chest is bus-powered, all of the wrapper's VBUS monitoring
is moot — which markedly reduces what had to be reimplemented.

Real cost: about 120 lines of plumbing, in exchange for total control of the
callbacks and one dependency fewer.
| MSC exclusive owner of the SD | simultaneous FATFS mount on the firmware side | two concurrent accesses to the same medium = corruption |
