/** Unity runner for the ota_manager host suite (pure session layer). */
#include "unity.h"

void setUp(void)
{
}

void tearDown(void)
{
}

extern void run_session_tests(void);

void app_main(void)
{
    UNITY_BEGIN();
    run_session_tests();
    UNITY_END();
}
