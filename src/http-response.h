#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H
#include <stdbool.h>
#include <stdio.h>

typedef struct http_response_t {
    unsigned state;
    unsigned short major;
    unsigned short minor;
    unsigned short response_code;
    unsigned long long content_length;
    unsigned long long content_seen;
    bool is_error : 1;
    bool is_content_length_seen : 1;
} http_response_t;

void
http_rsp_init(void);

size_t
http_rsp_parse(struct http_response_t* http,
    const unsigned char* px, size_t length, int *is_finished);

int
http_rsp_selftest(void);



#endif
