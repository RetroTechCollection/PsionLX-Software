/*
 * psionnet-find: print the address of PsionNet on this network, as
 * "host:port", and exit 0; exit 1 if none answers. For shell scripts --
 * psionnet-connect runs it at login to point Firefox at PsionNet.
 *
 *   psionnet-find [seconds]        (default 3)
 *
 * GPL-2.0-or-later, as part of PsionNet.
 */
#include <stdlib.h>
#include "psionnet-discover.h"

int main(int argc, char **argv)
{
    char host[128];
    int port = 0, tries = argc > 1 ? atoi(argv[1]) : 3;

    if (tries < 1 || tries > 30)
        tries = 3;
    if (!psionnet_discover(host, sizeof host, &port, tries))
        return 1;
    printf("%s:%d\n", host, port);
    return 0;
}
