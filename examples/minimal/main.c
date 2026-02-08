/** @file main.c
 *  @author T J Atherton
 *
 *  @brief Minimal terminal application demonstrating inline.  */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "inline.h"

/* **********************************************************************
 * Minimal REPL: Get input and echo it back
 * ********************************************************************** */

int main(void) {
    printf("Minimal inline editor test... (type 'quit' to exit)\n");

    inline_editor *edit = inline_new("> ");
    if (!edit) return 1;

    for (bool quit=false; !quit;) {
        char *line = inline_readline(edit);
        if (line) {
            if (strcmp(line, "quit")==0) {
                quit=true;
            } else printf("You entered: '%s'\n", line); 
            free(line);
        } else quit=true;
    }

    inline_free(edit);
    return 0;
}
