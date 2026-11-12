#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Execute the receive-abort arm across the unlink-to-ready hand-off.

A second CPU's timeout runs before the first waker publishes STATE_READY.
It must neither unlink twice nor become a second waker. An already-claimed
receiver must defer the abort under the same lock as delivery completion.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'kernel/sched/dispatch.c').read_text()
a = source.index('    case STATE_RECEIVE:', source.index('int rdecl force_ready('))
arm = source[a:source.index('    case STATE_MUTEX:', a)]
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#define CONFIG_PROCESSORS_MAX 4
#define CONFIG_SMP_MSGOPT 1
#define STATE_RECEIVE 5
#define STATE_SEND 4
#define STATE_NET_SEND 99
#define RTMUX_FLG_THR_BUFF_MSG 16u
#define atomic_clr(p,b) (*(p)&=~(b))
#define TYPE_CHANNEL 2
#define TYPE_MASK(t) (t)
#define RTMUX_ITF_MSG_DELIVERY 1u
#define RTMUX_ITF_WAIT_PENDING 2u
#define RTMUX_ITF_RCVNOTIFICATION 4u
#define ITF_CLR(t,b) ((t)->internal_flags&=~(b))
typedef struct {int type,lock,send_queue;} channel_t;
typedef struct {channel_t *channel;} connect_t;
typedef struct thread {
 int state;
 unsigned internal_flags,flags,priority;
 void *blocked_on, *restart;
 union {struct thread *thread;} next,prev;
} thread_t;
static channel_t ch={.type=TYPE_CHANNEL};
static unsigned unlinks, wakes, deferred, drops;
static int queued, inject_transition;
static thread_t *pending_target;
static void cpu_smp_mb(void) {}
static void kspinlock_lock(int *p) {
 assert(!*p);*p=1;
 if(inject_transition) {inject_transition=0;pending_target->internal_flags|=RTMUX_ITF_WAIT_PENDING;}
}
static void kspinlock_unlock(int *p) {
 assert(*p);*p=0;
 if(pending_target) pending_target->internal_flags&=~RTMUX_ITF_WAIT_PENDING;
}
static void unlink_thread(thread_t *t) {
 assert(ch.lock);
 if(t->internal_flags&RTMUX_ITF_WAIT_PENDING) {fputs("unready bridge crossed\n",stderr);__builtin_trap();}
 if(!queued) {fputs("duplicate unlink\n",stderr);__builtin_trap();}
 queued=0;unlinks++;
}
#define LINKPRIL_REM(t) unlink_thread(t)
#define pril_rem(q,t) unlink_thread(t)
static int connect_links_drop(connect_t *c) {(void)c;drops++;return 0;}
static void connect_detach(connect_t *c,unsigned p) {(void)c;(void)p;assert(0);}
static void force_ready_msgxfer(thread_t *t,unsigned err) {
 (void)t;(void)err;assert(ch.lock);deferred++;
}
static int abort_receive(thread_t *thp,unsigned err) {
 retry_wait:;
 channel_t *fr_chp=NULL;
 bool from_reply=false,unlinked=false;
 switch(thp->state) {
'''
tail = r'''
 default:assert(0);
 }
 /* Pause the first waker before publishing READY, then invoke a second abort
  * from main. Keeping RECEIVE here deliberately exposes the hand-off window. */
 wakes++;
 return 1;
}
int main(void) {
 thread_t th={.state=STATE_RECEIVE,.blocked_on=&ch};
 queued=1;
 assert(abort_receive(&th,9)==1);
 assert(unlinks==1 && wakes==1 && !ch.lock);
 assert(abort_receive(&th,110)==1);
 assert(unlinks==1 && wakes==1 && !ch.lock);
 th=(thread_t){.state=STATE_RECEIVE,.blocked_on=&ch,.internal_flags=RTMUX_ITF_MSG_DELIVERY};
 queued=1;
 assert(abort_receive(&th,110)==0);
 assert(queued && deferred==1 && unlinks==1 && wakes==1 && !ch.lock);
 /* Completion has unlinked but has not called ready yet. */
 th.blocked_on=NULL; th.internal_flags=0; queued=0;
 assert(abort_receive(&th,110)==1);
 assert(unlinks==1 && wakes==1 && !ch.lock);
 connect_t cop={.channel=&ch};
 th=(thread_t){.state=STATE_SEND,.blocked_on=&cop,.flags=RTMUX_FLG_THR_BUFF_MSG};
 queued=1;
 assert(abort_receive(&th,9)==1);
 assert(unlinks==2 && wakes==2 && drops==1 && !th.flags);
 assert(abort_receive(&th,9)==1);
 assert(unlinks==2 && wakes==2 && drops==1);
 th=(thread_t){.state=STATE_SEND,.blocked_on=&cop,.flags=RTMUX_FLG_THR_BUFF_MSG,
               .internal_flags=RTMUX_ITF_MSG_DELIVERY};
 queued=1;
 assert(abort_receive(&th,9)==0);
 assert(queued && deferred==2 && th.flags==RTMUX_FLG_THR_BUFF_MSG);
 assert(unlinks==2 && wakes==2 && drops==1 && !ch.lock);
 for(int send=0;send<2;send++) {
  th=(thread_t){.state=send?STATE_SEND:STATE_RECEIVE,.blocked_on=send?(void*)&cop:(void*)&ch};
  queued=1;unlinks=wakes=0;pending_target=&th;inject_transition=1;
  assert(abort_receive(&th,110)==1);
  assert(unlinks==1 && wakes==1 && !queued && !ch.lock);
 }
 puts("IPC wait owner: PASS (SEND/RECEIVE, two wakers, buffered transfer, new wait bridge)");
}
'''
with tempfile.TemporaryDirectory(prefix='rtmux-receive-owner-') as directory:
    out=Path(directory)
    for mutant in ('none','stale-owner','wait-bridge'):
        body=arm
        if mutant=='stale-owner':body=body.replace('thp->blocked_on = NULL;', '')
        if mutant=='wait-bridge':
            import re
            body=re.sub(r'if \(thp->internal_flags & RTMUX_ITF_WAIT_PENDING\) \{\s*kspinlock_unlock\(&\w+->lock\);\s*goto retry_wait;\s*\}', '', body)
        (out/'test.c').write_text(fixture+body+tail)
        subprocess.run(shlex.split(os.environ.get('HOST_CC','cc'))+
                       ['-std=c11','-O2',str(out/'test.c'),'-o',str(out/'test')],check=True)
        result=subprocess.run([str(out/'test')],text=True,capture_output=True)
        if mutant!='none':
            expected='duplicate unlink' if mutant=='stale-owner' else 'unready bridge crossed'
            assert result.returncode!=0 and expected in result.stderr,result
            print('IPC wait owner: '+mutant+' mutation rejected')
        else:
            assert result.returncode==0,result.stderr
            print(result.stdout.strip())
