int iso_forbids_empty_file_unixsock;
#ifndef _WIN32
#include "unix-sockets.h"

int
sock_nonblocking(socket_t fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return 0;
}

#endif

