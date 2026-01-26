
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
char *completefn(const char *buffer, void *ref, int index) {
    const char *end = buffer + strlen(buffer);
    const char *start = end; // Walk backwards to find start of last word
    while (start > buffer && isalpha((unsigned char)start[-1])) start--;

    // Now 'start' points to the last token 
    size_t tok_len = end - start;
    if (!tok_len) return NULL; 

    // Match against keyword dictionary
    for (int i = 0; words[i]; i++) {
        if (strncmp(start, words[i], tok_len) == 0) {
            if (index-- == 0) return (char*)words[i] + tok_len; // Return only the suffix
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
