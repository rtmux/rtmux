/* SPDX-License-Identifier: Apache-2.0 */
.text
.global __my_thread_exit
.hidden __my_thread_exit
.type __my_thread_exit, %function
__my_thread_exit:
    b pthread_exit
