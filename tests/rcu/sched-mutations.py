#!/usr/bin/env python3
"""Check that the RCU gate rejects selected broken lifetime invariants."""

import re
import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

# Each edit is restricted to a single function, so a surviving occurrence in
# another syscall cannot accidentally satisfy the check being exercised.
CASES = (
    ("info self", "kernel/sched/policy.c", "ksys_sched_info",
     "kap->pid == 0 || kap->pid == act->process->pid", "kap->pid == 0",
     "sched_info must bypass self pid"),
    ("info lookup", "kernel/sched/policy.c", "ksys_sched_info",
     "lookup_pid_rcu(kap->pid)", "lookup_pid(kap->pid)",
     "sched_info must bypass self pid"),
    ("info validation", "kernel/sched/policy.c", "ksys_sched_info",
     "user_slice_init(act, kap->info", "disabled_slice_init(act, kap->info",
     "sched_info must bypass self pid"),
    ("info unlock", "kernel/sched/policy.c", "ksys_sched_info",
     "rcu_sched_read_unlock();", "/* removed unlock */",
     "sched_info must bypass self pid"),
    ("info local guard", "kernel/sched/policy.c", "ksys_sched_info",
     "if (prp != act->process)", "if (0)",
     "sched_info must bypass self pid"),
    ("info denial", "kernel/sched/policy.c", "ksys_sched_info",
     "return EPERM;", "return EOK;", "sched_info must bypass self pid"),
    ("info fault", "kernel/sched/policy.c", "ksys_sched_info",
     "&info_slice))\n        return EFAULT;", "&info_slice))\n        return EOK;",
     "sched_info must bypass self pid"),
    ("get self", "kernel/sched/policy.c", "ksys_sched_get",
     "kap->pid == 0 || kap->pid == act->process->pid", "kap->pid == 0",
     "sched_get pid/tid lookup"),
    ("get lookup", "kernel/sched/policy.c", "ksys_sched_get",
     "lookup_pid_rcu(kap->pid)", "lookup_pid(kap->pid)",
     "sched_get pid/tid lookup"),
    ("get denial", "kernel/sched/policy.c", "ksys_sched_get",
     "return EPERM;", "return EOK;", "sched_get pid/tid lookup"),
    ("get fault", "kernel/sched/policy.c", "ksys_sched_get",
     "USER_SLICE_WR, &param_slice))\n        return EFAULT;",
     "USER_SLICE_WR, &param_slice))\n        return EOK;", "sched_get pid/tid lookup"),
    ("get validation", "kernel/sched/policy.c", "ksys_sched_get",
     "USER_SLICE_WR, &param_slice", "USER_SLICE_RD, &param_slice",
     "sched_get pid/tid lookup"),
    ("get copy", "kernel/sched/policy.c", "ksys_sched_get",
     "user_slice_copyout(&param_slice, &param)", "disabled_copyout(&param_slice, &param)",
     "sched_get pid/tid lookup"),
    ("get early copy", "kernel/sched/policy.c", "ksys_sched_get",
     "if (thp_rcu)\n        rcu_read_unlock(&tthread_rcu);\n    if (kap->param && user_slice_copyout(&param_slice, &param))",
     "if (kap->param && user_slice_copyout(&param_slice, &param))\n        return EFAULT;\n    if (thp_rcu)\n        rcu_read_unlock(&tthread_rcu);\n    if (0)",
     "sched_get pid/tid lookup"),
    ("set self", "kernel/sched/policy.c", "ksys_sched_set",
     "kap->pid == 0 || kap->pid == act->process->pid", "kap->pid == 0",
     "sched_set pid/tid path"),
    ("set lookup", "kernel/sched/policy.c", "ksys_sched_set",
     "lookup_pid_rcu(kap->pid)", "lookup_pid(kap->pid)",
     "sched_set pid/tid path"),
    ("set denial", "kernel/sched/policy.c", "ksys_sched_set",
     "return EPERM;", "return EOK;", "sched_set pid/tid path"),
    ("set sporadic copy", "kernel/sched/policy.c", "ksys_sched_set",
     "user_slice_get(act, &param, kap->param, sizeof(param))", "disabled_copy()",
     "sched_set pid/tid path"),
    ("set priority copy", "kernel/sched/policy.c", "ksys_sched_set",
     "user_slice_get(act, &param.sched_priority,", "disabled_copy(act, &param.sched_priority,",
     "sched_set pid/tid path"),
    ("set process pin", "kernel/sched/policy.c", "ksys_sched_set",
     "process_pin(prp)", "disabled_pin(prp)", "sched_set pid/tid path"),
    ("create attrs", "kernel/sched/thread.c", "ksys_thread_create",
     "kap->attr ? &attr_copy : NULL", "kap->attr", "thread_create explicit pid lookup"),
    ("create pin guard", "kernel/sched/thread.c", "ksys_thread_create",
     "if (prp_pinned)", "if (1)", "thread_create explicit pid lookup"),
    ("join validation", "kernel/sched/thread.c", "ksys_thread_join",
     "sizeof status, USER_SLICE_WR, &status_slice", "sizeof status, USER_SLICE_RD, &status_slice",
     "dead thread_join must reserve claim"),
    ("destroy stats", "kernel/sched/thread.c", "kcall_op_thread_ctl",
     "thread_destroy_stats_snapshot(&tds", "disabled_stats_snapshot(&tds",
     "thread destroy attribution must stay wired"),
    ("join snapshot", "kernel/thread/lifecycle.c", "thread_destroy",
     "jthp = thp->join;", "jthp = NULL;", "join wake paths must wait"),
    ("reclaim handoff", "kernel/thread/lifecycle.c", "thread_destroy",
     "    rcu_sched_synchronize();\n        (void) tthread_wait_unpinned(thp);",
     "    if (thread_reclaim_enqueue(thp, prp, last_active))\n            return;\n"
     "        rcu_sched_synchronize();\n        (void) tthread_wait_unpinned(thp);",
     "thread reap must stay synchronous"),
    ("sync reap", "kernel/thread/lifecycle.c", "thread_destroy",
     "(void) tthread_wait_unpinned(thp);", "/* removed pin wait */",
     "thread reap must drain RCU, pins, and active harts"),
    ("sync reap", "kernel/thread/lifecycle.c", "thread_destroy",
     "stats_pin_waited = tthread_wait_unpinned(thp);", "stats_pin_waited = 0;",
     "thread reap must drain RCU, pins, and active harts"),
    ("sync reap", "kernel/thread/lifecycle.c", "thread_destroy",
     "cap_revoke_object_all_tables(thp);", "/* removed revoke */",
     "thread reap must drain RCU, pins, and active harts"),
    ("server copy", "kernel/ipc/connect.c", "ksys_connect_server_info",
     "user_slice_put(act, kap->info, &info, sizeof(info))", "disabled_put()",
     "connect_server_info pid lookup"),
    ("server early copy", "kernel/ipc/connect.c", "ksys_connect_server_info",
     "kspinlock_lock(&prp->vec_slock);",
     "user_slice_put(act, kap->info, &info, sizeof(info));\n    kspinlock_lock(&prp->vec_slock);",
     "connect_server_info pid lookup"),
    ("send lock", "kernel/ipc/fastmsg.c", "sendv_body",
     "lookup_rcvid_open();", "/* removed read section */",
     "msg_sendv must open connobj_rcu"),
)


def run_gate(root):
    return subprocess.run(["sh", "scripts/check-rcu-sched.sh"], cwd=root,
                          text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def main():
    with tempfile.TemporaryDirectory(prefix="rtmux-rcu-mutations-") as tmp:
        work = Path(tmp)
        for directory in ("arch", "include", "internal", "kernel", "scripts", "tests"):
            shutil.copytree(ROOT / directory, work / directory, symlinks=True)
        baseline = run_gate(work)
        if baseline.returncode:
            raise SystemExit("baseline gate failed:\n" + baseline.stderr)
        for name, file, function, old, new, expected in CASES:
            path = work / file
            original = path.read_text()
            definition = re.search(r"^(?:static )?(?:int|void) (?:kdecl |rdecl )?" +
                                   re.escape(function) + r"\(", original, re.MULTILINE)
            if definition is None:
                raise SystemExit(f"{name}: function definition not found")
            start = definition.start()
            end = original.index("\n}", start) + 2
            body = original[start:end]
            if body.count(old) != 1:
                raise SystemExit(f"{name}: expected exactly one mutation site")
            path.write_text(original[:start] + body.replace(old, new, 1) + original[end:])
            try:
                result = run_gate(work)
            finally:
                path.write_text(original)
            if (result.returncode == 0 or expected not in result.stderr or
                    f"RCU detail: {name}" not in result.stderr):
                raise SystemExit(f"{name}: wrong result ({result.returncode}):\n{result.stderr}")
            print(f"{name}: rejected")


if __name__ == "__main__":
    main()
