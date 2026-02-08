/** @file main.c
 *  @author T J Atherton
 *
 *  @brief Example terminal application illustrating inline's key features.  */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "inline.h"

/* **********************************************************************
 * Callback definitions
 * ********************************************************************** */

/* -----------------------
 * Autocomplete
 * ----------------------- */

/** Null terminated list of keywords to autocomplete; these are for C (we include 'quit'). */
static const char *words[] = { 
    "auto","break","case","char","const","continue","default","do","double","else","enum",
    "extern","float","for","goto","if","int","long","quit","register","return","short","signed",
    "sizeof","static","struct","switch","typedef","union","unsigned","void","volatile","while", NULL };

/** @brief Autocomplete function.
 *  @details Here, we find the last word boundary and try to match the last word fragment against a 
 *           list of known keywords. If the list of possible matches is large, use a binary search or
 *           similar. */
const char *completefn(const char *buffer, void *ref, size_t *index) {
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
            return words[i] + tok_len;  // Return suffix only 
        }
    }

    return NULL; // No more words to match so we're done
}

/* -----------------------
 * Syntax coloring
 * ----------------------- */

/** Define color palette */
static int palette[] = {
    -1,                           // 0 = default
    INLINE_MAGENTA,               // 1 = keywords
    INLINE_RGB(0x33, 0xCC, 0xAA), // 2 = strings
    INLINE_RGB(0xD9, 0xA5, 0x21), // 3 = numbers
};

/** Helper to set the contents of an inline_color_span_t structure */
static inline void inline_set_colorspan(inline_colorspan_t *s, size_t end, int color) {
    s->byte_end   = end;
    s->color      = color; // Entry into palette
}

/** @brief Example C syntax highlighter 
 *  @details We are passed the start of a span to color and attempt to match: 
 *              keywords -> map to col 1 (note these are palette entries)
 *              strings -> map to col 2
 *              integers -> map to col 3
 *           Any unrecognized is mapped to col 0 (default) */
static bool syntaxhighlighterfn(const char *utf8, void *ref, size_t offset, inline_colorspan_t *out) {
    (void)ref; // unused

    if (utf8[offset] == '"') { // Match strings
        offset++;
        for (; utf8[offset] != '\0'; offset++) {
            if (utf8[offset] == '"' && utf8[offset - 1] != '\\') { // Skip escaped \"
                offset++;
                break;
            }
        }
        inline_set_colorspan(out, offset, 2); 
        return true;
    }

    if (isdigit((unsigned char)utf8[offset])) { // Match integers
        for (offset++; utf8[offset] && isdigit((unsigned char)utf8[offset]); offset++);
        inline_set_colorspan(out, offset, 3);
        return true;
    }

    if (isalpha((unsigned char)utf8[offset]) || utf8[offset] == '_') { // Match keywords
        size_t start = offset;

        // Find end of token:
        for (offset++; utf8[offset] && (isalnum((unsigned char)utf8[offset]) || utf8[offset] == '_'); offset++);

        size_t tok_len = offset - start; 
        for (size_t i = 0; words[i]; i++) { // Match against keywords
            const char *w = words[i];
            if (w[tok_len] == '\0' && strncmp(utf8 + start, w, tok_len) == 0) {
                inline_set_colorspan(out, offset, 1);
                return true;
            }
        }
    }

    inline_set_colorspan(out, offset + 1, 0); // Anything else simply return the next byte
    return true;
}

/* -----------------------
 * Multiline editing
 * ----------------------- */

/** @brief Multiline decision
 *  @details This function should decide if input is "complete" and if multiline input is meant. 
 *           Here, we used the heuristic of matching brackets and enter multiline if there's an
 *           unmatched open bracket. Real implementations should parse. */
static bool multilinefn(const char *in, void *ref) {
    int nb=0; 
    for (const char *c=in; *c!='\0'; c++) { // Match brackets
        switch (*c) {
            case '(': case '{': case '[': nb+=1; break; 
            case ')': case '}': case ']': nb-=1; break;
            default: break; 
        }
    }
    return (nb>0); // Is there an unmatched open bracket?
}

/* **********************************************************************
 * Minimal REPL: Get input and echo it back
 * ********************************************************************** */

int main(void) {
    printf("Inline editor test... (type 'quit' to exit)\n");

    /** Create editor */
    inline_editor *edit = inline_new("> ");
    if (!edit) {
        fprintf(stderr, "inline_new failed\n");
        return 1;
    }

    /** Configure editor */
    inline_sethistorylength(edit, 5); // Bound history
    inline_autocomplete(edit, completefn, NULL); // Configure editor with autocomplete
    inline_syntaxcolor(edit, syntaxhighlighterfn, NULL); // Configure editor with syntax highlighter
    inline_setpalette(edit, (int) sizeof(palette)/sizeof(int), palette);
    inline_multiline(edit, multilinefn, NULL, "~ "); // Configure editor for multiline editing

    for (bool quit=false; !quit;) {
        char *line = inline_readline(edit);
        if (line) {
            if (strcmp(line, "quit")==0) {
                quit=true;
            } else {
                printf("You entered: '"); 
                inline_displaywithsyntaxcoloring(edit, line); // Use highlighting function
                printf("'\n");
            }
            free(line); // We own line, so we must free it. 
        } else {
            printf("inline_readline returned NULL.\n");
        }
    }

    inline_free(edit); // All attached data is free'd. 
    return 0;
}
