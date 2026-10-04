/*
 * Find PsionNet on the local network: broadcast "PSIONNET?" to UDP 8899 and
 * read "PSIONNET <address> <port>". Shared by psionlx-software and
 * psionnet-find; the Spotify app carries its own copy of the same code.
 *
 * Sent to the limited broadcast and to each interface's subnet broadcast,
 * which some networks pass when they drop the other. If PsionNet names a
 * loopback address (a test on the Mac itself, or an emulator), the sender of
 * the reply is the way to reach it instead.
 */
#ifndef PSIONNET_DISCOVER_H
#define PSIONNET_DISCOVER_H

#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define PSIONNET_DISCOVERY_PORT 8899

static int psionnet_broadcasts(int fd, struct in_addr *out, int max)
{
    struct ifreq reqs[16];
    struct ifconf ifc;
    int n = 0, i;

    out[n++].s_addr = htonl(INADDR_BROADCAST);
    ifc.ifc_len = sizeof reqs;
    ifc.ifc_req = reqs;
    if (ioctl(fd, SIOCGIFCONF, &ifc) < 0)
        return n;
    for (i = 0; i < ifc.ifc_len / (int) sizeof(struct ifreq) && n < max; i++) {
        struct ifreq r = reqs[i];
        if (ioctl(fd, SIOCGIFFLAGS, &r) < 0 || !(r.ifr_flags & IFF_UP)
            || (r.ifr_flags & IFF_LOOPBACK) || !(r.ifr_flags & IFF_BROADCAST))
            continue;
        r = reqs[i];
        if (ioctl(fd, SIOCGIFBRDADDR, &r) == 0)
            out[n++] = ((struct sockaddr_in *) &r.ifr_broadaddr)->sin_addr;
    }
    return n;
}

/* Returns 1 and fills host/port if PsionNet answered within `tries` seconds. */
static int psionnet_discover(char *host, size_t hostlen, int *port, int tries)
{
    struct sockaddr_in to, from;
    struct in_addr bcast[8];
    struct timeval tv;
    fd_set r;
    char buf[256];
    socklen_t flen;
    int fd, on = 1, n, nb, b, attempt;

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return 0;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
    nb = psionnet_broadcasts(fd, bcast, 8);
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(PSIONNET_DISCOVERY_PORT);
    for (attempt = 0; attempt < tries; attempt++) {
        for (b = 0; b < nb; b++) {
            to.sin_addr = bcast[b];
            sendto(fd, "PSIONNET?\n", 10, 0, (struct sockaddr *) &to, sizeof to);
        }
        FD_ZERO(&r);
        FD_SET(fd, &r);
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        while (select(fd + 1, &r, NULL, NULL, &tv) > 0) {
            char h[128];
            int p = 0;
            flen = sizeof from;
            n = recvfrom(fd, buf, sizeof buf - 1, 0, (struct sockaddr *) &from, &flen);
            if (n <= 9)
                continue;
            buf[n] = '\0';
            if (sscanf(buf, "PSIONNET %127s %d", h, &p) != 2 || p <= 0 || p > 65535)
                continue;
            if (strncmp(h, "127.", 4) == 0 || strcmp(h, "0.0.0.0") == 0) {
                unsigned char *a = (unsigned char *) &from.sin_addr;
                snprintf(h, sizeof h, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
            }
            strncpy(host, h, hostlen - 1);
            host[hostlen - 1] = '\0';
            *port = p;
            close(fd);
            return 1;
        }
    }
    close(fd);
    return 0;
}

#endif
