#ifndef HTTP_REQUEST_H
#define HTTP_REQUEST_H
#include <stdio.h>

/**
 * Add or change a Field: in the HTTP request header
 */
size_t
http_edit_request(
    unsigned char **header, size_t header_length,
    const void *name, size_t name_length, 
    const void *value, size_t value_length);

int http_edit_request_selftest(void);

#endif
