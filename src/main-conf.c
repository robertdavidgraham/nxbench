#include "main-conf.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#ifdef _WIN32
#include "win-sockets.h"
#else
#include <unistd.h>
#include "unix-sockets.h"
#endif




static int
_add_target(main_conf_t *conf, const struct sockaddr *target, socklen_t target_length) {
    conf->targets_count++;
    conf->targets = realloc(conf->targets, conf->targets_count * sizeof(*conf->targets));
    memcpy(&conf->targets[conf->targets_count-1],
            target,
            target_length);
    return 0;
}
static int
_add_source(main_conf_t *conf, const struct sockaddr *source, socklen_t source_length) {
    conf->sources_count++;
    conf->sources = realloc(conf->sources, conf->sources_count * sizeof(*conf->sources));
    memcpy(&conf->sources[conf->sources_count-1],
            source,
            source_length);
    return 0;
}

static size_t
index_of(const char* str, size_t start, char delim) {
    size_t i;
    for (i = start; str[i] && str[i] != delim; i++)
        ;
    return i;
}

static int
_add_addresses2(main_conf_t* conf, const char *hostname, unsigned port) {
    struct addrinfo hints, * list, * item;
    int err;
    char portname[16];


    snprintf(portname, sizeof(portname), "%u", port);

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    err = getaddrinfo(hostname, portname, &hints, &list);
    if (err) {
        fprintf(stderr, "[-] failed lookup: %s:%s\n", hostname, portname);
        fprintf(stderr, "[-] err: %s\n", gai_strerror(err));
        return -1;
    }

    for (item = list; item; item = item->ai_next) {
        _add_target(conf, item->ai_addr, (socklen_t)item->ai_addrlen);
    }

    freeaddrinfo(list);

    return 0;
}

static int
_add_addresses(main_conf_t* conf, const char* value, unsigned port) {
    size_t start = 0;
    int err;
    for (;;) {
        size_t end = index_of(value, start, ',');
        char *field = malloc(end - start + 1);
        memcpy(field, value + start, end - start + 1);
        field[end - start] = '\0';
        err = _add_addresses2(conf, field, port);
        free(field);
        if (err)
            return err;
        start = end;
        if (value[start] == '\0')
            break;
        else
            start++;
    }
    return 0;
}


static int
_add_sources2(main_conf_t *conf, const char *hostname) {
    struct addrinfo hints, *list, *item;
    int err;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    err = getaddrinfo(hostname, NULL, &hints, &list);
    if (err) {
        fprintf(stderr, "[-] failed lookup: %s\n", hostname);
        fprintf(stderr, "[-] err: %s\n", gai_strerror(err));
        return -1;
    }

    for (item=list; item; item = item->ai_next) {
        _add_source(conf, item->ai_addr, (socklen_t)item->ai_addrlen);
    }

    freeaddrinfo(list);
    return 0;
}
static int
_add_sources(main_conf_t* conf, const char* value) {
    size_t start = 0;
    int err;
    for (;;) {
        size_t end = index_of(value, start, ',');
        char* field = malloc(end - start + 1);
        memcpy(field, value + start, end - start + 1);
        field[end - start] = '\0';
        err = _add_sources2(conf, field);
        free(field);
        if (err)
            return err;
        start = end;
        if (value[start] == '\0')
            break;
        else
            start++;
    }
    return 0;
}

static int
_parse_url(main_conf_t *conf, const char *url) {
    size_t offset = 0;
    size_t hostname_offset;
    size_t hostname_length;
    size_t path_offset;
    size_t path_length;

    if (memcmp(url, "http://", 7) != 0) {
        return 1;
    } else
        offset = 7;

    /* Strip any additional leading slashes */
    while (url[offset] == '/' || url[offset] == '\\')
        offset++;
    if (!isalnum((unsigned char)url[offset]))
        return -1;

    /* get the host */
    hostname_offset = offset;
    while (url[offset] && (url[offset] != '/' || url[offset] == '\\'))
        offset++;
    hostname_length = offset - hostname_offset;
    conf->server_name = malloc(hostname_length+1);
    memcpy(conf->server_name, url + hostname_offset, hostname_length);
    conf->server_name[hostname_length] = '\0';

    /* get the path */
    path_offset = offset;
    while (url[offset])
        offset++;
    path_length = offset - path_offset;
    conf->path = malloc(path_length + 2);
    memcpy(conf->path, url + path_offset, path_length);
    conf->path[path_length] = '\0';
    if (conf->path[0] != '/' && conf->path[0] != '\\') {
        memmove(conf->path+1, conf->path, path_length+1);
        conf->path[0] = '/';
    }

    return 0;
}

static int
is_equal(const char *lhs, const char *rhs) {
    return strcmp(lhs, rhs) == 0;
}
static long
_parse_number(const char *str) {
    long result = strtoul(str,0,0);
    return result;
}

static int
_set_parm(main_conf_t *conf, const char *name, const char *value) {
    int err;

    if (is_equal(name, "concurrency")) {
        conf->concurrent_connections = _parse_number(value);
        if (conf->concurrent_connections < 1) {
            fprintf(stderr, "[-] concurrency: bad value: %s\n", value);
            exit(1);
        }
        return 0;
    }

    if (is_equal(name, "count")) {
        conf->request_count = _parse_number(value);
        if (conf->request_count < 1) {
            fprintf(stderr, "[-] request count: bad value: %s\n", value);
            exit(1);
        }
        return 0;
    }

    if (is_equal(name, "targetip")) {
        err = _add_addresses(conf, value, conf->server_port);
        if (err || conf->targets_count == 0) {
            fprintf(stderr, "[-] no targetip found: %s\n", value);
            exit(1);
        }
        return 1;
    }

    if (is_equal(name, "sourceip")) {
        err = _add_sources(conf, value);
        if (err || conf->sources_count == 0) {
            fprintf(stderr, "[-] no source found: %s\n", value);
            exit(1);
        }
        return 1;
    }

    if (is_equal(name, "shutdown")) {
        conf->is_shutdown = 1;
        return 0;
    }

    return 0;
}

char *_append(char *lhs, size_t *lhs_length, const char *rhs) {
    size_t rhs_length = strlen(rhs);
    lhs = realloc(lhs, *lhs_length + rhs_length + 1);
    memcpy(lhs + *lhs_length, rhs, rhs_length);
    *lhs_length += rhs_length;
    lhs[*lhs_length] = '\0';
    return lhs;
}

main_conf_t *
main_conf_read(int argc, char *argv[]) {
    int i;
    int err;
    main_conf_t *conf;

    conf = calloc(1, sizeof(*conf));
    conf->server_port = 80;

    for (i=1; i<argc; i++) {
        const char *parm = argv[i];
        int more = 0;

        if (parm[0] != '-') {
            /* assume URL */
            err = _parse_url(conf, parm);
            if (err) {
                fprintf(stderr, "[-] bad URL\n");
                return NULL;
            }
        } else switch (parm[1]) {
            case '-':
                if (i + 1 < argc)
                    more = _set_parm(conf, parm+2, argv[i+1]);
                else
                    more = _set_parm(conf, parm+2, "");
                i += more;
                break;
            case 'c':
                if (parm[2])
                    _set_parm(conf, "concurrency", parm+2);
                else if (i + 1 < argc && argv[i+1][0] != '-') {
                    _set_parm(conf, "concurrency", argv[i+1]);
                    i++;
                } else {
                    fprintf(stderr, "[-] err: %s\n", parm);
                    exit(1);
                }
                break;
            case 'n':
                if (parm[2])
                    _set_parm(conf, "count", parm+2);
                else if (i + 1 < argc && argv[i+1][0] != '-') {
                    _set_parm(conf, "count", argv[i+1]);
                    i++;
                } else {
                    fprintf(stderr, "[-] err: %s\n", parm);
                    exit(1);
                }
                break;
        }
    }

    if (conf->concurrent_connections == 0)
        conf->concurrent_connections = 1;

    if (conf->request_count <= 1)
        conf->request_count = 1000;

    if (conf->server_port == 0)
        conf->server_port = 80;

    if (conf->targets_count == 0) {
        /* this is the norm, we do a DNS lookup on the server
         * name. We don't do this if we've been overridden by --targetip
         * option */
        err = _add_addresses(conf, conf->server_name, conf->server_port);
        if (err || conf->targets_count == 0) {
            fprintf(stderr, "[-] no target addresses found\n");
            return NULL;
        }
    }

    conf->request = _append(conf->request, &conf->request_length, "GET ");
    conf->request = _append(conf->request, &conf->request_length, conf->path);
    conf->request = _append(conf->request, &conf->request_length, " HTTP/1.1\r\n");
    conf->request = _append(conf->request, &conf->request_length, "Host: ");
    conf->request = _append(conf->request, &conf->request_length, conf->server_name);
    conf->request = _append(conf->request, &conf->request_length, "\r\n");
    conf->request = _append(conf->request, &conf->request_length, "User-Agent: nxbench/1.0\r\n");
    conf->request = _append(conf->request, &conf->request_length, "Accept: */*\r\n");
    conf->request = _append(conf->request, &conf->request_length, "\r\n");


    return conf;
}
