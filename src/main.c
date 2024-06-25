#include "main-conf.h"
#include "util-tui.h"
#include "util-rand.h" /* truely random numbers */
#include "main-pretest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#ifdef _WIN32
#include "win-sockets.h"
#include "win-epoll.h"
#else
#include <unistd.h>
#include "unix-sockets.h"
#endif

#define BUFFER_SIZE 1024

enum {REASON_ERROR, REASON_HANGUP, REASON_HANGUP2, REASON_READEND};

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
    } begin;
    struct {
        counter_t error;
        counter_t read;
        counter_t hangup;
        counter_t hangup2;
        counter_t unknown;
    } end;

    counter_t last;
} statistics_t;

typedef struct myinfo_t {
    socket_t fd;
    const char *request;
    size_t request_sent;
    size_t request_length;
    struct myinfo_t *next;
    int flag_success;
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

    info->request = conf->request;
    info->request_length = conf->request_length;
    info->request_sent = 0;
    info->flag_success = 0;

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


int create_connection(const main_conf_t *conf, running_t *run) {
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
        fprintf(stderr, "[-] local error: socket(): %s\n", sock_strerror(sockerrno));
        return -1;
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
    run->stats.begin.attempted.total++;

    return 0;
}

static int
_send_request(myinfo_t *info) {
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

static int
_close_connection(running_t *run, socket_t fd, struct epoll_event *event, int reason) {
    myinfo_t *info = event->data.ptr;
    int err;

    /* Remove from our connection list */
    err = epoll_ctl(run->epoll_fd, EPOLL_CTL_DEL, fd, event);
    if (err) {
        perror("epoll_ctl(EPOLL_CTL_DEL)");
        exit(1);
    }

    /* close the connection */
    closesocket(fd);

    /* put the info structure back into the pool */
    _info_free(run, info);

    /* we have one fewer concurrent connections */
    run->concurrency--;

    /* Record statistics */
    switch (reason) {
        case REASON_ERROR:
            run->stats.end.error.total++;
            break;
        case REASON_READEND:
            run->stats.end.read.total++;
            break;
        case REASON_HANGUP:
            run->stats.end.hangup.total++;
            break;
        case REASON_HANGUP2:
            run->stats.end.hangup2.total++;
            break;
        default:
            run->stats.end.unknown.total++;
            break;
    }
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
        create_connection(conf, run);
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

        if (flags & EPOLLOUT) {
            if (info->flag_success == 0) {
                run->stats.begin.succeeded.total++;
                info->flag_success = 1;
            }
            _send_request(info);

            /* If we've sent everything, then modify our record
             * so that we no longer receive this event */
            if (info->request_sent >= info->request_length) {
                int err;
                struct epoll_event eventmod = *event;
                eventmod.events = EPOLLIN  | EPOLLRDHUP;
                err = epoll_ctl(run->epoll_fd, EPOLL_CTL_MOD, fd, &eventmod);
                if (err) {
                    perror("EPOLL_CTL_MOD");
                }
                if (conf->is_shutdown)
                    shutdown(fd, SHUT_WR);
            }
            continue;
        }


        if (flags & EPOLLERR) {
            _close_connection(run, fd, event, REASON_ERROR);
            continue;
        }

        if (flags & EPOLLHUP) {
            _close_connection(run, fd, event, REASON_HANGUP);
            continue;
        }

        if (flags & EPOLLRDHUP) {
            _close_connection(run, fd, event, REASON_HANGUP2);
            continue;
        }

        if (flags & EPOLLIN) {
            char buffer[BUFFER_SIZE];
            int bytes_read = recv(fd, buffer, sizeof(buffer) - 1, 0);
            if (bytes_read > 0) {
                buffer[bytes_read] = '\0';
            } else {
                _close_connection(run, fd, event, REASON_READEND);
            }
            continue;
        }

        fprintf(stderr, "unknown\n");
    }


    return 1;
}

#ifdef _WIN32
LARGE_INTEGER
getFILETIMEoffset(void)
{
    SYSTEMTIME s;
    FILETIME f;
    LARGE_INTEGER t;

    s.wYear = 1970;
    s.wMonth = 1;
    s.wDay = 1;
    s.wHour = 0;
    s.wMinute = 0;
    s.wSecond = 0;
    s.wMilliseconds = 0;
    SystemTimeToFileTime(&s, &f);
    t.QuadPart = f.dwHighDateTime;
    t.QuadPart <<= 32;
    t.QuadPart |= f.dwLowDateTime;
    return (t);
}

#define CLOCK_REALTIME 1
int
clock_gettime(int X, struct timeval* tv)
{
    LARGE_INTEGER           t;
    FILETIME            f;
    double                  microseconds;
    static LARGE_INTEGER    offset;
    static double           frequencyToMicroseconds;
    static int              initialized = 0;
    static BOOL             usePerformanceCounter = 0;

    X = X;

    if (!initialized) {
        LARGE_INTEGER performanceFrequency;
        initialized = 1;
        usePerformanceCounter = QueryPerformanceFrequency(&performanceFrequency);
        if (usePerformanceCounter) {
            QueryPerformanceCounter(&offset);
            frequencyToMicroseconds = (double)performanceFrequency.QuadPart / 1000000.;
        }
        else {
            offset = getFILETIMEoffset();
            frequencyToMicroseconds = 10.;
        }
    }
    if (usePerformanceCounter) QueryPerformanceCounter(&t);
    else {
        GetSystemTimeAsFileTime(&f);
        t.QuadPart = f.dwHighDateTime;
        t.QuadPart <<= 32;
        t.QuadPart |= f.dwLowDateTime;
    }

    t.QuadPart -= offset.QuadPart;
    microseconds = (double)t.QuadPart / frequencyToMicroseconds;
    t.QuadPart = (LONGLONG)microseconds;
    tv->tv_sec = (long)(t.QuadPart / 1000000);
    tv->tv_usec = t.QuadPart % 1000000;
    return (0);
}

#endif

static running_t *
run_start(const main_conf_t *conf) {
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
    fprintf(stderr, "concurrency: %u" CEOL, (unsigned)run->concurrency);
    fprintf(stderr, CEOL);
    fprintf(stderr, "requests: %u" CEOL, (unsigned)run->stats.begin.attempted.total);
    fprintf(stderr, " success: %u" CEOL, (unsigned)run->stats.begin.succeeded.total);
    fprintf(stderr, "    fail: %u" CEOL, (unsigned)run->stats.begin.failed.total);
    fprintf(stderr, " req/sec: %u" CEOL, (unsigned)run->stats.begin.attempted.rate);
    fprintf(stderr, " suc/sec: %u" CEOL, (unsigned)run->stats.begin.succeeded.rate);
    fprintf(stderr, "fail/sec: %u" CEOL, (unsigned)run->stats.begin.failed.rate);

    fprintf(stderr, CEOL);
    fprintf(stderr, "  error: %u" CEOL, (unsigned)run->stats.end.error.total);
    fprintf(stderr, "   read: %u" CEOL, (unsigned)run->stats.end.read.total);
    fprintf(stderr, " hangup: %u" CEOL, (unsigned)run->stats.end.hangup.total);
    fprintf(stderr, "hangup2: %u" CEOL, (unsigned)run->stats.end.hangup2.total);

    fprintf(stderr, "  error/sec: %u" CEOL, (unsigned)run->stats.end.error.rate);
    fprintf(stderr, "   read/sec: %u" CEOL, (unsigned)run->stats.end.read.rate);
    fprintf(stderr, " hangup/sec: %u" CEOL, (unsigned)run->stats.end.hangup.rate);
    fprintf(stderr, "hangup2/sec: %u" CEOL, (unsigned)run->stats.end.hangup2.rate);

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

    /*
     * this parses the configuration parameters from
     * the command-line. After this point, all the configuration
     * paramets will be constants (not changed) during the running
     * of the program.
     */
    conf = main_conf_read(argc, argv);
    if (conf == NULL)
        return 1;

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
    run = run_start(conf);
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
