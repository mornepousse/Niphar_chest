set(srcs
    "main.c"
)

idf_component_register(
    SRCS ${srcs}
    INCLUDE_DIRS "."
    # Volontairement réduit à ce que le code utilise vraiment. Les phases
    # suivantes ajouteront sdmmc / console / tinyusb quand elles les utiliseront.
    REQUIRES esp_driver_gpio spi_flash
)

# Version appliquée au seul composant qui l'utilise : elle change à chaque
# commit, la garder locale évite d'invalider tout le projet à chaque fois.
target_compile_definitions(${COMPONENT_LIB} PRIVATE NIPHAR_VERSION="${NIPHAR_VERSION}")
