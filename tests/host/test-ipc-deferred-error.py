#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Execute deferred IPC abort encoding and every replay site; reject sign loss.

The real force_ready_msgxfer body runs against a simulated two-CPU transfer.
Queue/scheduler effects of force_ready are replaced with an argument recorder.
All errno/control-bit combinations must survive the hand-off unchanged.
"""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
macros = (ROOT / 'kernel/include/core/macros.h').read_text()
constants = '\n'.join(re.findall(r'^#define (?:_TOPBIT32|_FORCE_\w+|MSG_FORCE_ERROR)[^\n]*', macros, re.M))
source = (ROOT / 'kernel/sched/dispatch.c').read_text()
start = source.index('static void force_ready_msgxfer(')
body = source[start:source.index('\n#endif', start)]
# The three direct and three saved-status IPC sites, plus xfer preemption.
replays = []
for name in ['kernel/ipc/fastmsg.c', 'kernel/ipc/message.c', 'kernel/xfer/core.c']:
    text = (ROOT / name).read_text()
    for expression in re.findall(r'(?:force_ready\(thp, |thp_status = )(MSG_FORCE_ERROR\(thp\)|\(unsigned\) KSTATUS\(thp\))', text):
        replays.append(expression)
assert len(replays) == 7, f'deferred abort sites changed: {replays}'
fixture = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
typedef struct thread { uintptr_t status; unsigned internal_flags; struct thread *blocked_on; } thread_t;
#define KSTATUS(t) ((t)->status)
#define SETKSTATUS(t,v) ((t)->status=(uintptr_t)(v))
#define RTMUX_ITF_MSG_DELIVERY 1u
#define RTMUX_ITF_MSG_FORCE_RDY 2u
#define ITF_SET(t,b) ((t)->internal_flags|=(b))
#define IPI_RESCHED 1u
static unsigned ipi_cpu, ipi_command, ipi_count;
#define SENDIPI(h,c) (ipi_cpu=(h),ipi_command=(c),ipi_count++)
unsigned hart_ids[]={0,1};
static int num_processors=2;
static thread_t active;
static thread_t *get_active_thread(unsigned cpu) {return cpu==1 ? &active : 0;}
static unsigned received;
static void force_ready(thread_t *t,unsigned e) {(void)t;received=e;}
'''
main = r'''
int main(void) {
 const unsigned errors[]={0,EBADF,ESRCH,EINTR,ETIMEDOUT,EFAULT};
 const unsigned flags[]={_FORCE_SET_ERROR,_FORCE_KILL_SELF,_FORCE_NO_UNBLOCK};
 thread_t peer, *thp=&peer;
 unsigned cases=0;
 for(unsigned e=0;e<sizeof(errors)/sizeof(errors[0]);e++) {
  for(unsigned mask=0;mask<8;mask++) {
   unsigned request=errors[e];
   for(unsigned b=0;b<3;b++) if(mask&(1u<<b)) request|=flags[b];
   peer=(thread_t){.internal_flags=RTMUX_ITF_MSG_DELIVERY};
   active=(thread_t){.internal_flags=RTMUX_ITF_MSG_DELIVERY,.blocked_on=thp};
   ipi_count=0;
   force_ready_msgxfer(thp,request);
   assert(peer.internal_flags==(RTMUX_ITF_MSG_DELIVERY|RTMUX_ITF_MSG_FORCE_RDY));
   assert(ipi_count==1 && ipi_cpu==1 && ipi_command==IPI_RESCHED);
   /* REPLAYS */
   cases++;
  }
 }
 printf("deferred IPC abort: PASS (%u combinations, 7 replay sites)\n",cases);
}
'''
checks = '\n'.join('force_ready(thp, '+expr+'); if(received!=request) { fprintf(stderr,"abort changed: %x -> %x\\n",request,received); return 1; }' for expr in replays)
with tempfile.TemporaryDirectory(prefix='rtmux-ipc-abort-') as directory:
    out = Path(directory)
    for mutant in (False, True):
        replay = checks.replace('MSG_FORCE_ERROR(thp)', '(unsigned) KSTATUS(thp)') if mutant else checks
        (out / 'test.c').write_text(fixture + constants + '\n' + body + main.replace('/* REPLAYS */', replay))
        subprocess.run(shlex.split(os.environ.get('HOST_CC', 'cc')) +
                       ['-std=c11', '-O2', str(out / 'test.c'), '-o', str(out / 'test')], check=True)
        result = subprocess.run([str(out / 'test')], text=True, capture_output=True)
        if mutant:
            assert result.returncode != 0 and 'abort changed:' in result.stderr, result
            print('deferred IPC abort: former negative-status replay rejected')
        else:
            assert result.returncode == 0, result.stderr
            print(result.stdout.strip())
