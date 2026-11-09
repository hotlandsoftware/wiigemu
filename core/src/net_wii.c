/*
 * What upstream's netplay code (netplay.c, netplay_kaillera.c,
 * netplay_lists.c, verbatim) needs from the system that libogc doesn't
 * have: getaddrinfo() (IPv4 only, as the Wii's network is), select() (on
 * newlib's fd_set, over libogc's poll()), gemu/util.h's config directory -
 * sd:/gemu/ (or usb:/gemu/) - and the monitor's printf, which nothing
 * calls here (the Wii has no monitor).
 *
 * libogc's socket() hands out ordinary file descriptors (its "soc" device,
 * set up by net_init), so close(), send() and recv() work as they are.
 * network.h is kept out of this file: its fd_set isn't newlib's.
 */
#include "gemu/util.h"
#include "gemu/monitor.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ── getaddrinfo ─────────────────────────────────────────────────────────── */

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints,
                struct addrinfo **res)
{
    *res = NULL;
    if (hints && hints->ai_family != AF_UNSPEC && hints->ai_family != AF_INET) return EAI_FAMILY;
    struct in_addr ip;
    if (!node) {
        ip.s_addr = htonl(INADDR_ANY);
    } else if (!inet_aton(node, &ip)) {
        struct hostent *h = gethostbyname(node);
        if (!h || h->h_addrtype != AF_INET || !h->h_addr_list || !h->h_addr_list[0]) return EAI_NONAME;
        memcpy(&ip, h->h_addr_list[0], sizeof(ip));
    }
    long port = service ? strtol(service, NULL, 10) : 0;
    if (port < 0 || port > 65535) return EAI_SERVICE;

    struct { struct addrinfo ai; struct sockaddr_in sa; } *r = calloc(1, sizeof(*r));
    if (!r) return EAI_MEMORY;
    r->sa.sin_family = AF_INET;
    r->sa.sin_len = sizeof(r->sa);
    r->sa.sin_port = htons((uint16_t)port);
    r->sa.sin_addr = ip;
    r->ai.ai_family = AF_INET;
    r->ai.ai_socktype = hints ? hints->ai_socktype : 0;
    r->ai.ai_protocol = hints ? hints->ai_protocol : 0;
    r->ai.ai_addrlen = sizeof(r->sa);
    r->ai.ai_addr = (struct sockaddr *)&r->sa;
    *res = &r->ai;
    return 0;
}

void freeaddrinfo(struct addrinfo *ai)
{
    free(ai);        /* one block: the addrinfo and its address */
}

/* ── select, over poll ───────────────────────────────────────────────────── *
 * poll()'s own timeout can't be relied on (in Dolphin it waits for the next
 * packet however long that is, so a quiet session thread stops sending its
 * keepalives), so this polls without waiting, every couple of
 * milliseconds, until something is ready or the time is up. */

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

int select(int nfds, fd_set *rd, fd_set *wr, fd_set *ex, struct timeval *tv)
{
    struct pollfd p[FD_SETSIZE];
    int n = 0;
    for (int fd = 0; fd < nfds && fd < FD_SETSIZE; fd++) {
        short ev = 0;
        if (rd && FD_ISSET(fd, rd)) ev |= POLLIN;
        if (wr && FD_ISSET(fd, wr)) ev |= POLLOUT;
        if (ex && FD_ISSET(fd, ex)) ev |= POLLPRI;
        if (ev) p[n++] = (struct pollfd){ .fd = fd, .events = ev };
    }
    const uint64_t end = tv ? now_us() + (uint64_t)tv->tv_sec * 1000000u + (uint64_t)tv->tv_usec : 0;
    int r;
    for (;;) {
        r = poll(p, (nfds_t)n, 0);
        if (r != 0 || (tv && now_us() >= end)) break;
        usleep(2000);
    }
    if (rd) FD_ZERO(rd);
    if (wr) FD_ZERO(wr);
    if (ex) FD_ZERO(ex);
    if (r <= 0) return r;
    int ready = 0;
    for (int i = 0; i < n; i++) {
        short re = p[i].revents;
        if (rd && (re & (POLLIN | POLLHUP | POLLERR))) { FD_SET(p[i].fd, rd); ready++; }
        if (wr && (re & POLLOUT)) { FD_SET(p[i].fd, wr); ready++; }
        if (ex && (re & POLLPRI)) { FD_SET(p[i].fd, ex); ready++; }
    }
    return ready;
}

/* ── gemu/util.h ─────────────────────────────────────────────────────────── */

void gemu_sleep_ms(unsigned ms)
{
    usleep(ms * 1000u);
}

/* <storage>/gemu/<file>: the SD card's if it has a gemu folder, else the
 * USB drive's, else the SD card's anyway. */
void gemu_config_path(char *out, size_t len, const char *file)
{
    static const char *const roots[] = { "sd:/gemu", "usb:/gemu" };
    const char *root = roots[0];
    struct stat st;
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++)
        if (stat(roots[i], &st) == 0 && S_ISDIR(st.st_mode)) { root = roots[i]; break; }
    snprintf(out, len, "%s/%s", root, file);
}

void gemu_ensure_parent_dir(const char *path)
{
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    char *sep = strrchr(dir, '/');
    if (sep && sep > dir && sep[-1] != ':') { *sep = '\0'; mkdir(dir, 0777); }
}

/* ── The monitor (none on the Wii) ───────────────────────────────────────── */

void gemu_monitor_printf(const GemuMonitor *mon, const char *fmt, ...)
{
    (void)mon; (void)fmt;
}
