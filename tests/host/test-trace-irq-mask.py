#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Execute the real trace-ring append function with a simulated IRQ controller.

Only platform primitives are stubbed. Both enabled and masked callers exercise
empty, full, flushing, retry, drop, linear and circular buffer paths. Replacing
IRQ restoration with the former unconditional enable must fail the same test.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/trace/core.c').read_text()
start = source.index('int add_trace_event(uint32_t')
body = source[start:source.index('/* Adds buffer in the form of events. */', start)]
start = source.index('#define _TRACE_CLR_BUFF')
clear = source[start:source.index('static unsigned dropped_events;', start)]
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define EOK 0
#define _TRACE_FLAGS_FLUSH 1u
#define _TRACE_FLAGS_WRITING 2u
#define _TRACELEMENTS 8
#define _TRACE_CONTROL_C 0
#define _TRACE_STRUCT_S 0
#define _TRACE_CONTROL_BUFFER 0
#define INKERNEL_INTRMASK 1
#define _TRACE_GET_STRUCT(h) 1
#define _TRACE_MAKE_CODE(a,b,c,d) 0
static int irq_enabled;
static unsigned long cpu_irq_save(void) { int old=irq_enabled; irq_enabled=0; return old; }
static void cpu_irq_restore(unsigned long f) { irq_enabled=(int)f; }
static void interrupt_disable(void) { irq_enabled=0; }
static void interrupt_enable(void) { irq_enabled=1; }
static void spin_lock(int *p) { assert(!irq_enabled && !*p); *p=1; }
#define SPINLOCK(p) spin_lock(p)
#define SPINUNLOCK(p) (*(p)=0)
typedef struct { uint32_t header, data[3]; } traceevent_t;
typedef struct tracebuf {
 struct {
  unsigned flags, num_events, seq_buff_num;
  int spin;
  traceevent_t *tail_ptr, *begin_ptr;
  struct tracebuf *next;
 } h;
 traceevent_t data[8];
} tracebuf_t;
static struct { unsigned main_flags, skip_buff, max_events, ring_mode, send_buff_num; int state_spin; } trace_masks;
static struct { tracebuf_t *tracebuf; } private_data, *privateptr=&private_data;
static unsigned dropped_events, dropped_buffers;
static int force_emit[4], trace_force_flush;
static int get_inkernel(void) { return 0; }
static unsigned get_time_off(int init) { (void)init; return 1; }
static int em_event(tracebuf_t *p) { (void)p; return 0; }
'''
main = r'''
int main(void) {
 for (int enabled=0; enabled<2; enabled++) {
  for (int mode=0; mode<8; mode++) {
   tracebuf_t ring[3]={0};
   memset(&trace_masks,0,sizeof(trace_masks));
   trace_masks.main_flags=mode!=0;
   trace_masks.max_events=mode>=6 ? 1 : 4;
   trace_masks.ring_mode=mode==6;
   trace_masks.skip_buff=mode==5;
   for (int i=0;i<3;i++) {
    ring[i].h.next=&ring[(i+1)%3];
    ring[i].h.begin_ptr=ring[i].h.tail_ptr=ring[i].data;
    if (mode==3) ring[i].h.flags=_TRACE_FLAGS_FLUSH|_TRACE_FLAGS_WRITING;
    if (mode==4 || mode==5) ring[i].h.flags=_TRACE_FLAGS_FLUSH;
   }
   privateptr->tracebuf=mode==1 ? NULL : ring;
   irq_enabled=enabled;
   (void)add_trace_event(1,2,3,4);
   if (irq_enabled!=enabled) {
    fprintf(stderr,"IRQ mask changed: enabled=%d path=%d\n",enabled,mode);
    return 1;
   }
   assert(!trace_masks.state_spin);
   for (int i=0;i<3;i++) assert(!ring[i].h.spin);
  }
 }
 puts("trace IRQ mask: PASS (16 append paths)");
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix='rtmux-trace-mask-') as directory:
    out = Path(directory)
    for mutant in (False, True):
        code = body.replace('cpu_irq_restore(irq_flags);', 'interrupt_enable();') if mutant else body
        (out / 'test.c').write_text(fixture + clear + code + main)
        subprocess.run(shlex.split(os.environ.get('HOST_CC', 'cc')) +
                       ['-std=c11', '-O2', str(out / 'test.c'), '-o', str(out / 'test')], check=True)
        result = subprocess.run([str(out / 'test')], text=True, capture_output=True)
        if mutant:
            assert result.returncode != 0 and 'IRQ mask changed:' in result.stderr, result
            print('trace IRQ mask: old unconditional-enable mutation rejected')
        else:
            assert result.returncode == 0, result.stderr
            print(result.stdout.strip())
