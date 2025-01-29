#include "main-conf.h"
#include "util-tui.h"
#include "util-rand.h" /* truely random numbers */
#include "main-pretest.h"
#include "http-response.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <stdarg.h>

#ifdef _WIN32
#include "win-sockets.h"
#include "win-epoll.h"
#else
#include <unistd.h>
#include "unix-sockets.h"
#endif

#define BUFFER_SIZE 1024

#undef EPOLLRDHUP
#define EPOLLRDHUP 0

enum {REASON_ERROR, REASON_HANGUP, REASON_HANGUP2, REASON_READEND, REASON_PIPELINE, REASON_UNKNOWNx};

typedef struct counter_t {
    uint64_t total;
    uint64_t last;
    uint64_t rate;
} counter_t;

typedef struct statistics_t {
    counter_t first;
    struct {
        counter_t attempted;
        counter_t failed;
        counter_t succeeded;
        counter_t error;
        counter_t read;
        counter_t hangup;
        counter_t hangup2;
        counter_t pipeline;
        counter_t unknown;
    } con;
    struct {
        counter_t sent;
        counter_t recved;
        counter_t n100;
        counter_t n200;
        counter_t n300;
        counter_t n400;
        counter_t n500;
    } http;

    counter_t last;
} statistics_t;

typedef struct myinfo_t {
    socket_t fd;
    const char *request;
    size_t request_sent;
    size_t request_length;
    struct myinfo_t *next;
    int is_connected;
    http_response_t http;
} myinfo_t;

typedef struct running_t {
    size_t concurrency;
#ifdef _WIN32
    HANDLE epoll_fd;
#else
    int epoll_fd;
#endif
    struct epoll_event *events;
    myinfo_t *active;
    myinfo_t *freed;
    size_t request_count;
    statistics_t stats;
    uint64_t last_time;
    size_t last_cons;
    util_rand_t r;
} running_t;



static myinfo_t *
_info_alloc(const main_conf_t *conf, running_t *run) {
    myinfo_t *info;

    info = run->freed;
    if (info == NULL) {
        fprintf(stderr, "[*] ran out of memory\n");
        exit(1);
    }

    /* unlink from free list, add to active list */
    run->freed = info->next;
    memset(info, 0, sizeof(*info));
    info->next = run->active;
    run->active = info;

    info->request = (char*)conf->request;
    info->request_length = conf->request_length;
    info->request_sent = 0;
    info->is_connected = 0;

    return info;
}
static void
_info_free(running_t *run, myinfo_t *info) {
    run->active = info->next;
    info->next = run->freed;
    run->freed = info;
}

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

static bool
_connection_is_sent(myinfo_t* info) {
    return info->request_sent >= info->request_length;
}

static void
_connection_send_init(myinfo_t* info) {
    info->request_sent = 0;
}
static void
_connection_recv_init(myinfo_t* info) {
    memset(&info->http, 0, sizeof(info->http));
}

static int 
_connection_create(const main_conf_t *conf, running_t *run) {
    socket_t fd;
    const struct sockaddr *target;
    const struct sockaddr *source;
    int err;
    int addr_len;
    struct epoll_event event;
    myinfo_t *info;

again:
    /* Choose a random source and destination IP address */
    target = (struct sockaddr *)&conf->targets[util_rand32_uniform(&run->r, (unsigned)conf->targets_count)];
    if (conf->sources_count) {
        source = (struct sockaddr *)&conf->sources[util_rand32_uniform(&run->r, (unsigned)conf->sources_count)];
    } else
        source = NULL;
    if (source && (source->sa_family != target->sa_family))
        goto again;

    /* create socket for this connection */
    fd = socket(target->sa_family, SOCK_STREAM, 0);
    if (fd == -1) {
        tui_norm_screen();
        fprintf(stderr, "[-] socket(): %s\n", sock_strerror(sockerrno));
        switch (sockerrno) {
        case WSA(EMFILE):
            fprintf(stderr, "[-] FATAL: use ulimit to increase available file descriptors\n");
            break;
        }
        exit(1);
    }

    /* Bind to a source address */
    if (source) {
        addr_len = get_addr_length(source);
        err = bind(fd, source, addr_len);
        if (err) {
            fprintf(stderr, "[-] bind(): %s\n", sock_strerror(sockerrno));
            exit(1);
        }
    }

    /* set to non-blocking */
    sock_nonblocking(fd);
    addr_len = get_addr_length(target);

    /* initiate the connection to the target*/
    err = connect(fd, target, addr_len);
    if (err == -1 && sockerrno != WSA(EINPROGRESS) && sockerrno != WSA(EWOULDBLOCK)) {
        fprintf(stderr, "[-] connect(): %s\n", sock_strerror(sockerrno));
        closesocket(fd);
        return -1;
    }

    /* Get data specific to this connection */
    info = _info_alloc(conf, run);
    info->fd = fd;

    /* save the event data */
    memset(&event, 0, sizeof(event));
    event.data.ptr = info;
    event.events = EPOLLOUT | EPOLLIN  | EPOLLRDHUP;
    err = epoll_ctl(run->epoll_fd, EPOLL_CTL_ADD, fd, &event);
    if (err) {
        perror("epoll_ctl");
        exit(1);
    }

    run->concurrency++;
    run->request_count++;
    run->stats.con.attempted.total++;

    return 0;
}

static int
_connection_send(myinfo_t *info) {
    ssize_t bytes_sent;

    bytes_sent = send(  info->fd,
                        info->request + info->request_sent,
                        (int)(info->request_length - info->request_sent),
                        MSG_NOSIGNAL);
    if (bytes_sent > 0) {
        info->request_sent += bytes_sent;
        return 0;
    } else {
        return -1;
    }
}

static void
vLOGfd(socket_t fd, const char* fmt, va_list marker) {
    struct sockaddr_storage local_addr, remote_addr;
    socklen_t addr_len = sizeof(struct sockaddr_storage);
    char localhost[NI_MAXHOST], localport[NI_MAXSERV];
    char remotehost[NI_MAXHOST], remoteport[NI_MAXSERV];
    int err;

    err = getsockname(fd, (struct sockaddr*)&local_addr, &addr_len);
    if (err == -1) {
        perror("getsockname failed");
        return;
    }

    err = getpeername(fd, (struct sockaddr*)&remote_addr, &addr_len);
    if (err == -1) {
        perror("getpeername failed");
        return;
    }

    err = getnameinfo((struct sockaddr*)&local_addr, addr_len, localhost, NI_MAXHOST, localport, NI_MAXSERV, NI_NUMERICHOST | NI_NUMERICSERV);
    if (err != 0) {
        perror("getnameinfo failed for local address");
        return;
    }

    err = getnameinfo((struct sockaddr*)&remote_addr, addr_len, remotehost, NI_MAXHOST, remoteport, NI_MAXSERV, NI_NUMERICHOST | NI_NUMERICSERV);
    if (err != 0) {
        perror("getnameinfo failed for remote address");
    }

    fprintf(stderr, "[ ] [%s]:%s -> [%s]:%s: ",
        localhost, localport, remotehost, remoteport);
    vfprintf(stderr, fmt, marker);
}
static void
LOGfd(socket_t fd, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vLOGfd(fd, fmt, args);
    va_end(args);
}

static int
_connection_close(running_t *run, socket_t fd, struct epoll_event *event, int reason) {
    myinfo_t *info = event->data.ptr;
    int err;

    /* Remove from our connection list */
    err = epoll_ctl(run->epoll_fd, EPOLL_CTL_DEL, fd, event);
    if (err) {
        perror("epoll_ctl(EPOLL_CTL_DEL)");
        exit(1);
    }


    /* Record statistics */
    switch (reason) {
        case REASON_ERROR:
            run->stats.con.error.total++;
            int error = 0;
            socklen_t len = sizeof(error);
            int err;

            err = getsockopt(fd, SOL_SOCKET, SO_ERROR, (char*)&error, &len);
            if (err < 0) {
                tui_norm_screen();
                fprintf(stderr, "getsockopt(): %s\n", sock_strerror(sockerrno));
                exit(1);
            } else {
                tui_norm_screen();
                LOGfd(fd, "%s\n", sock_strerror(error));
                exit(1);
            }
            break;
        case REASON_READEND:
            run->stats.con.read.total++;
            break;
        case REASON_HANGUP:
            run->stats.con.hangup.total++;
            break;
        case REASON_HANGUP2:
            run->stats.con.hangup2.total++;
            break;
        case REASON_PIPELINE:
            run->stats.con.hangup2.total++;
            break;
        default:
            run->stats.con.unknown.total++;
            break;
    }

    /* close the connection */
    closesocket(fd);

    /* put the info structure back into the pool */
    _info_free(run, info);

    /* we have one fewer concurrent connections */
    run->concurrency--;

    return err;
}

int run_loop(const main_conf_t *conf, running_t *run) {
    size_t n;
    size_t i;

    /* If we don't have enough concurrent connections,
     * then add some new ones. Don't add them all at once,
     * but in batches. We care about the steady state running
     * of this, not optimizing startup time. */
    i=0;
    while (i++ < 10 && run->concurrency < conf->concurrent_connections) {
        _connection_create(conf, run);
    }
    if (run->concurrency == 0)
        return 0;

    /*
     * Now wait for incoming events
     */
    struct epoll_event *events = run->events;
    n = epoll_wait( run->epoll_fd,
                    events,
                    conf->concurrent_connections,
                    10);

    /*
     * process all the events that were returned
     */
    for (i = 0; i < n; i++) {
        struct epoll_event *event = &events[i];
        unsigned flags = event->events;
        myinfo_t *info = (myinfo_t*)event->data.ptr;
        socket_t fd = info->fd;

        /*
        * This is where we SEND requests.
        * This is where we detect CONNECTIONS.
        */
        if (flags & EPOLLOUT) {
            if (info->is_connected == 0) {
                run->stats.con.succeeded.total++;
                info->is_connected = true;
            }
            _connection_send(info);

            /* If we've sent everything, then modify our record
             * so that we no longer receive this event */
            if (_connection_is_sent(info)) {
                int err;
                struct epoll_event eventmod = *event;
                eventmod.events = EPOLLIN  | EPOLLRDHUP;
                err = epoll_ctl(run->epoll_fd, EPOLL_CTL_MOD, fd, &eventmod);
                if (err) {
                    perror("EPOLL_CTL_MOD");
                }
                if (conf->is_shutdown)
                    shutdown(fd, SHUT_WR);
                run->stats.http.sent.total++;
            }
            continue;
        }


        if (flags & EPOLLERR) {
            _connection_close(run, fd, event, REASON_ERROR);
            continue;
        }


        /*
         * This is where we RECEIVE responses.
         * This is where we also will SEND requests (after
         * receiving a complete response).
         */
        if (flags & EPOLLIN) {
            unsigned char buffer[BUFFER_SIZE];
            int bytes_read = recv(fd, buffer, sizeof(buffer) - 1, 0);
            if (bytes_read > 0) {
                int count;
                int is_finished = false;
                buffer[bytes_read] = '\0';
                count = http_rsp_parse(&info->http, buffer, bytes_read, &is_finished);
                if (count < (int)bytes_read) {
                    _connection_close(run, fd, event, REASON_PIPELINE);
                } else if (is_finished) {
                    run->stats.http.recved.total++;

                    if (flags & EPOLLHUP) {
                        _connection_close(run, fd, event, REASON_HANGUP);
                        continue;
                    }
                    if (flags & EPOLLRDHUP) {
                        _connection_close(run, fd, event, REASON_HANGUP2);
                        continue;
                    }

                    _connection_send_init(info);
                    _connection_send(info);
                    if (!_connection_is_sent(info)) {
                        /* We haven't sent everything */
                        int err;
                        struct epoll_event eventmod = *event;
                        eventmod.events = EPOLLOUT | EPOLLIN | EPOLLRDHUP;
                        err = epoll_ctl(run->epoll_fd, EPOLL_CTL_MOD, fd, &eventmod);
                        if (err) {
                            perror("EPOLL_CTL_MOD");
                        }
                    } else {
                        run->stats.http.sent.total++;
                        _connection_recv_init(info);
                    }
                } else if (!is_finished) {
                    /* continue waiting for the response to be finished */
                    if (flags & EPOLLHUP) {
                        _connection_close(run, fd, event, REASON_UNKNOWNx);
                        continue;
                    }
                    if (flags & EPOLLRDHUP) {
                        _connection_close(run, fd, event, REASON_UNKNOWNx);
                        continue;
                    }
                }

            } else if (bytes_read == 0) {
                if (flags & EPOLLHUP) {
                    _connection_close(run, fd, event, REASON_HANGUP);
                    continue;
                }
                if (flags & EPOLLRDHUP) {
                    _connection_close(run, fd, event, REASON_HANGUP2);
                    continue;
                }
                _connection_close(run, fd, event, REASON_READEND);
                continue;
            } else if (bytes_read < 0) {
                _connection_close(run, fd, event, REASON_ERROR);
            }
            continue;
        }

        if (flags & EPOLLHUP) {
            _connection_close(run, fd, event, REASON_HANGUP);
            continue;
        }

        if (flags & EPOLLRDHUP) {
            _connection_close(run, fd, event, REASON_HANGUP2);
            continue;
        }

        fprintf(stderr, "unknown\n");
    }


    return 1;
}

#ifdef _WIN32
#define CLOCK_REALTIME 1
int
clock_gettime(int clockid, struct timespec* tp) {
    if (clockid != CLOCK_REALTIME) {
        return -1; // Only CLOCK_REALTIME is supported in this implementation
    }

    FILETIME ft;
    ULARGE_INTEGER li;

    GetSystemTimeAsFileTime(&ft);
    li.LowPart = ft.dwLowDateTime;
    li.HighPart = ft.dwHighDateTime;

    // Convert FILETIME to UNIX epoch (January 1, 1970)
    li.QuadPart -= 116444736000000000LL;

    // Convert to seconds and nanoseconds
    tp->tv_sec = (time_t)(li.QuadPart / 10000000);
    tp->tv_nsec = (long)((li.QuadPart % 10000000) * 100);

    return 0;
}
#endif

static running_t *
worker_start(const main_conf_t *conf) {
    size_t i;
    running_t *run;

    /*
     * This sets up the running job. This contains everything
     * that changes during each run through the loop.
     */
    run = calloc(1, sizeof(*run));

    /*
     * Seed random number generator.
     */
    {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        util_rand_seed(&run->r, &ts.tv_nsec, sizeof(ts.tv_nsec));
    }

    /*
     * This created the central epoll event.
     */
    run->epoll_fd = epoll_create1(0);
    if (run->epoll_fd < 0) {
        fprintf(stderr, "[-] epoll_create1: %s\n", sock_strerror(sockerrno));
        return NULL;
    }

    /*
     * This creates a buffer that receives the list of incoming
     * events for each call to epoll_wait()
     */
    run->events = calloc(conf->concurrent_connections, sizeof(*run->events));

    /*
     * This creates a pool of allocated objects to contains the data
     * we associate with each connection. We use this pool instead
     * of malloc()/free() for each connection */
    for (i=0; i<conf->concurrent_connections; i++) {
        myinfo_t *info = calloc(1, sizeof(*info));
        info->next = run->freed;
        run->freed = info;
    }

    return run;
}

void stats_calculate_rates(running_t *run) {
    uint64_t now;
    uint64_t elapsed;
    struct timespec ts;
    statistics_t *stats = &run->stats;
    counter_t *c;

    /* get the current time in nanoseconds, plus the elapsed
     *time */
    clock_gettime(CLOCK_REALTIME, &ts);
    now = ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    elapsed = now - run->last_time;
    run->last_time = now;

    /* Do the tate calculation for all the counters */

    for (c = &stats->first; c < &stats->last; c++) {
        uint64_t diff = c->total - c->last;
        uint64_t rate = diff * 1000000000ULL / elapsed;
        c->rate = (uint64_t)floor(c->rate * 0.9 + rate * 0.1);
        c->last = c->total;
    }
}

void print_stats(const main_conf_t *conf, running_t *run) {
    size_t i;
    int err;

    stats_calculate_rates(run);

    tui_go_topleft();
    fprintf(stderr, "[ https://github.com/robertdavidgraham/nxbench - v0.1 ] " CEOL);
    fprintf(stderr, "website: %s:%u" CEOL, conf->server_name, conf->server_port);
    fprintf(stderr, "IP:");

    for (i=0; i<conf->targets_count; i++) {
        char hostname[64];
        char portname[16];
        const struct sockaddr *addr = (struct sockaddr*)&conf->targets[i];

        err = getnameinfo(addr, get_addr_length(addr),
            hostname, sizeof(hostname),
            portname, sizeof(portname),
            NI_NUMERICHOST |  NI_NUMERICSERV);
        if (err) {
            fprintf(stderr, "[-] getnameinfo(): %s\n", gai_strerror(err));
        }
        fprintf(stderr, " %s", hostname);
    }
    fprintf(stderr, CEOL);
    fprintf(stderr, CEOL);
    fprintf(stderr, "concurrency: %10u" CEOL, (unsigned)run->concurrency);
    fprintf(stderr, CEOL);
#define PSTAT(name, attempted) \
    fprintf(stderr, "%10s: %10llu   %6u/sec" CEOL, name, \
        (unsigned long long)run->stats.con.attempted.total, \
        (unsigned)run->stats.con.attempted.rate \
        );
    PSTAT("connect", attempted);
    PSTAT("success", succeeded);
    PSTAT("fail", failed);
    PSTAT("error", error);
    PSTAT("closed", read);
    PSTAT("hangup", hangup);
    PSTAT("hangup2", hangup2);
    PSTAT("unknown", unknown);
    fprintf(stderr, CEOL);

#define PSTAH(name, attempted) \
    fprintf(stderr, "%10s: %10llu   %6u/sec" CEOL, name, \
        (unsigned long long)run->stats.http.attempted.total, \
        (unsigned)run->stats.http.attempted.rate \
        );
    PSTAH("sent", sent);
    PSTAH("recv", recved);

    fprintf(stderr, CEOL);


}
int main(int argc, char *argv[]) {
    main_conf_t *conf;
    running_t *run;
    time_t now = 0;
    int err;

#ifdef _WIN32
    {
        WSADATA x;
        int err = WSAStartup(0x0202, &x);
        fprintf(stderr, "[+] WSAStartup() returned %d\n", err);
    }
#endif

    http_rsp_init();
    if (http_rsp_selftest() != 0) {
        fprintf(stderr, "[-] FATAL: programing error in http response\n");
        exit(1);
    }

    /*
     * this parses the configuration parameters from
     * the command-line. After this point, all the configuration
     * paramets will be constants (not changed) during the running
     * of the program.
     */
    conf = main_conf_read(argc, argv);
    if (conf == NULL) {
        fprintf(stderr, "-] FATAL: error reading configuration\n");
        return 1;
    }


    /*
     * Make sure all IP addresses are reachable, including that the
     * source IP addresses work (if configured).
     */
    err = pretest_connections(conf);
    if (err) {
        fprintf(stderr, "[-] failed connection test\n");
        exit(1);
    }


    tui_init(1);

    /*
     * create a running job object that will contain
     * all the chaning information during a run
     */
    run = worker_start(conf);
    if (run == NULL)
        return 1;

    /*
     * now run the job until we've sent the total number
     * of requests we were supposed to
     */
    while (run_loop(conf, run)) {
        if (now != time(0)) {
            now = time(0);
            print_stats(conf, run);
        }
    }


    return 0;
}
