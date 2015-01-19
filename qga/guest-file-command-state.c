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

#include <glib.h>
#include "qga/guest-agent-core.h"
#include "qga/guest-file-command-state.h"
#include "qapi/qmp/qerror.h"
#include "qemu/queue.h"

typedef struct GuestFileData {
    uint64_t id;
    GuestFileHandle handle;
    QTAILQ_ENTRY(GuestFileData) next;
} GuestFileData;

static struct {
    QTAILQ_HEAD(, GuestFileData) filehandles;
} guest_file_state;

void guest_file_init(void)
{
    QTAILQ_INIT(&guest_file_state.filehandles);
}

int64_t guest_file_handle_add(GuestFileHandle handle, Error **errp)
{
    GuestFileData *gfd;
    int64_t id;
    Error *local_err = NULL;

    id = ga_get_fd_handle(ga_state, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return -1;
    }

    gfd = g_malloc0(sizeof(GuestFileData));
    gfd->id = id;
    gfd->handle = handle;
    QTAILQ_INSERT_TAIL(&guest_file_state.filehandles, gfd, next);

    return id;
}

GuestFileHandle guest_file_handle_find(int64_t id, Error **err)
{
    GuestFileData *gfd;

    QTAILQ_FOREACH(gfd, &guest_file_state.filehandles, next) {
        if (gfd->id == id) {
            return gfd->handle;
        }
    }

    error_setg(err, "handle '%" PRId64 "' has not been found", id);
    return NULL;
}

void guest_file_handle_remove(int64_t id)
{
    GuestFileData *gfd = NULL;

    QTAILQ_FOREACH(gfd, &guest_file_state.filehandles, next) {
        if (gfd->id == id) {
            break;
        }
    }

    if (gfd) {
        QTAILQ_REMOVE(&guest_file_state.filehandles, gfd, next);
        g_free(gfd);
    }
}
