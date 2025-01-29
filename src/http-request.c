#include "http-request.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <stddef.h>

#ifdef _MSC_VER
#pragma warning(disable: 6308 28182)
#endif

enum What { spaces, notspaces, end_of_line, end_of_name };
static size_t
_skip(enum What what, const unsigned char* hdr, size_t offset, size_t header_length)
{
    switch (what) {
    case notspaces:
        while (offset < header_length && !isspace(hdr[offset] & 0xFF))
            offset++;
        break;
    case spaces:
        while (offset < header_length && hdr[offset] != '\n' && isspace(hdr[offset] & 0xFF))
            offset++;
        if (offset < header_length && hdr[offset] == '\n') {
            while (offset > 0 && hdr[offset - 1] == '\r')
                offset--;
        }
        break;
    case end_of_name:
        while (offset < header_length && (hdr[offset] != '\n' && hdr[offset] != ':' && !isspace(hdr[offset])))
            offset++;
        break;
    case end_of_line:
        while (offset < header_length && hdr[offset] != '\n')
            offset++;
        if (offset < header_length && hdr[offset] == '\n')
            offset++;
        break;
    }
    return offset;
}

static int
is_equal(const void* lhs, size_t lhs_length, const void* rhs, size_t rhs_length) {
    const unsigned char* clhs = (const unsigned char*)lhs;
    const unsigned char* crhs = (const unsigned char*)rhs;
    size_t i;
    if (lhs_length != rhs_length)
        return 0;

    for (i = 0; i < rhs_length; i++) {
        if (tolower(clhs[i]) != tolower(crhs[i]))
            return 0;
    }
    return 1;
}


/**
 * Used when editing our HTTP prototype request, it replaces the existing
 * field (start..end) with the new field. The header is resized and data moved
 * to accommodate this insertion.
 */
static ptrdiff_t
_http_insert(unsigned char** r_hdr, size_t start, size_t end, size_t header_length, const void* field, size_t field_length) {
    ptrdiff_t old_field_length = (end - start);
    ptrdiff_t new_header_length = header_length + field_length - old_field_length;
    unsigned char* hdr;

    if (new_header_length > (ptrdiff_t)header_length) {
        *r_hdr = realloc(*r_hdr, new_header_length + 1);
        (*r_hdr)[new_header_length] = '\0';
        hdr = *r_hdr;

        /* Shrink/expand the field */
        memmove(&hdr[start + field_length], &hdr[end], header_length - end + 1);

        /* Insert the new header at this location */
        memcpy(&hdr[start], field, field_length);

        return new_header_length;
    }
    else {
        hdr = *r_hdr;

        /* Shrink the field */
        memmove(&hdr[start + field_length], &hdr[end], header_length - end + 1);

        /* Insert the new header at this location */
        memcpy(&hdr[start], field, field_length);

        *r_hdr = realloc(*r_hdr, new_header_length + 1);
        (*r_hdr)[new_header_length] = '\0';

        return new_header_length;

    }
}


static size_t is_eol(const void* hdr, size_t offset, size_t header_length) {
    const unsigned* chdr = (const unsigned*)hdr;

    while (offset < header_length && chdr[offset] == '\r')
        offset++;
    if (offset < header_length && chdr[offset] == '\n')
        return offset + 1;
    return 0;
}


size_t
http_edit_request(
    unsigned char** hdr, size_t header_length,
    const void* name, size_t name_length,
    const void* value, size_t value_length) {
    size_t offset;
    size_t next;

    if (name_length == 0 && name != NULL)
        name_length = strlen(name);
    if (value_length == 0 && value != NULL)
        value_length = strlen(value);

    /* Skip leading whitespace */
    offset = 0;
    offset = _skip(spaces, *hdr, offset, header_length);

    /* Method */
    if (offset == header_length || is_eol(hdr, offset, header_length))
        header_length = _http_insert(hdr, offset, offset, header_length, "GET", 3);
    next = _skip(notspaces, *hdr, offset, header_length);
    if (is_equal(name, name_length, "method", 6)) {
        header_length = _http_insert(hdr, offset, next, header_length, value, value_length);
        name_length = 0;
    }
    offset = _skip(notspaces, *hdr, offset, header_length);


    /* Method space */
    if (offset == header_length || is_eol(hdr, offset, header_length))
        header_length = _http_insert(hdr, offset, offset, header_length, " ", 1);
    offset = _skip(spaces, *hdr, offset, header_length);

    /* URL */
    if (offset == header_length || is_eol(hdr, offset, header_length))
        header_length = _http_insert(hdr, offset, offset, header_length, "/", 1);
    next = _skip(notspaces, *hdr, offset, header_length);
    if (is_equal(name, name_length, "url", 3)) {
        header_length = _http_insert(hdr, offset, next, header_length, value, value_length);
        name_length = 0;
    }
    offset = _skip(notspaces, *hdr, offset, header_length);

    /* Space after URL */
    if (offset == header_length || is_eol(hdr, offset, header_length))
        header_length = _http_insert(hdr, offset, offset, header_length, " ", 1);
    offset = _skip(spaces, *hdr, offset, header_length);

    /* version */
    if (offset == header_length || is_eol(hdr, offset, header_length))
        header_length = _http_insert(hdr, offset, offset, header_length, "HTTP/1.1", 8);
    next = _skip(notspaces, *hdr, offset, header_length);
    if (is_equal(name, name_length, "version", 7)) {
        header_length = _http_insert(hdr, offset, next, header_length, value, value_length);
        name_length = 0;
    }
    offset = _skip(notspaces, *hdr, offset, header_length);

    /* end-of-line */
    if (offset == header_length)
        header_length = _http_insert(hdr, offset, offset, header_length, "\r\n", 2);
    offset = _skip(spaces, *hdr, offset, header_length);
    offset = _skip(end_of_line, *hdr, offset, header_length);

    /* make sure there's a blank line at the end */
    if (offset == header_length)
        header_length = _http_insert(hdr, offset, offset, header_length, "\r\n", 2);

    /* emumerate all header fields */
    while (!is_eol(hdr, offset, header_length)) {
        next = _skip(end_of_name, *hdr, offset, header_length);
        if (offset == next)
            break;
        if (!is_equal(name, name_length, *hdr + offset, next - offset)) {
            offset = _skip(end_of_line, *hdr, offset, header_length);
            continue;
        }

        /* remove field */
        next = _skip(end_of_line, *hdr, offset, header_length);
        header_length = _http_insert(hdr, offset, next, header_length, "", 0);
        break;
    }

    /* add the field, either where it was before in the header, or at the end */
    if (name_length) {
        header_length = _http_insert(hdr, offset, offset, header_length, name, name_length);
        offset += name_length;
        header_length = _http_insert(hdr, offset, offset, header_length, ": ", 2);
        offset += 2;
        header_length = _http_insert(hdr, offset, offset, header_length, value, value_length);
        offset += value_length;
        header_length = _http_insert(hdr, offset, offset, header_length, "\r\n", 2);
        offset += 2;
    }

    return header_length;
}

int http_edit_request_selftest(void) {
    struct test {
        const char* start;
        const char* name;
        const char* value;
        const char* result;
    };
    static const struct test tests[] = {
        {"GET /index.html HTTP/1.0\r\nConnection: close\r\n\r\n", "Connection", "keep-alive", "GET /index.html HTTP/1.0\r\nConnection: keep-alive\r\n\r\n"},
        {"GET /index.html HTTP/1.0\r\nConnection: keep-alive\r\n\r\n", "Connection", "closed", "GET /index.html HTTP/1.0\r\nConnection: closed\r\n\r\n"},
        {"", "Connection", "closed", "GET / HTTP/1.1\r\nConnection: closed\r\n\r\n"},
        {"", "version", "HTTP/1.1", "GET / HTTP/1.1\r\n\r\n"},
        {"", "", "", "GET / HTTP/1.1\r\n\r\n"},
        {"", "url", "/index.html", "GET /index.html HTTP/1.1\r\n\r\n"},
        {0, 0, 0, 0}
    };
    size_t i;

    for (i = 0; tests[i].start; i++) {
        unsigned char* hdr = (unsigned char *)strdup(tests[i].start);
        size_t hdr_len = strlen((char *)hdr);

        hdr_len = http_edit_request(&hdr, hdr_len, tests[i].name, 0, tests[i].value, 0);
        if (strcmp((char*)hdr, tests[i].result) != 0) {
            fprintf(stderr, "[-] http.request: programming error: test[%u]\n", (unsigned)i);
            fprintf(stderr, "[\n%.*s]\n", (unsigned)hdr_len, hdr);
            return 1;
        }
        free(hdr);
    }

    return 0;
}
