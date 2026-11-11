# Dump every hart of a live RTMux QEMU guest: registers, an S-mode backtrace,
# and the per-hart scheduler/IPI/FPU ownership fields. Run as
#   gdb-multiarch -batch -ex 'target remote :PORT' -x dump-state.gdb rtmux-kernel
# The guest is not frozen on connect, so the first command stops it. QEMU shows
# hart N as gdb thread N + 1.
set pagination off
set confirm off
set width 0
python
import gdb

inf = gdb.selected_inferior()
threads = sorted(inf.threads(), key=lambda t: t.num)
print("harts visible to gdb: %d" % len(threads))
for t in threads:
    t.switch()
    hart = t.num - 1
    print("=== hart %d ===" % hart)
    for expr in ("$pc", "$sp", "$ra", "$sstatus", "$scause", "$stval", "$sepc"):
        try:
            print("%s = %s" % (expr, gdb.parse_and_eval(expr)))
        except gdb.error as e:
            print("%s: unavailable (%s)" % (expr, e))
    try:
        gdb.execute("info registers")
    except gdb.error as e:
        print("info registers: %s" % e)
    try:
        gdb.execute("bt 32")
    except gdb.error as e:
        print("bt: %s" % e)
    # A hart caught inside the SBI firmware (an ecall for an IPI or fence) still
    # holds its S-mode registers, so unwind from sepc to see who it called from.
    try:
        pcv = int(gdb.parse_and_eval("$pc"))
        sepc = int(gdb.parse_and_eval("$sepc")) & 0xffffffffffffffff
        if pcv < 0xffffffc000000000 and sepc >= 0xffffffc000000000:
            print("  hart in firmware; S-mode backtrace from sepc 0x%x:" % sepc)
            gdb.execute("set $pc = $sepc")
            gdb.execute("bt 32")
    except gdb.error as e:
        print("  (sepc backtrace failed: %s)" % e)
    # Name the lock a hart is spinning on and print its contents.
    try:
        fr = gdb.newest_frame()
        n = 0
        while fr is not None and n < 6:
            if fr.name() in ("kspinlock_lock", "interrupt_lock", "kspinlock_lock_irqsave"):
                for vn in ("l", "__spin"):
                    try:
                        lk = fr.read_var(vn)
                        print("  SPINNING on lock %s = %s" % (lk, lk.dereference()))
                        break
                    except Exception:
                        pass
                break
            fr = fr.older()
            n += 1
    except Exception as e:
        print("  (lock detail failed: %s)" % e)
    # If this hart is inside a channel destroy, dump the channel being drained:
    # a stale queue head the drain cannot remove is what spins it.
    try:
        fr = gdb.newest_frame()
        while fr is not None:
            if fr.name() in ("ksys_channel_destroy", "_channel_destroy"):
                try:
                    chp = fr.read_var("chp")
                    print("  DESTROY frame %s chp %s chid %d destroying %d flags 0x%x" %
                          (fr.name(), chp, int(chp["chid"]), int(chp["destroying"]),
                           int(chp["flags"])))
                    tt = gdb.lookup_type("thread_t").pointer()
                    for qn in ("reply_queue", "receive_queue"):
                        q = chp[qn]
                        first = q
                        n = 0
                        while int(q) != 0 and n < 8:
                            qt = q.cast(tt)
                            print("    %s[%d] thp %s type 0x%x pid %d tid %d state %d "
                                  "itf 0x%x flags 0x%x blocked_on %s next %s prev %s" %
                                  (qn, n, qt, int(qt["type"]), int(qt["process"]["pid"]),
                                   int(qt["tid"]), int(qt["state"]),
                                   int(qt["internal_flags"]), int(qt["flags"]),
                                   qt["blocked_on"], qt["next"]["thread"],
                                   qt["prev"]["thread"]))
                            q = qt["next"]["thread"]
                            n += 1
                            if int(q) == int(first):
                                break
                    print("    send_queue %s" % chp["send_queue"])
                except Exception as e:
                    print("  (destroy detail failed: %s)" % e)
                break
            fr = fr.older()
    except Exception as e:
        print("  (frame walk failed: %s)" % e)
    for f in ("pc_syscall", "pc_active_prp", "pc_ipicmds", "pc_dispatch",
              "pc_active_fpu"):
        try:
            print("pcpu_array[%d].%s = %s" %
                  (hart, f, gdb.parse_and_eval("pcpu_array[%d].%s" % (hart, f))))
        except gdb.error as e:
            print("pcpu_array[%d].%s: unavailable (%s)" % (hart, f, e))

# Every live thread of every process: state, what it is blocked on, flags. All
# harts idle plus a non-READY thread here names the lost wakeup.
def kptr(v):
    try:
        return int(v) >= 0xffffffc000000000
    except gdb.error:
        return False

try:
    pv = gdb.parse_and_eval("process_vector")
    nproc = int(pv["nentries"])
    pvec = pv["vector"]
    print("=== threads ===")
    for i in range(nproc):
        prp = pvec[i]
        if not kptr(prp):
            continue
        prp = prp.cast(gdb.lookup_type("process_t").pointer())
        tv = prp["threads"]
        for j in range(int(tv["nentries"])):
            thp = tv["vector"][j]
            if not kptr(thp):
                continue
            thp = thp.cast(gdb.lookup_type("thread_t").pointer())
            st = int(thp["state"])
            if st == 0 or st > 20 or int(thp["process"]) != int(prp):
                continue  # dead, free-list slot, or not this process's thread
            print("proc %d (%s) tid %d state %d blocked_on %s flags 0x%x itf 0x%x" %
                  (int(prp["pid"]), str(prp), int(thp["tid"]), st,
                   thp["blocked_on"], int(thp["flags"]),
                   int(thp["internal_flags"])))
            try:
                if st == 2:  # READY on an idle hart: show where it is queued
                    print("  READY tid %d: runcpu %d runmask 0x%x prio %d dpp %s "
                          "next %s" %
                          (int(thp["tid"]), int(thp["runcpu"]),
                           int(thp["runmask"]), int(thp["priority"]),
                           thp["dpp"], thp["link3_link"]["next"]))
                elif st == 5:  # RECEIVE: blocked_on is the channel
                    chp = thp["blocked_on"].cast(
                        gdb.lookup_type("channel_t").pointer())
                    print("  RECV chp %s chid %d proc %s send %s rcv %s reply %s "
                          "destroying %d" %
                          (chp, int(chp["chid"]), chp["process"],
                           chp["send_queue"], chp["receive_queue"],
                           chp["reply_queue"], int(chp["destroying"])))
                    # Walk the channel's reply queue (circular via link3_link).
                    first = chp["reply_queue"]
                    q = first
                    n = 0
                    while int(q) != 0 and n < 8:
                        qt = q.cast(gdb.lookup_type("thread_t").pointer())
                        print("    replyq[%d] thp %s pid %d tid %d state %d itf 0x%x "
                              "blocked_on %s" %
                              (n, qt, int(qt["process"]["pid"]), int(qt["tid"]),
                               int(qt["state"]), int(qt["internal_flags"]),
                               qt["blocked_on"]))
                        q = qt["next"]["thread"]
                        n += 1
                        if int(q) == int(first):
                            break
                elif st == 6:  # REPLY: blocked_on is the connection
                    cop = thp["blocked_on"].cast(
                        gdb.lookup_type("connect_t").pointer())
                    print("  REPLYWAIT thp %s cop %s scoid %d channel %s syscall 0x%x "
                          "restart %s state-of-server-link %s" %
                          (thp, cop, int(cop["scoid"]), cop["channel"],
                           int(thp["syscall"]), thp["restart"],
                           thp["args"]["ms"]["server"]))
                    try:
                        print("  REPLYWAIT msg: %s" % thp["args"]["msbuff"]["buff"])
                    except Exception as e:
                        print("  (msg detail failed: %s)" % e)
            except Exception as e:
                print("  (detail failed: %s)" % e)
except gdb.error as e:
    print("thread walk failed: %s" % e)
print("DUMP COMPLETE: %d harts" % len(threads))
end
