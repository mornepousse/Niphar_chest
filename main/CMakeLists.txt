set(srcs
    "main.c"
    "storage/sd_card.c"
    "usb/usb_device.c"
    "usb/msc_disk.c"
    "usb/msc_lba.c"
    "console/console.c"
    "security/sec_confirm.c"
    "security/sec_store.c"
    "security/apdu.c"
)

idf_component_register(
    SRCS ${srcs}
    # ${BOARD_DIR} en premier : c'est de là que vient « board.h », sans qu'aucun
    # source n'ait à savoir quelle carte est sélectionnée.
    # "sys" et "security" à plat : les fichiers portés depuis KeSp s'incluent
    # ainsi, et les laisser verbatim vaut mieux que réécrire leurs includes à
    # chaque synchronisation amont.
    INCLUDE_DIRS "${BOARD_DIR}" "." "sys" "security"
    # Volontairement réduit à ce que le code utilise vraiment.
    REQUIRES esp_driver_gpio spi_flash sdmmc esp_driver_sdmmc console
             espressif__tinyusb esp_timer nvs_flash
    PRIV_REQUIRES usb
)

# TinyUSB cherche « tusb_config.h » sur son chemin d'inclusion, et c'est nous
# qui le fournissons (main/tusb_config.h) — voir main/idf_component.yml.
idf_component_get_property(tusb_lib espressif__tinyusb COMPONENT_LIB)
target_include_directories(${tusb_lib} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# Version appliquée au seul composant qui l'utilise : elle change à chaque
# commit, la garder locale évite d'invalider tout le projet à chaque fois.
target_compile_definitions(${COMPONENT_LIB} PRIVATE NIPHAR_VERSION="${NIPHAR_VERSION}")
