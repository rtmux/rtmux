#
# Kernel object list -- single source of truth.
# Included by kernel/Makefile (compilation) and arch/riscv/Makefile (linking).
#

KER_SRC = init/objects init/entry init/setup init/roottask \
          init/shutdown \
          printk/kprintf \
          core/priv core/globals core/idle \
          core/sys \
          cap/table \
          ipc/channel ipc/connect ipc/message ipc/fastmsg \
          sched/thread sched/policy \
          sync/wait sync/rcu \
          time/timer time/clock \
          irq/interrupt \
          mm/core mm/vaddrinfo mm/mcreate mm/map-xfer \
          mm/roottask-view \
          lib/walk-asinfo \
          module/initramfs-elf module/elf-validate \
          trace/ring \
          trace/instr/trace \
          trace/instr/tr-dumper

ifdef CONFIG_STALL_DETECTOR
KER_SRC += debug/stall-detector debug/hartwatch
endif

ifdef CONFIG_MONITORING_KTHREAD
KER_SRC += debug/monitor
endif

ifdef CONFIG_LOCK_DEBUG
KER_SRC += debug/lock-debug
endif

ifdef CONFIG_DEBUG_INJECT
KER_SRC += debug/inject
endif

# RCU lookup lifetime support lives in sync/rcu.
# Note: calls is arch-specific (RISC-V __kcall_dispatch).
# It's built as part of ARCH_SRC in arch/riscv/Makefile.

KER_OBJECT_SRC = object/alloc object/core object/vector \
                 object/query object/lookup

KER_THREAD_SRC = thread/lifecycle thread/special-return

KER_IPC_CORE_SRC = ipc/connect-core ipc/message-core ipc/notification ipc/event

KER_XFER_SRC = xfer/core xfer/check xfer/copy xfer/length \
               xfer/message xfer/notification xfer/user-slice

KER_SUBSYS_SRC = $(KER_OBJECT_SRC) $(KER_THREAD_SRC) \
                 $(KER_IPC_CORE_SRC) \
                 sched/dispatch \
                 sync/core \
                 time/timer-core time/clock-core \
                 irq/core \
                 fault/core \
                 debug/thread-debug \
                 security/cred \
                 core/misc \
                 trace/core \
                 $(KER_XFER_SRC)

KER_EXT_SRC = ext/bind ext/process \
               ext/misc \
               ext/page ext/stack \
               ext/cred ext/debug \
               ext/trace ext/query \
               ext/exit \
               ext/mm-xboundary ext/vaddrinfo \
               ext/manipulate ext/pa \
               ext/smp ext/register \
               ext/as-memcpy

KER_ALL_SRC = $(KER_SRC) $(KER_SUBSYS_SRC) $(KER_EXT_SRC)
