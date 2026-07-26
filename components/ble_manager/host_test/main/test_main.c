/** Unity runner for the ble_manager host suite (pure pack/ident layer). */
#include "unity.h"

void setUp(void)
{
}

void tearDown(void)
{
}

extern void run_pack_tests(void);

void app_main(void)
{
    UNITY_BEGIN();
    run_pack_tests();
    UNITY_END();
}
