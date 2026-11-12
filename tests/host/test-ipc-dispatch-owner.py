#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run the actual block-and-ready primitive with a receiver from another CPU.

Every enqueue must mutate the queue whose dispatch lock is held. Both FIFO/RR
and local/remote affinity paths exercise a receiver retaining a remote dpp.
"""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
source=(ROOT/'kernel/sched/dispatch.c').read_text()
a=source.index('static void rdecl block_and_ready_default(thread_t *thp)\n{')
body=source[a:source.index('\n\n\n/**',a)]
fixture=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#define rdecl
#define CONFIG_PROCESSORS_MAX 4
#define STATE_READY 2
#define STATE_RUNNING 1
#define STATE_STOPPED 3
#define SCHED_FIFO 1
#define RTMUX_FLG_TIMEOUT_MASK 255
#define RTMUX_FLG_THR_TO_BE_STOPPED 1
#define RTMUX_FLG_THR_KILLSELF 2
#define IPI_RESCHED 1
struct dispatch;
typedef struct {struct dispatch *owner; unsigned count;} queue_t;
typedef struct dispatch {int locked; queue_t q;} kdispatch_t;
typedef struct thread {
 unsigned flags,timeout_flags,priority,policy,runmask,state;
 void *restart;
 struct {void *next,*prev;} link3_link;
 uint64_t timestamp_last_block;
 kdispatch_t *dpp;
} thread_t;
static kdispatch_t local,remote;
#define LOCAL_DPP (&local)
static thread_t *current;
static unsigned get_cpunum(void) {return 0;}
static thread_t *get_active_thread(unsigned cpu) {(void)cpu;return current;}
static void set_active_thread(unsigned cpu,thread_t *t) {(void)cpu;current=t;}
#define chk_dispatch(x) ((void)0)
#define chk_thread(x) ((void)0)
#define SS_STOP_RUNNING_LOCKED(t,b) ((void)0)
#define SS_MARK_ACTIVATION(t) ((void)0)
#define SNAP_TIME_INLINE(t,b) ((void)0)
#define _TRACE_TH_EMIT_STATE(t,s) ((void)0)
#define SCHED_LOCK_LOCAL() (assert(!local.locked),local.locked=1)
#define SCHED_UNLOCK_LOCAL() (assert(local.locked),local.locked=0)
#define DISPATCH_HIGHEST_PRI(d) 10
#define DISPATCH_LST(d,p) ((d)->q)
static void enqueue(queue_t *q) {
 if(!q->owner->locked) {fputs("unlocked remote enqueue\n",stderr);exit(3);}
 q->count++;
}
#define LINK3_END(q,l,t) enqueue(&(q))
#define LINK3_BEG(q,l,t) enqueue(&(q))
#define DISPATCH_SET(d,t) assert((d)->locked && (t)->dpp==(d))
#define FIND_HIGHEST(t) ((t)=NULL)
#define SENDIPI(c,b) ((void)0)
static void timeout_start(thread_t *t) {(void)t;}
static void timeout_stop(thread_t *t) {(void)t;}
static void block(void) {current=NULL;}
static int select_cpu(thread_t *t) {(void)t;return 1;}
static void crash(void) {abort();}
static void mark_running(thread_t *t) {assert(local.locked);current=t;t->dpp=&local;t->state=STATE_RUNNING;}
static void sched_account_now(void) {}
static void rcu_sched_quiescent(void) {}
'''
main=r'''
int main(void) {
 for(unsigned affinity=0;affinity<2;affinity++) {
  for(unsigned policy=1;policy<3;policy++) {
   local=(kdispatch_t){.q.owner=&local};remote=(kdispatch_t){.q.owner=&remote};
   thread_t sender={.priority=10,.state=6,.dpp=&local};
   thread_t receiver={.priority=10,.policy=policy,.runmask=affinity,.state=5,.dpp=&remote};
   current=&sender;
   block_and_ready_default(&receiver);
   assert(receiver.state==STATE_READY && receiver.dpp==&local);
   assert(local.q.count==1 && remote.q.count==0 && !local.locked && !remote.locked);
  }
 }
 puts("IPC dispatch owner: PASS (4 cross-CPU FIFO/RR and affinity paths)");
}
'''
with tempfile.TemporaryDirectory(prefix='rtmux-dispatch-owner-') as directory:
    out=Path(directory)
    for mutant in (False,True):
        code=body.replace('dpp = LOCAL_DPP;\n    thp->dpp = dpp;', 'dpp = thp->dpp;') if mutant else body
        (out/'test.c').write_text(fixture+code+main)
        subprocess.run(shlex.split(os.environ.get('HOST_CC','cc'))+
                       ['-std=c11','-O2',str(out/'test.c'),'-o',str(out/'test')],check=True)
        result=subprocess.run([str(out/'test')],text=True,capture_output=True)
        if mutant:
            assert result.returncode!=0 and 'unlocked remote enqueue' in result.stderr,result
            print('IPC dispatch owner: former remote-queue mutation rejected')
        else:
            assert result.returncode==0,result.stderr
            print(result.stdout.strip())
