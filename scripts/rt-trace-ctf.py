#!/usr/bin/env python3
"""Convert tracelogger raw serial lines into a minimal CTF 1.8 trace directory.

`/bin/tracelogger` prints kernel ring records as:

    tracelogger: buf 0 ev 1 header=0x7001 d=0x12,0x34,0x56

This host tool preserves those words in a Babeltrace/Trace Compass consumable
CTF stream and names the core IPC/thread/process events so traces can be
filtered before deeper critical-path reconstruction exists.

SPDX-License-Identifier: Apache-2.0
"""

import argparse
import os
import re
import shutil
import struct
import sys
import uuid

from rt_trace_common import (  # shared with the other rt-trace parsers
    cycles_to_ns,
    read_cycles_per_sec,
    trace_class,
    trace_event,
)

EVENT_RE = re.compile(
    r"tracelogger:\s+buf\s+\d+\s+ev\s+\d+\s+"
    r"header=(0x[0-9a-fA-F]+|\d+)\s+"
    r"d=(0x[0-9a-fA-F]+|\d+),(0x[0-9a-fA-F]+|\d+),(0x[0-9a-fA-F]+|\d+)"
)

CTF_MAGIC = 0xC1FC1FC1
STREAM_ID = 0
EVENT_UNKNOWN = 0
EVENT_CONTROL_TIME = 1
EVENT_CONTROL_BUFFER = 2
EVENT_PROCESS = 3
EVENT_THREAD_STATE = 4
EVENT_COMM_SMSG = 5
EVENT_COMM_RMSG = 6
EVENT_COMM_REPLY = 7
EVENT_LKL_HOST_ENTER = 8
EVENT_LKL_HOST_EXIT = 9
EVENT_LKL_MEASURE_BEGIN = 10
EVENT_LKL_MEASURE_END = 11
EVENT_LKL_MEASURE_WAKE = 16
EVENT_LKL_MEASURE_MONO_LOW = 17
EVENT_LKL_MEASURE_MONO_HIGH = 18
EVENT_IRQ_CLAIM = 12
EVENT_IST_DISPATCH = 13
EVENT_PI_BOOST = 14
EVENT_SYS_PATH = 15
EVENT_LKL_HOST_OP_BASE = 32

TRACE_CONTROL_C = 1
TRACE_INT_C = 3
TRACE_PR_TH_C = 4
TRACE_SYSTEM_C = 5
TRACE_USER_C = 6
TRACE_COMM_C = 7

CONTROL_TIME = 1
CONTROL_BUFFER = 2
INT_IRQ_CLAIM = 5
INT_IST_DISPATCH = 6
SYS_PI_BOOST = 13
SYS_PATH = 2
USER_LKL_HOST_ENTER = 0x200
USER_LKL_HOST_EXIT = 0x201
USER_LKL_MEASURE_BEGIN = 0x202
USER_LKL_MEASURE_END = 0x203
USER_LKL_MEASURE_WAKE = 0x204
USER_LKL_MEASURE_MONO_LOW = 0x205
USER_LKL_MEASURE_MONO_HIGH = 0x206
COMM_SMSG = 0
COMM_RMSG = 2
COMM_REPLY = 10

EVENT_NAMES = {
    EVENT_UNKNOWN: "rtmux_unknown",
    EVENT_CONTROL_TIME: "control_time",
    EVENT_CONTROL_BUFFER: "control_buffer",
    EVENT_PROCESS: "process",
    EVENT_THREAD_STATE: "thread_state",
    EVENT_COMM_SMSG: "comm_smsg",
    EVENT_COMM_RMSG: "comm_rmsg",
    EVENT_COMM_REPLY: "comm_reply",
    EVENT_LKL_HOST_ENTER: "lkl_host_enter",
    EVENT_LKL_HOST_EXIT: "lkl_host_exit",
    EVENT_LKL_MEASURE_BEGIN: "lkl_measure_begin",
    EVENT_LKL_MEASURE_END: "lkl_measure_end",
    EVENT_LKL_MEASURE_WAKE: "lkl_measure_wake",
    EVENT_LKL_MEASURE_MONO_LOW: "lkl_measure_mono_low",
    EVENT_LKL_MEASURE_MONO_HIGH: "lkl_measure_mono_high",
    EVENT_IRQ_CLAIM: "irq_claim",
    EVENT_IST_DISPATCH: "ist_dispatch",
    EVENT_PI_BOOST: "pi_boost",
    EVENT_SYS_PATH: "sys_path",
}

LKL_HOST_OP_NAMES = {
    1: "page_alloc",
    2: "page_free",
    3: "sem_down",
    4: "mutex_lock",
    5: "thread_join",
    6: "timer_set_oneshot",
    7: "mmap",
    8: "munmap",
}

for op, name in LKL_HOST_OP_NAMES.items():
    EVENT_NAMES[EVENT_LKL_HOST_OP_BASE + op * 2] = f"lkl_host_enter_{name}"
    EVENT_NAMES[EVENT_LKL_HOST_OP_BASE + op * 2 + 1] = f"lkl_host_exit_{name}"


def parse_u32(text):
    value = int(text, 0)
    if value < 0 or value > 0xFFFFFFFF:
        raise ValueError(f"{text!r} outside uint32_t range")
    return value


def read_events(path):
    events = []
    cycles_per_sec = read_cycles_per_sec(path)
    with open(path, encoding="utf-8", errors="replace") as f:
        for lineno, line in enumerate(f, 1):
            match = EVENT_RE.search(line)
            if not match:
                continue
            try:
                header, d0, d1, d2 = (parse_u32(v) for v in match.groups())
            except ValueError as e:
                raise SystemExit(f"{path}:{lineno}: {e}") from e
            events.append((header, d0, d1, d2, cycles_per_sec))
    if not events:
        raise SystemExit(f"{path}: no tracelogger raw events found")
    return events


def event_clock(events):
    last_raw = None
    wrap_base = 0
    last = 0
    for _, d0, _, _, cycles_per_sec in events:
        if last_raw is not None and d0 < last_raw and last_raw - d0 > 0x80000000:
            wrap_base += 0x100000000
        last_raw = d0
        # Simple RTMux trace records store the time offset in d0.  Some control
        # records use d0 differently; clamp so CTF event time never goes back.
        last = max(last, cycles_to_ns(wrap_base + d0, cycles_per_sec))
        yield last


def lkl_host_event_id(ev, op):
    if op not in LKL_HOST_OP_NAMES:
        return EVENT_LKL_HOST_ENTER if ev == USER_LKL_HOST_ENTER else EVENT_LKL_HOST_EXIT
    base = EVENT_LKL_HOST_OP_BASE + op * 2
    return base if ev == USER_LKL_HOST_ENTER else base + 1


def ctf_event_id(header, d1):
    cls = trace_class(header)
    ev = trace_event(header)
    if cls == TRACE_CONTROL_C:
        if ev == CONTROL_TIME:
            return EVENT_CONTROL_TIME
        if ev == CONTROL_BUFFER:
            return EVENT_CONTROL_BUFFER
    if cls == TRACE_INT_C:
        if ev == INT_IRQ_CLAIM:
            return EVENT_IRQ_CLAIM
        if ev == INT_IST_DISPATCH:
            return EVENT_IST_DISPATCH
    if cls == TRACE_SYSTEM_C:
        if ev == SYS_PATH:
            return EVENT_SYS_PATH
        if ev == SYS_PI_BOOST:
            return EVENT_PI_BOOST
    if cls == TRACE_PR_TH_C:
        return EVENT_PROCESS if ev >= 64 else EVENT_THREAD_STATE
    if cls == TRACE_USER_C:
        if ev == USER_LKL_HOST_ENTER:
            return lkl_host_event_id(ev, d1)
        if ev == USER_LKL_HOST_EXIT:
            return lkl_host_event_id(ev, d1)
        if ev == USER_LKL_MEASURE_BEGIN:
            return EVENT_LKL_MEASURE_BEGIN
        if ev == USER_LKL_MEASURE_END:
            return EVENT_LKL_MEASURE_END
        if ev == USER_LKL_MEASURE_WAKE:
            return EVENT_LKL_MEASURE_WAKE
        if ev == USER_LKL_MEASURE_MONO_LOW:
            return EVENT_LKL_MEASURE_MONO_LOW
        if ev == USER_LKL_MEASURE_MONO_HIGH:
            return EVENT_LKL_MEASURE_MONO_HIGH
    if cls == TRACE_COMM_C:
        if ev == COMM_SMSG:
            return EVENT_COMM_SMSG
        if ev == COMM_RMSG:
            return EVENT_COMM_RMSG
        if ev == COMM_REPLY:
            return EVENT_COMM_REPLY
    return EVENT_UNKNOWN


def event_fields(event_id):
    names = {
        EVENT_THREAD_STATE: ("pid", "tid"),
        EVENT_COMM_SMSG: ("srcvid", "peer_pid"),
        EVENT_COMM_RMSG: ("srcvid", "peer_pid"),
        EVENT_COMM_REPLY: ("srcvid", "peer_pid"),
        EVENT_LKL_HOST_ENTER: ("op", "value"),
        EVENT_LKL_HOST_EXIT: ("op", "value"),
        EVENT_LKL_MEASURE_BEGIN: ("mode", "sample"),
        EVENT_LKL_MEASURE_END: ("mode", "sample"),
        EVENT_LKL_MEASURE_WAKE: ("mode", "sample"),
        EVENT_LKL_MEASURE_MONO_LOW: ("phase_mode_sample", "mono_low"),
        EVENT_LKL_MEASURE_MONO_HIGH: ("phase_mode_sample", "mono_high"),
        EVENT_IRQ_CLAIM: ("irq", "target_pid"),
        EVENT_IST_DISPATCH: ("pid", "tid"),
        EVENT_PI_BOOST: ("pid_tid", "old_new_priority"),
    }
    d1_name, d2_name = names.get(event_id, ("d1", "d2"))
    if event_id >= EVENT_LKL_HOST_OP_BASE:
        d1_name, d2_name = "op", "value"
    return f"""        uint32_t header;
        uint32_t raw_time;
        uint32_t {d1_name};
        uint32_t {d2_name};
        uint32_t trace_class;
        uint32_t trace_event;
        uint32_t cpu;
        uint32_t struct_type;
"""


def event_metadata():
    blocks = []
    for event_id, name in EVENT_NAMES.items():
        blocks.append(f"""
event {{
    name = "{name}";
    id = {event_id};
    stream_id = {STREAM_ID};
    fields := struct {{
{event_fields(event_id).rstrip()}
    }};
}};
""")
    return "".join(blocks)


def metadata(trace_uuid):
    return f"""/* CTF 1.8 */
typealias integer {{ size = 8; align = 8; signed = false; }} := uint8_t;
typealias integer {{ size = 32; align = 32; signed = false; byte_order = le; }} := uint32_t;
typealias integer {{ size = 64; align = 8; signed = false; byte_order = le; }} := uint64_t;
typealias floating_point {{
    exp_dig = 8;
    mant_dig = 24;
    byte_order = le;
    align = 32;
}} := float;

trace {{
    major = 1;
    minor = 8;
    uuid = "{trace_uuid}";
    byte_order = le;
    packet.header := struct {{
        uint32_t magic;
        uint8_t uuid[16];
        uint32_t stream_id;
    }};
}};

env {{
    domain = "rtmux";
    tracer_name = "rt-trace-ctf.py";
}};

clock {{
    name = monotonic;
    uuid = "{uuid.uuid5(trace_uuid, 'monotonic')}";
    description = "RTMux trace time offset";
    freq = 1000000000;
    offset_s = 0;
    offset = 0;
}};

stream {{
    id = {STREAM_ID};
    packet.context := struct {{
        uint64_t timestamp_begin;
        uint64_t timestamp_end;
        uint64_t content_size;
        uint64_t packet_size;
        uint32_t events_discarded;
    }};
    event.header := struct {{
        uint32_t id;
        uint64_t timestamp;
    }};
}};

{event_metadata()}"""


def write_trace(events, outdir):
    trace_uuid = uuid.uuid4()
    timestamps = list(event_clock(events))
    event_bytes = bytearray()
    for ts, (header, d0, d1, d2, _cycles_per_sec) in zip(timestamps, events):
        event_bytes += struct.pack(
            "<IQIIIIIIII",
            ctf_event_id(header, d1),
            ts,
            header,
            d0,
            d1,
            d2,
            trace_class(header),
            trace_event(header),
            (header >> 24) & 0x3F,
            (header >> 30) & 0x3,
        )

    packet_header_len = 24
    packet_context_len = 36
    content_bytes = packet_header_len + packet_context_len + len(event_bytes)
    packet = bytearray()
    packet += struct.pack("<I16sI", CTF_MAGIC, trace_uuid.bytes, STREAM_ID)
    packet += struct.pack(
        "<QQQQI",
        timestamps[0],
        timestamps[-1],
        content_bytes * 8,
        content_bytes * 8,
        0,
    )
    packet += event_bytes

    if os.path.exists(outdir):
        shutil.rmtree(outdir)
    os.makedirs(outdir)
    with open(os.path.join(outdir, "metadata"), "w", encoding="utf-8") as f:
        f.write(metadata(trace_uuid))
    with open(os.path.join(outdir, "stream"), "wb") as f:
        f.write(packet)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("log", help="guest serial log containing tracelogger lines")
    ap.add_argument("-o", "--out", required=True, help="output CTF directory")
    args = ap.parse_args()

    events = read_events(args.log)
    write_trace(events, args.out)
    print(f"rt-trace-ctf: {len(events)} events -> {args.out}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
