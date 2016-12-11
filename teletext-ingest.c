#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <err.h>
#include "telxcc.h"

void print_frame(frame_t *frame) {
    printf("%"PRIu64" %"PRIu64" %s",
            frame->show_timestamp,
            frame->hide_timestamp,
            frame->text
    );
}

int main(const int argc, char *argv[]) {
    uint16_t pid;
    uint16_t page;
    in_addr_t addr;
    uint32_t port;

    int s;
    int e;

    if (argc != 5)
        errx(1, "usage: teletext-ingest <pid> <page> <addr> <port>\n");

    pid = strtoul(argv[1], NULL, 10);
    page = strtoul(argv[2], NULL, 10);
    addr = inet_addr(argv[3]);
    port = strtoul(argv[4], NULL, 10);

    // Multicast receiver
    s = socket(AF_INET, SOCK_DGRAM, PF_UNSPEC);
    if (s == -1)
        err(1, "socket");

    struct sockaddr_in sin = (struct sockaddr_in) {0};
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_port = htons(port);

    e = bind(s, (struct sockaddr *) &sin, sizeof sin);
    if (e == -1)
        err(1, "bind");

    e = setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP,
            (struct ip_mreq[]){{.imr_multiaddr.s_addr = addr, .imr_interface.s_addr = htonl(INADDR_ANY)}},
            sizeof(struct ip_mreq));

    if (e == -1)
        err(1, "setsockopt");

    telxcc(s, pid, page, &print_frame);

    return 0;
}
