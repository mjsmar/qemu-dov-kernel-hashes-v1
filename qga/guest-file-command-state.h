#ifndef GUEST_FILE_COMMAND_STATE_H
#define GUEST_FILE_COMMAND_STATE_H
/*
 * Interfaces for tracking state associated with guest-file-* commands
 *
 * Copyright IBM Corp. 2015
 *
 * Authors:
 *  Michael Roth      <mdroth@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

//#include "qga/guest-agent-core.h"
#include "qapi/qmp/qerror.h"

#ifdef _WIN32
typedef HANDLE GuestFileHandle;
#else
typedef FILE *GuestFileHandle;
#endif

void guest_file_init(void);
int64_t guest_file_handle_add(GuestFileHandle handle, Error **errp);
int64_t guest_file_handle_add_fd(int fd, const char *mode, Error **errp);
GuestFileHandle guest_file_handle_find(int64_t id, Error **err);
void guest_file_handle_remove(int64_t id);

#endif
