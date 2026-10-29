/* vortex-adbd: a small adb daemon for Vortex.
 *
 *   vortex-adbd [-u FFSDIR [-b GADGETDIR]] [-t [ADDR:]PORT]
 *
 *   -u FFSDIR     serve adb over USB through FunctionFS mounted at FFSDIR
 *                 (e.g. /dev/usb-ffs/adb)
 *   -b GADGETDIR  after writing the USB descriptors, bind the configfs
 *                 gadget to the UDC (the gadget cannot bind before)
 *   -t [ADDR:]PORT  also listen on TCP (adb connect PHONE:PORT); ADDR
 *                 defaults to all interfaces
 *
 * Services: shell: (interactive, on a pty), shell:CMD and exec:CMD (pipes),
 * sync: (push/pull/ls, protocol v1), reboot:[bootloader|recovery|edl].
 * Runs as root.  There is no authentication: it is a debugging tool for a
 * phone on your desk, like a developer build of Android with "ro.adb.secure=0".
 *
 * Design: one process, one poll loop.  The USB OUT endpoint has no poll(),
 * so a thread copies it into a socketpair and the loop treats USB and TCP
 * alike, as byte streams of adb messages.  Each adb stream is a local file
 * descriptor (pty, socketpair) served by a forked child.
 */
#include <dirent.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <pty.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>

#include <arpa/inet.h>
#include <linux/reboot.h>
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#define A_SYNC 0x434e5953
#define A_CNXN 0x4e584e43
#define A_OPEN 0x4e45504f
#define A_OKAY 0x59414b4f
#define A_CLSE 0x45534c43
#define A_WRTE 0x45545257
#define A_AUTH 0x48545541

#define A_VERSION   0x01000001  /* no payload checksums needed */
#define MAX_PAYLOAD (64 * 1024)
#define MAX_STREAMS 32
/* Largest USB request: the ChipIdea controller (ci_hdrc, MSM8909) and
 * others limit one transfer to 16 KiB, as Android's adbd assumes. */
#define USB_CHUNK   16384

#define BANNER "device::ro.product.name=vortex;ro.product.model=Nokia_6300_4G;" \
               "ro.product.device=leo;features="

struct amsg {
    uint32_t cmd, arg0, arg1, len, check, magic;
};

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("vortex-adbd: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

static int readx(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

static int writex(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n) {
        ssize_t r = write(fd, p, n);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return -1;
        p += r;
        n -= (size_t)r;
    }
    return 0;
}

/* ---- transport --------------------------------------------------------- */

static struct {
    int in_fd;          /* byte stream of messages from the host */
    int out_fd;         /* where messages to the host go */
    int is_usb;
    int connected;      /* CNXN seen */
    uint8_t *rbuf;      /* partial message */
    size_t rlen;
} T = { .in_fd = -1, .out_fd = -1 };

static int usb_write(const void *data, size_t n)
{
    const uint8_t *p = data;
    while (n) {
        size_t c = n < USB_CHUNK ? n : USB_CHUNK;
        if (writex(T.out_fd, p, c) < 0)
            return -1;
        p += c;
        n -= c;
    }
    return 0;
}

static int send_msg(uint32_t cmd, uint32_t arg0, uint32_t arg1, const void *data, uint32_t len)
{
    struct amsg m = { cmd, arg0, arg1, len, 0, cmd ^ 0xffffffffu };
    const uint8_t *d = data;
    for (uint32_t i = 0; i < len; i++)
        m.check += d[i];
    if (T.out_fd < 0)
        return -1;
    if (T.is_usb) {
        /* USB: header and payload are separate transfers */
        if (writex(T.out_fd, &m, sizeof(m)) < 0 || (len && usb_write(data, len) < 0)) {
            logf_("USB write failed: %s", strerror(errno));
            return -1;
        }
        return 0;
    }
    {
        uint8_t buf[sizeof(m) + MAX_PAYLOAD];
        memcpy(buf, &m, sizeof(m));
        if (len)
            memcpy(buf + sizeof(m), data, len);
        return writex(T.out_fd, buf, sizeof(m) + len);
    }
}

/* ---- streams ----------------------------------------------------------- */

struct stream {
    uint32_t id, remote;
    int fd;
    pid_t pid;
    int can_send;  /* the host acknowledged our last WRTE */
};

static struct stream S[MAX_STREAMS];
static uint32_t next_id = 1;

static struct stream *find_stream(uint32_t id)
{
    for (int i = 0; i < MAX_STREAMS; i++)
        if (S[i].fd >= 0 && S[i].id == id)
            return &S[i];
    return NULL;
}

static void close_stream(struct stream *s)
{
    if (s->fd < 0)
        return;
    close(s->fd);
    if (s->pid > 0)
        kill(-s->pid, SIGHUP);
    s->fd = -1;
    s->pid = 0;
}

static void close_all_streams(void)
{
    for (int i = 0; i < MAX_STREAMS; i++)
        close_stream(&S[i]);
}

static void child_setup(void)
{
    signal(SIGCHLD, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    setenv("HOME", "/root", 1);
    setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/usr/libexec/vortex", 1);
    setenv("TERM", "xterm-256color", 0);
    if (chdir("/root") != 0 && chdir("/") != 0) {
        /* nothing better to do */
    }
}

static const char *login_shell(void)
{
    return access("/bin/bash", X_OK) == 0 ? "/bin/bash" : "/bin/sh";
}

/* Interactive shell on a pty. */
static int svc_shell_pty(pid_t *pid)
{
    int master;
    *pid = forkpty(&master, NULL, NULL, NULL);
    if (*pid < 0)
        return -1;
    if (*pid == 0) {
        const char *sh = login_shell();
        child_setup();
        execl(sh, sh, "-l", (char *)NULL);
        _exit(127);
    }
    return master;
}

/* Run a function or a command in a child connected by a socketpair. */
static int svc_fork(pid_t *pid, void (*fn)(int fd, const char *arg), const char *arg)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0)
        return -1;
    *pid = fork();
    if (*pid < 0) {
        close(sv[0]);
        close(sv[1]);
        return -1;
    }
    if (*pid == 0) {
        setsid();
        child_setup();
        close(sv[0]);
        if (T.in_fd >= 0)
            close(T.in_fd);
        fn(sv[1], arg);
        _exit(0);
    }
    close(sv[1]);
    return sv[0];
}

static void run_command(int fd, const char *cmd)
{
    dup2(fd, 0);
    dup2(fd, 1);
    dup2(fd, 2);
    if (fd > 2)
        close(fd);
    execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
    _exit(127);
}

static void say(int fd, const char *msg)
{
    writex(fd, msg, strlen(msg));
}

static void do_reboot(int fd, const char *arg)
{
    sync();
    if (!*arg) {
        say(fd, "rebooting\n");
        execl("/bin/sh", "sh", "-c", "reboot || reboot -f", (char *)NULL);
        _exit(1);
    }
    /* reboot into bootloader (lk2nd fastboot), recovery or edl */
    say(fd, "rebooting\n");
    sleep(1);
    syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_RESTART2, arg);
    say(fd, "reboot failed\n");
}

static void unsupported(int fd, const char *svc)
{
    say(fd, "vortex-adbd: not supported: ");
    say(fd, svc);
    say(fd, "\n");
}

/* ---- sync (adb push / pull / ls) --------------------------------------- */

#define ID(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define ID_STAT ID('S', 'T', 'A', 'T')
#define ID_LIST ID('L', 'I', 'S', 'T')
#define ID_DENT ID('D', 'E', 'N', 'T')
#define ID_SEND ID('S', 'E', 'N', 'D')
#define ID_RECV ID('R', 'E', 'C', 'V')
#define ID_DATA ID('D', 'A', 'T', 'A')
#define ID_DONE ID('D', 'O', 'N', 'E')
#define ID_OKAY ID('O', 'K', 'A', 'Y')
#define ID_FAIL ID('F', 'A', 'I', 'L')
#define ID_QUIT ID('Q', 'U', 'I', 'T')
#define SYNC_DATA_MAX (64 * 1024)

static int sync_fail(int fd, const char *why)
{
    uint32_t h[2] = { ID_FAIL, (uint32_t)strlen(why) };
    if (writex(fd, h, sizeof(h)) < 0)
        return -1;
    return writex(fd, why, h[1]);
}

static int sync_stat(int fd, const char *path)
{
    struct stat st;
    uint32_t r[4] = { ID_STAT, 0, 0, 0 };
    if (lstat(path, &st) == 0) {
        r[1] = st.st_mode;
        r[2] = (uint32_t)st.st_size;
        r[3] = (uint32_t)st.st_mtime;
    }
    return writex(fd, r, sizeof(r));
}

static int sync_list(int fd, const char *path)
{
    DIR *d = opendir(path);
    struct dirent *e;
    uint32_t done[5] = { ID_DONE, 0, 0, 0, 0 };
    while (d && (e = readdir(d))) {
        char full[PATH_MAX];
        struct stat st;
        uint32_t h[5];
        size_t n = strlen(e->d_name);
        snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
        if (lstat(full, &st) != 0)
            continue;
        h[0] = ID_DENT;
        h[1] = st.st_mode;
        h[2] = (uint32_t)st.st_size;
        h[3] = (uint32_t)st.st_mtime;
        h[4] = (uint32_t)n;
        if (writex(fd, h, sizeof(h)) < 0 || writex(fd, e->d_name, n) < 0) {
            closedir(d);
            return -1;
        }
    }
    if (d)
        closedir(d);
    return writex(fd, done, sizeof(done));
}

static void mkdirs(char *path)
{
    for (char *p = path + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = 0;
        mkdir(path, 0755);
        *p = '/';
    }
}

static int sync_send(int fd, char *spec, uint8_t *buf)
{
    char *comma = strrchr(spec, ',');
    mode_t mode = 0644;
    int out, err = 0;
    char why[256] = "";

    if (comma) {
        *comma = 0;
        mode = (mode_t)strtoul(comma + 1, NULL, 0) & 0777;
    }
    mkdirs(spec);
    unlink(spec);
    out = open(spec, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode ? mode : 0644);
    if (out < 0) {
        err = 1;
        snprintf(why, sizeof(why), "cannot create %s: %s", spec, strerror(errno));
    }
    for (;;) {
        uint32_t h[2];
        if (readx(fd, h, sizeof(h)) < 0)
            return -1;
        if (h[0] == ID_DATA) {
            if (h[1] > SYNC_DATA_MAX || readx(fd, buf, h[1]) < 0)
                return -1;
            if (!err && writex(out, buf, h[1]) < 0) {
                err = 1;
                snprintf(why, sizeof(why), "write %s: %s", spec, strerror(errno));
            }
        } else if (h[0] == ID_DONE) {
            uint32_t ok[2] = { ID_OKAY, 0 };
            if (out >= 0) {
                struct utimbuf ut = { (time_t)h[1], (time_t)h[1] };
                if (close(out) != 0 && !err) {
                    err = 1;
                    snprintf(why, sizeof(why), "close %s: %s", spec, strerror(errno));
                }
                utime(spec, &ut);
            }
            if (err) {
                unlink(spec);
                return sync_fail(fd, why);
            }
            return writex(fd, ok, sizeof(ok));
        } else {
            return -1;
        }
    }
}

static int sync_recv(int fd, const char *path, uint8_t *buf)
{
    int in = open(path, O_RDONLY | O_CLOEXEC);
    uint32_t done[2] = { ID_DONE, 0 };
    if (in < 0) {
        char why[PATH_MAX + 64];
        snprintf(why, sizeof(why), "cannot open %s: %s", path, strerror(errno));
        return sync_fail(fd, why);
    }
    for (;;) {
        ssize_t n = read(in, buf + 8, SYNC_DATA_MAX);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            close(in);
            return sync_fail(fd, strerror(errno));
        }
        if (n == 0)
            break;
        ((uint32_t *)buf)[0] = ID_DATA;
        ((uint32_t *)buf)[1] = (uint32_t)n;
        if (writex(fd, buf, (size_t)n + 8) < 0) {
            close(in);
            return -1;
        }
    }
    close(in);
    return writex(fd, done, sizeof(done));
}

static void svc_sync(int fd, const char *unused)
{
    static uint8_t buf[SYNC_DATA_MAX + 8];
    char path[PATH_MAX + 32];
    (void)unused;
    for (;;) {
        uint32_t h[2];
        int r;
        if (readx(fd, h, sizeof(h)) < 0 || h[0] == ID_QUIT)
            return;
        if (h[1] >= sizeof(path) || readx(fd, path, h[1]) < 0)
            return;
        path[h[1]] = 0;
        switch (h[0]) {
        case ID_STAT: r = sync_stat(fd, path); break;
        case ID_LIST: r = sync_list(fd, path); break;
        case ID_SEND: r = sync_send(fd, path, buf); break;
        case ID_RECV: r = sync_recv(fd, path, buf); break;
        default: r = sync_fail(fd, "unknown sync command"); break;
        }
        if (r < 0)
            return;
    }
}

/* ---- protocol ---------------------------------------------------------- */

static int open_service(const char *svc, pid_t *pid)
{
    *pid = 0;
    if (!strcmp(svc, "shell:") || !strcmp(svc, "shell"))
        return svc_shell_pty(pid);
    if (!strncmp(svc, "shell:", 6))
        return svc_fork(pid, run_command, svc + 6);
    if (!strncmp(svc, "exec:", 5))
        return svc_fork(pid, run_command, svc + 5);
    if (!strcmp(svc, "sync:"))
        return svc_fork(pid, svc_sync, NULL);
    if (!strncmp(svc, "reboot:", 7))
        return svc_fork(pid, do_reboot, svc + 7);
    if (!strncmp(svc, "tcpip:", 6) || !strcmp(svc, "usb:") || !strcmp(svc, "root:") ||
        !strcmp(svc, "unroot:") || !strncmp(svc, "remount", 7))
        return svc_fork(pid, unsupported, svc);
    return -1;
}

static void reset_connection(void)
{
    close_all_streams();
    T.connected = 0;
    T.rlen = 0;
}

static void handle(const struct amsg *m, uint8_t *data)
{
    struct stream *s;

    switch (m->cmd) {
    case A_CNXN:
        logf_("CNXN from host: version %08x, max payload %u", m->arg0, m->arg1);
        close_all_streams();
        T.connected = 1;
        send_msg(A_CNXN, A_VERSION, MAX_PAYLOAD, BANNER, sizeof(BANNER) - 1);
        logf_("host connected");
        break;
    case A_OPEN: {
        char svc[1024];
        size_t n = m->len < sizeof(svc) - 1 ? m->len : sizeof(svc) - 1;
        pid_t pid;
        int fd, slot = -1;
        memcpy(svc, data, n);
        svc[n] = 0;
        logf_("open %s", svc);
        for (int i = 0; i < MAX_STREAMS && slot < 0; i++)
            if (S[i].fd < 0)
                slot = i;
        fd = slot >= 0 ? open_service(svc, &pid) : -1;
        if (fd < 0) {
            send_msg(A_CLSE, 0, m->arg0, NULL, 0);
            break;
        }
        fcntl(fd, F_SETFD, FD_CLOEXEC);
        S[slot] = (struct stream){ next_id++, m->arg0, fd, pid, 1 };
        send_msg(A_OKAY, S[slot].id, S[slot].remote, NULL, 0);
        break;
    }
    case A_WRTE:
        s = find_stream(m->arg1);
        if (!s) {
            send_msg(A_CLSE, 0, m->arg0, NULL, 0);
            break;
        }
        if (m->len && writex(s->fd, data, m->len) < 0) {
            send_msg(A_CLSE, s->id, s->remote, NULL, 0);
            close_stream(s);
            break;
        }
        send_msg(A_OKAY, s->id, s->remote, NULL, 0);
        break;
    case A_OKAY:
        s = find_stream(m->arg1);
        if (s)
            s->can_send = 1;
        break;
    case A_CLSE:
        s = find_stream(m->arg1);
        if (s)
            close_stream(s);
        break;
    default:
        /* AUTH, SYNC, STLS: never requested by this daemon */
        break;
    }
}

/* Consume complete messages from T.rbuf. */
static void parse_input(void)
{
    size_t off = 0;
    while (T.rlen - off >= sizeof(struct amsg)) {
        struct amsg m;
        memcpy(&m, T.rbuf + off, sizeof(m));
        if (m.magic != (m.cmd ^ 0xffffffffu) || m.len > MAX_PAYLOAD) {
            logf_("bad message header, resetting");
            reset_connection();
            return;
        }
        if (T.rlen - off < sizeof(m) + m.len)
            break;
        handle(&m, T.rbuf + off + sizeof(m));
        off += sizeof(m) + m.len;
    }
    memmove(T.rbuf, T.rbuf + off, T.rlen - off);
    T.rlen -= off;
}

/* ---- USB (FunctionFS) -------------------------------------------------- */

struct func_desc {
    struct usb_interface_descriptor intf;
    struct usb_endpoint_descriptor_no_audio source, sink;
} __attribute__((packed));

static const struct {
    struct usb_functionfs_descs_head_v2 header;
    __le32 fs_count, hs_count;
    struct func_desc fs, hs;
} __attribute__((packed)) descriptors = {
    .header = {
        .magic = FUNCTIONFS_DESCRIPTORS_MAGIC_V2,
        .length = sizeof(descriptors),
        .flags = FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC,
    },
    .fs_count = 3,
    .hs_count = 3,
#define FUNC(mps) { \
        .intf = { .bLength = sizeof(struct usb_interface_descriptor), \
                  .bDescriptorType = USB_DT_INTERFACE, .bNumEndpoints = 2, \
                  .bInterfaceClass = 0xff, .bInterfaceSubClass = 0x42, \
                  .bInterfaceProtocol = 1, .iInterface = 1 }, \
        .source = { .bLength = sizeof(struct usb_endpoint_descriptor_no_audio), \
                    .bDescriptorType = USB_DT_ENDPOINT, .bEndpointAddress = 1 | USB_DIR_OUT, \
                    .bmAttributes = USB_ENDPOINT_XFER_BULK, .wMaxPacketSize = mps }, \
        .sink = { .bLength = sizeof(struct usb_endpoint_descriptor_no_audio), \
                  .bDescriptorType = USB_DT_ENDPOINT, .bEndpointAddress = 2 | USB_DIR_IN, \
                  .bmAttributes = USB_ENDPOINT_XFER_BULK, .wMaxPacketSize = mps }, \
    }
    .fs = FUNC(64),
    .hs = FUNC(512),
#undef FUNC
};

#define STR_INTERFACE "ADB Interface"
static const struct {
    struct usb_functionfs_strings_head header;
    struct {
        __le16 code;
        char str[sizeof(STR_INTERFACE)];
    } __attribute__((packed)) lang0;
} __attribute__((packed)) strings = {
    .header = {
        .magic = FUNCTIONFS_STRINGS_MAGIC,
        .length = sizeof(strings),
        .str_count = 1,
        .lang_count = 1,
    },
    .lang0 = { 0x0409, STR_INTERFACE },
};

static struct {
    int ep0, ep_out, ep_in;
    int pipe_w;  /* reader thread -> poll loop */
} U = { -1, -1, -1, -1 };

/* Read one USB transfer of exactly n bytes (header or payload), like
 * Android's adbd: the request then always ends where the host's transfer
 * does, whatever the controller does with short packets. */
static int usb_read_exact(uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        size_t want = n - got < USB_CHUNK ? n - got : USB_CHUNK;
        ssize_t r = read(U.ep_out, buf + got, want);
        if (r < 0 && errno == EINTR)
            continue;
        if (r <= 0)
            return -1;
        got += (size_t)r;
    }
    return 0;
}

static void *usb_reader(void *unused)
{
    static uint8_t buf[MAX_PAYLOAD + sizeof(struct amsg)];
    int errors = 0;
    (void)unused;
    for (;;) {
        struct amsg m;
        if (usb_read_exact(buf, sizeof(m)) < 0) {
            if (errors++ == 0)
                logf_("USB read failed: %s", strerror(errno));
            usleep(100000);  /* not enabled / cable out */
            continue;
        }
        errors = 0;
        memcpy(&m, buf, sizeof(m));
        if (m.magic != (m.cmd ^ 0xffffffffu) || m.len > MAX_PAYLOAD) {
            logf_("USB: bad header %08x, dropped", m.cmd);
            continue;
        }
        if (m.len && usb_read_exact(buf + sizeof(m), m.len) < 0) {
            logf_("USB payload read failed: %s", strerror(errno));
            continue;
        }
        if (writex(U.pipe_w, buf, sizeof(m) + m.len) < 0)
            return NULL;
    }
    return NULL;
}

static int usb_init(const char *ffs, const char *gadget)
{
    char path[PATH_MAX];
    int sv[2];
    pthread_t th;

    snprintf(path, sizeof(path), "%s/ep0", ffs);
    U.ep0 = open(path, O_RDWR | O_CLOEXEC);
    if (U.ep0 < 0) {
        logf_("%s: %s", path, strerror(errno));
        return -1;
    }
    if (writex(U.ep0, &descriptors, sizeof(descriptors)) < 0 ||
        writex(U.ep0, &strings, sizeof(strings)) < 0) {
        logf_("writing USB descriptors: %s", strerror(errno));
        return -1;
    }
    snprintf(path, sizeof(path), "%s/ep1", ffs);
    U.ep_out = open(path, O_RDONLY | O_CLOEXEC);
    snprintf(path, sizeof(path), "%s/ep2", ffs);
    U.ep_in = open(path, O_WRONLY | O_CLOEXEC);
    if (U.ep_out < 0 || U.ep_in < 0) {
        logf_("opening endpoints: %s", strerror(errno));
        return -1;
    }
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) < 0)
        return -1;
    U.pipe_w = sv[1];
    T.in_fd = sv[0];
    T.out_fd = U.ep_in;
    T.is_usb = 1;
    if (pthread_create(&th, NULL, usb_reader, NULL) != 0)
        return -1;

    if (gadget) {
        /* the function is ready: attach the gadget to the USB controller */
        char udc[64] = "";
        DIR *d = opendir("/sys/class/udc");
        struct dirent *e;
        while (d && (e = readdir(d)))
            if (e->d_name[0] != '.')
                snprintf(udc, sizeof(udc), "%s", e->d_name);
        if (d)
            closedir(d);
        snprintf(path, sizeof(path), "%s/UDC", gadget);
        {
            int fd = open(path, O_WRONLY | O_CLOEXEC);
            if (fd < 0 || !*udc || writex(fd, udc, strlen(udc)) < 0)
                logf_("binding %s to UDC '%s' failed: %s", gadget, udc, strerror(errno));
            if (fd >= 0)
                close(fd);
        }
    }
    logf_("USB ready");
    return 0;
}

static void usb_ep0_event(void)
{
    struct usb_functionfs_event ev[4];
    ssize_t n = read(U.ep0, ev, sizeof(ev));
    for (int i = 0; n > 0 && i < (int)(n / (ssize_t)sizeof(ev[0])); i++) {
        switch (ev[i].type) {
        case FUNCTIONFS_ENABLE:
            logf_("USB enabled");
            reset_connection();
            break;
        case FUNCTIONFS_DISABLE:
        case FUNCTIONFS_UNBIND:
            logf_("USB disabled");
            reset_connection();
            break;
        case FUNCTIONFS_SETUP:
            /* no class requests: stall them */
            if (ev[i].u.setup.bRequestType & USB_DIR_IN) {
                if (read(U.ep0, NULL, 0) < 0) { /* stalled */ }
            } else if (write(U.ep0, NULL, 0) < 0) { /* stalled */ }
            break;
        default:
            break;
        }
    }
}

/* ---- TCP --------------------------------------------------------------- */

static int tcp_listen(const char *spec)
{
    struct sockaddr_in a = { .sin_family = AF_INET };
    const char *colon = strrchr(spec, ':');
    int port = atoi(colon ? colon + 1 : spec);
    int one = 1, fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;
    a.sin_port = htons((uint16_t)port);
    if (colon) {
        char host[64];
        snprintf(host, sizeof(host), "%.*s", (int)(colon - spec), spec);
        if (inet_pton(AF_INET, host, &a.sin_addr) != 1) {
            logf_("bad address %s", host);
            close(fd);
            return -1;
        }
    }
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(fd, 2) < 0) {
        logf_("TCP %s: %s", spec, strerror(errno));
        close(fd);
        return -1;
    }
    logf_("listening on TCP %s", spec);
    return fd;
}

/* ---- main loop --------------------------------------------------------- */

int main(int argc, char **argv)
{
    const char *ffs = NULL, *gadget = NULL, *tcp = NULL;
    int lfd = -1, opt;
    static uint8_t rbuf[2 * (MAX_PAYLOAD + sizeof(struct amsg))];

    while ((opt = getopt(argc, argv, "u:b:t:")) != -1) {
        switch (opt) {
        case 'u': ffs = optarg; break;
        case 'b': gadget = optarg; break;
        case 't': tcp = optarg; break;
        default:
            fprintf(stderr, "usage: vortex-adbd [-u FFSDIR [-b GADGETDIR]] [-t [ADDR:]PORT]\n");
            return 2;
        }
    }
    if (!ffs && !tcp) {
        fprintf(stderr, "vortex-adbd: nothing to serve (use -u and/or -t)\n");
        return 2;
    }
    for (int i = 0; i < MAX_STREAMS; i++)
        S[i].fd = -1;
    T.rbuf = rbuf;
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);  /* children are reaped automatically */

    if (ffs && usb_init(ffs, gadget) != 0)
        return 1;
    if (tcp && (lfd = tcp_listen(tcp)) < 0 && !ffs)
        return 1;

    for (;;) {
        struct pollfd p[MAX_STREAMS + 3];
        int np = 0, i_in = -1, i_ep0 = -1, i_l = -1, map[MAX_STREAMS];

        if (T.in_fd >= 0) {
            i_in = np;
            p[np++] = (struct pollfd){ T.in_fd, POLLIN, 0 };
        }
        if (U.ep0 >= 0) {
            i_ep0 = np;
            p[np++] = (struct pollfd){ U.ep0, POLLIN, 0 };
        }
        if (lfd >= 0) {
            i_l = np;
            p[np++] = (struct pollfd){ lfd, POLLIN, 0 };
        }
        for (int i = 0; i < MAX_STREAMS; i++) {
            map[i] = -1;
            if (S[i].fd >= 0 && S[i].can_send && T.connected) {
                map[i] = np;
                p[np++] = (struct pollfd){ S[i].fd, POLLIN, 0 };
            }
        }
        if (poll(p, (nfds_t)np, -1) < 0) {
            if (errno == EINTR)
                continue;
            return 1;
        }

        if (i_ep0 >= 0 && p[i_ep0].revents)
            usb_ep0_event();

        if (i_l >= 0 && (p[i_l].revents & POLLIN)) {
            int c = accept4(lfd, NULL, NULL, SOCK_CLOEXEC), one = 1;
            if (c >= 0) {
                /* one host at a time; a TCP client takes over from USB */
                reset_connection();
                if (!T.is_usb && T.in_fd >= 0)
                    close(T.in_fd);
                setsockopt(c, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                T.in_fd = T.out_fd = c;
                T.is_usb = 0;
                logf_("TCP client connected");
            }
            continue;  /* the fd set changed */
        }

        if (i_in >= 0 && p[i_in].revents) {
            ssize_t n = read(T.in_fd, T.rbuf + T.rlen, sizeof(rbuf) - T.rlen);
            if (n > 0) {
                T.rlen += (size_t)n;
                parse_input();
            } else if (!T.is_usb) {
                /* TCP client gone: fall back to USB if we have it */
                logf_("TCP client disconnected");
                reset_connection();
                close(T.in_fd);
                T.in_fd = T.out_fd = -1;
                if (ffs) {
                    int sv[2];
                    /* the USB reader writes into U.pipe_w; recreate its peer */
                    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0) {
                        int old = U.pipe_w;
                        U.pipe_w = sv[1];
                        close(old);
                        T.in_fd = sv[0];
                        T.out_fd = U.ep_in;
                        T.is_usb = 1;
                    }
                }
            }
            continue;  /* streams may have been closed */
        }

        for (int i = 0; i < MAX_STREAMS; i++) {
            static uint8_t buf[MAX_PAYLOAD];
            ssize_t n;
            if (map[i] < 0 || !p[map[i]].revents || S[i].fd < 0)
                continue;
            n = read(S[i].fd, buf, sizeof(buf));
            if (n > 0) {
                S[i].can_send = 0;
                send_msg(A_WRTE, S[i].id, S[i].remote, buf, (uint32_t)n);
            } else if (n == 0 || (errno != EINTR && errno != EAGAIN)) {
                send_msg(A_CLSE, S[i].id, S[i].remote, NULL, 0);
                close_stream(&S[i]);
            }
        }
    }
}
