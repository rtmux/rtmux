/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <fcntl.h>
#include <sys/iomsg.h>
#include "connect.h"

__hidden int _connect(int base,
                      const char *path,
                      mode_t mode,
                      unsigned oflag,
                      unsigned sflag,
                      unsigned subtype,
                      int testcancel,
                      unsigned accessl,
                      unsigned file_type,
                      unsigned extra_type,
                      unsigned extra_len,
                      const void *extra,
                      unsigned response_len,
                      void *response,
                      int *status)
{
    return _connect_entry(base, path, mode, oflag, sflag, subtype, testcancel,
                          accessl, file_type, extra_type, extra_len, extra,
                          response_len, response, status, NULL, 0);
}
