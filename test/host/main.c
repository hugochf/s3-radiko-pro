/* Host-side unit-test runner (Phase 21). Build & run: make -C test/host */
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

void run_hls_parse_tests(void);
void run_radiko_parse_tests(void);
void run_ota_parse_tests(void);

int main(void)
{
    UNITY_BEGIN();
    run_hls_parse_tests();
    run_radiko_parse_tests();
    run_ota_parse_tests();
    return UNITY_END();
}
