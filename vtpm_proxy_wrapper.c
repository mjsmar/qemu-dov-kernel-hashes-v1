/*
 * Wrapper for communicating wth an emulator backend through
 * tpm_vtpm_proxy kernel module
 *
 * Example usage:
 *
 *
 * build:
 *
 *   $ gcc vtpm_proxy_wrapper.c -o vtpm_proxy_wrapper
 *
 *
 * start emulator:
 *
 *   $ ibm-tpm2/src/tpm_server
 *   TPM command server listening on port 2321
 *   Platform server listening on port 2322
 *
 *
 * create /dev/tpmX /dev/tpmrmX and connect them to emulator via TCP:
 *
 *   $ sudo modprobe tpm_vtpm_proxy
 *   $ ./vtpm_proxy_wrapper -h
 *   ...
 *   $ sudo ./vtpm_proxy_wrapper 192.168.2.158 2321 mssim 1
 *   vtpm_proxy created at /dev/tpm1 and /dev/tpmrm1
 *   ...
 *   read request from VTPM_PROXY client (fd: 4, raw size: 11)
 *   sending request to emulator (wrapped size: 20)
 *   reading response from emulator
 *   sending response to VTPM_PROXY client (raw size: 10)
 *   ...
 *
 *
 * allow pseries guest to send TPM commands through H_TPM_COMM:
 *
 *   qemu-system-ppc64 -machine pseries,tpm-device-file=/dev/tpmrm1 ...
 *
 *
 * directly execute TPM commands via ibm-tpm2-tss:
 *
 *   $ sudo -s 
 *   $ export TPM_INTERFACE_TYPE=dev
 *   $ export TPM_SERVER_TYPE=raw
 *   $ export TPM_DEVICE=/dev/tpmrm1
 *   $ cd ibm-tpm2-tss/utils
 *   $ ./pcrextend -ha 4 -ic "test"
 *   $ ./pcrread -ha 4
 *   count 1 pcrUpdateCounter 24 
 *    digest length 32
 *    29 c3 26 1d 57 fd 59 43 27 1a ed 13 90 5d 66 d5 
 *    7b 51 2a 50 5b a8 aa eb 27 56 dc 3d 80 a5 64 5d 
 *
 *
 * Copyright IBM Corp. 2019
 *
 * Authors:
 *  Michael Roth      <mdroth@linux.vnet.ibm.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#define _GNU_SOURCE 1
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>
#include <error.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <sys/param.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <endian.h>
#include <signal.h>
#include <linux/vtpm_proxy.h>

#define VTPM_PROXY_CTRL_PATH "/dev/vtpmx"
#define TPM_BUFFER_SZ 4096

static int verbosity = 0;
static bool stopped = false;

#define dprintf(fmt...) do { if (verbosity > 0) printf(fmt); } while (0)

struct MSSimHeader {
    uint32_t cmd;
    uint8_t locality;
    uint32_t size;
} __attribute__((packed)); 

typedef struct MSSimHeader MSSimHeader;

struct MSSimRespHeader {
    uint32_t size;
} __attribute__((packed)); 

typedef struct MSSimRespHeader MSSimRespHeader;

static void handle_sigint(int signum)
{
    stopped = true;
}

void usage(const char *name)
{
    printf("%s <emulator host> <emulator port> [raw|mssim(default)] [$verbosity]\n"
           "  verbosity=0: default\n"
           "  verbosity=1: debug (to stdout)\n"
           "  verbosity=2: debug (to stdout) + print req/resp buffers (to stderr)\n",
           name);
}

static void dump_buf(const char *desc, void *buf, size_t offset, ssize_t count)
{
    if (verbosity < 2)
        return;

    fprintf(stderr, "=== %s ===\n", desc);
    while (count > 0) {
        int start = offset % 8;
        int i;

        fprintf(stderr, "  ");
        if (start > 0) {
            int i;
            fprintf(stderr, "%08lx: ", (offset / 8) * 8);
            for (i = 0; i < start; i++) {
                fprintf(stderr, "   ");
            }
        } else {
            fprintf(stderr, "%08lx: ", offset);
        }

        for (i = start; i < 8 && i < count; i++) {
            fprintf(stderr, "%02x ", *(uint8_t *)(buf + offset + i - start));
        }
        fprintf(stderr, "\n");
        offset += i - start;
        count -= i - start;
    }
}

static ssize_t read_uninterrupted(int fd, uint8_t *buf, size_t size)
{
    ssize_t ret;

    do {
         ret = read(fd, buf, size);
    } while (ret == 0 || (ret == -1 && (errno == EINTR && !stopped)));

    return ret;
}

static ssize_t write_uninterrupted(int fd, const uint8_t *buf, size_t size)
{
    ssize_t ret;

    do {
        ret = write(fd, buf, size);
    } while (ret == 0 || (ret == -1 && errno == EINTR));

    return ret;
}

static ssize_t send_uninterrupted(int fd, const uint8_t *buf, size_t size)
{
    ssize_t sent = 0;

    do {
        ssize_t count;
        count = send(fd, buf, size - sent, 0);
        if (count == -1) {
            /* we shouldn't get EAGAIN for !O_NONBLOCK, but just in case */
            if (errno != EWOULDBLOCK && errno != EAGAIN
                && errno != EINTR) {
                return -1;
            }
            continue;
        }
        sent += count;
        buf += count;
    } while (sent != size);

    return sent;
}

static ssize_t recv_uninterrupted_min(int fd, uint8_t *buf, size_t size, size_t min_size)
{
    ssize_t received = 0;

    do {
        ssize_t count;
        count = recv(fd, buf, size - received, 0);
        if (count == -1) {
            /* we shouldn't get EAGAIN for !O_NONBLOCK, but just in case */
            if (errno != EWOULDBLOCK && errno != EAGAIN
                && errno != EINTR) {
                error(0, errno, "failed to read response from emulator");
                return -1;
            }
            continue;
        }
        received += count;
        buf += count;
    } while (received < min_size);

    return received;
}

static int intercept_set_locality(int proxy_fd)
{
    uint8_t req_buf[TPM_BUFFER_SZ];
    uint8_t fake_resp_buf[] = {
        0x80, 0x01, 0x00, 0x00,
        0x00, 0x0a, 0x00, 0x00,
        0x00, 0x00
    };
    ssize_t req_sz, sent;

    /* read request from proxy client */
    req_sz = read_uninterrupted(proxy_fd, req_buf, TPM_BUFFER_SZ);
    if (req_sz == -1) {
        error(0, errno, "failed to read command from VTPM_PROXY client");
        return -1;
    }

    sent = write_uninterrupted(proxy_fd, fake_resp_buf, 10);
    if (sent == -1) {
        error(0, errno, "failed to send response to VTPM_PROXY client");
    }

    return 0;
}

/*
 * forward a single request from vtpm_proxy to emulator and return the
 * response to vtpm_proxy
 *
 * returns 0 on success, 1 if stop requested, -1 on error
 */
static int process_proxy_cmd(int proxy_fd, int emulator_fd, bool mssim)
{
    uint8_t req_buf[TPM_BUFFER_SZ + sizeof(MSSimHeader)];
    uint8_t resp_buf[TPM_BUFFER_SZ + sizeof(MSSimHeader)];
    uint8_t *buf_ptr;
    ssize_t req_sz, sent = 0, received = 0, expected_resp_sz = 0, raw_resp_sz;
    ssize_t req_hdr_sz = 0, resp_hdr_sz = 0;

    /* leave room for MS/IBM req/resp headers */
    if (mssim) {
        req_hdr_sz = sizeof(MSSimHeader);
        resp_hdr_sz = sizeof(MSSimRespHeader);
    }

    /* read request from proxy client */
    buf_ptr = req_buf + req_hdr_sz;
    req_sz = read_uninterrupted(proxy_fd, buf_ptr, TPM_BUFFER_SZ);
    if (req_sz == -1) {
        if (!(errno == EINTR && stopped)) {
            error(0, errno, "failed to read command from VTPM_PROXY client");
            return -1;
        } else {
            return 1;
        }
    }

    dprintf("read request from VTPM_PROXY client (fd: %d, raw size: %zu)\n",
            proxy_fd, req_sz);
    dump_buf("raw request", buf_ptr, 0, req_sz);

    if (mssim) {
        MSSimHeader *mshdr = (MSSimHeader *)req_buf;
        mshdr->cmd = htobe32(8); //send command
        mshdr->locality = 0;
        mshdr->size = htobe32(req_sz);
    }

    /* forward request to emulator */
    dprintf("sending request to emulator (wrapped size: %zd)\n",
            req_sz + req_hdr_sz);
    buf_ptr = req_buf;
    dump_buf("wrapped request", buf_ptr, 0, req_hdr_sz + req_sz);


    sent = send_uninterrupted(emulator_fd, buf_ptr, req_hdr_sz + req_sz);
    if (sent == -1) { 
        error(0, errno, "failed to send command to emulator");
        return -1;
    }

    /* get emulator response */
    dprintf("reading response from emulator\n");
    buf_ptr = resp_buf;

    /* read enough to get the response length from header */
    received = recv_uninterrupted_min(emulator_fd, buf_ptr,
                                      TPM_BUFFER_SZ + resp_hdr_sz,
                                      resp_hdr_sz + 6);

    /* get resp size from resp header */
    expected_resp_sz = be32toh(*(uint32_t *)&resp_buf[resp_hdr_sz + 2]);
    expected_resp_sz += resp_hdr_sz;
    if (mssim) {
        expected_resp_sz += 4; // MSSim sentinel value?
    }

    /* read the rest of the request */
    if (received < expected_resp_sz) {
        dprintf("reading remaining response from emulator\n");
        buf_ptr += received;
        received = recv_uninterrupted_min(emulator_fd, buf_ptr,
                                          expected_resp_sz - received,
                                          expected_resp_sz - received);
    }

    dump_buf("wrapped response", resp_buf, 0, expected_resp_sz);

    /* send response to proxy client */
    if (mssim) {
        buf_ptr = &resp_buf[resp_hdr_sz];
        raw_resp_sz = expected_resp_sz - resp_hdr_sz - 4;
    } else {
        buf_ptr = resp_buf;
        raw_resp_sz = expected_resp_sz;
    }

    dprintf("sending response to VTPM_PROXY client (raw size: %zu)\n",
            raw_resp_sz);
    dump_buf("raw response", buf_ptr, 0, raw_resp_sz);

    sent = write_uninterrupted(proxy_fd, buf_ptr, raw_resp_sz);
#if 0
    do {
        sent = write(proxy_fd, buf_ptr, raw_resp_sz);
    } while (sent == 0 || (sent == -1 && errno == EINTR));
#endif

    if (sent == -1) {
        error(0, errno, "failed to send response to VTPM_PROXY client");
    }

    return 0;
}

/* connect to emulator via TCP and return session fd */
static int emulator_connect(const char *host, int port)
{
    int sockfd, ret;
    struct addrinfo *ai_list, *ai;
    struct addrinfo hintai = {
        .ai_family = AF_INET, //AF_UNSPEC to include ipv6
        .ai_socktype = SOCK_STREAM,
        .ai_flags = 0,
        .ai_protocol = 0,
    };
    char port_str[8];

    sprintf(port_str, "%d", port);

    ret = getaddrinfo(host, port_str, &hintai, &ai_list);
    if (ret == -1) {
        error(0, errno, "getaddrinfo() failed");
        return -1;
    }

    for (ai = ai_list; ai != NULL; ai = ai->ai_next) {
        sockfd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sockfd == -1) {
            error(0, errno, "socket() failed");
            goto out;
        }

        ret = connect(sockfd, ai->ai_addr, ai->ai_addrlen);
        if (ret == -1) {
            error(0, errno, "connect() failed");
            goto out;
        } else {
            ret = sockfd;
            break;
        }
    }

out:
    freeaddrinfo(ai_list);
    return ret;
}

/* create character device front-end and get a handle */
static int vtpm_proxy_new(void)
{
    struct vtpm_proxy_new_dev vtpm_proxy = { 0 };
    int ctrl_fd, ret;

    ctrl_fd = open(VTPM_PROXY_CTRL_PATH, O_RDWR);
    if (ctrl_fd == -1) {
        error(0, errno, "unable to open vtpm_proxy control path (%s)",
              VTPM_PROXY_CTRL_PATH);
        return -1;
    }

    vtpm_proxy.flags = VTPM_PROXY_FLAG_TPM2;
    ret = ioctl(ctrl_fd, VTPM_PROXY_IOC_NEW_DEV, &vtpm_proxy);
    if (ret == -1) {
        error(0, errno, "failed to execute vtpm_proxy ioctl");
        goto out_close;
    }

    printf("vtpm_proxy created at /dev/tpm%d and /dev/tpmrm%d\n",
           vtpm_proxy.tpm_num, vtpm_proxy.tpm_num);

    ret = vtpm_proxy.fd;

out_close:
    close(ctrl_fd);
    return ret;
}

int main(int argc, char **argv)
{
    const char *host;
    int port, ret, proxy_fd, emulator_fd;
    bool use_mssim = true;
    bool skip_locality = true;

    if (argc < 3) {
        usage(argv[0]);
        return (argc > 1 && !strcmp(argv[1], "-h")) ? 0 : -1;
    }

    host = argv[1];
    port = atoi(argv[2]);

    if (argc > 3) {
        if (!strcmp(argv[3], "raw")) {
            use_mssim = false;
        } else if (!strcmp(argv[3], "mssim")) {
            use_mssim = true;
        } else {
            error(0, 0, "invalid mode argument: \"%s\"", argv[3]);
            usage(argv[0]);
            return -1;
        }
    }

    if (argc > 4) {
        verbosity = atoi(argv[4]);
        if (verbosity < 0) {
            error(-1, 0, "invalid verbosity argument: \"%d\"", verbosity);
        }
    }

    proxy_fd = vtpm_proxy_new();
    if (proxy_fd == -1) {
        error(-1, 0, "failed to create vtpm_proxy instance");
    }

    emulator_fd = emulator_connect(host, port);
    if (emulator_fd == -1) {
        close(proxy_fd);
        error(-1, 0, "failed to connect to emulator at host %s, port %d",
              host, port);
        usage(argv[0]);
        return -1;
    }

    /*
     * TODO: VTPM_PROXY sends a number of commands to the emulator backend as
     * part of creating the character device. The first is a
     * TPM2_CC_SET_LOCALITY, which the IBM TPM simulator fails to handle,
     * instead returning a TPM2_RC_COMMAND_CODE error code. This causes the
     * vtpm_proxy chardev to cease functioning (returning EPIPE on reads).
     *
     * It's not clear yet why this TPM2_CC_SET_LOCALITY errors on the
     * emulator size, but for now just intercept it and send a fake success
     * response to VTPM_PROXY.
     */
    if (skip_locality)
        intercept_set_locality(proxy_fd);

    /*
     * TODO: getting signalled during a request/response exchange currently
     * causes instability/crashes in tpm_vtpm_proxy module (possibly due to
     * TPM command cancellation handling). work around this by catching
     * signals and handling it gracefully (i.e. finish the current req/resp
     * pair before exiting), but should probably try to fix module crash
     * at some point.
     *
     * Still get the following issue however if TSS client is still sending
     * commands to tpmrmX while vtpm_proxy is exiting:
     *
     * [40833.051353] tpm tpm1: tpm_transmit: tpm_recv: error -32
     * [40833.051380] tpm tpm1: tpm2_save_context: failed with a system error -32
     * [40833.051500] tpm tpm1: tpm2_load_context: failed with a system error -32
     * [40833.051540] tpm tpm1: tpm2_load_context: failed with a system error -3
     *
     * This seems to be the result of TSS trying to write to tpmrmX, but since
     * the device has been set to !OPENED an EPIPE is returned, which fails
     * the tpm2_load_context in tpmrmX driver (and in some cases a subsequent
     * tpm2_flush_context is made which may generate a crash).
     *
     * Subsequent tpmrm_write() -> put_device() -> tpm_device_delete() path
     * then causes in some (most, apparently) cases:
     *
     * [  472.479035] kernel BUG at /build/linux-uQJ2um/linux-4.15.0/mm/slub.c:296!
     * [  472.479057] invalid opcode: 0000 [#1] SMP PTI
     * ...
     * [  472.479596] Call Trace:
     * [  472.479617]  ? default_wake_function+0x12/0x20
     * [  472.479645]  ? autoremove_wake_function+0x12/0x40
     * [  472.479661]  ? __wake_up_common+0x73/0x130
     * [  472.479680]  ? tpm_dev_release+0x66/0x70
     * [  472.479697]  kfree+0x165/0x180
     * [  472.479708]  ? kfree+0x165/0x180
     * [  472.479721]  tpm_dev_release+0x66/0x70
     * [  472.479733]  device_release+0x35/0x90
     * [  472.479747]  kobject_release+0x6a/0x180
     * [  472.479759]  kobject_put+0x28/0x50
     * [  472.479771]  put_device+0x17/0x20
     * [  472.479783]  tpm_try_get_ops+0x44/0x50
     * [  472.479805]  tpm_common_write+0x91/0x150
     * [  472.479825]  tpmrm_write+0x1c/0x20
     * [  472.479838]  __vfs_write+0x1b/0x40
     *
     * This can be avoided for now by making sure to stop clients from sending
     * commands before terminating this program.
     */
    if (signal(SIGINT, handle_sigint) == SIG_ERR)
        error(-1, errno, "failed to set SIGINT handler");
    if (signal(SIGTERM, handle_sigint) == SIG_ERR)
        error(-1, errno, "failed to set SIGTERM handler");
    if (signal(SIGQUIT, handle_sigint) == SIG_ERR)
        error(-1, errno, "failed to set SIGQUIT handler");

    /* process TPM commands from vtpm_proxy client */
    do {
        ret = process_proxy_cmd(proxy_fd, emulator_fd, use_mssim);
    } while (ret == 0 && !stopped);

    if (ret == -1) {
        error(0, 0, "proxy died unexpectedly\n");
    } else {
        printf("exiting: closing vtpm_proxy device and emulator connection\n");
    }

    close(emulator_fd);
    close(proxy_fd);
    return ret;
}
