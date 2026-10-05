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

/* setenv()/unsetenv() are POSIX, not C11. */
#define _POSIX_C_SOURCE 200809L

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ods2_time.h"

#define TICKS 10000000ULL   /* 100 ns ticks per second */
#define HOUR  (3600ULL * TICKS)

/* VMS time is Modified Julian Date * 86400 * 10^7: both count from
   17-NOV-1858 00:00. MJDs below are from published tables, so these
   anchors don't share any arithmetic with the code under test. */
static uint64_t vms_from_mjd(uint64_t mjd, int h, int m, int s)
{
    return (mjd * 86400ULL + (uint64_t) h * 3600 + (uint64_t) m * 60 + (uint64_t) s) * TICKS;
}

/* What the code did before the fix: UTC seconds straight in. */
static uint64_t old_utc_formula(time_t t)
{
    return ((uint64_t) t + 3506716800ULL) * 10000000ULL;
}

static uint64_t wall(int y, int mo, int d, int h, int mi, int s)
{
    struct tm tm;
    memset(&tm, 0, sizeof tm);
    tm.tm_year = y - 1900; tm.tm_mon = mo - 1; tm.tm_mday = d;
    tm.tm_hour = h; tm.tm_min = mi; tm.tm_sec = s;
    return ods2_vms_time_from_tm(&tm);
}

static void use_zone(const char *tz, const char *ods2_tz)
{
    if (tz) setenv("TZ", tz, 1); else unsetenv("TZ");
    if (ods2_tz) setenv("ODS2_TZ", ods2_tz, 1); else unsetenv("ODS2_TZ");
}

/* Unix times used below (independently: `date -u -d ... +%s`):
     2026-10-05 14:14:00 UTC = 1791209640  (10:14 EDT, the issue's case)
     2026-01-15 15:00:00 UTC = 1768489200  (10:00 EST)
     2026-03-08 06:59:59 UTC = 1772953199  (01:59:59 EST, just before DST)
     2026-03-08 07:00:00 UTC = 1772953200  (03:00:00 EDT, just after) */
#define T_ISSUE   ((time_t) 1791209640)
#define T_WINTER  ((time_t) 1768489200)
#define T_PRE_DST ((time_t) 1772953199)
#define T_DST     ((time_t) 1772953200)

#define US_EASTERN "EST5EDT,M3.2.0,M11.1.0"  /* POSIX rule: needs no tzdata */

int main(void)
{
    /* --- the calendar arithmetic itself --- */
    assert(wall(1858, 11, 17, 0, 0, 0) == 0);
    assert(wall(1858, 11, 17, 0, 0, 1) == TICKS);
    assert(wall(1858, 11, 16, 23, 59, 59) == 0); /* before the base date: clamped */
    assert(wall(1970, 1, 1, 0, 0, 0) == 3506716800ULL * TICKS);   /* MJD 40587 */
    assert(wall(2000, 1, 1, 0, 0, 0) == vms_from_mjd(51544, 0, 0, 0));
    assert(wall(2000, 2, 29, 12, 0, 0) == vms_from_mjd(51603, 12, 0, 0)); /* leap day */
    assert(wall(2026, 10, 5, 10, 14, 0) == vms_from_mjd(61318, 10, 14, 0));
    assert(wall(2100, 3, 1, 0, 0, 0) == vms_from_mjd(88128, 0, 0, 0)); /* 2100: not leap */
    printf("PASS: wall-clock fields convert to VMS time exactly (checked against "
           "published MJDs, leap days and the 1858 base date)\n");

    /* --- UTC stays available, and matches the old output --- */
    use_zone("UTC0", NULL);
    assert(ods2_vms_time_from_unix(T_ISSUE) == old_utc_formula(T_ISSUE));
    assert(ods2_vms_time_from_unix(T_WINTER) == old_utc_formula(T_WINTER));
    use_zone("America/Los_Angeles", "UTC0");
    assert(ods2_vms_time_from_unix(T_ISSUE) == old_utc_formula(T_ISSUE));
    printf("PASS: on UTC (TZ or ODS2_TZ) the dates are exactly what the old "
           "code wrote\n");

    /* --- the issue: a US Eastern guest --- */
    use_zone(US_EASTERN, NULL);
    assert(ods2_vms_time_from_unix(T_ISSUE) == wall(2026, 10, 5, 10, 14, 0));
    assert(old_utc_formula(T_ISSUE) - ods2_vms_time_from_unix(T_ISSUE) == 4 * HOUR);
    assert(ods2_vms_time_from_unix(T_WINTER) == wall(2026, 1, 15, 10, 0, 0));
    assert(old_utc_formula(T_WINTER) - ods2_vms_time_from_unix(T_WINTER) == 5 * HOUR);
    printf("PASS: TZ=US Eastern stamps 10:14 EDT as 10:14, not 14:14 (the "
           "issue's case), and uses EST (-5h) in winter\n");

    /* Daylight saving is decided per instant, not once per run. */
    assert(ods2_vms_time_from_unix(T_PRE_DST) == wall(2026, 3, 8, 1, 59, 59));
    assert(ods2_vms_time_from_unix(T_DST)     == wall(2026, 3, 8, 3, 0, 0));
    printf("PASS: the spring-forward boundary goes 01:59:59 -> 03:00:00, as "
           "a VMS clock on Eastern time would\n");

    /* East of Greenwich: the dates move the other way. */
    use_zone("JST-9", NULL);
    assert(ods2_vms_time_from_unix(T_ISSUE) - old_utc_formula(T_ISSUE) == 9 * HOUR);
    printf("PASS: a zone ahead of UTC (JST, +9h) is handled too\n");

    /* --- ODS2_TZ picks the guest's zone over the host's --- */
    use_zone("UTC0", US_EASTERN);
    assert(ods2_vms_time_from_unix(T_ISSUE) == wall(2026, 10, 5, 10, 14, 0));
    assert(strcmp(getenv("TZ"), "UTC0") == 0);    /* host TZ put back */
    use_zone(NULL, US_EASTERN);
    assert(ods2_vms_time_from_unix(T_ISSUE) == wall(2026, 10, 5, 10, 14, 0));
    assert(getenv("TZ") == NULL);                 /* left unset as found */
    use_zone("UTC0", "");                         /* empty ODS2_TZ: ignored */
    assert(ods2_vms_time_from_unix(T_ISSUE) == old_utc_formula(T_ISSUE));
    printf("PASS: ODS2_TZ overrides the host zone and leaves TZ exactly as it "
           "was; an empty ODS2_TZ is ignored\n");

    /* The real tz database name the issue suggests, when installed. */
    if (access("/usr/share/zoneinfo/America/New_York", R_OK) == 0) {
        use_zone("America/New_York", NULL);
        assert(ods2_vms_time_from_unix(T_ISSUE) == wall(2026, 10, 5, 10, 14, 0));
        assert(ods2_vms_time_from_unix(T_WINTER) == wall(2026, 1, 15, 10, 0, 0));
        printf("PASS: TZ=America/New_York gives the same Eastern dates\n");
    } else {
        printf("SKIP: no tzdata for America/New_York on this host (the POSIX "
               "rule checks above cover the same zone)\n");
    }

    /* --- "now" goes through the same path --- */
    {
        uint64_t before, now, after;
        use_zone(US_EASTERN, NULL);
        before = ods2_vms_time_from_unix(time(NULL));
        now = ods2_vms_time_now();
        after = ods2_vms_time_from_unix(time(NULL));
        assert(before <= now && now <= after);
        printf("PASS: ods2_vms_time_now() uses the configured zone\n");
    }

    /* --- on-disk byte order --- */
    {
        uint8_t b[8];
        ods2_store_vms_time(b, 0x0102030405060708ULL);
        assert(b[0] == 0x08 && b[1] == 0x07 && b[6] == 0x02 && b[7] == 0x01);
        printf("PASS: ods2_store_vms_time() writes little-endian, as FI2$Q_* "
               "dates are stored\n");
    }

    printf("\nods2_time_selftest: all checks passed\n");
    return 0;
}
