// pipe() for ESP-IDF, which has none. BFC's poll_reactor wakes itself by
// writing single bytes to a pipe; two UDP sockets on 127.0.0.1, connected to
// each other, behave the same for that (poll, non-blocking read/write).

#include "lwip/sockets.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

static int bind_loopback(int fd, struct sockaddr_in* out)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = 0;
    socklen_t len = sizeof(*out);
    if (bind(fd, (struct sockaddr*)&a, sizeof(a)) != 0 ||
        getsockname(fd, (struct sockaddr*)out, &len) != 0)
    {
        return -1;
    }
    return 0;
}

int pipe(int fds[2])
{
    struct sockaddr_in ra, wa;
    const int r = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    const int w = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (r < 0 || w < 0 || bind_loopback(r, &ra) != 0 ||
        bind_loopback(w, &wa) != 0 ||
        connect(w, (struct sockaddr*)&ra, sizeof(ra)) != 0 ||
        connect(r, (struct sockaddr*)&wa, sizeof(wa)) != 0)
    {
        const int err = errno;
        if (r >= 0)
        {
            close(r);
        }
        if (w >= 0)
        {
            close(w);
        }
        errno = err;
        return -1;
    }
    fds[0] = r;
    fds[1] = w;
    return 0;
}
