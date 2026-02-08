# in|line 

Inline - a small, grapheme-aware[*] line editor for embedding in other applications. 

At the core of many interactive terminal applications is a Read-Evaluate-Print Loop (REPL) where the user supplies input and the application responds accordingly. Inline supplies the "read" component of this pattern, and returns a UTF8 encoded string that the application can process. Inline benefits the user by providing features such as syntax highlighting, history, autocomplete, copy/paste, and multiline editing, while remaining lightweight and portable.

A minimal application using inline is as simple as: 

    #include "inline.h" 

    int main(void) {
        inline_editor *edit = inline_new(">"); // Create an editor and set the prompt
        for (bool done=false; !done; ) {
            char *line = inline_readline(edit); // Read a line of text
            if (line && line[0]=='q') done=true; // Quit
            printf("%s\n", line);
            free(line); // You own the string returned from inline_readline 
        } 
        
        inline_free(edit); // Free the editor and attached data
    }

Inline is intentionally callback-driven. The editor owns editing, rendering, and terminal interaction; the host application owns the semantics of the input. Features such as syntax highlighting and multiline editing are configured by supplying callback functions. Core components of inline's grapheme processing engine can be replaced. Further details are in "inline_api.md" supplied in this repository. 

Inline is cross platform: both POSIX-like operating systems (macOS, linux) and Windows are supported. There are no dependencies beyond the C standard library and either standard windows or POSIX libraries. Inline is provided under the [MIT license](LICENSE). 

[*] A "grapheme" is a sequence of Unicode codepoints that correspond approximately to what a user perceives as a single character or glyph on the display. A full definition is in Unicode Standard Annex #29.

## Who is Inline for?

inline is a good fit if you are building:

- a language REPL
- an interactive debugger or CLI tool
- a runtime or interpreter with embedded input
- a terminal application that must handle modern Unicode text correctly

## Why Inline? 

Modern text is significantly more complicated than the ASCII strings that physical terminals were designed to display. Users may input text from multiple languages, for example, as well as emoji and other graphical characters. Simple assumptions such as 'one byte = one visible character' or even 'one codepoint = one visible character' are not correct in Unicode-encoded text, and both processing and displaying such text is nontrivial. At the same time, terminal applications better match the rest of a modern GUI if they support syntax highlighting and provide autocomplete suggestions. 

Inline has been designed to provide these features while remaining small (~2000 lines of code and as little as 50kb compiled). Care has been taken to keep the API focussed but expressive: inline places very few constraints on data structures and does not impose a particular token model, for example. Graphemes are the central abstraction, which simplifies the overall implementation; inline supports most common graphemes natively. Programmers implementing Terminal applications where complex grapheme sequences are expected are likely to utilize a unicode library like `libunistring` or `libgrapheme` anyway; inline's grapheme engine can therefore be overridden through callbacks using such libraries without recompilation. 

## Non-goals and limitations

Inline is not intended to be a full replacement for traditional libraries like readline. It does not implement advanced features such as vi-style editing modes, history expansion, or complete Unicode grapheme width correctness across all terminals. Instead, inline focuses on correctness for common modern terminal usage, while remaining lightweight and easy to embed.

## Other line editors

Inline is one of many possible choices for a line editor, each of which may be useful in different applications. A brief comparison is shown below: 

|              | Size        | Grapheme aware? | Syntax highlighting? | Multiline? | Use case                                       |
| ------------ | ----------- | --------------- | -------------------- | ---------- | ---------------------------------------------  |
| readline     | large       | some            | no                   | yes        | Legacy applications, shells                    |
| linenoise    | small       | no              | no                   | yes        | When size is the primary concern               |
| linenoise-ng | small       | partial         | no                   | yes        | Lightweight C++ replacement with Unicode fixes |
| bestline     | small       | no              | no                   | yes        | Pragmatic readline-style editing               |
| replxx       | medium      | partial         | yes                  | yes        | Feature-rich C++ REPLs                         |
| isocline     | medium      | yes             | yes                  | yes        | Modern, opinionated C line editor              |
| **inline**   | small       | yes             | yes                  | yes        | Embeddable, grapheme-aware terminal editing    |

## Documentation

- [API reference](docs/inline_api.md)