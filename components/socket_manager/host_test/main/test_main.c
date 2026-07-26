/** Unity runner for the socket_manager host suite (pure policy layer). */
#include "unity.h"

void setUp(void)
{
}

void tearDown(void)
{
}

extern void run_policy_tests(void);

void app_main(void)
{
    UNITY_BEGIN();
    run_policy_tests();
    UNITY_END();
}
