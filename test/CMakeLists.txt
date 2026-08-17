# Tests hôte du coffre Niphar — compilés par le compilateur de la machine, pas
# par la toolchain ESP-IDF. Seule la logique pure y entre : ce qui touche au
# matériel n'est pas testable ici, et c'est ce qui justifie de l'en séparer.
#
# Lancé par scripts/fast.sh avant le build firmware.
cmake_minimum_required(VERSION 3.16)
project(niphar_tests C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Werror -g -DTEST_HOST")

add_executable(test_runner
    test_main.c
    test_link_proto.c
    test_msc_lba.c
    test_cr_crc16.c
    test_sec_confirm.c
    test_sec_store.c
    test_apdu.c
    test_ctaphid.c
    test_otp_proto.c
    test_openpgp_do.c
    test_openpgp_card.c
    test_sec_gate.c
    test_usb_mode.c
    test_ccid_zlp.c
    test_usb_mode_cycle.c
    test_button_debounce.c
    test_led_state.c
    test_screen_view.c
    test_screen_anim.c
    test_screen_layout.c
    ../main/link/link_proto.c
    ../main/sys/cr_crc16.c
    ../main/security/sec_confirm.c
    ../main/security/sec_store.c
    ../main/security/apdu.c
    ../main/security/ctaphid.c
    ../main/security/otp_proto.c
    ../main/security/openpgp_do.c
    ../main/security/openpgp_card.c
    ../main/usb/msc_lba.c
    ../main/usb/usb_mode_name.c
    nvs_fake.c
)

# `main/` sur le chemin d'inclusion : les tests incluent « link/link_proto.h »
# exactement comme le firmware, sans chemin relatif tordu.
# sys et security à plat : comme dans le firmware, les fichiers portés depuis
# KeSp s'incluent ainsi.
target_include_directories(test_runner PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/sys
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/security
)
