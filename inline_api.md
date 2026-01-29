# in|line 

Inline is a small, grapheme-aware line editor designed for embedding in other applications. It provides a modern text model and features such as syntax highlighting, history, autocomplete, copy/paste, and multiline editing, while remaining lightweight and portable.

This document describes the complete inline API; the 11 API functions are also documented in inline.h

## Minimal line editing

A minimal line editor is set up and used in a few lines of C: 

    inline_editor *edit = inline_new(">"); // Create an editor
    char *line = inline_readline(edit); // Obtain a line of text
    free(line); 
    inline_free(edit); // Free the editor and attached data

You may call `inline_readline` as many times as you wish. Each time it will return a utf8 encoded string that has been malloc allocated; you now own the string and must free it when you're done with it. With these four lines, you already get basic line editing with grapheme awareness, history, copy/paste. Additional features are enabled by calling a few configuration methods, often requiring you to supply a callback function that helps inline work. 

A more complete line editor configuration looks like this: 

    inline_editor *edit = inline_new("> ");
    if (!edit) return; // Replace with failure handling code

    inline_autocomplete(edit, completefn, NULL); // Configure editor with autocomplete
    inline_syntaxcolor(edit, syntaxhighlighterfn, NULL); // Configure editor with syntax highlighter
    inline_setpalette(edit, nentries, palette);

where `completefn` and `syntaxhighlighterfn` are callback functions defined elsewhere, and palette is a table of colors you supply. 

## Background on Graphemes

A utf8 encoded string is a sequence of unicode codepoints that may be 1-4 bytes in length. A codepoint might be a single byte representing an ASCII character (e.g. the letter Z encoded by 5A), a character from a different alphabet (e.g. Ω encoded by the byte sequence CE A9), or a symbol (e.g. 🦋 encoded as F0 9F A6 8B). 

A challenge for working with text is that the user's perception of what constitutes a "single" character does not map cleanly onto codepoints. For example, an accented character like á can be represented by the byte sequence C3 A1, but can also be represented by a sequence of two codepoints: 61 representing 'a' followed by CC 81 representing the acute accent.

Emoji can have very complex representations. For example, emoji can have modifiable skin tones like 👍🏽, which is represented by the sequence F0 9F 91 8D (👍 'thumbs up') followed by F0 9F 8F BD (medium skin tone modifier). The emoji 👨‍👩‍👧‍👦, depicting a family, is actually seven codepoints and 25 bytes. 

In each of these cases, the user would expect a user interface to treat these characters as a single coherent unit, which is called a "grapheme". They may well be unaware of the complexities of text representation, and could have obtained it from a number of sources such as selecting it from a menu or copying and pasted from the web.

Any functional line editor must be able to separate the user's input into its constituent graphemes, and use these as the fundamental unit of navigation. 

## Grapheme splitting 

Recognizing a grapheme is a nontrivial task. The Unicode space is large, and special codepoints that participate in grapheme construction interact in multiple ways. Libraries like `libunistring` or `libgrapheme` provide functions that perform the splitting correctly according to the unicode standard, but are somewhat heavyweight and unnecessary for applications where exotic sequences are not expected. 

Inline therefore take the following approach: a basic grapheme splitter is provided by default that is capable of recognizing many common sequences including those shown above [accents, skin tone modifiers, joined codepoints]. This should be good enough for many use cases. 

Where the default splitter is inadequate, it is very likely that the host application will already include a suitable external library anyway. Inline therefore allows you to supply your own splitter as a callback with the following signature:

    size_t split(const char *in, const char *end);

The callback is provided with the start and end point of a string, and must return the size in bytes of the first grapheme or 0 indicating that no complete grapheme could be read. The callback is expected to be state free and should not allocate memory or modify the input buffer. 

Different unicode libraries provide suitable functions with slightly different signatures, so you should use a shim function like the ones below. 

libunistring: 

    #include <unigbrk.h>

    size_t libunistring_graphemefn(const char *in, const char *end) {
        char *next = (char *) u8_grapheme_next((uint8_t *) in, (uint8_t *) end);
        if (next>in) return next-in;
        return 0;
    }

libgrapheme: 

    #include <grapheme.h>

    size_t libgrapheme_graphemefn(const char *in, const char *end) {
        return grapheme_next_character_break_utf8(in, end-in);
    }

## Autocomplete

## Syntax highlighting

## Multiline editing

## Utility functions

Inline also provides a couple of functions to assist programmers implementing terminal applications. 