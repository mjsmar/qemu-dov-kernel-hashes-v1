/*
 * QEMU Guest Agent common/cross-platform command implementations
 *
 * Copyright IBM Corp. 2012
 *
 * Authors:
 *  Michael Roth      <mdroth@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include <glib.h>
#include "qga/guest-agent-core.h"
#include "qga-qmp-commands.h"
#include "qapi/qmp/qerror.h"
#include "qga/guest-file-command-state.h"
#if 0
#ifndef _WIN32
#include <sys/wait.h>
#include <signal.h>
#endif
#endif

/* Note: in some situations, like with the fsfreeze, logging may be
 * temporarilly disabled. if it is necessary that a command be able
 * to log for accounting purposes, check ga_logging_enabled() beforehand,
 * and use the QERR_QGA_LOGGING_DISABLED to generate an error
 */
void slog(const gchar *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    g_logv("syslog", G_LOG_LEVEL_INFO, fmt, ap);
    va_end(ap);
}

int64_t qmp_guest_sync_delimited(int64_t id, Error **errp)
{
    ga_set_response_delimited(ga_state);
    return id;
}

int64_t qmp_guest_sync(int64_t id, Error **errp)
{
    return id;
}

void qmp_guest_ping(Error **errp)
{
    slog("guest-ping called");
}

static void qmp_command_info(QmpCommand *cmd, void *opaque)
{
    GuestAgentInfo *info = opaque;
    GuestAgentCommandInfo *cmd_info;
    GuestAgentCommandInfoList *cmd_info_list;

    cmd_info = g_malloc0(sizeof(GuestAgentCommandInfo));
    cmd_info->name = g_strdup(qmp_command_name(cmd));
    cmd_info->enabled = qmp_command_is_enabled(cmd);
    cmd_info->success_response = qmp_has_success_response(cmd);

    cmd_info_list = g_malloc0(sizeof(GuestAgentCommandInfoList));
    cmd_info_list->value = cmd_info;
    cmd_info_list->next = info->supported_commands;
    info->supported_commands = cmd_info_list;
}

struct GuestAgentInfo *qmp_guest_info(Error **errp)
{
    GuestAgentInfo *info = g_malloc0(sizeof(GuestAgentInfo));

    info->version = g_strdup(QEMU_VERSION);
    qmp_for_each_command(qmp_command_info, info);
    return info;
}

static struct {
    QTAILQ_HEAD(, GuestExecInfo) processes;
    int64_t next_handle;
} guest_exec_state;

typedef struct GuestExecInfo {
    GPid gpid;
    char *cmdline;
    int64_t handle; /* this should be UUID */
    gint fd_in;
    gint fd_out;
    gint fd_err;
    bool reaped;
    GuestExecStatus last_exec_status;
    QTAILQ_ENTRY(GuestExecInfo) next;
} GuestExecInfo;

static void guest_exec_init(void)
{
    QTAILQ_INIT(&guest_exec_state.processes);
    guest_exec_state.next_handle = 0;
}

static GuestExecInfo *guest_exec_info_new(GPid gpid, const char *cmdline,
                                          gint fd_in, gint fd_out, gint fd_err)
{
    GuestExecInfo *gei;

    gei = g_new(GuestExecInfo, 1);
    gei->gpid = gpid;
    gei->cmdline = g_strdup(cmdline);
    gei->fd_in = fd_in;
    gei->fd_out = fd_out;
    gei->fd_err = fd_err;
    gei->handle = -1;
    gei->reaped = false;

    return gei;
}

/* TODO: USE UUIDs! */
static int64_t guest_exec_info_register(GuestExecInfo *gei)
{
    gei->handle = guest_exec_state.next_handle++;
    QTAILQ_INSERT_TAIL(&guest_exec_state.processes, gei, next);

    return gei->handle;
}

static GuestExecInfo *guest_exec_info_find(int64_t handle)
{
    GuestExecInfo *gei;

    QTAILQ_FOREACH(gei, &guest_exec_state.processes, next)
    {
        if (gei->handle == handle) {
            return gei;
        }
    }

    return NULL;
}

static void guest_exec_info_remove(int64_t handle)
{
    GuestExecInfo *gei = guest_exec_info_find(handle);

    if (!gei) {
        return;
    }

    QTAILQ_REMOVE(&guest_exec_state.processes, gei, next);
    g_free(gei->cmdline);
    g_free(gei);
}

#ifndef _WIN32
GuestExecStatus *qmp_guest_exec_status(int64_t handle, bool has_wait, bool wait,
                                       bool has_timeout, int64_t timeout,
                                       Error **errp)
{
    GuestExecInfo *gei;
    GuestExecStatus *ges;
    int status, ret, i = 0;

    gei = guest_exec_info_find(handle);
    if (!gei) {
        error_setg(errp, "process not found for handle %"PRId64, handle);
        return NULL;
    }

    if (gei->reaped) {
        /* already reaped, so return previous status instead of waiting */
        ges = g_new0(GuestExecStatus, 1);
        ges->exited = gei->last_exec_status.exited;
        ges->exit_code = gei->last_exec_status.exit_code;
        return ges;
    }

    if (has_wait && wait && has_timeout && timeout > 0) {
        while (1) {
            if (i++ >= timeout) {
                error_setg(errp,
                           "exceeded %"PRId64" ms timeout waiting for process",
                           timeout);
                return NULL;
            }

            ret = waitpid(gei->gpid, &status, WNOHANG);
            if (ret == -1) {
                error_setg_errno(errp, errno, "waitpid error, pid: %d", gei->gpid);
                return NULL;
            }
            if (WIFEXITED(status)) {
                break;
            }
            usleep(1000);
        }
    } else {
        ret = waitpid(gei->gpid, &status, (has_wait && wait) ? 0 : WNOHANG);
        if (ret == -1) {
            error_setg_errno(errp, errno, "waitpid error, pid: %d", gei->gpid);
            return NULL;
        }
    }

    ges = g_new0(GuestExecStatus, 1);
    ges->handle = gei->handle;
    if (ret > 0 && WIFEXITED(status)) {
        ges->exited = true;
        ges->exit_code = WEXITSTATUS(status);
        gei->last_exec_status.exit_code = true;
        gei->last_exec_status.exited = true;
        gei->reaped = true;
    } else {
        ges->exited = false;
        ges->exit_code = 0;
    }

    return ges;
}

#else
GuestExecStatus *qmp_guest_exec_status(int64_t handle, bool has_wait, bool wait,
                                       bool has_timeout, int64_t timeout,
                                       Error **errp)
{
    DWORD p_exit_code = 0;
    bool ret;
    GuestExecInfo *gei;
    GuestExecStatus *ges;
    int64_t duration = 0;

    gei = guest_exec_info_find(handle);
    if (!gei) {
        error_setg(errp, "process not found for handle %"PRId64, handle);
        return NULL;
    }

    if (gei->reaped) {
        /* already reaped, return previous exec status */
        ges = g_new0(GuestExecStatus, 1);
        ges->exited = gei->last_exec_status.exited;
        ges->exit_code = gei->last_exec_status.exit_code;
    }

    do {
        ret = GetExitCodeProcess(gei->gpid, &p_exit_code);
        if (!ret) {
            error_setg(errp, "failed to obtain process exit code: %d",
                       (int)GetLastError());
            return NULL;
        }
        Sleep(1);
        if (has_timeout && duration > timeout) {
            break;
        } duration++; } while (has_wait && wait && p_exit_code == STILL_ACTIVE);

    ges = g_new(GuestExecStatus, 1);
    /* TODO: expose actual handle? only seems useful on linux */
    ges->handle = gei->handle;
    if (p_exit_code == STILL_ACTIVE) {
        ges->exited = false;
        ges->exit_code = 0;
    } else {
        ges->exited = true;
        ges->exit_code = p_exit_code;
        gei->last_exec_status.exited = true;
        gei->last_exec_status.exit_code = true;
        gei->reaped = true;
    }

    return ges;
}
#endif /* WIN32 */

void qmp_guest_exec_close(int64_t handle, Error **errp)
{
    GuestExecInfo *gei;
    GuestExecStatus *ges;
    Error *local_err = NULL;

    ges = qmp_guest_exec_status(handle, false, false, false, 0, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        goto out;
    }

    if (ges->exited) {
        goto out;
    }
   
    gei = guest_exec_info_find(handle);
    if (!gei) {
        error_setg(errp, "process handle %"PRId64" not found", handle);
        return;
    }
#ifndef _WIN32
    kill(gei->gpid, SIGKILL);
#else
    TerminateProcess(gei->gpid, -1);
#endif

out:
    guest_exec_info_remove(handle);
}

#define QGA_DEBUG

static GuestExecInfo *guest_exec_spawn(const char *path, strList *args,
                                       bool interactive, Error **errp)
{
    GSpawnFlags default_flags;
    gboolean ret;
    GPid gpid;
    gchar **argv;
    GError *gerr = NULL;
    gint fd_in = -1, fd_out = -1, fd_err = -1;

    default_flags = G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD;
    default_flags |= G_SPAWN_CLOEXEC_PIPES;

    /* TODO: fix this obviously */
    int i;
    argv = g_new0(gchar *, 1024);
    strList *args_cur;
    argv[0] = (char *)path;
    argv[1] = NULL;
    for (i = 1, args_cur = args; args_cur; i++, args_cur = args_cur->next) {
        g_debug("processing arg: %s", args_cur->value);
        argv[i] = args_cur->value;
    }

    ret = g_spawn_async_with_pipes(NULL, argv, NULL,
                                   default_flags, NULL, NULL, &gpid,
                                   interactive ? &fd_in : NULL, &fd_out, &fd_err, &gerr);

    if (gerr) {
        error_setg(errp, "failed to execute command: %s, %s", path,
                   gerr->message);
        return NULL;
    }
    if (!ret) {
        error_setg(errp, "failed to execute command");
        return NULL;
    }

    return guest_exec_info_new(gpid, argv[0], fd_in, fd_out, fd_err);
}

#ifdef QGA_DEBUG
static void print_gei(GuestExecInfo *gei)
{
    g_print("gei->gpid: %d\n", (int)gei->gpid);
    g_print("gei->cmdline: %s\n", gei->cmdline);
    g_print("gei->handle: %d\n", (int)gei->handle);
    g_print("gei->fd_in: %d\n", (int)gei->fd_in);
    g_print("gei->fd_out: %d\n", (int)gei->fd_out);
    g_print("gei->fd_err: %d\n", (int)gei->fd_err);
}
#endif

GuestExecAsyncResponse *qmp_guest_exec_async(const char *path,
                                             bool has_args, strList *args,
                                             bool has_interactive,
                                             bool interactive,
                                             Error **errp)
{
    GuestExecAsyncResponse *ger;
    GuestExecInfo *gei;
    int32_t handle;
    Error *local_err = NULL;
    //FILE *fh;

    gei = guest_exec_spawn(path, has_args ? args : NULL,
                           has_interactive && interactive, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return NULL;
    }

    //print_gei(gei);
    ger = g_new0(GuestExecAsyncResponse, 1);

    if (has_interactive && interactive) {
        ger->has_handle_stdin = true;
        ger->handle_stdin =
            guest_file_handle_add_fd(gei->fd_in, "a", &local_err);
        if (local_err) {
            error_propagate(errp, local_err);
            return NULL;
        }
    }

    ger->has_handle_stdout = true;
    ger->handle_stdout =
            guest_file_handle_add_fd(gei->fd_out, "r", &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return NULL;
    }

    ger->has_handle_stderr = true;
    ger->handle_stderr =
            guest_file_handle_add_fd(gei->fd_err, "r", &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return NULL;
    }

    handle = guest_exec_info_register(gei);
    ger->status = qmp_guest_exec_status(handle, false, false, false, 0, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return NULL;
    }

    return ger;
}

typedef struct GuestExecPipeData {
    GIOChannel *chan; /* TODO: don't really need this */
    GString *buffer;
    gsize buffer_max;
    Error *err;
    bool done;
    int debug_id;
#ifdef _WIN32
    GPollFD pollfd;
#endif
} GuestExecPipeData;

static GuestExecPipeData *guest_exec_pipe_data_new(GIOChannel *chan,
                                                   int debug_id)
{
    GuestExecPipeData *pd = g_new0(GuestExecPipeData, 1);

    pd->chan = chan;
    pd->buffer = g_string_new("");
    pd->buffer_max = QGA_EXEC_BUFFER_MAX;
    pd->debug_id = debug_id;
    pd->done = false;
#ifdef _WIN32
    g_io_channel_win32_make_pollfd(chan,
                                   G_IO_IN | G_IO_OUT | G_IO_HUP | G_IO_ERR,
                                   &pd->pollfd);
    g_io_channel_set_encoding(chan, NULL, NULL);
    g_io_channel_set_buffered(chan, false);
#endif

    return pd;
}

static gboolean guest_exec_pipe_cb(GIOChannel *chan, GIOCondition condition,
                                   gpointer user_data)
{
    GuestExecPipeData *pd = user_data;
    gchar buf[1024+1];
    gsize count = 0;
    GError *gerr = NULL;
    GIOStatus gstatus = 0;
#ifdef _WIN32
    GIOCondition poll_condition = 0;
#endif

g_warning("cb called, debug_id: %d", pd->debug_id);
g_warning("marker 0");
    if (pd->done || pd->err) {
        return false;
    }

g_warning("marker 0a");
g_warning("condition: %d", (int)condition);
    if (!(condition & G_IO_IN)) {
g_warning("closed channel" );
        goto out;
    }

g_warning("marker 1");
    if (pd->buffer->len >= pd->buffer_max) {
g_warning("marker 1a");
        error_setg(&pd->err,
                   "error reading pipe, %"PRId64" buffer size limit exceeded",
                   (int64_t)pd->buffer_max);
        goto out;
    }

#ifdef _WIN32
    if (g_io_channel_win32_poll(&pd->pollfd, 1, 0)) {
        poll_condition = pd->pollfd.revents;
    }
    if (poll_condition & G_IO_IN) {
        gstatus = g_io_channel_read_chars(pd->chan, buf,
                                          MIN(pd->buffer_max - pd->buffer->len, 1024),
                                          &count, &gerr);
    }
g_warning("marker 1a.win32, poll_condition: %d", (int)poll_condition);
#else
g_warning("marker 1b");
    gstatus = g_io_channel_read_chars(pd->chan, buf,
                                      MIN(pd->buffer_max - pd->buffer->len, 1024),
                                      &count, &gerr);
#endif
g_warning("marker 1c");
    if (gerr) {
g_warning("marker 1d");
        error_setg(&pd->err, "error reading pipe: %s", gerr->message);
        g_error_free(gerr);
        goto out;
    }
g_warning("marker 2");

    switch (gstatus) {
g_warning("marker 3");
    case G_IO_STATUS_ERROR:
g_warning("marker 4");
        error_setg(&pd->err, "error reading pipe");
        /* fall through to channel teardown */
    case G_IO_STATUS_EOF:
g_warning("marker 5");
        break;
    case G_IO_STATUS_NORMAL:
g_warning("marker 6");
        g_string_append_len(pd->buffer, buf, count);
        buf[1024] = 0;
        g_print("current buffer:\n%s\n", buf);
        /* fall through to re-poll later */
    case G_IO_STATUS_AGAIN:
g_warning("marker 7");
        return true;
    }

    pd->done = true;

out:
g_warning("exiting handler for debug_id: %d", pd->debug_id);
    pd->done = true;
    //g_main_loop_quit(pd->main_loop);
    return false;
}

typedef struct GuestExecTimerData {
    bool timed_out;
    GSource *stdout_source;
    GSource *stderr_source;
    int64_t handle;
} GuestExecTimerData;

static gboolean guest_exec_timer_cb(gpointer opaque)
{
    GuestExecTimerData *timer_data = opaque;
    GuestExecInfo *gei = guest_exec_info_find(timer_data->handle);

    g_warning("process timed out");
    g_assert(gei);

    timer_data->timed_out = true;

    g_warning("closing process gpid: %d", (int)gei->gpid);
#ifndef _WIN32
    kill(gei->gpid, SIGKILL);
#else
    TerminateProcess(gei->gpid, -1);
#endif

    g_spawn_close_pid(gei->gpid);
    g_warning("done");
    g_source_destroy(timer_data->stdout_source);
    g_source_destroy(timer_data->stderr_source);

    return false;
}

static void guest_exec_buffer_output(int32_t handle, gchar **stdout_buf,
                                     gchar **stderr_buf, int64_t timeout,
                                     Error **errp)
{
    GMainContext *ctx;
    GIOChannel *stdout_chan, *stderr_chan;
    GSource *stdout_source, *stderr_source, *timer_source;
    GuestExecPipeData *stdout_pd, *stderr_pd;
    GuestExecInfo *gei = guest_exec_info_find(handle);
    GError *gerr = NULL;
    GuestExecTimerData timer_data = {
        .timed_out = false,
    };

#ifndef _WIN32
    stdout_chan = g_io_channel_unix_new(gei->fd_out);
    stderr_chan = g_io_channel_unix_new(gei->fd_err);
#else
#include <sys/stat.h>
    struct _stati64 st;
    if (_fstati64(gei->fd_out, &st) == -1) {
        g_warning("error stat()'ing stdout");
    }
    if (st.st_mode & _S_IFCHR) {
        g_warning("w32: stdout is a console fd");
    } else {
        g_warning("w32: stdout not a console fd");
    }
    if (_fstati64(gei->fd_err, &st) == -1) {
        g_warning("error stat()'ing stderr");
    }
    if (st.st_mode & _S_IFCHR) {
        g_warning("w32: stderr is a console fd");
    } else {
        g_warning("w32: stderr not a console fd");
    }
    stdout_chan = g_io_channel_win32_new_fd(gei->fd_out);
    stderr_chan = g_io_channel_win32_new_fd(gei->fd_err);
#endif
    /* TODO: shouldn't need this */
    /* TODO: doesn't work? */
    g_io_channel_set_flags(stdout_chan, G_IO_FLAG_NONBLOCK, &gerr);
    if (gerr) {
        g_warning("error setting non-blocking stdout for child");
    }
    g_io_channel_set_flags(stderr_chan, G_IO_FLAG_NONBLOCK, &gerr);
    if (gerr) {
        g_warning("error setting non-blocking stderr for child");
    }
    ctx = g_main_context_new();
    //loop = g_main_loop_new(ctx, false);

    stdout_pd = guest_exec_pipe_data_new(stdout_chan, 100);
    stderr_pd = guest_exec_pipe_data_new(stderr_chan, 101);
    stdout_source = g_io_create_watch(stdout_chan, G_IO_IN | G_IO_ERR | G_IO_HUP);
    stderr_source = g_io_create_watch(stderr_chan, G_IO_IN | G_IO_ERR | G_IO_HUP);
    timer_data.stdout_source = stdout_source;
    timer_data.stderr_source = stderr_source;
    timer_data.handle = handle;
    g_source_attach(stdout_source, ctx);
    g_source_attach(stderr_source, ctx);
    g_source_set_callback(stdout_source, (GSourceFunc)guest_exec_pipe_cb, stdout_pd, NULL);
    g_source_set_callback(stderr_source, (GSourceFunc)guest_exec_pipe_cb, stderr_pd, NULL);

    if (timeout) {
        timer_source = g_timeout_source_new(timeout);
        g_source_attach(timer_source, ctx);
        g_source_set_callback(timer_source, guest_exec_timer_cb, &timer_data, NULL);
        g_source_set_priority(timer_source, G_PRIORITY_HIGH);
    }

    while (!(stdout_pd->done && stderr_pd->done)) {
        if (timer_data.timed_out) {
            error_setg(errp, "process exceeded %"PRId64" ms timeout", timeout);
            return;
        }
        g_main_context_iteration(ctx, true);
    }

g_warning("dmarker 0");

    *stdout_buf = stdout_pd->buffer->str;
    *stderr_buf = stderr_pd->buffer->str;

g_warning("dmarker 1");

    /* TODO: cleanup pipe data */
}

GuestExecResponse *qmp_guest_exec(const char *path,
                                  bool has_args, strList *args,
                                  bool has_timeout, int64_t timeout,
                                  Error **errp)
{
    int64_t handle;
    GuestExecInfo *gei;
    GuestExecStatus *ges;
    GuestExecResponse *ger;
    gchar *stdout_buf = NULL, *stderr_buf = NULL;
    Error *local_err = NULL;
    Error *local_err2 = NULL;

    gei = guest_exec_spawn(path, has_args ? args : NULL, false, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        return NULL;
    }

    handle = guest_exec_info_register(gei);
#ifdef QGA_DEBUG
    print_gei(gei);
#endif

    /* TODO: select on fd in/out and buffer out, handle timeout.
     * call blocking exec_status when eof on both handle, or timeout
     * occurs
     */
    guest_exec_buffer_output(handle, &stdout_buf, &stderr_buf,
                             has_timeout ? timeout : 10000, &local_err);
    if (local_err) {
        error_propagate(errp, local_err);
        //local_err = NULL;
    }

    /* FIXME: this seems to still leave zombies behind on timeout */
    ges = qmp_guest_exec_status(handle, true, true, true, has_timeout ? timeout : 10000, &local_err2);
    if (local_err2) {
        error_propagate(errp, local_err2);
    }

    if (local_err || local_err2) {
        return NULL;
    }

    g_print("stdout: %s", stdout_buf);
    g_print("stderr: %s", stderr_buf);
    ger = g_new0(GuestExecResponse, 1);
    ger->status = ges;
    /* TODO: base64 encode the response */
    ger->has_stdout_buffer = true;
    ger->stdout_buffer = stdout_buf;
    ger->has_stderr_buffer = true;
    ger->stderr_buffer = stderr_buf;

    return ger;
}

/* register init/cleanup routines for stateful command groups */
void ga_command_state_init_common(GAState *s, GACommandState *cs)
{
    ga_command_state_add(cs, guest_exec_init, NULL);
}
