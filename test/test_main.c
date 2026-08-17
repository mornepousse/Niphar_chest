/* Lanceur des tests hôte du coffre Niphar. */
#include "test_framework.h"

int _test_pass_count = 0;
int _test_fail_count = 0;

extern void test_link_proto(void);
extern void test_msc_lba(void);
extern void test_cr_crc16(void);
extern void test_sec_confirm(void);
extern void test_sec_store(void);
extern void test_apdu(void);
extern void test_otp_proto(void);
extern void test_openpgp_do(void);
extern void test_openpgp_card(void);
extern void test_sec_gate(void);
extern void test_usb_mode(void);
extern void test_ccid_zlp(void);
extern void test_usb_mode_cycle(void);
extern void test_button_debounce(void);
extern void test_led_state(void);
extern void test_screen_view(void);
extern void test_screen_anim(void);

int main(void)
{
    printf("=== tests hôte — coffre Niphar ===\n");

    test_link_proto();
    test_cr_crc16();
    test_sec_confirm();
    test_sec_store();
    test_apdu();
    test_otp_proto();
    test_openpgp_do();
    test_openpgp_card();
    test_sec_gate();
    test_msc_lba();
    test_usb_mode();
    test_ccid_zlp();
    test_usb_mode_cycle();
    test_button_debounce();
    test_led_state();
    test_screen_view();
    test_screen_anim();

    printf("\n=================================\n");
    printf("%d assertions OK, %d échecs\n", _test_pass_count, _test_fail_count);
    return _test_fail_count == 0 ? 0 : 1;
}
