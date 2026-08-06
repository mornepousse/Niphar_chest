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
