/*
 * _syspage_ptr — pointer to the system page
 *
 * Set by process startup code (crt0 or ldso) from the syspage address passed in
 * the auxiliary vector.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sys/syspage.h>

struct syspage_entry *_syspage_ptr;
