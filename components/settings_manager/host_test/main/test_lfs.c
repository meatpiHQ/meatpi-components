/**
 * @file test_lfs.c
 * @brief Unit tests for sm_lfs_probe(): what the settings partition's
 *        superblock pair holds before the mount (2026-10-06).
 */
#include <string.h>

#include "unity.h"

#include "settings_manager_private.h"

/* ---- the superblock probe (2026-10-06) ---------------------------------- */

/* a block as LittleFS writes it: revision, a tag, "littlefs", a tag, the
 * inline struct {version, block_size, block_count, name_max, file_max,
 * attr_max} little-endian; the rest erased */
static void sm_lfs_superblock(uint8_t *blk, size_t len, size_t at,
                                uint32_t version, uint32_t block_size,
                                uint32_t block_count)
{
    memset(blk, 0xFF, len);
    memset(blk, 0x00, at); /* revision + the tag before the name */
    memcpy(blk + at, "littlefs", 8);
    memset(blk + at + 8, 0x5A, 4); /* the struct's tag (XOR-chained) */

    uint32_t v[6] = { version, block_size, block_count, 64, 0x7FFFFFFF, 1022 };

    for (int i = 0; i < 6; i++)
    {
        blk[at + 12 + i * 4 + 0] = (uint8_t)(v[i]);
        blk[at + 12 + i * 4 + 1] = (uint8_t)(v[i] >> 8);
        blk[at + 12 + i * 4 + 2] = (uint8_t)(v[i] >> 16);
        blk[at + 12 + i * 4 + 3] = (uint8_t)(v[i] >> 24);
    }
}

/* a blank pair is a first boot; our own superblock reads our block count;
   the factory firmware's 6 MB filesystem reads 1536 (the report of
   2026-10-06); one erased block of the pair is still a filesystem */
void test_sm_lfs_probe_blank_ours_and_factory(void)
{
    uint8_t b0[256];
    uint8_t b1[256];
    uint32_t n = 0;

    memset(b0, 0xFF, sizeof(b0));
    memset(b1, 0xFF, sizeof(b1));
    TEST_ASSERT_EQUAL_INT(SM_LFS_BLANK, sm_lfs_probe(b0, b1, 256, 4096, &n));

    sm_lfs_superblock(b0, 256, 8, 0x00020001, 4096, 64);
    sm_lfs_superblock(b1, 256, 8, 0x00020001, 4096, 64);
    TEST_ASSERT_EQUAL_INT(SM_LFS_LITTLEFS, sm_lfs_probe(b0, b1, 256, 4096, &n));
    TEST_ASSERT_EQUAL_UINT32(64, n);

    sm_lfs_superblock(b0, 256, 8, 0x00020001, 4096, 1536);
    sm_lfs_superblock(b1, 256, 8, 0x00020001, 4096, 1536);
    TEST_ASSERT_EQUAL_INT(SM_LFS_LITTLEFS, sm_lfs_probe(b0, b1, 256, 4096, &n));
    TEST_ASSERT_EQUAL_UINT32(1536, n);

    memset(b0, 0xFF, sizeof(b0));                  /* block 0 erased */
    TEST_ASSERT_EQUAL_INT(SM_LFS_LITTLEFS, sm_lfs_probe(b0, b1, 256, 4096, &n));
    TEST_ASSERT_EQUAL_UINT32(1536, n);

    sm_lfs_superblock(b0, 256, 40, 0x00020001, 4096, 1472); /* deeper in */
    memset(b1, 0xFF, sizeof(b1));
    TEST_ASSERT_EQUAL_INT(SM_LFS_LITTLEFS, sm_lfs_probe(b0, b1, 256, 4096, &n));
    TEST_ASSERT_EQUAL_UINT32(1472, n);
}

/* what is not a LittleFS of ours: a FAT boot sector, data blocks, a name
   with no sane struct behind it, another block size, a version-1
   filesystem, a struct cut off by the probe's length, bad arguments */
void test_sm_lfs_probe_other_and_bad_input(void)
{
    uint8_t b0[256];
    uint8_t b1[256];
    uint32_t n = 99;

    memset(b0, 0xFF, sizeof(b0));
    memset(b1, 0xFF, sizeof(b1));
    memcpy(b0, "ë<MSDOS5.0", 11);     /* FAT */
    TEST_ASSERT_EQUAL_INT(SM_LFS_OTHER, sm_lfs_probe(b0, b1, 256, 4096, &n));

    for (size_t i = 0; i < sizeof(b0); i++)       /* data blocks */
    {
        b0[i] = (uint8_t)(i * 7 + 3);
        b1[i] = (uint8_t)(i * 13 + 1);
    }
    TEST_ASSERT_EQUAL_INT(SM_LFS_OTHER, sm_lfs_probe(b0, b1, 256, 4096, &n));

    memset(b0, 0xFF, sizeof(b0));
    memcpy(b0 + 8, "littlefs", 8);                /* the name alone */
    memset(b1, 0xFF, sizeof(b1));
    TEST_ASSERT_EQUAL_INT(SM_LFS_OTHER, sm_lfs_probe(b0, b1, 256, 4096, &n));

    sm_lfs_superblock(b0, 256, 8, 0x00020001, 256, 1536); /* block size */
    TEST_ASSERT_EQUAL_INT(SM_LFS_OTHER, sm_lfs_probe(b0, b1, 256, 4096, &n));
    sm_lfs_superblock(b0, 256, 8, 0x00010007, 4096, 1536); /* version 1 */
    TEST_ASSERT_EQUAL_INT(SM_LFS_OTHER, sm_lfs_probe(b0, b1, 256, 4096, &n));
    sm_lfs_superblock(b0, 256, 8, 0x00020001, 4096, 0);    /* no blocks */
    TEST_ASSERT_EQUAL_INT(SM_LFS_OTHER, sm_lfs_probe(b0, b1, 256, 4096, &n));

    sm_lfs_superblock(b0, 256, 8, 0x00020001, 4096, 1536);
    TEST_ASSERT_EQUAL_INT(SM_LFS_OTHER, sm_lfs_probe(b0, b1, 30, 4096, &n)); /* cut */
    TEST_ASSERT_EQUAL_INT(SM_LFS_OTHER, sm_lfs_probe(NULL, b1, 256, 4096, &n));
    TEST_ASSERT_EQUAL_INT(SM_LFS_OTHER, sm_lfs_probe(b0, b1, 256, 4096, NULL));
    TEST_ASSERT_EQUAL_UINT32(99, n);
}
