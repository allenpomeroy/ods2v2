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

/*
 * ods2_time.h - VMS 64-bit timestamps for FI2$Q_CREDATE / FI2$Q_REVDATE.
 *
 * A VMS binary time counts 100 ns ticks since 17-NOV-1858 00:00 *local
 * time*. VMS keeps no zone with it: the system clock, and every file
 * date, is plain wall-clock time. Stamping a file with the host's UTC
 * puts it hours ahead of (or behind) a VMS guest that isn't on UTC, and
 * ANALYZE/DISK reports FUTCREDAT / FUTREVDAT.
 *
 * So "now" is converted using a time zone, chosen in this order:
 *
 *   1. ODS2_TZ, if set and non-empty. Any value TZ accepts works:
 *      ODS2_TZ=America/New_York, ODS2_TZ=EST5EDT, ODS2_TZ=UTC (for a
 *      VMS system that genuinely runs on UTC). Use this when the
 *      guest's zone differs from the host's without changing TZ for
 *      everything else in your shell.
 *   2. Otherwise the host's local zone, which honours TZ as usual
 *      (so `TZ=America/New_York ods2 ...` works too).
 *
 * The zone should be the one the VMS guest's clock is set to.
 */
#ifndef ODS2_TIME_H
#define ODS2_TIME_H

#include <stdint.h>
#include <time.h>

/* VMS time for wall-clock fields already broken down (year, month,
   day, hour, minute, second as in struct tm; other fields ignored).
   No zone conversion happens here - the fields are taken as the
   local time VMS should store. */
uint64_t ods2_vms_time_from_tm(const struct tm *wall);

/* VMS time for the instant `t` (seconds since the Unix epoch, UTC),
   expressed in the zone chosen as described above. */
uint64_t ods2_vms_time_from_unix(time_t t);

/* ods2_vms_time_from_unix(time(NULL)). */
uint64_t ods2_vms_time_now(void);

/* Stores `vms_time` little-endian into 8 bytes at `out`, the on-disk
   layout of a FI2$Q_* date. */
void ods2_store_vms_time(uint8_t *out, uint64_t vms_time);

#endif /* ODS2_TIME_H */
