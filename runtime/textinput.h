// Typed text for the game's text entries (runtime/hle/swkbd.c): the physical keyboard, while the game asks for text.
#pragma once
#include <stdbool.h>
#include <stddef.h>

union SDL_Event;

enum { TI_NONE, TI_CHANGED, TI_ENTER, TI_CANCEL };

// game side (any thread)
void textinput_begin(const char *initial, int max_chars);  // the game shows a text entry (max_chars <= 0: no limit)
void textinput_end(void);                                  // it is gone (decided, or closed by the game)
int textinput_poll(char *out, size_t size, int *chars);    // TI_*: what happened since the last poll; the text now

// window side (the main thread)
bool textinput_active(void);
bool textinput_event(const union SDL_Event *e);  // true: the event was typing (not for the controller mapping)
void textinput_update(void);                     // each loop pass: the window's text input on / off to match
void textinput_type(const char *utf8);           // as typed (MHGU_CONTROL "type", paste)
void textinput_key(int what);                    // TI_ENTER, TI_CANCEL, or TI_NONE for Backspace
