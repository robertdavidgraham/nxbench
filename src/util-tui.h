/*
    Simple Terminal UI tools
*/
#ifndef UTIL_TUI_H
#define UTIL_TUI_H

#define CEOL "\033[K\n"
int tui_init(int is_alternate_buffer);
int tui_get_size(unsigned *width, unsigned *height);

void tui_clear_screen(void);
void tui_go_topleft(void);
void tui_clear_eol(void);

void tui_alt_screen(void);
void tui_norm_screen(void);


#endif
