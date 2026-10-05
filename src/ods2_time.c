/*
 * MIT License
 *
 * Copyright (c) 2026 Allen Pomeroy
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/* localtime_r(), setenv(), unsetenv() and tzset() are POSIX, not C11;
   the build uses -std=c11, so ask for them explicitly. */
#define _POSIX_C_SOURCE 200809L

#include "ods2_time.h"
#include <stdlib.h>
#include <string.h>

/* Days from 1970-01-01 to the proleptic Gregorian date y-m-d (m 1-12).
   Howard Hinnant's days_from_civil: exact for any year, no tables,
   no dependence on the C library's own time zone handling. */
static int64_t days_from_civil(int64_t y, int m, int d)
{
    int64_t era, yoe, doy, doe;

    y -= m <= 2;
    era = (y >= 0 ? y : y - 399) / 400;
    yoe = y - era * 400;                                        /* [0, 399] */
    doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;       /* [0, 365] */
    doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;                /* [0, 146096] */
    return era * 146097 + doe - 719468;
}

/* 17-NOV-1858 (the VMS base date) is 40587 days before 1970-01-01:
   40587 * 86400 = 3,506,716,800 seconds, the constant the old code
   added to UTC seconds. */
#define VMS_EPOCH_DAYS_BEFORE_UNIX 40587
#define VMS_TICKS_PER_SECOND       10000000ULL

uint64_t ods2_vms_time_from_tm(const struct tm *wall)
{
    int64_t days, secs;

    days = days_from_civil((int64_t) wall->tm_year + 1900, wall->tm_mon + 1,
                           wall->tm_mday) + VMS_EPOCH_DAYS_BEFORE_UNIX;
    secs = days * 86400 + (int64_t) wall->tm_hour * 3600
         + (int64_t) wall->tm_min * 60 + wall->tm_sec;
    if (secs < 0) return 0; /* before 1858: not representable */
    return (uint64_t) secs * VMS_TICKS_PER_SECOND;
}

/* Breaks t down in the zone named by ODS2_TZ, by setting TZ to it
   for the duration of the call and putting the caller's TZ back
   afterwards. Returns 0 if ODS2_TZ isn't set (or is empty). */
static int localtime_in_ods2_tz(time_t t, struct tm *out)
{
    const char *zone = getenv("ODS2_TZ");
    const char *old;
    char *saved = NULL;
    int ok;

    if (zone == NULL || zone[0] == '\0') return 0;

    old = getenv("TZ");
    if (old != NULL) {
        size_t n = strlen(old) + 1;
        saved = malloc(n);
        if (saved == NULL) return 0; /* fall back to the host zone */
        memcpy(saved, old, n);
    }

    if (setenv("TZ", zone, 1) != 0) {
        free(saved);
        return 0;
    }
    tzset();
    ok = localtime_r(&t, out) != NULL;

    if (saved != NULL) {
        setenv("TZ", saved, 1);
        free(saved);
    } else {
        unsetenv("TZ");
    }
    tzset();
    return ok;
}

uint64_t ods2_vms_time_from_unix(time_t t)
{
    struct tm wall;

    if (!localtime_in_ods2_tz(t, &wall)) {
        tzset(); /* pick up TZ as it is now */
        if (localtime_r(&t, &wall) == NULL) {
            /* No usable zone information at all: UTC is the only
               honest answer left (the old behaviour). */
            if (gmtime_r(&t, &wall) == NULL) return 0;
        }
    }
    return ods2_vms_time_from_tm(&wall);
}

uint64_t ods2_vms_time_now(void)
{
    return ods2_vms_time_from_unix(time(NULL));
}

void ods2_store_vms_time(uint8_t *out, uint64_t vms_time)
{
    int i;

    for (i = 0; i < 8; i++) {
        out[i] = (uint8_t) ((vms_time >> (i * 8)) & 0xff);
    }
}
