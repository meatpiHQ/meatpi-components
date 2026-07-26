/** Unity runner for the api_http host suite (pure utility layer). */
#include "unity.h"

void setUp(void)
{
}

void tearDown(void)
{
}

extern void run_util_tests(void);

void app_main(void)
{
    UNITY_BEGIN();
    run_util_tests();
    UNITY_END();
}
