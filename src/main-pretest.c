#define _CRT_SECURE_NO_WARNINGS 1
#include "main-pretest.h"
#include "main-conf.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>

#ifdef _WIN32
#include "win-sockets.h"
#else
#include <unistd.h>
#include "unix-sockets.h"
#endif

static int
get_addr_length(const struct sockaddr *target) {
        switch (target->sa_family) {
        case PF_INET:
            return sizeof(struct sockaddr_in);
            break;
        case PF_INET6:
            return sizeof(struct sockaddr_in6);
            break;
        default:
            fprintf(stderr, "[-] unknown address family\n");
            return 0;
            break;
    }
}

static void
set_nonblocking(socket_t fd) {
#ifdef _WIN32
    unsigned long flags = 1;
    ioctlsocket(fd, FIONBIO, &flags);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}


static int
test_connection(const struct sockaddr *source, const struct sockaddr *target) {
    socket_t fd = -1;
    int err;
    char hostname[64];
    char portname[16];
    char sourcename[64] = "default";

    /* format the addresses for logging */
    err = getnameinfo(target, get_addr_length(target),
        hostname, sizeof(hostname),
        portname, sizeof(portname),
        NI_NUMERICHOST |  NI_NUMERICSERV);
    if (err) {
        fprintf(stderr, "[-] getnameinfo(): %s\n", gai_strerror(err));
        return 1;
    }
    if (source) {
        err = getnameinfo(source, get_addr_length(source),
                sourcename, sizeof(sourcename),
                0, 0,
                NI_NUMERICHOST );
        if (err) {
            fprintf(stderr, "[-] getnameinfo(): %s\n", gai_strerror(err));
            return 1;
        }
    }

    /* create socket for this connection */
    fd = socket(target->sa_family, SOCK_STREAM, 0);
    if (fd == -1) {
        fprintf(stderr, "[-] %s -> %s:%s: socket(): %s\n",
            sourcename, hostname, portname,
            sock_strerror(sockerrno));
        goto fail;
    }

    /* Bind to a source address */
    if (source) {
        int addr_len = get_addr_length(source);
        err = bind(fd, source, addr_len);
        if (err) {
            fprintf(stderr, "[-] %s -> %s:%s: bind(): %s\n",
                sourcename, hostname, portname,
                sock_strerror(sockerrno));
            goto fail;
        }
    }

    /* Set to non-blocking. We do this because it's hard to make
     * the connect function to reliably timeout within a period
     * of time. It's more reliable using the select() function
     * for that purpose. */
    set_nonblocking(fd);

    /* initiate the connection to the target*/
    err = connect(fd, target, get_addr_length(target));
    if (err == 0) {
        /* This should never happen, technically, because it's
         * possible when the system is overloaded. */
        goto success;
    }

    /* We should never get an error here. Instead, errors should
     * happen later when we call select(). But just in case, we
     * handle the case here.
     * Note that we expect "in progress" because the socket is
     * non-blocking. */
    if (err == -1 && sockerrno != WSA(EINPROGRESS) && sockerrno != WSA(EWOULDBLOCK)) {
        fprintf(stderr, "[-] %s -> %s:%s: connect(): %s\n",
            sourcename, hostname, portname,
            sock_strerror(sockerrno));
            goto fail;
    }

    fd_set wait_set = { 0 };
    fd_set err_set = { 0 };
    struct timeval tv;
    socket_t nfds = fd + 1;
    int x;

    FD_ZERO(&wait_set);
    FD_SET(fd, &wait_set);
    FD_SET(fd, &err_set);
    tv.tv_sec = 5; /* wait for up to 5 seconds */
    tv.tv_usec = 0;

    /* Now we wait up to 5 seconds to connect */
    x = select((int)nfds, NULL, &wait_set, &err_set, &tv);
    if (x == 0) {
        errno = ETIMEDOUT;
        fprintf(stderr, "[-] %s -> %s:%s: connect(): %s\n",
                sourcename, hostname, portname,
                sock_strerror(sockerrno));
        goto fail;
    }
    if (x < 0) {
        fprintf(stderr, "[-] %s -> %s:%s: select(): %s\n",
                sourcename, hostname, portname,
                sock_strerror(sockerrno));
        goto fail;
    }
    if (FD_ISSET(fd, &err_set)) {
        int error;
        socklen_t len = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&error, &len) < 0) {
            goto fail;
        }
        errno = error;
        fprintf(stderr, "[-] %s -> %s:%s: connect(): %s\n",
                sourcename, hostname, portname,
                sock_strerror(sockerrno));
        goto fail;
    }

    fprintf(stderr, "[+] %s -> %s:%s: connect(): %s\n",
            sourcename, hostname, portname,
            "success");

success:
    closesocket(fd);
    return 0;
fail:
    if (fd != -1)
        closesocket(fd);
    return 1;
}
static int
test_destinations(const main_conf_t *conf, const struct sockaddr *source) {
    size_t i;
    int err;

    if (conf->targets_count == 0) {
        fprintf(stderr, "[-] no targets\n");
        return 1; /*fail*/
    }

    for (i=0; i<conf->targets_count; i++) {
        struct sockaddr *target;
        target = (struct sockaddr*)&conf->targets[i];

        err = test_connection(source, target);
        if (err)
            break;
    }
    return err;
}

/*
 * This is called on startup, after reading the configuration, in order
 * to make sure that all the source IP addresses can reach all the
 * destination IP addresses. It makes a one-to-one connection, so
 * if there are 10 source IP address and 10 destination IP addresses,
 * a total of 100 connections will be made. This only tests connectivity,
 * if a TCP connection can be established. It immediately closes the
 * test connection instead of making a web request.
 */
int
pretest_connections(const struct main_conf_t *conf) {
    int err;

    if (conf->sources_count == 0) {
        /* In this case, we haven't specified any source IP address
         * and are letting the operating system pick one for us */
        err = test_destinations(conf, NULL);
    } else {
        size_t i;
        for (i=0; i<conf->sources_count; i++) {
            struct sockaddr *source;
            source = (struct sockaddr *)&conf->sources[i];
            err = test_destinations(conf, source);
            if (err)
                break;
        }
    }
    return err;
}
