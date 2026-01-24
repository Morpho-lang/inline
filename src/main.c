#include <stdio.h>
#include <stdlib.h>
#include "inline.h"

int main(void) {
    printf("Inline editor test...\n");

    inline_editor *ed = inline_new("> ");
    if (!ed) {
        fprintf(stderr, "inline_new failed\n");
        return 1;
    }

    printf("Editor created successfully.\n");
\
    char *line = inline_readline(ed);

    if (line) {
        printf("You entered: '%s'\n", line);
    } else {
        printf("inline_readline returned NULL.\n");
    }

    inline_free(ed);
    printf("Editor freed.\n");

    return 0;
}
