
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

/** Set the contents of an inline_color_span_t structure */
static inline void inline_set_colorspan(inline_colorspan_t *s, size_t start, size_t end, int color) {
    s->byte_start = start;
    s->byte_end   = end;
    s->color      = color;
}

/** Example C syntax highlighter highlighting keywords (col 1), strings (col 2), integers (col 3) */
static bool syntaxhighlighterfn(const char *utf8, void *ref, size_t offset, inline_colorspan_t *out) {
    (void)ref; // unused

    if (utf8[offset] == '"') { // Strings
        size_t start = offset;
        offset++;
        for (; utf8[offset] != '\0'; offset++) {
            if (utf8[offset] == '"' && utf8[offset - 1] != '\\') {
                offset++;
                break;
            }
        }
        inline_set_colorspan(out, start, offset, 2);
        return true;
    }

    if (isdigit((unsigned char)utf8[offset])) { // Integers
        size_t start = offset;
        for (offset++; utf8[offset] && isdigit((unsigned char)utf8[offset]); offset++);
        inline_set_colorspan(out, start, offset, 3);
        return true;
    }

    if (isalpha((unsigned char)utf8[offset]) || utf8[offset] == '_') { // Match keywords
        size_t start = offset;

        // Find end of token:
        for (offset++; utf8[offset] && (isalnum((unsigned char)utf8[offset]) || utf8[offset] == '_'); offset++);

        size_t tok_len = offset - start;
        for (size_t i = 0; words[i]; i++) {
            const char *w = words[i];
            if (w[tok_len] == '\0' && strncmp(utf8 + start, w, tok_len) == 0) {
                inline_set_colorspan(out, start, offset, 1);
                return true;
            }
        }
    }


    inline_set_colorspan(out, offset, offset + 1, 0); // Anything else 
    return true;
}

static int palette[] = {
    -1,  // 0 = default
     5,  // 1 = purple (keywords)
     4,  // 2 = dark blue (strings)
     6   // 3 = cyan (numbers)
};

/** Multiline function */
static bool multilinefn(const char *in, void *ref) {
    int nb=0; 
    for (const char *c=in; *c!='\0'; c++) { // Match brackets
        switch (*c) {
            case '(': case '{': case '[': nb+=1; break; 
            case ')': case '}': case ']': nb-=1; break;
            default: break; 
        }
    }
    return (nb>0); // Is there an unmatched bracket?
}

int main(void) {
    printf("Inline editor test...\n");

    inline_editor *edit = inline_new("> ");
    if (!edit) {
        fprintf(stderr, "inline_new failed\n");
        return 1;
    }

    inline_autocomplete(edit, completefn, NULL); // Configure editor with autocomplete
    inline_syntaxcolor(edit, syntaxhighlighterfn, NULL); // Configure editor with syntax highlighter
    inline_setpalette(edit, (int) sizeof(palette)/sizeof(int), palette);
    inline_multiline(edit, multilinefn, NULL, "~ "); // Configure editor for multiline editing

    printf("Editor created successfully.\n");

    for (int i=0; i<1; i++) {
        char *line = inline_readline(edit);
        if (line) {
            printf("You entered: '"); 
            inline_displaywithsyntaxcoloring(edit, line);
            printf("'\n");
            if (strcmp(line, "quit")==0) break;
            free(line);
        } else {
            printf("inline_readline returned NULL.\n");
        }
    }

    inline_free(edit);
    printf("Editor freed.\n");

    return 0;
}
