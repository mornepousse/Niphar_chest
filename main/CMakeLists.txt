set(srcs
    "main.c"
    "storage/sd_card.c"
    "console/console.c"
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    # Volontairement réduit à ce que le code utilise vraiment. La phase
    # suivante ajoutera esp_tinyusb quand le MSC arrivera.
    REQUIRES esp_driver_gpio spi_flash sdmmc esp_driver_sdmmc console
)

# Version appliquée au seul composant qui l'utilise : elle change à chaque
# commit, la garder locale évite d'invalider tout le projet à chaque fois.
target_compile_definitions(${COMPONENT_LIB} PRIVATE NIPHAR_VERSION="${NIPHAR_VERSION}")
