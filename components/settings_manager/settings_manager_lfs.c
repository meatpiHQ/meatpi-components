/*
 * This file is part of the MeatPi components project.
 *
 * Copyright (C) 2022-2026 MeatPi Electronics.
 * Written by Ali Slim <ali@meatpi.com>
 *
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file settings_manager_lfs.c
 * @brief The LittleFS superblock probe the settings mount runs first
 *        (2026-10-06). Pure, host-tested. A copy of filesystem's
 *        fs_lfs_probe(): this component sits below filesystem and the two
 *        must not depend on each other for forty lines.
 *
 * A unit that ran the factory firmware keeps that firmware's 6 MB LittleFS
 * at the address of our 256 KB settings partition, and a flash without an
 * erase leaves it there. LittleFS mounts a superblock whatever block count
 * it claims (the ESP port takes the count from the superblock), so the
 * factory filesystem "mounts" on a partition a 24th of its size and every
 * access past the end fails: 83 error lines at boot, a latched
 * boot_errors fault, no setting persisted (bench, 2026-10-06). The
 * superblock entry sits at the start of the pair's blocks: the 8-byte name
 * "littlefs", a 4-byte tag, the inline struct {version, block_size,
 * block_count, name_max, file_max, attr_max} as little-endian u32s (tags
 * are XOR-chained, the data is not).
 */
#include <string.h>

#include "settings_manager_private.h"

#define SM_LFS_MAGIC      "littlefs"
#define SM_LFS_MAGIC_LEN  8u
#define SM_LFS_TAG_LEN    4u
#define SM_LFS_STRUCT_LEN 24u   /* six little-endian u32s */
#define SM_LFS_MAX_BLOCKS (1u << 24)

static bool sm_blank(const uint8_t *buf, size_t len)
{
    for (size_t i = 0; i < len; i++)
    {
        if (buf[i] != 0xFF)
        {
            return false;
        }
    }

    return true;
}

static uint32_t sm_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* the first superblock entry of a block whose struct makes sense */
static bool sm_block_superblock(const uint8_t *blk, size_t len,
                                uint32_t block_size, uint32_t *block_count)
{
    size_t need = SM_LFS_MAGIC_LEN + SM_LFS_TAG_LEN + SM_LFS_STRUCT_LEN;

    for (size_t m = 0; m + need <= len; m++)
    {
        if (memcmp(blk + m, SM_LFS_MAGIC, SM_LFS_MAGIC_LEN) != 0)
        {
            continue;
        }

        const uint8_t *st = blk + m + SM_LFS_MAGIC_LEN + SM_LFS_TAG_LEN;
        uint32_t version = sm_le32(st);
        uint32_t bsize = sm_le32(st + 4);
        uint32_t bcount = sm_le32(st + 8);

        if ((version >> 16) == 2u && bsize == block_size && bcount >= 2u &&
            bcount <= SM_LFS_MAX_BLOCKS)
        {
            *block_count = bcount;
            return true;
        }
    }

    return false;
}

sm_lfs_kind_t sm_lfs_probe(const uint8_t *blk0, const uint8_t *blk1,
                           size_t len, uint32_t block_size,
                           uint32_t *block_count)
{
    if (blk0 == NULL || blk1 == NULL || block_count == NULL ||
        len < SM_LFS_MAGIC_LEN + SM_LFS_TAG_LEN + SM_LFS_STRUCT_LEN)
    {
        return SM_LFS_OTHER;
    }

    if (sm_blank(blk0, len) && sm_blank(blk1, len))
    {
        return SM_LFS_BLANK;
    }

    if (sm_block_superblock(blk0, len, block_size, block_count) ||
        sm_block_superblock(blk1, len, block_size, block_count))
    {
        return SM_LFS_LITTLEFS;
    }

    return SM_LFS_OTHER;
}
