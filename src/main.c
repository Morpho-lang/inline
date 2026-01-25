
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "inline.h"

/** Use C keywords as an example */
static const char *words[] = { 
    "auto","break","case","char","const","continue","default","do","double","else","enum",
    "extern","float","for","goto","if","int","long","register","return","short","signed",
    "sizeof","static","struct","switch","typedef","union","unsigned","void","volatile","while",
     NULL };

/** Autocomplete function */
char *completefn(const char *prefix, void *ref, int index) {
    size_t len = strlen(prefix);
    for (int i = 0; words[i]; i++) {
        if (strncmp(prefix, words[i], len) == 0) {
            if (index-- == 0)
                return (char*)words[i];
        }
    }
    return NULL;
}

int main(void) {
    printf("Inline editor test...\n");

    inline_editor *edit = inline_new("> ");
    if (!edit) {
        fprintf(stderr, "inline_new failed\n");
        return 1;
    }

    inline_autocomplete(edit, completefn, NULL); // Configure editor with autocomplete

    printf("Editor created successfully.\n");

    char *line = inline_readline(edit);

    if (line) {
        printf("You entered: '%s'\n", line);
        free(line);
    } else {
        printf("inline_readline returned NULL.\n");
    }

    inline_free(edit);
    printf("Editor freed.\n");

    return 0;
}
