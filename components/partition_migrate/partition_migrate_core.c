/**
 * @file partition_migrate_core.c
 * @brief Partition table parse and compare: pure, host-tested.
 */
#include "partition_migrate_core.h"

#include <stdio.h>
#include <string.h>

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

size_t pm_table_parse(const uint8_t *buf, size_t len, pm_entry_t *out,
                      size_t max_entries)
{
    size_t n = 0;

    if (buf == NULL || out == NULL)
    {
        return 0;
    }

    for (size_t off = 0; off + PM_ENTRY_LEN <= len && off < PM_TABLE_MAX_LEN;
         off += PM_ENTRY_LEN)
    {
        const uint8_t *e = buf + off;
        uint16_t magic = rd16(e);

        if (magic == PM_MAGIC_MD5 || magic == 0xFFFFU)
        {
            break;              /* the MD5 entry or the blank terminator */
        }

        if (magic != PM_MAGIC || n >= max_entries || rd32(e + 8) == 0)
        {
            return 0;
        }

        out[n].type    = e[2];
        out[n].subtype = e[3];
        out[n].offset  = rd32(e + 4);
        out[n].size    = rd32(e + 8);
        memcpy(out[n].label, e + 12, 16);
        out[n].label[16] = '\0';
        out[n].flags   = rd32(e + 28);
        n++;
    }

    return n;
}

bool pm_entry_is_anchored(const pm_entry_t *e)
{
    if (e->type == PM_TYPE_APP)
    {
        return true;
    }

    return e->type == PM_TYPE_DATA &&
           (e->subtype == PM_DATA_OTA || e->subtype == PM_DATA_PHY ||
            e->subtype == PM_DATA_NVS);
}

static bool same_entry(const pm_entry_t *a, const pm_entry_t *b)
{
    return a->type == b->type && a->subtype == b->subtype &&
           a->offset == b->offset && a->size == b->size &&
           a->flags == b->flags && strcmp(a->label, b->label) == 0;
}

static bool has_entry(const pm_entry_t *list, size_t n, const pm_entry_t *e)
{
    for (size_t i = 0; i < n; i++)
    {
        if (same_entry(&list[i], e))
        {
            return true;
        }
    }

    return false;
}

static void say(char *why, size_t why_len, const char *text)
{
    if (why != NULL && why_len > 0)
    {
        snprintf(why, why_len, "%s", text);
    }
}

/* "label SIZE KB at 0xOFFSET" appended to why, comma-separated */
static void describe(char *why, size_t why_len, const pm_entry_t *e,
                     bool first)
{
    size_t used;

    if (why == NULL || why_len == 0)
    {
        return;
    }

    used = strlen(why);

    if (used >= why_len)
    {
        return;
    }

    snprintf(why + used, why_len - used, "%s%s %lu KB at 0x%lX",
             first ? "" : ", ", e->label,
             (unsigned long)(e->size / 1024U), (unsigned long)e->offset);
}

pm_verdict_t pm_table_compare(const uint8_t *flash, size_t flash_len,
                              const uint8_t *ours, size_t ours_len,
                              char *why, size_t why_len)
{
    pm_entry_t f[PM_MAX_ENTRIES];
    pm_entry_t o[PM_MAX_ENTRIES];
    size_t nf;
    size_t no;
    bool same;
    bool first;

    no = pm_table_parse(ours, ours_len, o, PM_MAX_ENTRIES);

    if (no == 0)
    {
        say(why, why_len, "this build's own table does not parse");
        return PM_TABLE_FOREIGN;
    }

    nf = pm_table_parse(flash, flash_len, f, PM_MAX_ENTRIES);

    if (nf == 0)
    {
        say(why, why_len, "no partition table in flash");
        return PM_TABLE_FOREIGN;
    }

    same = nf == no;

    for (size_t i = 0; same && i < nf; i++)
    {
        same = same_entry(&f[i], &o[i]);
    }

    if (same)
    {
        say(why, why_len, "this build's table");
        return PM_TABLE_SAME;
    }

    /* the anchors must match both ways */
    for (size_t i = 0; i < nf; i++)
    {
        if (pm_entry_is_anchored(&f[i]) && !has_entry(o, no, &f[i]))
        {
            if (why != NULL && why_len > 0)
            {
                snprintf(why, why_len,
                         "%s in flash has no match in this build", f[i].label);
            }

            return PM_TABLE_FOREIGN;
        }
    }

    for (size_t i = 0; i < no; i++)
    {
        if (pm_entry_is_anchored(&o[i]) && !has_entry(f, nf, &o[i]))
        {
            if (why != NULL && why_len > 0)
            {
                snprintf(why, why_len,
                         "%s of this build has no match in flash", o[i].label);
            }

            return PM_TABLE_FOREIGN;
        }
    }

    /* only data partitions differ: name them, flash first, then ours */
    say(why, why_len, "");
    first = true;

    for (size_t i = 0; i < nf; i++)
    {
        if (!has_entry(o, no, &f[i]))
        {
            describe(why, why_len, &f[i], first);
            first = false;
        }
    }

    if (why != NULL && why_len > 0)
    {
        size_t used = strlen(why);

        if (used < why_len)
        {
            snprintf(why + used, why_len - used, " -> ");
        }
    }

    first = true;

    for (size_t i = 0; i < no; i++)
    {
        if (!has_entry(f, nf, &o[i]))
        {
            describe(why, why_len, &o[i], first);
            first = false;
        }
    }

    return PM_TABLE_MIGRATE;
}
