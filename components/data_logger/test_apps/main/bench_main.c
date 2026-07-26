/**
 * @file bench_main.c
 * @brief data_logger storage benchmark (TASK_data_logger.md §3): sqlite
 *        write-rate matrix on the REAL SD card — FATFS vs littlefs —
 *        plus a raw-binary append ceiling. Prints one line per leg:
 *          BENCH <leg> rows=<n> ms=<t> rows_per_s=<r> file_kb=<s>
 *        and "BENCH DONE" at the end (serial capture asserts on it).
 *
 * Row shape = the legacy param_data row: (int64 ts, int param_id,
 * double value). The littlefs phase REFORMATS the bench card and
 * restores FATFS afterwards (explicit meatpi ask, bench card only).
 */
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_err.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"

#include "sqlite3.h"

static const char *TAG = "dl_bench";

/* WiCAN Pro SD pins (single source: external_storage.c) */
#define SD_CLK 21
#define SD_CMD 47
#define SD_D0  14
#define SD_D1  13
#define SD_D2  12
#define SD_D3  48

#define MNT "/sd"
#define ROWS 4000

static sdmmc_card_t *s_card;

/* ---- card bring-up -------------------------------------------------------------- */

static esp_err_t mount_fat(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();

    slot.clk = SD_CLK;
    slot.cmd = SD_CMD;
    slot.d0 = SD_D0;
    slot.d1 = SD_D1;
    slot.d2 = SD_D2;
    slot.d3 = SD_D3;
    slot.width = 4;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    esp_vfs_fat_sdmmc_mount_config_t cfg =
    {
        .format_if_mount_failed = true,   /* bench card: reformat OK */
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };

    return esp_vfs_fat_sdmmc_mount(MNT, &host, &slot, &cfg, &s_card);
}

static void unmount_fat(void)
{
    esp_vfs_fat_sdcard_unmount(MNT, s_card);
    s_card = NULL;
}

/* ---- helpers ---------------------------------------------------------------------- */

static int64_t file_kb(const char *path)
{
    struct stat st;

    return (stat(path, &st) == 0) ? st.st_size / 1024 : -1;
}

static void report(const char *leg, int rows, int64_t us, const char *db)
{
    double s = (double)us / 1e6;

    printf("BENCH %-18s rows=%d ms=%lld rows_per_s=%.0f file_kb=%lld\n",
           leg, rows, (long long)(us / 1000), rows / s,
           (long long)(db ? file_kb(db) : 0));
}

static sqlite3 *open_db(const char *path, const char *pragmas)
{
    sqlite3 *db = NULL;

    unlink(path);

    char journal[64];

    snprintf(journal, sizeof(journal), "%s-journal", path);
    unlink(journal);

    if (sqlite3_open(path, &db) != SQLITE_OK)
    {
        ESP_LOGE(TAG, "open %s failed", path);
        return NULL;
    }

    if (pragmas != NULL)
    {
        sqlite3_exec(db, pragmas, NULL, NULL, NULL);
    }

    sqlite3_exec(db,
                 "CREATE TABLE param_data (timestamp INTEGER, "
                 "param_id INTEGER, value REAL);",
                 NULL, NULL, NULL);
    return db;
}

/* Insert @p rows with @p batch rows per transaction. */
static int64_t insert_rows(sqlite3 *db, int rows, int batch,
                           bool churn_pragmas)
{
    sqlite3_stmt *stmt = NULL;

    sqlite3_prepare_v2(db,
                       "INSERT INTO param_data VALUES (?, ?, ?);", -1,
                       &stmt, NULL);

    int64_t t0 = esp_timer_get_time();

    for (int done = 0; done < rows;)
    {
        if (churn_pragmas)
        {
            /* what legacy did around EVERY store call */
            sqlite3_exec(db, "PRAGMA synchronous = OFF;", NULL, NULL, NULL);
            sqlite3_exec(db, "PRAGMA journal_mode = MEMORY;", NULL, NULL,
                         NULL);
        }

        sqlite3_exec(db, "BEGIN;", NULL, NULL, NULL);

        int n = (rows - done < batch) ? rows - done : batch;

        for (int i = 0; i < n; i++, done++)
        {
            sqlite3_bind_int64(stmt, 1, 1783000000LL + done);
            sqlite3_bind_int(stmt, 2, done % 24);
            sqlite3_bind_double(stmt, 3, done * 0.25);
            sqlite3_step(stmt);
            sqlite3_reset(stmt);
        }

        sqlite3_exec(db, "COMMIT;", NULL, NULL, NULL);

        if (churn_pragmas)
        {
            /* the "restore" half (WAL is compiled out — fails silently,
               exactly like legacy) */
            sqlite3_exec(db, "PRAGMA synchronous = NORMAL;", NULL, NULL,
                         NULL);
            sqlite3_exec(db, "PRAGMA journal_mode = WAL;", NULL, NULL,
                         NULL);
        }
    }

    int64_t us = esp_timer_get_time() - t0;

    sqlite3_finalize(stmt);
    return us;
}

static void sql_matrix(const char *fs_tag)
{
    char path[64], leg[48];
    sqlite3 *db;

    /* 1. what legacy actually did: 10-row stores with pragma churn */
    snprintf(path, sizeof(path), MNT "/bench1.db");
    db = open_db(path, NULL);
    snprintf(leg, sizeof(leg), "%s_legacy10", fs_tag);
    report(leg, ROWS / 4, insert_rows(db, ROWS / 4, 10, true), path);
    sqlite3_close(db);
    unlink(path);

    /* 2. naive: default journal (DELETE), one row per transaction */
    snprintf(path, sizeof(path), MNT "/bench2.db");
    db = open_db(path, NULL);
    snprintf(leg, sizeof(leg), "%s_naive_b1", fs_tag);
    report(leg, ROWS / 8, insert_rows(db, ROWS / 8, 1, false), path);
    sqlite3_close(db);
    unlink(path);

    /* 3-4. tuned: pragmas ONCE (memory journal), batch 64 / 256 */
    static const char *TUNED =
        "PRAGMA journal_mode = MEMORY; PRAGMA temp_store = 2;";

    snprintf(path, sizeof(path), MNT "/bench3.db");
    db = open_db(path, TUNED);
    snprintf(leg, sizeof(leg), "%s_mem_b64", fs_tag);
    report(leg, ROWS, insert_rows(db, ROWS, 64, false), path);
    sqlite3_close(db);
    unlink(path);

    snprintf(path, sizeof(path), MNT "/bench4.db");
    db = open_db(path, TUNED);
    snprintf(leg, sizeof(leg), "%s_mem_b256", fs_tag);
    report(leg, ROWS, insert_rows(db, ROWS, 256, false), path);
    sqlite3_close(db);
    unlink(path);

    /* 5. journal OFF variant */
    snprintf(path, sizeof(path), MNT "/bench5.db");
    db = open_db(path, "PRAGMA journal_mode = OFF; PRAGMA temp_store = 2;");
    snprintf(leg, sizeof(leg), "%s_off_b256", fs_tag);
    report(leg, ROWS, insert_rows(db, ROWS, 256, false), path);
    sqlite3_close(db);
    unlink(path);

    /* 6. 4 KB pages (set before table creation) + query probe */
    snprintf(path, sizeof(path), MNT "/bench6.db");
    db = open_db(path,
                 "PRAGMA page_size = 4096; PRAGMA journal_mode = MEMORY; "
                 "PRAGMA temp_store = 2;");
    snprintf(leg, sizeof(leg), "%s_pg4k_b256", fs_tag);
    report(leg, ROWS, insert_rows(db, ROWS, 256, false), path);

    int64_t t0 = esp_timer_get_time();
    sqlite3_stmt *q;

    sqlite3_prepare_v2(db,
                       "SELECT COUNT(*), AVG(value) FROM param_data "
                       "WHERE timestamp > 1783001000;", -1, &q, NULL);
    sqlite3_step(q);

    int cnt = sqlite3_column_int(q, 0);

    sqlite3_finalize(q);
    printf("BENCH %s_query rows=%d ms=%lld rows_per_s=0 file_kb=0\n",
           fs_tag, cnt,
           (long long)((esp_timer_get_time() - t0) / 1000));
    sqlite3_close(db);
    unlink(path);

    /* 7. raw binary append (the ceiling): 16-byte packed records */
    snprintf(path, sizeof(path), MNT "/bench.raw");
    unlink(path);

    FILE *f = fopen(path, "wb");

    if (f != NULL)
    {
        struct __attribute__((packed))
        {
            int64_t ts;
            int32_t id;
            float   value;
        } rec;
        static uint8_t buf[256 * 16];
        size_t w = 0;

        t0 = esp_timer_get_time();

        for (int i = 0; i < ROWS; i++)
        {
            rec.ts = 1783000000LL + i;
            rec.id = i % 24;
            rec.value = i * 0.25f;
            memcpy(buf + w, &rec, sizeof(rec));
            w += sizeof(rec);

            if (w == sizeof(buf))
            {
                fwrite(buf, 1, w, f);
                w = 0;
            }
        }

        if (w > 0)
        {
            fwrite(buf, 1, w, f);
        }

        fflush(f);
        fsync(fileno(f));
        fclose(f);
        snprintf(leg, sizeof(leg), "%s_raw_b256", fs_tag);
        report(leg, ROWS, esp_timer_get_time() - t0, path);
        unlink(path);
    }
}

/* ---- littlefs on the SD card -------------------------------------------------------- */

/* Manual host+card bring-up (the FAT convenience wrapper owns and
   tears down its card handle, so the phases can't share one). */
static sdmmc_card_t s_raw_card;

static bool raw_card_init(void)
{
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();

    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();

    slot.clk = SD_CLK;
    slot.cmd = SD_CMD;
    slot.d0 = SD_D0;
    slot.d1 = SD_D1;
    slot.d2 = SD_D2;
    slot.d3 = SD_D3;
    slot.width = 4;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    if (sdmmc_host_init() != ESP_OK ||
        sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot) != ESP_OK)
    {
        return false;
    }

    memset(&s_raw_card, 0, sizeof(s_raw_card));

    if (sdmmc_card_init(&host, &s_raw_card) != ESP_OK)
    {
        ESP_LOGE(TAG, "raw card init failed");
        sdmmc_host_deinit();
        return false;
    }

    return true;
}

static bool mount_littlefs_sd(void)
{
    esp_vfs_littlefs_conf_t cfg =
    {
        .base_path = MNT,
        .sdcard = &s_raw_card,
        .format_if_mount_failed = true,   /* bench card: reformat OK */
    };

    esp_err_t err = esp_vfs_littlefs_register(&cfg);

    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "littlefs mount failed: %s", esp_err_to_name(err));
        return false;
    }

    return true;
}

void app_main(void)
{
    printf("BENCH START rows=%d\n", ROWS);

    if (mount_fat() != ESP_OK)
    {
        printf("BENCH FAIL no card\n");
        return;
    }

    sqlite3_initialize();

    printf("BENCH phase FATFS\n");
    sql_matrix("fat");
    unmount_fat();                        /* full host teardown        */

    /* ---- littlefs phase (reformats the bench card) --------------------- */
    printf("BENCH phase LITTLEFS (reformatting bench card)\n");

    bool lfs_ok = raw_card_init() && mount_littlefs_sd();

    if (lfs_ok)
    {
        sql_matrix("lfs");
        esp_vfs_littlefs_unregister_sdmmc(&s_raw_card);
    }

    sdmmc_host_deinit();

    /* ---- restore FATFS: mount-fails -> auto-format back to FAT --------- */
    printf("BENCH phase RESTORE-FAT\n");

    esp_err_t err = mount_fat();          /* format_if_mount_failed=true */

    if (err == ESP_OK)
    {
        FILE *probe = fopen(MNT "/bench_ok.txt", "w");

        if (probe != NULL)
        {
            fputs("data_logger bench restore marker\n", probe);
            fclose(probe);
        }

        unmount_fat();
    }

    printf("BENCH restore_fat %s (littlefs_phase %s)\n",
           err == ESP_OK ? "OK" : "FAILED", lfs_ok ? "ran" : "SKIPPED");
    printf("BENCH DONE\n");
}
