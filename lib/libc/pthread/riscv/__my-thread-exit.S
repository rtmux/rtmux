/*
 * __my_thread_exit() — RISC-V trampoline for thread exit.
 *
 * When a thread's start function returns, it "returns" to this
 * function (set as exitfunc in pthread_attr_default).  We move
 * the return value (a0) into the argument register and tail-call
 * pthread_exit().
 *
 * License: Apache 2.0
 */

    .text
    .globl __my_thread_exit
    .hidden __my_thread_exit
    .type  __my_thread_exit, @function

__my_thread_exit:
    /* a0 already holds the thread function's return value */
    tail   pthread_exit

    .size __my_thread_exit, .-__my_thread_exit
