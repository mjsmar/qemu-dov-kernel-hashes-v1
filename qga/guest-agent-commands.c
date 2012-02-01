/*
 * QEMU Guest Agent commands
 *
 * Copyright IBM Corp. 2011
 *
 * Authors:
 *  Michael Roth      <mdroth@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include <glib.h>

#if defined(__linux__)
#include <mntent.h>
#include <linux/fs.h>

#if defined(__linux__) && defined(FIFREEZE)
#define CONFIG_FSFREEZE
#endif
#endif

#include <sys/types.h>
#include <sys/ioctl.h>
#include "qga/guest-agent-core.h"
#include "qga-qmp-commands.h"
#include "qerror.h"
#include "qemu-queue.h"

static GAState *ga_state;

/* Note: in some situations, like with the fsfreeze, logging may be
 * temporarilly disabled. if it is necessary that a command be able
 * to log for accounting purposes, check ga_logging_enabled() beforehand,
 * and use the QERR_QGA_LOGGING_DISABLED to generate an error
 */
static void slog(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    g_logv("syslog", G_LOG_LEVEL_INFO, fmt, ap);
    va_end(ap);
}

int64_t qmp_guest_sync(int64_t id, Error **errp)
{
    return id;
}

void qmp_guest_ping(Error **err)
{
    slog("guest-ping called");
}

struct GuestAgentInfo *qmp_guest_info(Error **err)
{
    GuestAgentInfo *info = g_malloc0(sizeof(GuestAgentInfo));
    GuestAgentCommandInfo *cmd_info;
    GuestAgentCommandInfoList *cmd_info_list;
    char **cmd_list_head, **cmd_list;

    info->version = g_strdup(QGA_VERSION);

    cmd_list_head = cmd_list = qmp_get_command_list();
    if (*cmd_list_head == NULL) {
        goto out;
    }

    while (*cmd_list) {
        cmd_info = g_malloc0(sizeof(GuestAgentCommandInfo));
        cmd_info->name = strdup(*cmd_list);
        cmd_info->enabled = qmp_command_is_enabled(cmd_info->name);

        cmd_info_list = g_malloc0(sizeof(GuestAgentCommandInfoList));
        cmd_info_list->value = cmd_info;
        cmd_info_list->next = info->supported_commands;
        info->supported_commands = cmd_info_list;

        g_free(*cmd_list);
        cmd_list++;
    }

out:
    g_free(cmd_list_head);
    return info;
}

void qmp_guest_shutdown(bool has_mode, const char *mode, Error **err)
{
    int ret;
    const char *shutdown_flag;

    slog("guest-shutdown called, mode: %s", mode);
    if (!has_mode || strcmp(mode, "powerdown") == 0) {
        shutdown_flag = "-P";
    } else if (strcmp(mode, "halt") == 0) {
        shutdown_flag = "-H";
    } else if (strcmp(mode, "reboot") == 0) {
        shutdown_flag = "-r";
    } else {
        error_set(err, QERR_INVALID_PARAMETER_VALUE, "mode",
                  "halt|powerdown|reboot");
        return;
    }

    ret = fork();
    if (ret == 0) {
        /* child, start the shutdown */
        setsid();
        fclose(stdin);
        fclose(stdout);
        fclose(stderr);

        ret = execl("/sbin/shutdown", "shutdown", shutdown_flag, "+0",
                    "hypervisor initiated shutdown", (char*)NULL);
        if (ret) {
            slog("guest-shutdown failed: %s", strerror(errno));
        }
        exit(!!ret);
    } else if (ret < 0) {
        error_set(err, QERR_UNDEFINED_ERROR);
    }
}

typedef struct GuestFileHandle {
    uint64_t id;
    FILE *fh;
    QTAILQ_ENTRY(GuestFileHandle) next;
} GuestFileHandle;

static struct {
    QTAILQ_HEAD(, GuestFileHandle) filehandles;
} guest_file_state;

static void guest_file_handle_add(FILE *fh)
{
    GuestFileHandle *gfh;

    gfh = g_malloc0(sizeof(GuestFileHandle));
    gfh->id = fileno(fh);
    gfh->fh = fh;
    QTAILQ_INSERT_TAIL(&guest_file_state.filehandles, gfh, next);
}

static GuestFileHandle *guest_file_handle_find(int64_t id)
{
    GuestFileHandle *gfh;

    QTAILQ_FOREACH(gfh, &guest_file_state.filehandles, next)
    {
        if (gfh->id == id) {
            return gfh;
        }
    }

    return NULL;
}

int64_t qmp_guest_file_open(const char *path, bool has_mode, const char *mode, Error **err)
{
    FILE *fh;
    int fd;
    int64_t ret = -1;

    if (!has_mode) {
        mode = "r";
    }
    slog("guest-file-open called, filepath: %s, mode: %s", path, mode);
    fh = fopen(path, mode);
    if (!fh) {
        error_set(err, QERR_OPEN_FILE_FAILED, path);
        return -1;
    }

    /* set fd non-blocking to avoid common use cases (like reading from a
     * named pipe) from hanging the agent
     */
    fd = fileno(fh);
    ret = fcntl(fd, F_GETFL);
    ret = fcntl(fd, F_SETFL, ret | O_NONBLOCK);
    if (ret == -1) {
        error_set(err, QERR_QGA_COMMAND_FAILED, "fcntl() failed");
        fclose(fh);
        return -1;
    }

    guest_file_handle_add(fh);
    slog("guest-file-open, handle: %d", fd);
    return fd;
}

void qmp_guest_file_close(int64_t handle, Error **err)
{
    GuestFileHandle *gfh = guest_file_handle_find(handle);
    int ret;

    slog("guest-file-close called, handle: %ld", handle);
    if (!gfh) {
        error_set(err, QERR_FD_NOT_FOUND, "handle");
        return;
    }

    ret = fclose(gfh->fh);
    if (ret == -1) {
        error_set(err, QERR_QGA_COMMAND_FAILED, "fclose() failed");
        return;
    }

    QTAILQ_REMOVE(&guest_file_state.filehandles, gfh, next);
    g_free(gfh);
}

struct GuestFileRead *qmp_guest_file_read(int64_t handle, bool has_count,
                                          int64_t count, Error **err)
{
    GuestFileHandle *gfh = guest_file_handle_find(handle);
    GuestFileRead *read_data = NULL;
    guchar *buf;
    FILE *fh;
    size_t read_count;

    if (!gfh) {
        error_set(err, QERR_FD_NOT_FOUND, "handle");
        return NULL;
    }

    if (!has_count) {
        count = QGA_READ_COUNT_DEFAULT;
    } else if (count < 0) {
        error_set(err, QERR_INVALID_PARAMETER, "count");
        return NULL;
    }

    fh = gfh->fh;
    buf = g_malloc0(count+1);
    read_count = fread(buf, 1, count, fh);
    if (ferror(fh)) {
        slog("guest-file-read failed, handle: %ld", handle);
        error_set(err, QERR_QGA_COMMAND_FAILED, "fread() failed");
    } else {
        buf[read_count] = 0;
        read_data = g_malloc0(sizeof(GuestFileRead));
        read_data->count = read_count;
        read_data->eof = feof(fh);
        if (read_count) {
            read_data->buf_b64 = g_base64_encode(buf, read_count);
        }
    }
    g_free(buf);
    clearerr(fh);

    return read_data;
}

GuestFileWrite *qmp_guest_file_write(int64_t handle, const char *buf_b64,
                                     bool has_count, int64_t count, Error **err)
{
    GuestFileWrite *write_data = NULL;
    guchar *buf;
    gsize buf_len;
    int write_count;
    GuestFileHandle *gfh = guest_file_handle_find(handle);
    FILE *fh;

    if (!gfh) {
        error_set(err, QERR_FD_NOT_FOUND, "handle");
        return NULL;
    }

    fh = gfh->fh;
    buf = g_base64_decode(buf_b64, &buf_len);

    if (!has_count) {
        count = buf_len;
    } else if (count < 0 || count > buf_len) {
        g_free(buf);
        error_set(err, QERR_INVALID_PARAMETER, "count");
        return NULL;
    }

    write_count = fwrite(buf, 1, count, fh);
    if (ferror(fh)) {
        slog("guest-file-write failed, handle: %ld", handle);
        error_set(err, QERR_QGA_COMMAND_FAILED, "fwrite() error");
    } else {
        write_data = g_malloc0(sizeof(GuestFileWrite));
        write_data->count = write_count;
        write_data->eof = feof(fh);
    }
    g_free(buf);
    clearerr(fh);

    return write_data;
}

struct GuestFileSeek *qmp_guest_file_seek(int64_t handle, int64_t offset,
                                          int64_t whence, Error **err)
{
    GuestFileHandle *gfh = guest_file_handle_find(handle);
    GuestFileSeek *seek_data = NULL;
    FILE *fh;
    int ret;

    if (!gfh) {
        error_set(err, QERR_FD_NOT_FOUND, "handle");
        return NULL;
    }

    fh = gfh->fh;
    ret = fseek(fh, offset, whence);
    if (ret == -1) {
        error_set(err, QERR_QGA_COMMAND_FAILED, strerror(errno));
    } else {
        seek_data = g_malloc0(sizeof(GuestFileRead));
        seek_data->position = ftell(fh);
        seek_data->eof = feof(fh);
    }
    clearerr(fh);

    return seek_data;
}

void qmp_guest_file_flush(int64_t handle, Error **err)
{
    GuestFileHandle *gfh = guest_file_handle_find(handle);
    FILE *fh;
    int ret;

    if (!gfh) {
        error_set(err, QERR_FD_NOT_FOUND, "handle");
        return;
    }

    fh = gfh->fh;
    ret = fflush(fh);
    if (ret == EOF) {
        error_set(err, QERR_QGA_COMMAND_FAILED, strerror(errno));
    }
}

static void guest_file_init(void)
{
    QTAILQ_INIT(&guest_file_state.filehandles);
}

#if defined(CONFIG_FSFREEZE)

typedef struct GuestFsfreezeMount {
    char *dirname;
    char *devtype;
    QTAILQ_ENTRY(GuestFsfreezeMount) next;
} GuestFsfreezeMount;

typedef QTAILQ_HEAD(, GuestFsfreezeMount) GuestFsfreezeMountList;

static void guest_fsfreeze_free_mount_list(GuestFsfreezeMountList *mounts)
{
    GuestFsfreezeMount *mount, *temp;

    if (!mounts) {
        return;
    }

    QTAILQ_FOREACH_SAFE(mount, mounts, next, temp) {
        QTAILQ_REMOVE(mounts, mount, next);
        g_free(mount->dirname);
        g_free(mount->devtype);
        g_free(mount);
    }
}

/*
 * Walk the mount table and build a list of local file systems
 */
static int guest_fsfreeze_build_mount_list(GuestFsfreezeMountList *mounts)
{
    struct mntent *ment;
    GuestFsfreezeMount *mount;
    char const *mtab = MOUNTED;
    FILE *fp;

    fp = setmntent(mtab, "r");
    if (!fp) {
        g_warning("fsfreeze: unable to read mtab");
        return -1;
    }

    while ((ment = getmntent(fp))) {
        /*
         * An entry which device name doesn't start with a '/' is
         * either a dummy file system or a network file system.
         * Add special handling for smbfs and cifs as is done by
         * coreutils as well.
         */
        if ((ment->mnt_fsname[0] != '/') ||
            (strcmp(ment->mnt_type, "smbfs") == 0) ||
            (strcmp(ment->mnt_type, "cifs") == 0)) {
            continue;
        }

        mount = g_malloc0(sizeof(GuestFsfreezeMount));
        mount->dirname = g_strdup(ment->mnt_dir);
        mount->devtype = g_strdup(ment->mnt_type);

        QTAILQ_INSERT_TAIL(mounts, mount, next);
    }

    endmntent(fp);

    return 0;
}

/*
 * Return status of freeze/thaw
 */
GuestFsfreezeStatus qmp_guest_fsfreeze_status(Error **err)
{
    if (ga_is_frozen(ga_state)) {
        return GUEST_FSFREEZE_STATUS_FROZEN;
    }

    return GUEST_FSFREEZE_STATUS_THAWED;
}

/*
 * Walk list of mounted file systems in the guest, and freeze the ones which
 * are real local file systems.
 */
int64_t qmp_guest_fsfreeze_freeze(Error **err)
{
    int ret = 0, i = 0;
    GuestFsfreezeMountList mounts;
    struct GuestFsfreezeMount *mount;
    int fd;
    char err_msg[512];

    slog("guest-fsfreeze called");

    QTAILQ_INIT(&mounts);
    ret = guest_fsfreeze_build_mount_list(&mounts);
    if (ret < 0) {
        return ret;
    }

    /* cannot risk guest agent blocking itself on a write in this state */
    ga_set_frozen(ga_state);

    QTAILQ_FOREACH(mount, &mounts, next) {
        fd = qemu_open(mount->dirname, O_RDONLY);
        if (fd == -1) {
            sprintf(err_msg, "failed to open %s, %s", mount->dirname,
                    strerror(errno));
            error_set(err, QERR_QGA_COMMAND_FAILED, err_msg);
            goto error;
        }

        /* we try to cull filesytems we know won't work in advance, but other
         * filesytems may not implement fsfreeze for less obvious reasons.
         * these will report EOPNOTSUPP, we simply ignore these when tallying
         * the number of frozen filesystems.
         *
         * Anything otherwise means a failure to freeze a filesystem we
         * expect to be freezable, so return an error in those cases
         * and return system to thawed state.
         */
        ret = ioctl(fd, FIFREEZE);
        if (ret == -1) {
            if (errno != EOPNOTSUPP) {
                sprintf(err_msg, "failed to freeze %s, %s",
                        mount->dirname, strerror(errno));
                error_set(err, QERR_QGA_COMMAND_FAILED, err_msg);
                close(fd);
                goto error;
            }
        } else {
            i++;
        }
        close(fd);

    }

    guest_fsfreeze_free_mount_list(&mounts);
    return i;

error:
    guest_fsfreeze_free_mount_list(&mounts);
    if (i > 0) {
        qmp_guest_fsfreeze_thaw(NULL);
    }
    return 0;
}

/*
 * Walk list of frozen file systems in the guest, and thaw them.
 */
int64_t qmp_guest_fsfreeze_thaw(Error **err)
{
    int ret;
    GuestFsfreezeMountList mounts;
    GuestFsfreezeMount *mount;
    int fd, i = 0, logged;

    QTAILQ_INIT(&mounts);
    ret = guest_fsfreeze_build_mount_list(&mounts);
    if (ret) {
        error_set(err, QERR_QGA_COMMAND_FAILED,
                  "failed to enumerate filesystems");
        return 0;
    }

    QTAILQ_FOREACH(mount, &mounts, next) {
        logged = false;
        fd = qemu_open(mount->dirname, O_RDONLY);
        if (fd == -1) {
            continue;
        }
        /* we have no way of knowing whether a filesystem was actually unfrozen
         * by the current call to FITHAW, only that if an error was returned,
         * the filesystem was *not* unfrozen by that particular call.
         *
         * Since multiple preceeding FIFREEZEs require multiple calls to FITHAW
         * to unfreeze, continuing issuing FITHAW until an error is returned,
         * in which case either the filesystem is in an unfreezable state, or,
         * more likely, it was thawed previously (and is now).
         *
         * Also, since the last previously successful call is the one that did
         * the actual unfreeze, we can use this to provide an accurate count of
         * the number of filesystems unfrozen by guest-fsfreeze-thaw, which may
         * be useful for determining whether a filesystem was unfrozen during
         * the freeze/thaw phase by another application/user.
         *
         */
        do {
            ret = ioctl(fd, FITHAW);
            if (ret == 0 && !logged) {
                i++;
                logged = true;
            }
        } while (ret == 0);
        close(fd);
    }

    ga_unset_frozen(ga_state);
    guest_fsfreeze_free_mount_list(&mounts);
    return i;
}

#else
/*
 * Walk list of mounted file systems in the guest, and freeze the ones which
 * are real local file systems.
 */
int64_t qmp_guest_fsfreeze_freeze(Error **err)
{
    error_set(err, QERR_UNSUPPORTED);

    return 0;
}

/*
 * Walk list of frozen file systems in the guest, and thaw them.
 */
int64_t qmp_guest_fsfreeze_thaw(Error **err)
{
    error_set(err, QERR_UNSUPPORTED);

    return 0;
}
#endif

/* register init/cleanup routines for stateful command groups */
void ga_command_state_init(GAState *s, GACommandState *cs)
{
    ga_state = s;
    ga_command_state_add(cs, guest_file_init, NULL);
}
