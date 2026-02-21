![Inline](../assets/inlinelogo.png#gh-light-mode-only)![Inline](../assets/inlinelogo-dark.png#gh-dark-mode-only)

Inline is a small, grapheme-aware line editor designed for embedding in other applications. It provides a modern text model and features such as syntax highlighting, history, autocomplete, copy/paste, and multiline editing, while remaining lightweight and portable.

This document describes the complete inline API; the 11 core API functions and some additional helpers are also documented in inline.h. An example application that reads lines of C source code with all features is provided to illustrate how to implement callbacks required by the API. 

In this document, “grapheme” refers to a Unicode extended grapheme cluster as defined by Unicode Standard Annex #29.

## Minimal line editing

A minimal line editor is set up and used in a few lines of C: 

```c
    inline_editor *edit = inline_new(">"); // Create an editor
    char *line = inline_readline(edit); // Read a line of text
    free(line); 
    inline_free(edit); // Free the editor and attached data
```

You may call `inline_readline` as many times as you wish. Each time it will return a utf8 encoded string that has been malloc allocated; you now own the string and must free it when you're done with it. With these four lines, you already get basic line editing with grapheme awareness, history, copy/paste. Additional features are enabled by calling a few configuration methods, often requiring you to supply a callback function that helps inline work. 

A more complete line editor configuration looks like this: 

```c
    inline_editor *edit = inline_new("> ");
    if (!edit) return; // Replace with failure handling code

    inline_autocomplete(edit, completefn, NULL); // Configure editor with autocomplete
    inline_syntaxcolor(edit, syntaxhighlighterfn, NULL); // Configure editor with syntax highlighter
    inline_setpalette(edit, nentries, palette);
```

where `completefn` and `syntaxhighlighterfn` are callback functions defined elsewhere, and palette is a table of colors you supply. 

## Background on Graphemes

A utf8 encoded string is a sequence of Unicode codepoints that may be 1-4 bytes in length. A codepoint might be a single byte representing an ASCII character (e.g. the letter Z encoded by 5A), a character from a different alphabet (e.g. Ω encoded by the byte sequence CE A9), or a symbol (e.g. 🦋 encoded as F0 9F A6 8B). 

A challenge for working with text is that the user's perception of what constitutes a "single" character does not map cleanly onto codepoints. For example, an accented character like á can be represented by the byte sequence C3 A1, but can also be represented by a sequence of two codepoints: 61 representing 'a' followed by CC 81 representing the acute accent.

Emoji can have very complex representations. For example, emoji can have modifiable skin tones like 👍🏽, which is represented by the sequence F0 9F 91 8D (👍 'thumbs up') followed by F0 9F 8F BD (medium skin tone modifier). The emoji 👨‍👩‍👧‍👦, depicting a possible family, is actually seven codepoints and 25 bytes. 

In each of these cases, the user would expect a user interface to treat these characters as a single coherent unit, which is called a "grapheme". They may well be unaware of the complexities of text representation, and could have obtained a grapheme from a number of sources such as selecting it from a menu or copying and pasting from the web.

Any functional line editor must be able to separate the user's input into its constituent graphemes, and use these as the fundamental unit of navigation. 

## Grapheme splitting 

Recognizing a grapheme is a nontrivial task. The Unicode space is large, and special codepoints that participate in grapheme construction interact in multiple ways. Libraries like `libunistring` or `libgrapheme` provide functions that perform the splitting correctly according to the Unicode standard, but are somewhat heavyweight and unnecessary for applications where exotic sequences are not expected. 

Inline therefore takes the following approach: a basic grapheme splitter is provided by default that is capable of recognizing many common sequences including those shown above [accents, skin tone modifiers, joined codepoints]. This should be good enough for many use cases. 

Where the default splitter is inadequate, it is very likely that the host application will already include a suitable external library anyway. Inline therefore allows you to supply your own splitter as a callback by calling a configuration function:

```c
    void inline_setgraphemesplitter(inline_editor *edit, inline_graphemefn fn);
```

the callback function must have the following signature:

```c
    size_t split(const char *in, const char *end);
```

When called by inline, the callback is provided with the start and end point of a string, and must return the size in bytes of the first grapheme or 0 indicating that no complete grapheme could be read. The callback is expected to be state free and should not allocate memory or modify the input buffer. 

Different Unicode libraries provide suitable functions with slightly different signatures, so you should use a shim function like the ones below. 

libunistring: 

```c
    #include <unigbrk.h>

    size_t libunistring_graphemefn(const char *in, const char *end) {
        char *next = (char *) u8_grapheme_next((uint8_t *) in, (uint8_t *) end);
        if (next>in) return next-in;
        return 0;
    }
```

libgrapheme: 

```c
    #include <grapheme.h>

    size_t libgrapheme_graphemefn(const char *in, const char *end) {
        return grapheme_next_character_break_utf8(in, end-in);
    }
```

## Grapheme display width calculations

In order to display and navigate text correctly, a line editor must be able to predict how graphemes will appear on the terminal. Some graphemes occupy a single column, as in a regular ASCII character, but many are wider. While the 👨‍👩‍👧‍👦 emoji contains many codepoints, for example, it is typically displayed in only two columns. In contrast to grapheme separation, which is standardized by the Unicode Consortium, terminal behavior varies widely. Many terminals only handle a subset of graphemes, and some do not display any correctly at all. Even when a grapheme is visually correct, terminal applications may miscalculate their display width, leading to incorrect cursor positioning and other behavior that may appear odd to the user. Some Unicode characters are almost never handled correctly, such as the ⸻ (three-em dash, codepoint 0x2e3b or utf8 byte sequence E2 B8 BB). 

Inline therefore provides a simple heuristic width estimator that works with many terminals and graphemes correctly, but allows the programmer to supply their own by calling a configuration function:

```c
    inline_setgraphemewidth(inline_editor *edit, inline_widthfn fn);
```

The callback has the following signature:

```c
    typedef int (*inline_widthfn)(const char *g, size_t len);
```

When called by inline, the callback is provided with a pointer to a grapheme and its length. It should return the display width of the grapheme in columns. 

In practice, programmers embedding inline are expected to override the default width estimator less frequently than the grapheme splitter. Importantly, note that width estimation functions provided by existing libraries (such as `u8_width()` in libunistring) are often *less* correct for interactive terminal use, as they operate on individual codepoints rather than complete graphemes. Typical use cases for supplying a custom width estimator include handling terminal-specific or grapheme-specific quirks or supporting applications that involve complex writing systems.

## History

Inline can maintain a history of previous input that the user can recall during an editing session using the up/down arrow keys. A new entry is added to the history by `inline_readline` after editing is complete before it returns to the caller; the host program need not add the entry itself. The contents of the history are managed by `inline` and free'd when the editor is free'd with `inline_free`. Two API functions are provided to control the history. The first,  

```c
    inline_sethistorylength(inline_editor *edit, int maxlen);
```

allows the programmer to set a length limit. The value of `maxlen` indicates, if `maxlen` is positive, the maximum number of history values stored. If `maxlen` is zero, the history feature is disabled entirely. Negative values of `maxlen` indicate unlimited history, which is the default setting. `inline_sethistorylength` may be called multiple times; if there are most history entries stored than allowed by `maxlen` excess entries are immediately free'd.

It is possible to add entries to the history manually using, 

```c
    inline_addhistory(inline_editor *edit, const char *entry);
```

The contents of `entry` are immediately copied into the history list provided entry is not `NULL` or empty; `entry` itself is not stored directly. The return value of `inline_addhistory` indicates whether the contents of `entry` were successfully added to the history list. 

## Autocomplete

Inline provides a mechanism for the application to offer autocomplete suggestions to the user. For example, a language REPL might wish to suggest keywords matching partially completed input. Inline handles when and if to offer suggestions but provides a callback mechanism to gather them from you. If you wish to enable the suggestion mechanism, use the configuration function:

```c
    inline_autocomplete(inline_editor *edit, inline_completefn fn, void *ref);
```

You must supply a callback function, and you may also supply an opaque pointer `ref`. Inline stores `ref` and passes it to the callback, but does not do anything else with it. You may change
the reference at any time by calling `inline_autocomplete` again. 

The callback has the following signature: 

```c
    typedef const char *(*inline_completefn) (const char *utf8, void *ref, size_t *index);
```

Inline invokes this callback when it wishes to gather suggestions by calling it repeatedly with a pointer to the complete input buffer `utf8` and the reference pointer `ref`. The parameter `index` points to a `size_t` variable that inline initializes to zero at the start of each suggestion-gathering sequence. The callback should return a pointer to a matching completion, or `NULL` when no further matches are available. The callback must update the value of `index` between calls to avoid returning the same suggestion more than once.

Importantly, the returned string must correspond to the *remaining* characters of the suggestion and not the complete match. For example, if the user typed "ty" in an application reading C source code, the callback would recognize "typedef" as a matching completion and would return "pedef". Typically this is achieved by returning an offset pointer `(match + strlen(partialmatch))`.

Inline does not interpret the contents of the `index` variable beyond initializing it to zero. This allows applications to implement the callback using a variety of data structures, such as linear scans, trees, or hash tables, without imposing additional constraints.

Inline immediately copies any returned suggestion string and does not attempt to free the pointer returned by the callback. In practice, suggestions are usually drawn from static strings or from existing data structures such as symbol tables. The callback may therefore safely return a pointer into an existing string, provided it remains valid until the next call.

Autocomplete callbacks should be implemented efficiently to ensure a smooth user experience. Avoid memory allocation or expensive matching operations within the callback.

## Syntax highlighting

Syntax highlighting is a feature that displays elements of the buffer in different colors to provide the user with semantic information and facilitate their reading of the text. This is provided via a callback mechanism with a simple division of labor: the callback is responsible for parsing the text and deciding which elements should be highlighted and the color to use, while inline is responsible for the actual display. 

To enable the feature, you must call the configuration function:

```c
    inline_syntaxcolor(inline_editor *edit, inline_syntaxcolorfn fn, void *ref);
```

You must supply a callback function and an opaque reference pointer `ref`, which can be `NULL`. Inline stores the pointer and passes it to the callback but does not otherwise attempt to examine the contents. To change the reference pointer, simply call `inline_syntaxcolor` again with a new value. It is also necessary to provide a palette as will be described below. 

The callback has the following signature: 

```c
    typedef bool (*inline_syntaxcolorfn) (const char *utf8, void *ref, size_t offset, inline_colorspan_t *out);
```

When inline wishes to understand how to highlight the buffer, it repeatedly calls the callback to determine the next span to color. The complete contents of the input buffer are passed in `utf8`. The callback should start scanning the buffer from byte offset `offset` and determine the end of the next span to color. Spans must not overlap and should be monotonically increasing. When the callback has done so, it fills out the structure `inline_colorspan_t`:

```c
    typedef struct {
        size_t byte_end; /* exclusive end of color span */
        int color;     /* Index into color palette */
    } inline_colorspan_t;
```

The callback must update the `byte_end` and `color` entries and notably must always advance `byte_end` beyond `offset` or undefined behavior occurs. Note that the `color` entry is an index into a palette array, not a color value directly. The callback should return `true` if there are more spans to color and `false` to halt highlighting and display any remaining text in the default color. Inline resets terminal attributes at the end of each redraw.

The palette is configured by calling: 

```c
    inline_setpalette(inline_editor *edit, int count, const int *palette);
```

The caller supplies an array of integers representing colors and `count` indicating the length of the array. Inline copies the contents of the array and does not store the pointer you supply. 

Colors are encoded as follows: 

* The value of -1 indicates "use the default text color". 
* Values 0-7 are the classic ANSI terminal colors. inline.h supplies macros for these: INLINE_BLACK, INLINE_RED, INLINE_GREEN, INLINE_YELLOW, INLINE_BLUE, INLINE_MAGENTA, INLINE_CYAN, INLINE_WHITE.
* Values 8-255 are color codes for 256 color terminals. 256 color terminals use values 0-7 the same as the ANSI terminals.
* 24-bit RGB colors are encoded as 0x01RRGGBB. Note that the high byte is set to 1 to enable inline to distinguish purely blue shades from 256 color mode codes. A macro INLINE_RGB(r,g,b) is provided in inline.h to assemble these from r,g,b hex values. 

Inline does not validate color values; invalid values result in undefined terminal output. Not all terminals provide 24-bit color or even 256 color modes and inline does not attempt to determine the color capabilities of the terminal automatically at runtime. It is therefore recommended that color settings be provided as a user-configurable option in the host application. 

## Multiline editing

Multiline editing is enabled by calling a configuration function:

```c
    bool inline_multiline(inline_editor *edit, inline_multilinefn fn, void *ref, const char *continuation_prompt);
```

You must supply a callback that is used to decide whether to enter multiline mode given particular input, and an opaque reference pointer `ref` that will be supplied to the callback, which can be `NULL`. As for other inline API functions, inline simply stores this reference and does not attempt to inspect or modify its contents. You may also specify a special prompt `continuation_prompt` that will be displayed only on continuation lines, or supply `NULL` to use the default prompt. Inline copies this prompt immediately and does not store the string you supply. `inline_multiline` returns true if the callback and prompt are set successfully. 

The callback has the following signature: 

```c
    typedef bool (*inline_multilinefn) (const char *utf8, void *ref);
```

When inline wishes to decide whether to enter multiline editing mode---typically this might happen when the user presses the Return or Enter key---it calls your callback with the complete contents of the input buffer in `utf8` and the reference pointer you supplied at configuration. 

The callback should inspect the text and return `true` to enter multiline mode or `false` otherwise. If the callback returns `true`, inline inserts a newline into the input buffer and continues editing on a new line. When the callback later returns `false`, multiline mode ends and  `inline_readline` returns the complete buffer (including embedded newlines).

The callback should be fast and side-effect free; it may be called on every Enter press. Hence, the decision is usually made heuristically rather than by detailed parsing. This simple example checks for an unmatched opening parenthesis for example, as might be useful in implementing a simple calculator or LISP interpreter:

```c
    static bool multilinefn(const char *in, void *ref) {
        int nb=0; 
        for (char *c=in; *c!='\0'; c++) { // Match parentheses
            switch (*c) {
                case '(': nb+=1; break; 
                case ')': nb-=1; break;
                default: break; 
            }
        }
        return (nb>0); // Is there an unmatched left parenthesis?
    }
```

## Terminal helper functions

Inline also provides a small number of utility functions to assist programmers implementing terminal-based applications. 

The first,

```c
    bool inline_checktty(void);
```

returns `true` if both `stdin` and `stdout` refer to terminal devices capable of interactive input and output, and `false` otherwise. This can be used to detect whether the application is running in an interactive terminal or whether input or output has been redirected (for example, via a UNIX pipe or file).

A second function,

```c
    bool inline_checksupported(void);
```

returns `true` if the current terminal is likely capable of processed output, i.e. VT100 escape sequences, etc. This can be used to detect whether the terminal application should use such sequences. 

To get the width of the terminal, use

```c
    bool inline_getterminalwidth(int *width);
```

which sets the contents of `width` to the width of the terminal in columns and returns `true` on success. If the operation fails, `width` is not modified; the caller could use a default value in this case, for example.

To ensure the terminal is in UTF8 mode (necessary on windows),

```c
    void inline_setutf8(void);
```

To emit a string to `stdout` using the same mechanism as `inline`, use,

```c
    void inline_emit(const char *str);
```

To emit a control code corresponding to a given palette color, 

```c
    void inline_emitcolor(int color);
```

The color is supplied in the format used for `inline_setpalette` above.

To display a string using inline’s syntax highlighting mechanism without enabling interactive editing, use: 

```c
    void inline_displaywithsyntaxcoloring(inline_editor *edit, const char *string);
```

This function applies the same syntax highlighting callback configuration used by the interactive editor, but does not read input or modify editor state. It simply renders the supplied string to the terminal, with no newline appended, and resets the terminal state after rendering.

A language REPL could use this, for example, to display source code with error messages or the ability to list sections of code in a debugger. 

## Crash conditions and signal handling

When `inline_readline` enters raw mode it also installs “emergency” handlers so the terminal is restored if the process is interrupted. On POSIX this uses the signal mechanism for SIGTERM, SIGQUIT, SIGHUP (graceful termination), SIGSEGV, SIGABRT, SIGBUS, SIGFPE (crash signals), and SIGWINCH (resize); on Windows it uses `SetConsoleCtrlHandler`. The handlers attempt to restore the saved terminal state, then chain to any previous handler when appropriate, and finally re-raise/reset to the default disposition so the process terminates normally. A small `atexit` restore is also registered as a last resort. Signal handlers are removed when raw mode is exited.

If you embed inline into an application that already owns signal handling (or must not install/replace handlers), compile with `INLINE_NO_SIGNALS` defined to disable this feature. When `INLINE_NO_SIGNALS` is defined, inline will still restore the terminal on the normal return path, but it will not install signal/console handlers—so your application is responsible for restoring terminal state on abnormal termination.
