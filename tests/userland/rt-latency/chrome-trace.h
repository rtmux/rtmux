/*
 * chrome-trace.h - emit measured latency spans as Chrome Trace Event JSON.
 *
 * The RT benchmarks already record nanosecond spans; this turns each one into a
 * Chrome Trace Event ("X"/complete phase) on a "chrome-event:" line. A host
 * tool (scripts/rt-trace-json.py) lifts those lines out of the guest serial log
 * and assembles a Perfetto-loadable trace, so the latency-tail investigation
 * becomes a timeline instead of a wall of printf. Host-side only; nothing about
 * the measured spans changes.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef RT_LATENCY_CHROME_TRACE_H
#define RT_LATENCY_CHROME_TRACE_H

#include <stdint.h>
#include <stdio.h>

/* Map a span-class name to a stable small positive Perfetto track id, so each
 * class (fs, net, fs-rt, ...) lands on its own row. FNV-1a over the name.
 */
static inline int chrome_tid(const char *name)
{
    uint32_t h = 2166136261u;

    while (*name)
        h = (h ^ (uint32_t) (unsigned char) *name++) * 16777619u;
    return (int) (h % 4093u) + 1;
}

/* Emit one Chrome Trace Event for a span on a "chrome-event:" line. ts/dur are
 * microseconds (Chrome's unit) printed at nanosecond resolution with integer
 * math -- the freestanding libc printf has no float support, but the
 * fixed-point "<us>.<ns-fraction>" form is a valid JSON number and Perfetto
 * reads it.
 *
 * `name` is emitted raw into the JSON string, so it must be a trusted fixed
 * identifier (a benchmark mode label) with no quotes or backslashes -- callers
 * must not pass user-controlled text. Escaping is deliberately omitted to keep
 * the emitter a few lines.
 */
static inline void chrome_emit_span(const char *name, int tid,
                                    uint64_t start_ns, uint64_t dur_ns)
{
    printf("chrome-event: {\"ph\":\"X\",\"pid\":1,\"tid\":%d,"
           "\"name\":\"%s\",\"ts\":%llu.%03llu,\"dur\":%llu.%03llu}\n",
           tid, name,
           (unsigned long long) (start_ns / 1000u),
           (unsigned long long) (start_ns % 1000u),
           (unsigned long long) (dur_ns / 1000u),
           (unsigned long long) (dur_ns % 1000u));
}

#endif /* RT_LATENCY_CHROME_TRACE_H */
