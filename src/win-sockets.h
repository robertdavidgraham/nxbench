#ifdef _WIN32
#ifndef WIN_SOCKETS_H
#define WIN_SOCKETS_H

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0500
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <intrin.h>
#include <process.h>
#if defined(_MSC_VER)
#pragma comment(lib, "ws2_32")
#endif
typedef SOCKET socket_t;
typedef ptrdiff_t ssize_t;
#define MSG_NOSIGNAL 0
#define CLOCK_MONOTONIC 1
#define SHUT_WR SD_SEND
#pragma warning(disable: 6011)
#define sockerrno WSAGetLastError()

int sock_nonblocking(socket_t fd);
#pragma warning(disable: 6387 5011 6301 6011)
#pragma warning(disable: 6308 28182)
#define WSA(e) WSA##e
const char* sock_strerror(int errorCode);

#endif
#endif
