#pragma once
#include <stdint.h>
#include <stddef.h>

/* Tiny log shim: prints to stdout like printf AND records into a ring
 * buffer so the log can be pulled over HTTP via /logs. */
void dlog_init(void);
void dlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int  dlog_dump(char *out, size_t out_size);

/* Drop-in for printf in main.c. Keeps existing lines as-is. */
#define DLOG dlog
