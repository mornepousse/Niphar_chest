/* Lanceur des tests hôte du coffre Niphar. */
#include "test_framework.h"

int _test_pass_count = 0;
int _test_fail_count = 0;

extern void test_link_proto(void);
extern void test_msc_lba(void);
extern void test_cr_crc16(void);
extern void test_sec_confirm(void);

int main(void)
{
    printf("=== tests hôte — coffre Niphar ===\n");

    test_link_proto();
    test_cr_crc16();
    test_sec_confirm();
    test_msc_lba();

    printf("\n=================================\n");
    printf("%d assertions OK, %d échecs\n", _test_pass_count, _test_fail_count);
    return _test_fail_count == 0 ? 0 : 1;
}
