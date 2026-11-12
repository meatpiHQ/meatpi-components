/**
 * @file test_fault_count.c
 * @brief Unit tests for a fault code's count and its cap (2026-11-12): the
 *        count stops at DEV_STATUS_FAULT_COUNT_MAX and the flash writes
 *        stop with it.
 */
#include <string.h>

#include "unity.h"

#include "dev_status_fault.h"
#include "dev_status_manager_private.h"

/* a device's life in boots: one occurrence per boot, the store written
   when the step says so; returns the writes */
static int boots(uint32_t *stored, int n)
{
    int writes = 0;

    for (int b = 0; b < n; b++)
    {
        uint32_t ram = *stored; /* the boot loads the stored count */

        if (dsm_fault_count_step(&ram, ram == 0, false))
        {
            *stored = ram;
            writes++;
        }
    }

    return writes;
}

void test_fault_first_occurrence_is_counted_and_written(void)
{
    uint32_t count = 0;

    TEST_ASSERT_TRUE(dsm_fault_count_step(&count, true, false));
    TEST_ASSERT_EQUAL_UINT32(1, count);
}

void test_fault_recurrence_is_written_once_per_boot(void)
{
    uint32_t count = 1;

    /* the boot's first occurrence of a known code: counted, written */
    TEST_ASSERT_TRUE(dsm_fault_count_step(&count, false, false));
    TEST_ASSERT_EQUAL_UINT32(2, count);

    /* more of it in the same boot: counted in RAM, not written */
    TEST_ASSERT_FALSE(dsm_fault_count_step(&count, false, true));
    TEST_ASSERT_FALSE(dsm_fault_count_step(&count, false, true));
    TEST_ASSERT_EQUAL_UINT32(4, count);
}

void test_fault_count_stops_at_the_cap(void)
{
    uint32_t count = 0;

    for (int i = 0; i < 1000; i++)
    {
        (void)dsm_fault_count_step(&count, i == 0, i > 0);
    }

    TEST_ASSERT_EQUAL_UINT32(DEV_STATUS_FAULT_COUNT_MAX, count);
}

void test_fault_that_recurs_every_boot_stops_writing_at_the_cap(void)
{
    uint32_t stored = 0;

    /* one write per boot up to the cap: the occurrence that reaches it is
       the last one written */
    TEST_ASSERT_EQUAL_INT(DEV_STATUS_FAULT_COUNT_MAX,
                          boots(&stored, DEV_STATUS_FAULT_COUNT_MAX));
    TEST_ASSERT_EQUAL_UINT32(DEV_STATUS_FAULT_COUNT_MAX, stored);

    /* and not one more in a thousand boots behind it */
    TEST_ASSERT_EQUAL_INT(0, boots(&stored, 1000));
    TEST_ASSERT_EQUAL_UINT32(DEV_STATUS_FAULT_COUNT_MAX, stored);
}

void test_fault_one_short_of_the_cap_is_still_written(void)
{
    uint32_t count = DEV_STATUS_FAULT_COUNT_MAX - 1;

    TEST_ASSERT_TRUE(dsm_fault_count_step(&count, false, false));
    TEST_ASSERT_EQUAL_UINT32(DEV_STATUS_FAULT_COUNT_MAX, count);

    /* the next boot's occurrence: full, nothing to write */
    TEST_ASSERT_FALSE(dsm_fault_count_step(&count, false, false));
    TEST_ASSERT_EQUAL_UINT32(DEV_STATUS_FAULT_COUNT_MAX, count);
}

void test_fault_count_of_an_earlier_build_is_kept(void)
{
    /* a record stored before the cap existed: not cut down, not written */
    uint32_t count = 137;

    TEST_ASSERT_FALSE(dsm_fault_count_step(&count, false, false));
    TEST_ASSERT_EQUAL_UINT32(137, count);
}

void test_fault_capped_says_this_many_or_more(void)
{
    dev_status_fault_t f;

    memset(&f, 0, sizeof(f));
    f.count = 1;
    TEST_ASSERT_FALSE(dev_status_manager_fault_capped(&f));
    f.count = DEV_STATUS_FAULT_COUNT_MAX - 1;
    TEST_ASSERT_FALSE(dev_status_manager_fault_capped(&f));
    f.count = DEV_STATUS_FAULT_COUNT_MAX;
    TEST_ASSERT_TRUE(dev_status_manager_fault_capped(&f));
    f.count = 137;
    TEST_ASSERT_TRUE(dev_status_manager_fault_capped(&f));
    TEST_ASSERT_FALSE(dev_status_manager_fault_capped(NULL));
}

void test_fault_record_layout_is_the_stored_one(void)
{
    /* the store is this struct, slot after slot, in one NVS blob: its size
       is the store's format, and a count's cap changes nothing of it */
    TEST_ASSERT_EQUAL_INT(84, (int)sizeof(dev_status_fault_t));
    TEST_ASSERT_TRUE(DEV_STATUS_FAULT_COUNT_MAX >= 2);
}
