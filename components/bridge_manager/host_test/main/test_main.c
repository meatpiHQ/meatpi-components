/** Unity runner for the bridge_manager host suite. */
#include "unity.h"

void setUp(void)
{
}

void tearDown(void)
{
}

extern void run_config_tests(void);
extern void run_contract_tests(void);

void app_main(void)
{
    UNITY_BEGIN();
    run_config_tests();
    run_contract_tests();
    UNITY_END();
}
