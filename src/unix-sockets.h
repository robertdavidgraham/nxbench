#ifndef _WIN32
#ifndef UNIX_SOCKETS_H
#define UNIX_SOCKETS_H

#include <errno.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <fcntl.h>
typedef int socket_t;
#define closesocket(fd) close(fd)
#ifndef SOCKET_ERROR
#define SOCKET_ERROR -1
#endif
#define sockerrno errno
#endif
#define WSA(e) e

int sock_nonblocking(socket_t fd);
#define sock_strerror(e) strerror(e)


#endif