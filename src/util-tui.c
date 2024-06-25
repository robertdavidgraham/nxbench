#include "util-tui.h"
#include <signal.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
typedef ptrdiff_t ssize_t;
#include <io.h>
#define open _open
#define read _read
#define write _write
#else
#include <unistd.h>
#endif

struct {
    int is_alternate_buffer;
} tui;

static void say(const char *str) {
    write(2, str, (unsigned)strlen(str));
}

static void _tui_hide_cursor(void) { say("\033[?25l"); }
static void _tui_norm_cursor(void) { say("\033[?25h"); }
static void _tui_alt_screen(void) { say("\033[?1049h\033[H"); }
static void _tui_norm_screen(void) { say("\033[?1049l"); }
void tui_clear_screen(void) { say("\033[1;1H\033[2J");}
void tui_go_topleft(void) { say("\033[1;1H"); }
void tui_clear_eol(void) { say("\033[K"); }


static void 
_tui_cleanup(void) {
    /* Restore original screen */
    if (tui.is_alternate_buffer) {
        _tui_norm_screen();
    }

    /* Restore cursor to blinking */
    _tui_norm_cursor();
}

void handle_signal(int sig) {
    switch (sig) {
        case SIGTERM:
        case SIGINT:
            _tui_cleanup();
            exit(1);
            break;
    }
}

int tui_init(int is_alternate_buffer) {
    tui.is_alternate_buffer = is_alternate_buffer;

    atexit(_tui_cleanup);
    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);
    if (tui.is_alternate_buffer) {
        _tui_alt_screen();
    }
    _tui_hide_cursor();

    return 0;
}


int tui_get_size(unsigned *rows, unsigned *cols) {
    //static unsigned char ESC = 27;
    int state = 0;
    enum {S_START, S_SAW_ESC, S_SAW_BRACKET, S_SAW_EIGHT,
        S_ROWS, S_COLS, S_END};
    say("\033[18t");

    *rows = 0;
    *cols = 0;

    /* ESC [ 8 ; <rows> ; <cols> t*/
    for (;;) {
        unsigned char c;
        ssize_t len;

        len = read(0, &c, 1);
        if (len == 0) {
            fprintf(stderr, "[-] err getting term size\n");
            break;
        }

        switch (state) {
            case S_START:
            case S_SAW_ESC:
            case S_SAW_BRACKET:
            case S_SAW_EIGHT:
                if (c != "\x1b" "[8;"[state-S_START]) {
                    fprintf(stderr, "[-] err getting term size\n");
                    return -1;
                }
                state++;
                break;
            case S_ROWS:
                if (c == ';')
                    state++;
                else if (!isdigit(c)) {
                    fprintf(stderr, "[-] err getting term size\n");
                    return -1;
                } else {
                    (*rows) *= 10;
                    (*rows) += c - '0';
                }
                break;
            case S_COLS:
                if (c == 't')
                    state++;
                else if (!isdigit(c)) {
                    fprintf(stderr, "[-] err getting term size\n");
                    return -1;
                } else {
                    (*cols) *= 10;
                    (*cols) += c - '0';
                }
                break;
            case S_END:
                return 0;
        }
    }
    return 1;
}
