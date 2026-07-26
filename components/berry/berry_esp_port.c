/**
 * @file berry_esp_port.c
 * @brief ESP-IDF glue for the vendored Berry VM: PSRAM heap + a
 *        console-write hook that routes Berry's print() to the log.
 *
 * The VM heap lives in PSRAM (internal RAM is the scarce pool and a
 * script's working set can be tens of KB). berry_conf.h routes
 * BE_EXPLICIT_MALLOC/REALLOC/FREE here.
 */
#include <stddef.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "berry.h"

static const char *TAG = "script_engine";

void *berry_port_malloc(size_t size)
{
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void berry_port_free(void *ptr)
{
    heap_caps_free(ptr);
}

void *berry_port_realloc(void *ptr, size_t size)
{
    return heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/* Berry calls be_writebuffer for print()/output; route it to a line
 * buffer flushed to the log, AND (when a capture sink is set by
 * script_engine around a run) append to that sink for /api/scripts. */
static char s_line[256];
static size_t s_line_len;
static char *s_cap;
static size_t s_cap_size;
static size_t s_cap_len;

void berry_port_set_capture(char *buf, size_t cap)
{
    s_cap = buf;
    s_cap_size = cap;
    s_cap_len = (buf != NULL) ? strnlen(buf, cap) : 0;
}

void berry_port_write(const char *buffer, size_t length)
{
    /* append to the capture sink (if any) verbatim */
    if (s_cap != NULL && s_cap_len + 1 < s_cap_size)
    {
        size_t room = s_cap_size - 1 - s_cap_len;
        size_t take = (length < room) ? length : room;

        memcpy(s_cap + s_cap_len, buffer, take);
        s_cap_len += take;
        s_cap[s_cap_len] = '\0';
    }

    /* mirror to the log, line-buffered */
    for (size_t i = 0; i < length; i++)
    {
        char c = buffer[i];

        if (c == '\n' || s_line_len >= sizeof(s_line) - 1)
        {
            s_line[s_line_len] = '\0';
            ESP_LOGI(TAG, "berry: %s", s_line);
            s_line_len = 0;

            if (c == '\n')
            {
                continue;
            }
        }

        if (c != '\r')
        {
            s_line[s_line_len++] = c;
        }
    }
}
