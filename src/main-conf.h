#ifndef MAIN_CONF_H
#define MAIN_CONF_H
#include <stdio.h>
struct sockaddr_storage;

typedef struct main_conf_t {
    unsigned concurrent_connections; /* -c */
    unsigned long long request_count; /* -n */
    char *server_name;
    char *path;
    unsigned server_port;
    char *request;
    size_t request_length;

    /* The list of target IP addresses, often only a single
     * one. */
    struct sockaddr_storage *targets;
    size_t targets_count;

    /* The list of source IP addresses, if unspecified, then
     * we let the system choose */
    struct sockaddr_storage *sources;
    size_t sources_count;

    int is_shutdown;
} main_conf_t;

main_conf_t *
main_conf_read(int argc, char *argv[]);

#endif
