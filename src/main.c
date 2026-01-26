
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "inline.h"

/** Use C keywords as an example */
static const char *words[] = { 
    "auto","break","case","char","const","continue","default","do","double","else","enum",
    "extern","float","for","goto","if","int","long","register","return","short","signed",
    "sizeof","static","struct","switch","typedef","union","unsigned","void","volatile","while",
     NULL };

/** Autocomplete function */
char *completefn(const char *buffer, void *ref, size_t *index) {
    const char *end = buffer + strlen(buffer);
    const char *start = end;

    // Walk backwards to find start of last word 
    while (start > buffer && isalpha((unsigned char)start[-1])) start--;

    size_t tok_len = end - start;
    if (!tok_len) return NULL;

    // Match against keyword dictionary, starting at *index
    for (size_t i = *index; words[i]; i++) {
        if (strncmp(start, words[i], tok_len) == 0) {
            *index = i + 1;  // Advance iteration state 
            return (char*)words[i] + tok_len;  // Return suffix only 
        }
    }

    return NULL; // No more words to match so we're done
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
