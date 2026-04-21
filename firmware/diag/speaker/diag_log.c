/*
 * Ring-buffered log so /logs can return the sweep output when running
 * on battery with no USB.
 */

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "diag_log.h"

#define RING_SIZE 16384

static char s_ring[RING_SIZE];
static size_t s_write = 0;
static size_t s_used = 0;
static SemaphoreHandle_t s_lock = NULL;

void dlog_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
}

static void ring_write(const char *data, size_t n)
{
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (size_t i = 0; i < n; i++) {
        s_ring[s_write] = data[i];
        s_write = (s_write + 1) % RING_SIZE;
        if (s_used < RING_SIZE) s_used++;
    }
    xSemaphoreGive(s_lock);
}

void dlog(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    /* Serial (USB) passthrough. */
    fputs(buf, stdout);
    fflush(stdout);
    /* HTTP-accessible ring. */
    ring_write(buf, (size_t)n);
}

int dlog_dump(char *out, size_t out_size)
{
    if (!s_lock || out_size == 0) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t n_copy = s_used > out_size ? out_size : s_used;
    if (s_used < RING_SIZE) {
        memcpy(out, s_ring, n_copy);
    } else {
        size_t first = RING_SIZE - s_write;
        size_t to_copy_first = first > n_copy ? n_copy : first;
        memcpy(out, s_ring + s_write, to_copy_first);
        if (n_copy > to_copy_first) {
            memcpy(out + to_copy_first, s_ring, n_copy - to_copy_first);
        }
    }
    int result = (int)n_copy;
    xSemaphoreGive(s_lock);
    return result;
}
