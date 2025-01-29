#include "http-response.h"
#include "smack.h"
#include <stdio.h>
#include <ctype.h>
#include <stdbool.h>
#include <string.h>

#ifdef _MSC_VER
#pragma warning(disable: 6385)
#endif
static struct SMACK* http_fields;
static struct SMACK* html_fields;


struct Patterns {
    const char* pattern;
    unsigned pattern_length;
    unsigned id;
    unsigned is_anchored;
    //unsigned extra;
};

enum {
    HTTPFIELD_INCOMPLETE,
    HTTPFIELD_SERVER,
    HTTPFIELD_CONTENT_LENGTH,
    HTTPFIELD_CONTENT_TYPE,
    HTTPFIELD_VIA,
    HTTPFIELD_LOCATION,
    HTTPFIELD_UNKNOWN,
    HTTPFIELD_NEWLINE,
};
static struct Patterns http_field_names[] = {
    {"Server:",          7, HTTPFIELD_SERVER,           SMACK_ANCHOR_BEGIN},
    {"Content-Length:", 15, HTTPFIELD_CONTENT_LENGTH,   SMACK_ANCHOR_BEGIN},
    {"Content-Type:",   13, HTTPFIELD_CONTENT_TYPE,     SMACK_ANCHOR_BEGIN},
    {"Via:",             4, HTTPFIELD_VIA,              SMACK_ANCHOR_BEGIN},
    {"Location:",        9, HTTPFIELD_LOCATION,         SMACK_ANCHOR_BEGIN},
    {":",                1, HTTPFIELD_UNKNOWN, 0},
    {"\n",               1, HTTPFIELD_NEWLINE, 0},
    {0,0,0,0}
};

enum {
    HTML_INCOMPLETE,
    HTML_TITLE,
    HTML_UNKNOWN,
};
static struct Patterns html_field_names[] = {
    {"<TiTle",          6, HTML_TITLE, 0},
    {0,0,0,0}
};

/*****************************************************************************
 * Initialize some stuff that's part of the HTTP state-machine-parser.
 *****************************************************************************/
void
http_rsp_init(void) {
    unsigned i;

    /*
     * These match HTTP Header-Field: names
     */
    http_fields = smack_create("http", SMACK_CASE_INSENSITIVE);
    for (i = 0; http_field_names[i].pattern; i++)
        smack_add_pattern(
            http_fields,
            http_field_names[i].pattern,
            http_field_names[i].pattern_length,
            http_field_names[i].id,
            http_field_names[i].is_anchored);
    smack_compile(http_fields);

    /*
     * These match HTML <tag names
     */
    html_fields = smack_create("html", SMACK_CASE_INSENSITIVE);
    for (i = 0; html_field_names[i].pattern; i++)
        smack_add_pattern(
            html_fields,
            html_field_names[i].pattern,
            html_field_names[i].pattern_length,
            html_field_names[i].id,
            html_field_names[i].is_anchored);
    smack_compile(html_fields);

}



/***************************************************************************
 * BIZARRE CODE ALERT!
 *
 * This uses a "byte-by-byte state-machine" to parse the response HTTP
 * header. This is standard practice for high-performance network
 * devices, but is probably unfamiliar to the average network engineer.
 *
 * The way this works is that each byte of input causes a transition to
 * the next state. That means we can parse the response from a server
 * without having to buffer packets. The server can send the response
 * one byte at a time (one packet for each byte) or in one entire packet.
 * Either way, we don't. We don't need to buffer the entire response
 * header waiting for the final packet to arrive, but handle each packet
 * individually.
 *
 * This is especially useful with our custom TCP stack, which simply
 * rejects out-of-order packets.
 ***************************************************************************/
size_t
http_rsp_parse(struct http_response_t *http,
    const unsigned char* px, size_t length, int *is_done)
{
    unsigned state = http->state;
    size_t offset = 0;
    unsigned i;
    unsigned state2;
    size_t id;
    enum {
        VER_HI=5,
        VER_LOW=6,
        RSP_CODE_START=7,
        RSP_CODE,
        RSP_CODE_END,
        FIELD_START,
        FIELD_NAME,
        FIELD_COLON,
        FIELD_VALUE_START,
        FIELD_VALUE_CONTENTS,
        FIELD_VALUE_END,
        CONTENT,
        CONTENT_TAG,
        CONTENT_FIELD,

        DONE_PARSING
    };


    state2 = (state >> 16) & 0xFFFF;
    id = (state >> 8) & 0xFF;
    state = (state >> 0) & 0xFF;

    for (i = (unsigned)offset; i < length; i++) {
        switch (state) {
        case 0: case 1: case 2: case 3: case 4:
            if (toupper(px[i]) != "HTTP/"[state]) {
                state = DONE_PARSING;
                http->is_error = true;
            } else {
                http->major = 0;
                http->minor = 0;
                http->is_error = false;
                http->content_length = 0;
                state++;
            }
            break;
        case 5:
            if (px[i] == '.')
                state++;
            else if (!isdigit(px[i])) {
                state = DONE_PARSING;
                http->is_error = true;
            } else {
                http->major *= 10;
                http->major += px[i] - '0';
            }
            break;
        case 6:
            if (isspace(px[i]))
                state++;
            else if (!isdigit(px[i])) {
                state = DONE_PARSING;
                http->is_error = true;
            } else {
                http->minor *= 10;
                http->minor += px[i] - '0';
            }
            break;
        case RSP_CODE_START:
            if (isdigit(px[i])) {
                http->response_code = px[i] - '0';
                state = RSP_CODE;
            } else if (isspace(px[i])) {
                ; /* do nothing, stay in same state */
            } else if (px[i] == '\n') {
                http->response_code = 0;
                state = FIELD_START;
            } else {
                /* something is broken */
                http->response_code = 0;
                state = RSP_CODE_END;
            }
            break;
        case RSP_CODE:
            if (isdigit(px[i])) {
                http->response_code *= 10;
                http->response_code += px[i] - '0';
            } else if (px[i] == '\n') {
                http->response_code = 0;
                state = FIELD_START;
            } else {
                state = RSP_CODE_END;
            }
            break;
        case RSP_CODE_END:
            if (px[i] == '\n') {
                state = FIELD_START;
                http->content_length = 0;
                http->is_content_length_seen = false;
            }
            break;
        case FIELD_START:
            if (px[i] == '\r')
                break;
            else if (px[i] == '\n') {
                state2 = 0;
                state = CONTENT;
                http->content_seen = 0;
                break;
            } else {
                state2 = 0;
                state = FIELD_NAME;
                /* drop down */
            }
            /* fall through*/

        case FIELD_NAME:
            if (px[i] == '\r')
                break;
            id = smack_search_next(http_fields, &state2, px, &i, (unsigned)length);
            i--;
            if (id == HTTPFIELD_NEWLINE) {
                state2 = 0;
                state = FIELD_START;
            }
            else if (id == SMACK_NOT_FOUND) {
                ; /* continue here */
            } else if (id == HTTPFIELD_UNKNOWN) {
                /* Oops, at this point, both ":" and "Server:" will match.
                 * Therefore, we need to make sure ":" was found, and not
                 * a known field like "Server:" */
                size_t id2;

                id2 = smack_next_match(http_fields, &state2);
                if (id2 != SMACK_NOT_FOUND)
                    id = id2;

                state = FIELD_COLON;
            } else
                state = FIELD_COLON;
            break;
        case FIELD_COLON:
            if (px[i] == '\n') {
                state = FIELD_START;
                break;
            } else if (isspace(px[i])) {
                break;
            } else {
                //field_name(banout, id, http_fields);
                state = FIELD_VALUE_START;
                /* fall through  */
            }
            /* fall through */

        case FIELD_VALUE_START:
            if (px[i] == '\r')
                break;
            else if (px[i] == '\n') {
                state = FIELD_START;
                break;
            } else if (isspace(px[i])) {
                continue;
            } else
                state = FIELD_VALUE_CONTENTS;

            /* do specific things for specific contents */
            switch (id) {
            case HTTPFIELD_CONTENT_LENGTH:
                if (isdigit(px[i])) {
                    http->is_content_length_seen = true;
                    http->content_length = px[i] - '0';
                    state = FIELD_VALUE_CONTENTS;
                } else {
                    http->content_length = 0;
                    state = FIELD_VALUE_END;
                }
                break;
            }
            break;
        case FIELD_VALUE_CONTENTS:
            if (px[i] == '\n') {
                state = FIELD_START;
                break;
            }
            switch (id) {
            case HTTPFIELD_CONTENT_LENGTH:
                if (isdigit(px[i])) {
                    http->content_length *= 10;
                    http->content_length += px[i] - '0';
                } else {
                    state = FIELD_VALUE_END;
                }
                break;
            }
            break;
        case FIELD_VALUE_END:
            /* ignore everything until the end-of-line (EOL) */
            if (px[i] == '\n') {
                state = FIELD_START;
                break;
            }
            break;
        case CONTENT:
        case CONTENT_TAG:
        case CONTENT_FIELD:
        case DONE_PARSING:
        default:
            /* Handle everything else in the next loop */
            goto end_header; /* this goto is ass, I can't find an elegant way around it */
            break;
        }
    }

end_header:

    if (state < CONTENT)
        goto end;
    offset = i;
 
    

    /* Only process to the end of "content", not past the end. */
    if (http->is_content_length_seen && length > offset + http->content_length - http->content_seen)
        length = offset + http->content_length - http->content_seen;

    for (i = (unsigned)offset; i < length; i++) {
        switch (state) {
        case CONTENT:
        {
            unsigned next = i;

            id = smack_search_next(html_fields, &state2, px, &next, (unsigned)length);

            if (id != SMACK_NOT_FOUND) {
                state = CONTENT_TAG;
            }

            i = next - 1;
        }
        break;
        case CONTENT_TAG:
            for (; i < length; i++) {

                if (px[i] == '>') {
                    state = CONTENT_FIELD;
                    break;
                }
            }
            break;
        case CONTENT_FIELD:
            if (px[i] == '<')
                state = CONTENT;
            else {
                ; //TODO banout_append_char(banout, PROTO_HTML_TITLE, px[i]);
            }
            break;
        case DONE_PARSING:
        default:
            i = (unsigned)length;
            break;
        }
    }

    http->content_seen += length - offset;
    if (http->is_content_length_seen && http->content_length == http->content_seen) {
        state = DONE_PARSING;
    }
    if (state == DONE_PARSING) {
        *is_done = true;
    }


end:
    /* Combine state elements back together */
    if (state == DONE_PARSING)
        http->state = state;
    else
        http->state = (state2 & 0xFFFF) << 16
        | ((unsigned)id & 0xFF) << 8
        | (state & 0xFF);

    return length;
}


static const char* test_response =
"HTTP/1.0 200 OK\r\n"
"Date: Wed, 13 Jan 2021 18:18:25 GMT\r\n"
"Expires: -1\r\n"
"Cache-Control: private, max-age=0\r\n"
"Content-Type: text/html; charset=ISO-8859-1\r\n"
"Content-Length: 20\r\n"
"P3P: CP=\x22This is not a P3P policy! See g.co/p3phelp for more info.\x22\r\n"
"Server: gws\r\n"
"X-XSS-Protection: 0\r\n"
"X-Frame-Options: SAMEORIGIN\r\n"
"Set-Cookie: 1P_JAR=2021-01-13-18; expires=Fri, 12-Feb-2021 18:18:25 GMT; path=/; domain=.google.com; Secure\r\n"
"Set-Cookie: NID=207=QioO2ZqRsR6k1wtvXjuuhLrXYtl6ki8SQhf56doo_wcADvldNoHfnKvFk1YXdxSVTWnmqHQVPC6ZudGneMs7vDftJ6vB36B0OCDy_KetZ3sOT_ZAHcmi1pAGeO0VekZ0SYt_UXMjcDhuvNVW7hbuHEeXQFSgBywyzB6mF2EVN00; expires=Thu, 15-Jul-2021 18:18:25 GMT; path=/; domain=.google.com; HttpOnly\r\n"
"Accept-Ranges: none\r\n"
"Vary: Accept-Encoding\r\n"
"\r\n"
"<title>abcde</title>";

static struct {
    unsigned response_code;
    unsigned content_length;
    int is_finished;
    const char* test;
} rsptests[] = {
    {200, 0, true, "HTTP/1.0 200 OK\r\nContent-Length: 0\r\n\r\n"},
    {200, 10, false, "HTTP/1.0 200 OK\r\nContent-Length: 10\r\n\r\nABCD"},
    {200, 10, true, "HTTP/1.0 200 OK\r\nContent-Length: 10\r\n\r\nABCDEFGHIJKLMNOP"},
    {0,0,0,0}
};


int
http_rsp_selftest(void) {
    int i;

    for (i = 0; rsptests[i].test; i++) {
        int is_finished = 0;
        struct http_response_t http = { 0 };
        
        http_rsp_parse(&http, (unsigned char *)rsptests[i].test, strlen(rsptests[i].test), &is_finished);
        
        if (rsptests[i].response_code != http.response_code
            || rsptests[i].content_length != http.content_length
            || rsptests[i].is_finished != is_finished) {
            fprintf(stderr, "[-] [%d] HTTP response parsing error\n", i);
            return 1;
        }
    }

    {
        int is_finished = 0;
        struct http_response_t http = { 0 };

        http_rsp_parse(&http, (unsigned char *)test_response, strlen(test_response), &is_finished);
    }

    return 0;
}