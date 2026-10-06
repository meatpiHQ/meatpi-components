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
 * @file filesystem_mount.c
 * @brief What runs before the internal LittleFS partition is mounted: the
 *        superblock probe and the quiet format of a partition that is not
 *        ours (blank after an erase, or the factory firmware's filesystem
 *        under our layout, 2026-10-06). Split out of filesystem.c.
 */
#include "esp_attr.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_partition.h"

#include "filesystem_private.h"

#define FS_LFS_BLOCK_SIZE 4096u
#define FS_PROBE_LEN      256u  /* of each superblock block */

static const char *TAG = "filesystem";

/**
 * What the named partition's superblock pair holds (fs_lfs_probe): BLANK
 * after an erase, a LittleFS with its block count, or something else. A
 * missing partition or a read error answers LITTLEFS with the partition's
 * own block count, so the normal mount path runs.
 */
static fs_lfs_kind_t fs_partition_probe(const char *label, uint32_t *blocks,
                                        uint32_t *found)
{
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    static uint8_t probe[2][FS_PROBE_LEN] EXT_RAM_BSS_ATTR; /* boot only */

    *blocks = 0;
    *found = 0;

    if (part == NULL)
    {
        return FS_LFS_LITTLEFS;
    }

    *blocks = part->size / FS_LFS_BLOCK_SIZE;
    *found = *blocks;

    for (uint32_t blk = 0; blk < 2; blk++)
    {
        if (esp_partition_read(part, blk * FS_LFS_BLOCK_SIZE, probe[blk],
                               FS_PROBE_LEN) != ESP_OK)
        {
            return FS_LFS_LITTLEFS;
        }
    }

    return fs_lfs_probe(probe[0], probe[1], FS_PROBE_LEN, FS_LFS_BLOCK_SIZE,
                        found);
}

void fs_mount_prepare(const char *label)
{
    /* Before the mount, read the superblock pair (fs_lfs_probe). First boot
     * after an erase: the partition is all 0xFF, and letting
     * esp_vfs_littlefs_register() discover that makes lfs_mount() log
     * "Corrupted dir pair" at E level before format_if_mount_failed kicks
     * in; those E lines latched a boot_errors fault on every brand-new
     * unit (fresh-unit bench 2026-08-31). A unit flashed over the factory
     * firmware without an erase (2026-10-06) has that firmware's 6 MB
     * LittleFS under our partitions: this one starts in its middle, so the
     * pair holds data blocks of it (or nothing), while the settings
     * partition starts on its superblock. Neither is ours: format quietly
     * and say so once at W. A LittleFS of our own size that fails to mount
     * still takes the loud path: that IS a fault worth seeing. */
    uint32_t blocks;
    uint32_t found;
    fs_lfs_kind_t kind = fs_partition_probe(label,
                                            &blocks, &found);
    bool format_first = true;

    if (kind == FS_LFS_BLANK)
    {
        ESP_LOGI(TAG, "'%s' is blank (first boot): formatting",
                 label);
    }
    else if (kind == FS_LFS_OTHER)
    {
        ESP_LOGW(TAG, "'%s' holds no LittleFS superblock (another "
                 "firmware's data): formatting", label);
    }
    else if (found != blocks)
    {
        ESP_LOGW(TAG, "'%s' holds a LittleFS of %lu blocks on a %lu-block "
                 "partition (another firmware's layout): formatting",
                 label, (unsigned long)found,
                 (unsigned long)blocks);
    }
    else
    {
        format_first = false;
    }

    if (format_first)
    {
        esp_err_t ferr = esp_littlefs_format(label);

        if (ferr != ESP_OK)
        {
            ESP_LOGW(TAG, "pre-format failed: %s (mount will retry)",
                     esp_err_to_name(ferr));
        }
    }

}
