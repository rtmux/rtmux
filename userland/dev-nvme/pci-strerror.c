/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * Thread-safe string lookup for pci_err_t values. Values that mirror errno are
 * delegated to strerror() so we don't duplicate libc's table.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include "pci.h"

const char *pci_strerror(pci_err_t err)
{
    switch (err) {
    case PCI_ERR_OK:
        return "no error";
    case PCI_ERR_SERVER_NOT_AVAILABLE:
        return "PCI server not available";
    default:
        if ((int) err >= 0 && (int) err < PCI_ERR_SERVER_NOT_AVAILABLE)
            return strerror((int) err);
        return "unknown PCI error";
    }
}
