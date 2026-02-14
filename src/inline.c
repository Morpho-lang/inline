/** @file inline.c
 *  @author T J Atherton
 *
 *  @brief A simple grapheme aware line editor with history, completion, multiline editing and syntax highlighting */

#include "inline.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#ifdef _WIN32
    #include <windows.h>
    #include <io.h>
    #include <conio.h>
    #define read _read
    #define write _write
    #define isatty _isatty
    #define STDIN_FILENO _fileno(stdin)
    #define STDOUT_FILENO _fileno(stdout)
#else
    #include <termios.h>
    #include <unistd.h>
    #include <sys/ioctl.h>
    #include <sys/types.h>
    #include <signal.h>
    #include <strings.h>
#endif

#define INLINE_DEFAULT_BUFFER_SIZE 128
#define INLINE_DEFAULT_PROMPT ">"

#define INLINE_ESCAPECODE_MAXLENGTH 32

#define INLINE_INVALID -1

#define INLINE_TAB_WIDTH 2

//#define INLINE_NO_SIGNALS // <- Uncomment to disable installation of signals

#ifdef _WIN32
typedef DWORD termstate_t;
#else
typedef struct termios termstate_t;
#endif

/* **********************************************************************
 * Line editor data structures and configuration
 * ********************************************************************** */

/** Simple list of strings type */
typedef struct inline_stringlist {
    char **items;   // List of strings
    int count;      // Number of strings
    int index;      // Current index
} inline_stringlist_t;

/** Viewport */
typedef struct {
    int first_visible_line;  // Vertical scroll offset
    int first_visible_col;   // Horizontal scroll offset
    int screen_rows;         // Viewport height
    int screen_cols;         // Viewport width (excludes prompt, which is not part of the viewport)
} inline_viewport;

/** The editor data structure */
typedef struct inline_editor {
    char *prompt;
    char *continuation_prompt;

    int ncols;                            // Number of columns

    char *buffer;                         // Buffer holding UTF8
    size_t buffer_len;                    // Length of contents in bytes
    size_t buffer_size;                   // Size of buffer allocated in bytes

    char *clipboard;                      // Clipboard buffer
    size_t clipboard_len;                 // Length of contents in bytes
    size_t clipboard_size;                // Size of clipboard in bytes

    size_t *graphemes;                    // Offset to each grapheme
    int grapheme_count;                   // Number of graphemes
    size_t grapheme_size;                 // Size of grapheme buffer in bytes

    size_t *lines;                        // Offset to each line
    int line_count;                       // Number of lines
    size_t line_size;                     // Size of line buffer in bytes

    int cursor_posn;                      // Position of cursor in graphemes
    int selection_posn;                   // Selection posn in graphemes
    int term_cursor_row;                  // Record the cursor's physical row
    int term_lines_drawn;                 // Record how many lines were previously drawn

    inline_syntaxcolorfn syntax_fn;       // Syntax coloring callback
    void *syntax_ref;                     // User reference

    int *palette;                         // Palette: list of colors
    int palette_count;                    // Length of palette list

    inline_completefn complete_fn;        // Autocomplete callback
    void *complete_ref;                   // User reference

    inline_stringlist_t suggestions;      // List of suggestions from autocompleter
    bool suggestion_shown;                // Set if renderer was able to show a suggestion

    inline_multilinefn multiline_fn;      // Multiline callback
    void *multiline_ref;                  // User reference

    inline_graphemefn grapheme_fn;        // Custom grapheme splitter
    inline_widthfn width_fn;              // Custom grapheme width function

    inline_stringlist_t history;          // List of history entries
    int max_history_length;               // Maximum length of the history

    inline_viewport viewport;             // Terminal viewport

#ifdef _WIN32                             // Preserve terminal state
    termstate_t termstate_in;
    termstate_t termstate_out;
#else
    termstate_t termstate;
#endif
    bool rawmode_enabled;                 // Record if rawmode has already been enabled

    bool refresh;                         // Set to refresh on next redraw
} inline_editor;

static inline_editor *inline_lasteditor = NULL;

// Forward declarations
static char *inline_strdup(const char *s);
static void inline_disablerawmode(inline_editor *edit);
static void inline_stringlist_init(inline_stringlist_t *list);
static void inline_stringlist_clear(inline_stringlist_t *list);
static void inline_recomputelines(inline_editor *edit);
static void inline_recomputegraphemes(inline_editor *edit);
static bool inline_insert(inline_editor *edit, const char *bytes, size_t nbytes);
static void inline_clear(inline_editor *edit);
static void inline_clearselection(inline_editor *edit);
static void inline_clearsuggestions(inline_editor *edit);
static bool inline_stringwidth(inline_editor *edit, const char *str, int *width);

/* -----------------------
 * New/free API
 * ----------------------- */

/** API function to create a new line editor */
inline_editor *inline_new(const char *prompt) {
    inline_editor *edit = calloc(1, sizeof(*edit)); // All contents are zero'd
    if (!edit) return NULL;

    edit->prompt = inline_strdup(prompt ? prompt : INLINE_DEFAULT_PROMPT);
    if (!edit->prompt) goto inline_new_cleanup;

    edit->buffer_size = INLINE_DEFAULT_BUFFER_SIZE; // Allocate initial buffer
    edit->buffer = malloc(edit->buffer_size);
    if (!edit->buffer) goto inline_new_cleanup;

    edit->buffer[0] = '\0'; // Ensure zero terminated
    edit->buffer_len = 0;

    edit->selection_posn = INLINE_INVALID; // No selection
    edit->max_history_length = INLINE_INVALID; // Unlimited history

    inline_stringlist_init(&edit->suggestions);
    inline_stringlist_init(&edit->history);

    inline_recomputegraphemes(edit);
    inline_recomputelines(edit);

    return edit;

inline_new_cleanup:
    inline_free(edit);
    return NULL;
}

/** API function to free a line editor and associated resources */
void inline_free(inline_editor *edit) {
    if (!edit) return;

    free(edit->prompt);
    free(edit->continuation_prompt);

    free(edit->buffer);
    free(edit->graphemes);
    free(edit->lines);
    free(edit->clipboard);

    inline_clearsuggestions(edit);
    inline_stringlist_clear(&edit->history);

    free(edit->palette);

    if (inline_lasteditor==edit) inline_lasteditor = NULL;

    free(edit);
}

/* -----------------------
 * Configuration API
 * ----------------------- */

/** API function to enable syntax coloring */
void inline_syntaxcolor(inline_editor *edit, inline_syntaxcolorfn fn, void *ref) {
    edit->syntax_fn = fn;
    edit->syntax_ref = ref;
}

/** API function to set the color palette */
bool inline_setpalette(inline_editor *edit, int count, const int *palette) {
    free(edit->palette); // Clear any old palette data
    edit->palette = NULL;
    edit->palette_count = 0;

    if (count <= 0 || palette == NULL) return false;

    edit->palette = malloc(sizeof(int) * count);
    if (!edit->palette) return false;

    memcpy(edit->palette, palette, sizeof(int) * count);
    edit->palette_count = count;
    return true;
}

/** API function to enable autocomplete */
void inline_autocomplete(inline_editor *edit, inline_completefn fn, void *ref) {
    edit->complete_fn = fn;
    edit->complete_ref = ref;
}

/** API function to enable multiline editing */
bool inline_multiline(inline_editor *edit, inline_multilinefn fn, void *ref, const char *continuation_prompt) {
    edit->multiline_fn = fn;
    edit->multiline_ref = ref;

    char *p = inline_strdup(continuation_prompt ? continuation_prompt : edit->prompt);
    if (p) {
        free(edit->continuation_prompt);
        edit->continuation_prompt = p;
    }
    return (p!=NULL);
}

/** API function to use a custom grapheme splitter */
void inline_setgraphemesplitter(inline_editor *edit, inline_graphemefn fn) {
    edit->grapheme_fn = fn;
}

/** API function to use a custom grapheme width function */
void inline_setgraphemewidth(inline_editor *edit, inline_widthfn fn) {
    edit->width_fn = fn;
}

/* **********************************************************************
 * Platform-dependent code
 * ********************************************************************** */

/* ----------------------------------------
 * Check terminal features
 * ---------------------------------------- */

/** API function to check whether stdin and stdout are TTYs. */
bool inline_checktty(void) {
    return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
}

/** Check whether the terminal type is supported. */
bool inline_checksupported(void) {
#ifndef _WIN32
    const char *term = getenv("TERM");
    if (!term || !*term) return false;

    static const char *deny[] = { "dumb", "cons25", "emacs", NULL };

    for (const char **p = deny; *p; p++) {
        if (strcasecmp(term, *p) == 0) return false;
    }
#endif
    return true; // Windows and other terminals are supported
}

/** Read the width from the terminal */
bool inline_getterminalwidth(int *width) {
#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;

    if (GetConsoleScreenBufferInfo(h, &csbi)) {
        if (width) *width=csbi.srWindow.Right - csbi.srWindow.Left + 1;
        return true;
    }
#else
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != -1 && ws.ws_col > 0) {
        if (width) *width=ws.ws_col;
        return true;
    }
#endif
    return false;
}

/** Update the terminal width */
static void inline_updateterminalwidth(inline_editor *edit) {
    int width = 80; // fallback
    inline_getterminalwidth(&width);
    edit->ncols = width;
}

/** Update viewport width based on current terminal width (preserves viewport position) */
static void inline_updateviewportwidth(inline_editor *edit) {
    int prompt_width;
    if (!inline_stringwidth(edit, edit->prompt, &prompt_width)) prompt_width = 0;
    edit->viewport.screen_cols = edit->ncols - prompt_width - 1; // Reserve last col to avoid pending wrap state
}

/* ----------------------------------------
 * Handle crashes
 * ---------------------------------------- */

static void inline_atexitrestore(void) {
    if (inline_lasteditor) inline_disablerawmode(inline_lasteditor);
}

#ifdef _WIN32
static bool termstate_set = false;
static termstate_t termstate_in;
static termstate_t termstate_out;
static int resize_pending = 0;
static bool consolehandler_installed = false;
static BOOL WINAPI inline_consolehandler(DWORD ctrl) {
    (void) ctrl;
    if (termstate_set) {
        HANDLE hIn  = GetStdHandle(STD_INPUT_HANDLE);
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        SetConsoleMode(hIn,  termstate_in);
        SetConsoleMode(hOut, termstate_out);
    }
    return FALSE; // Allow default behavior
}
#else
termstate_t termstate;
static volatile sig_atomic_t termstate_set = 0;
static volatile sig_atomic_t resize_pending = 0;
typedef struct {
    int sig;
    void (*handler)(int, siginfo_t *, void *);
    int flags;
    bool has_previous;
    bool installed;
    struct sigaction previous;
} signalhandlerstate_t;

signalhandlerstate_t *inline_findsighandler(int sig);

static bool inline_callprevious(int sig, siginfo_t *info, void *ucontext) {
    signalhandlerstate_t *handler = inline_findsighandler(sig);
    if (!handler || !handler->has_previous || !handler->installed ||
        handler->previous.sa_handler == SIG_IGN || handler->previous.sa_handler == SIG_DFL) return false;

#ifdef SA_SIGINFO
    if (handler->previous.sa_flags & SA_SIGINFO) {
        if (handler->previous.sa_sigaction) handler->previous.sa_sigaction(sig, info, ucontext);
        return true;
    }
#endif
    if (handler->previous.sa_handler) {
        handler->previous.sa_handler(sig);
        return true;
    }
    return false;
}

static void inline_restoredisposition(int sig) {
    signalhandlerstate_t *handler = inline_findsighandler(sig);
    
    if (handler && handler->has_previous && handler->previous.sa_handler != SIG_IGN) {
        sigaction(sig, &handler->previous, NULL);
    } else {
        struct sigaction restore;
        memset(&restore, 0, sizeof(restore));
        restore.sa_handler = SIG_DFL;
        sigemptyset(&restore.sa_mask);
        sigaction(sig, &restore, NULL);
    }
}

static void inline_emergencyrestore(void) {
    if (termstate_set) tcsetattr(STDIN_FILENO, TCSAFLUSH, &termstate);
}
static void inline_signalwinchhandler(int sig, siginfo_t *info, void *ucontext) {
    resize_pending=1;
    inline_callprevious(sig, info, ucontext);
}
static void inline_signalgracefulhandler(int sig, siginfo_t *info, void *ucontext) {
    inline_emergencyrestore();
    if (inline_callprevious(sig, info, ucontext)) return; // If the previous signal handler was called and returned, we do too
    inline_restoredisposition(sig);
    kill(getpid(), sig);
    _Exit(128 + sig);
}
static void inline_signalcrashhandler(int sig, siginfo_t *info, void *ucontext) {
    inline_emergencyrestore();
    inline_restoredisposition(sig);
    kill(getpid(), sig);
    _Exit(128 + sig);
}

static signalhandlerstate_t siglist[] = {
    { SIGWINCH, inline_signalwinchhandler,    SA_SIGINFO | SA_RESTART, false, false, { {0} } },
    { SIGTERM,  inline_signalgracefulhandler, SA_SIGINFO,              false, false, { {0} } },
    { SIGQUIT,  inline_signalgracefulhandler, SA_SIGINFO,              false, false, { {0} } },
    { SIGHUP,   inline_signalgracefulhandler, SA_SIGINFO,              false, false, { {0} } },
    { SIGSEGV,  inline_signalcrashhandler,    SA_SIGINFO,              false, false, { {0} } },
    { SIGABRT,  inline_signalcrashhandler,    SA_SIGINFO,              false, false, { {0} } },
    { SIGBUS,   inline_signalcrashhandler,    SA_SIGINFO,              false, false, { {0} } },
    { SIGFPE,   inline_signalcrashhandler,    SA_SIGINFO,              false, false, { {0} } },
};

signalhandlerstate_t *inline_findsighandler(int sig) {
    for (size_t i = 0; i < sizeof(siglist)/sizeof(siglist[0]); i++) if (siglist[i].sig == sig) return &siglist[i];
    return NULL;
}
#endif

static int install_count = 0;

/** Register emergency exit and signal handlers */
static void inline_registeremergencyhandlers(void) {
    install_count++;
    if (install_count>1) return;

    static bool atexit_registered=false;
    if (!atexit_registered) { atexit(inline_atexitrestore); atexit_registered=true; }
#ifdef _WIN32
    if (SetConsoleCtrlHandler(inline_consolehandler, TRUE)) consolehandler_installed=true;
#else
    #ifndef INLINE_NO_SIGNALS
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);

    for (size_t i = 0; i < sizeof(siglist)/sizeof(siglist[0]); i++) {
        siglist[i].has_previous = false;
        siglist[i].installed    = false;

        if (sigaction(siglist[i].sig, NULL, &siglist[i].previous) == 0) { // Get previous action
            if (siglist[i].previous.sa_handler == SIG_IGN) continue; // Skip ignored signals
            siglist[i].has_previous = true;
        } else memset(&siglist[i].previous, 0, sizeof(siglist[i].previous)); // Wipe

        sa.sa_sigaction = siglist[i].handler;
        sa.sa_flags     = siglist[i].flags;
        if (sigaction(siglist[i].sig, &sa, NULL) == 0) siglist[i].installed=true;
    }
    #endif
#endif
}

/** Restore emergency handlers previously installed */
static void inline_restoreemergencyhandlers(void) {
    if (install_count>0) install_count--;
    if (install_count>0) return;
#ifdef _WIN32
    if (consolehandler_installed)
    if (SetConsoleCtrlHandler(inline_consolehandler, FALSE)) consolehandler_installed = false;
#else
    #ifndef INLINE_NO_SIGNALS
    for (size_t i = 0; i < sizeof(siglist)/sizeof(siglist[0]); i++) {
        if (!siglist[i].has_previous || !siglist[i].installed) continue;
        sigaction(siglist[i].sig, &siglist[i].previous, NULL); // Restore previous handler

        siglist[i].installed = false; // Wipe
        siglist[i].has_previous = false;
        memset(&siglist[i].previous, 0, sizeof(siglist[i].previous));
    }
    #endif
#endif
}

/* ----------------------------------------
 * Switch to/from raw mode
 * ---------------------------------------- */

/** Enable utf8 */
void inline_setutf8(void) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
}

/** Enter raw mode */
static bool inline_enablerawmode(inline_editor *edit) {
    if (edit->rawmode_enabled) return true;

#ifdef _WIN32
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (!GetConsoleMode(hIn, &mode)) return false;
    edit->termstate_in = mode;
    mode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT); // Disable cooked mode
    mode |= ENABLE_VIRTUAL_TERMINAL_INPUT;
    if (!SetConsoleMode(hIn, mode)) return false; // Disable cooked mode

    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!GetConsoleMode(hOut, &edit->termstate_out)) return false;
    DWORD newOut = edit->termstate_out | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    if (!SetConsoleMode(hOut, newOut)) return false; // Enable VT output

    if (!termstate_set) {
        termstate_in = edit->termstate_in;
        termstate_out = edit->termstate_out;
        termstate_set = true;
    }
#else
    if (tcgetattr(STDIN_FILENO, &edit->termstate) == -1) return false;

    struct termios raw = edit->termstate;
    /* Input: Turn off: IXON   - software flow control (ctrl-s and ctrl-q)
                        ICRNL  - translate CR into NL (ctrl-m)
                        BRKINT - parity checking
                        ISTRIP - strip bit 8 of each input byte */
    raw.c_iflag &= ~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
    /* Output: Turn off: OPOST - output processing */
    raw.c_oflag &= ~(OPOST);
    /* Character: CS8 Set 8 bits per byte */
    raw.c_cflag |= (CS8);
    /* Turn off: ECHO   - causes keypresses to be printed immediately
                 ICANON - canonical mode, reads line by line
                 IEXTEN - literal (ctrl-v)
                 ISIG   - turn off signals (ctrl-c and ctrl-z) */
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    /* Set return condition for control characters */
    raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0; /* 1 byte, no timer */

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) return false;
    if (!termstate_set) {
        termstate = edit->termstate;
        termstate_set = true;
    }
#endif
    inline_lasteditor = edit; // Record last editor
    inline_registeremergencyhandlers();

    edit->rawmode_enabled = true;
    return true;
}

/** Restore terminal state to normal */
static void inline_disablerawmode(inline_editor *edit) {
    if (!edit || !edit->rawmode_enabled) return;

#ifdef _WIN32
    HANDLE hIn  = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleMode(hIn,  edit->termstate_in);
    SetConsoleMode(hOut, edit->termstate_out);
#else
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &edit->termstate);
#endif

    fputs("\r", stdout); // Print a carriage return to ensure we're back on the left hand side
    edit->rawmode_enabled = false;
    inline_restoreemergencyhandlers();
}

/* **********************************************************************
 * Utility functions
 * ********************************************************************** */

/** Min and max */
#define imin(a,b) ( a<b ? a : b)
#define imax(a,b) ( a>b ? a : b)

/** Duplicate a string */
static char *inline_strdup(const char *s) {
    if (!s) return NULL;

    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (!p) return NULL;

    memcpy(p, s, n);
    return p;
}

/** Ensure the buffer can grow by at least `extra` bytes. */
static bool inline_extendbufferby(inline_editor *edit, size_t extra) {
    if (extra > SIZE_MAX - edit->buffer_len - 1) return false; // Prevent overflow
    size_t required = edit->buffer_len + extra + 1;  // +1 for null terminator

    if (required <= edit->buffer_size) return true;  // Sufficient space already

    size_t newcap = edit->buffer_size ? edit->buffer_size : INLINE_DEFAULT_BUFFER_SIZE;
    while (newcap < required) {
        if (newcap > SIZE_MAX / 2) return false;
        newcap *= 2; // Grow exponentially
    }

    void *p = realloc(edit->buffer, newcap);
    if (!p) return false;
    edit->buffer = p;
    edit->buffer_size = newcap;

    return true;
}

/* ----------------------------------------
 * Grapheme splitting
 * ---------------------------------------- */

/** Determine length of utf8 character from the first byte */
static inline int inline_utf8length(unsigned char b) {
    if ((b & 0x80) == 0x00) return 1;      // 0xxxxxxx
    if ((b & 0xE0) == 0xC0) return 2;      // 110xxxxx
    if ((b & 0xF0) == 0xE0) return 3;      // 1110xxxx
    if ((b & 0xF8) == 0xF0) return 4;      // 11110xxx
    return 0;                              // Invalid or continuation
}

/** Decode utf8 into an integer */
static inline uint32_t inline_utf8decode(const unsigned char *p) {
    int len = inline_utf8length(*p);
    switch (len) {
        case 1: return p[0];
        case 2: return ((p[0] & 0x1F) << 6) |  (p[1] & 0x3F);
        case 3: return ((p[0] & 0x0F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
        case 4: return ((p[0] & 0x07) << 18) | ((p[1] & 0x3F) << 12) | ((p[2] & 0x3F) << 6) |  (p[3] & 0x3F);
        default: break; 
    }
    return 0;
}

/** Codepoint definition */
typedef struct {
    const unsigned char *seq;
    size_t len;
} codepoint_t;

#define CODEPOINT(s) { (const unsigned char *) s, sizeof(s)-1 }

/** Suffix codepoints modify the previous codepoint, but don't join */
static const codepoint_t suffix_extenders[] = {
    CODEPOINT("\xEF\xB8\x8E"),    // VS15 (U+FE0E) text presentation
    CODEPOINT("\xEF\xB8\x8F"),    // VS16 (U+FE0F) emoji presentation
    CODEPOINT("\xE2\x83\xA3"),    // U+20E3 Keycap combining mark

    // Emoji skin tone modifiers U+1F3FB–U+1F3FF
    CODEPOINT("\xF0\x9F\x8F\xBB"), // light skin tone
    CODEPOINT("\xF0\x9F\x8F\xBC"), // medium-light skin tone
    CODEPOINT("\xF0\x9F\x8F\xBD"), // medium skin tone
    CODEPOINT("\xF0\x9F\x8F\xBE"), // medium-dark skin tone
    CODEPOINT("\xF0\x9F\x8F\xBF"), // dark skin tone
};
static const size_t suffix_count = sizeof(suffix_extenders) / sizeof(suffix_extenders[0]);

/* Joiner codepoints connect the next codepoint into the same grapheme */
static const codepoint_t joiners[] = {
    CODEPOINT("\xE2\x80\x8D"),   // ZWJ (U+200D)
};
static const size_t joiners_count = sizeof(joiners) / sizeof(joiners[0]);
#undef CODEPOINT

/** Matches a codepoint against a table of possible matches */
static size_t inline_matchcodepoint(size_t table_count, const codepoint_t *table, const unsigned char *p, const unsigned char *end) {
    size_t remaining = (size_t)(end - p);
    for (size_t i = 0; i < table_count; i++) {
        const codepoint_t *cp = &table[i];
        if (remaining >= cp->len && memcmp(p, cp->seq, cp->len) == 0) {
            return cp->len;
        }
    }
    return 0; // no match
}

/** Check if a codepoint is extended pictographic */
static bool inline_isextendedpictographic(uint32_t cp) {
    if (cp >= 0x1F300 && cp <= 0x1FAFF) return true; // Emoji blocks
    if (cp >= 0x2600 && cp <= 0x26FF) return true; // Misc symbols
    if (cp >= 0x2700 && cp <= 0x27BF) return true; // Dingbats
    return false;
}

/** Minimal heuristic grapheme splitter */
static size_t inline_graphemesplit(const char *in, const char *end) {
    const unsigned char *p = (const unsigned char *)in;
    const unsigned char *uend = (const unsigned char *)end;
    if (p >= uend) return 0;

    size_t len = inline_utf8length(*p); // Decode first codepoint
    if (len == 0) len = 1;
    if ((size_t)(uend - p) < len) return (size_t)(uend - p);

    const unsigned char *prev = p;  // start of previous codepoint
    uint32_t prev_cp = inline_utf8decode(p);
    p += len;

    while (p < uend && *p >= 0xCC && *p <= 0xCF) { // Combining marks
        len = inline_utf8length(*p);
        if (len == 0 || (size_t)(uend - p) < len) break;

        prev = p;
        prev_cp = inline_utf8decode(p);
        p += len;
    }

    do { // Emoji modifiers / VS16 / keycap extenders
        len = inline_matchcodepoint(suffix_count, suffix_extenders, p, uend);
        if (len) {
            prev = p;
            prev_cp = inline_utf8decode(p);
            p += len;
        }
    } while (len != 0);

    for (;;) { // ZWJ sequences
        len = inline_matchcodepoint(joiners_count, joiners, p, uend); // Check for ZWJ
        if (len == 0) break;

        p += len;  // Skip ZWJ itself
        if (p >= uend) break;

        size_t next_len = inline_utf8length(*p); // Decode next codepoint
        if (next_len == 0 || (size_t)(uend - p) < next_len) break;

        uint32_t next_cp = inline_utf8decode(p);

        // Only join if both sides are emoji (extended pictographic)
        if (!inline_isextendedpictographic(prev_cp) ||
            !inline_isextendedpictographic(next_cp)) break;

        prev = p; // Join: consume next codepoint
        prev_cp = next_cp;
        p += next_len;

        size_t slen; // Consume suffix extenders 
        while ((slen = inline_matchcodepoint(suffix_count, suffix_extenders, p, uend)) != 0) {
            prev = p;
            prev_cp = inline_utf8decode(p);
            p += slen;
        }
    }

    return (size_t)(p - (const unsigned char *)in);
}

/* ----------------------------------------
 * Grapheme buffer
 * ---------------------------------------- */

/** Compute grapheme locations */
static void inline_recomputegraphemes(inline_editor *edit) {
    size_t needed = edit->buffer_len + 1; // Assume 1 byte per character as a worst case + sentinel

    size_t required_bytes = needed * sizeof(size_t); // Ensure capacity
    if (required_bytes > edit->grapheme_size) {
        size_t newsize = (edit->grapheme_size ? edit->grapheme_size : INLINE_DEFAULT_BUFFER_SIZE);
        while (newsize < required_bytes) {
            if (newsize > SIZE_MAX / 2) return;
            newsize *= 2;
        }

        size_t *new = realloc(edit->graphemes, newsize);
        if (!new) {
            edit->grapheme_count = 0;
            return;
        }

        edit->graphemes = new;
        edit->grapheme_size = newsize;
    }

    // Select splitter
    inline_graphemefn fn = (edit->grapheme_fn ? edit->grapheme_fn : inline_graphemesplit);

    size_t count = 0;
    const char *p = edit->buffer, *end = edit->buffer + edit->buffer_len;

    while (p < end) { // Walk the buffer and record grapheme boundaries
        edit->graphemes[count++] = (size_t)(p - edit->buffer);
        size_t len = fn(p, end);
        if (len == 0) len = 1; // Malformed grapheme
        if (len > (size_t)(end - p)) len = (size_t)(end - p); // Size longer than buffer
        p += len;
    }

    edit->graphemes[count] = edit->buffer_len; // Ensure last entry points to end of buffer
    edit->grapheme_count = (int) count;
}

/** Finds the start and end of grapheme i in bytes */
static inline void inline_graphemerange(inline_editor *edit, int i, size_t *start, size_t *end) {
    if (i < 0 || i >= edit->grapheme_count) { // Handle out of bounds access (incl. i representing end of line)
        if (start) *start = edit->buffer_len;
        if (end)   *end   = edit->buffer_len;
        return;
    }

    if (start) *start = edit->graphemes[i];
    if (end) *end = edit->graphemes[i+1];
}

/** Finds the first grapheme index using binary search whose start byte is >= byte_off */
static int inline_findgraphemeindex(inline_editor *edit, size_t byte_off) {
    int lo = 0, hi = edit->grapheme_count;

    while (lo < hi) {
        int mid = (lo + hi) / 2;
        size_t mid_off = edit->graphemes[mid];

        if (mid_off < byte_off) lo = mid + 1;
        else hi = mid;
    }

    return lo;
}

/* ----------------------------------------
 * Line buffer
 * ---------------------------------------- */

/** Compute line locations */
static void inline_recomputelines(inline_editor *edit) {
    int count = 0;

    for (int g = 0; g < edit->grapheme_count; g++) // Count newline graphemes
        if (edit->buffer[edit->graphemes[g]] == '\n') count++;

    size_t needed = sizeof(size_t) * (count + 2); // Need count+2 entries: first line + each newline + sentinel
    if (needed > edit->line_size) {
        size_t *new = realloc(edit->lines, needed);
        if (!new) return;
        edit->lines = new;
        edit->line_size = needed;
    }

    int i = 0;
    edit->lines[i++] = 0; // First line always starts at 0

    for (int g = 0; g < edit->grapheme_count; g++)  // Subsequent lines start after each newline
        if (edit->buffer[edit->graphemes[g]] == '\n')
            edit->lines[i++] = edit->graphemes[g] + 1;

    edit->lines[i] = edit->buffer_len; // Sentinel
    edit->line_count = i;
}

/* ----------------------------------------
 * Grapheme display width
 * ---------------------------------------- */

/** Check for ZWJ, VS16, keycap */
static bool inline_checkextenders(const unsigned char *g, size_t len) {
    for (size_t i = 0; i + 2 < len; i++) {
        unsigned char a = g[i], b = g[i+1], c = g[i+2];
        if (a == 0xE2 && b == 0x80 && c == 0x8D) return true;  // ZWJ
        if (a == 0xEF && b == 0xB8 && c == 0x8F) return true;  // VS16
        if (a == 0xE2 && b == 0x83 && c == 0xA3) return true;  // keycap
    }
    return false;
}

/** Predict the display width of a grapheme */
static int inline_graphemewidth(const char *p, size_t len) {
    const unsigned char *g = (const unsigned char *) p;
    if (!len) return 0;
    if (g[0] == '\t') return INLINE_TAB_WIDTH; // Tab
    if (g[0] < 0x80) return 1; // ASCII fast path

    if (len >= 2 && (g[0] == 0xCC || g[0] == 0xCD)) return 0; // Combining-only grapheme (rare)
    if (inline_checkextenders(g, len)) return 2; // Check for ZWJ, VS16 and other extenders
    if (len >= 2 && g[0] == 0xEF && (g[1] == 0xBC || g[1] == 0xBD)) return 2; // Fullwidth forms (U+FF00 block)

    if (len >= 4 && (g[0] & 0xF8) == 0xF0) { // Emoji block (U+1F300–U+1FAFF)
        if ((g[1] & 0xC0) != 0x80 || (g[2] & 0xC0) != 0x80 || (g[3] & 0xC0) != 0x80) return 1;
        unsigned cp = ((g[0] & 0x07) << 18) | ((g[1] & 0x3F) << 12) |
                      ((g[2] & 0x3F) << 6) | (g[3] & 0x3F);
        if (cp >= 0x1F300 && cp <= 0x1FAFF) return 2;
    }

    if (len >= 3 && g[0] >= 0xE4 && g[0] <= 0xE9) { // CJK Unified Ideographs (U+4E00–U+9FFF)
        if ((g[1] & 0xC0) != 0x80 || (g[2] & 0xC0) != 0x80) return 1;
        unsigned cp = ((g[0] & 0x0F) << 12) | ((g[1] & 0x3F) << 6) | (g[2] & 0x3F);
        if (cp >= 0x4E00 && cp <= 0x9FFF) return 2;
    }

    return 1;
}

/** Calculate the display width of a utf8 string using current grapheme splitter/width estimator */
static bool inline_stringwidth(inline_editor *edit, const char *str, int *width) {
    inline_graphemefn split_fn = (edit->grapheme_fn ? edit->grapheme_fn : inline_graphemesplit);
    inline_widthfn width_fn = (edit->width_fn ? edit->width_fn : inline_graphemewidth);

    const char *p = str, *end = str + strlen(str);
    *width = 0;

    while (p < end) {
        size_t glen = split_fn(p, end);
        if (glen == 0) return false; // Malformed utf8 codepoint
        *width += width_fn(p, glen);
        p += glen;
    }
    return true;
}

/** Compute terminal width of grapheme range [g_start, g_end) */
static inline int inline_graphemerangewidth(inline_editor *edit, int g_start, int g_end) {
    inline_widthfn width_fn = (edit->width_fn ? edit->width_fn : inline_graphemewidth);
    int width = 0;
    for (int g = g_start; g < g_end; g++) {
        size_t s, e;
        inline_graphemerange(edit, g, &s, &e);
        width += width_fn(edit->buffer + s, e - s);
    }
    return width;
}

/* ----------------------------------------
 * String lists
 * ---------------------------------------- */

/** Initialize a stringlist structure */
static void inline_stringlist_init(inline_stringlist_t *list) {
    list->items=NULL;
    list->count=0;
    list->index=INLINE_INVALID;
}

/** Add an entry to a stringlist */
static bool inline_stringlist_add(inline_stringlist_t *list, const char *s) {
    if (!s) return false; // Never add a null pointer
    char *copy = inline_strdup(s);
    if (!copy) return false;
    char **newitems = realloc(list->items, sizeof(char*) * (list->count + 1));
    if (!newitems) { free(copy); return false; } // Don't update if realloc fails

    list->items = newitems;
    list->items[list->count] = copy;
    list->count++;
    return true;
}

/** Removes and frees the first element of the stringlist;  */
static void inline_stringlist_popfront(inline_stringlist_t *list) {
    if (list->count == 0) return;
    free(list->items[0]);

    // Shift pointers down safely (overlapping copy)
    if (list->count > 1) memmove(list->items, list->items + 1, sizeof(char*) * (list->count - 1));
    list->count--;
}

/** Clear a stringlist */
static void inline_stringlist_clear(inline_stringlist_t *list) {
    if (list->items) {
        for (int i = 0; i < list->count; i++) free(list->items[i]);
        free(list->items);
    }

    inline_stringlist_init(list);
}

/** Get the current string in a stringlist */
static int inline_stringlist_count(inline_stringlist_t *list) {
    return list->count;
}

/** Get the current string in a stringlist */
static const char *inline_stringlist_current(inline_stringlist_t *list) {
    if (list->count == 0 || list->index<0 || list->index >= list->count) return NULL;
    return list->items[list->index];
}

/** Advance the current index by delta with optional wrapping */
static void inline_stringlist_advance(inline_stringlist_t *list, int delta, bool wrap) {
    if (list->count == 0 || list->index<0) return;
    if (list->index >= list->count) list->index = list->count - 1; // Clamp
    if (wrap) {
        list->index = (list->index + delta + list->count) % list->count;
    } else {
        list->index = list->index + delta;
        if (list->index<0) list->index=0;
        if (list->index>=list->count) list->index=list->count-1;
    }
}

/* ----------------------------------------
 * Selections
 * ---------------------------------------- */

/** Find the start and end points of a selection, if one is present.  */
static bool inline_selectionrange(inline_editor *edit, int *sel_l, int *sel_r, size_t *start, size_t *end) {
    if (edit->selection_posn == INLINE_INVALID) return false;

    int l = imin(edit->selection_posn, edit->cursor_posn);
    int r = imax(edit->selection_posn, edit->cursor_posn);
    
    if (sel_l) *sel_l = l;
    if (sel_r) *sel_r = r;
    if (start) inline_graphemerange(edit, l, start, NULL);
    if (end) inline_graphemerange(edit, r, end,   NULL);
    
    return true;
}

/* ----------------------------------------
 * Clipboard
 * ---------------------------------------- */

/** Copies a string of given length onto the clipboard. */
static bool inline_copytoclipboard(inline_editor *edit, const char *string, size_t length) {
    if (!string || length==0) { // Empty clipboard
        if (edit->clipboard) edit->clipboard[0] = '\0';
        edit->clipboard_len = 0;
        return true;
    }

    size_t needed = length + 1; // Check if we have sufficient capacity and realloc if necessary
    if (needed > edit->clipboard_size) {
        size_t newsize = edit->clipboard_size ? edit->clipboard_size : INLINE_DEFAULT_BUFFER_SIZE;
        while (newsize < needed) {
            if (newsize > SIZE_MAX / 2) return false;
            newsize *= 2;
        }

        char *newbuf = realloc(edit->clipboard, newsize);
        if (!newbuf) return false; // Leave clipboard unchanged on allocation failure

        edit->clipboard = newbuf;
        edit->clipboard_size = newsize;
    }

    memmove(edit->clipboard, string, length); // Copy onto clipboard
    edit->clipboard[length] = '\0'; // Ensure null termination
    edit->clipboard_len = length;
    return true;
}

/* ----------------------------------------
 * Autocomplete / Suggestions
 * ---------------------------------------- */

/** Check if the cursor is at the end of the buffer */
static bool inline_atend(inline_editor *edit) {
    return edit->cursor_posn == edit->grapheme_count;
}

/** Adds a suggestion to the suggestion list */
static void inline_addsuggestion(inline_editor *edit, const char *s) {
    inline_stringlist_add(&edit->suggestions, s);
}

/** Clears the suggestion list */
static void inline_clearsuggestions(inline_editor *edit) {
    inline_stringlist_clear(&edit->suggestions);
}

/** Generates suggestions by repeatedly calling the completion callback */
static void inline_generatesuggestions(inline_editor *edit) {
    if (!edit->complete_fn) return;
    inline_clearsuggestions(edit);
    if (edit->selection_posn!=INLINE_INVALID) return; // Enforce that suggestions cannot be generated while a selection is active

    if (edit->buffer && inline_atend(edit)) {
        size_t index = 0;
        const char *s;
        while ((s=edit->complete_fn(edit->buffer, edit->complete_ref, &index))!=0) {
            inline_addsuggestion(edit, s);
        }
        if (edit->suggestions.count > 0) edit->suggestions.index = 0;
    }
}

/** Check if suggestions are available */
static bool inline_havesuggestions(inline_editor *edit) {
    return inline_stringlist_count(&edit->suggestions) > 0;
}

/** Returns the current suggestion */
static const char *inline_currentsuggestion(inline_editor *edit) {
    if (!inline_havesuggestions(edit)) return NULL;
    return inline_stringlist_current(&edit->suggestions);
}

/** Advance through the suggestions by delta (can be negative; we wrap around) */
static void inline_advancesuggestions(inline_editor *edit, int delta) {
    inline_stringlist_advance(&edit->suggestions, delta, true);
}

/* ----------------------------------------
 * History
 * ---------------------------------------- */

/** Set the history length. */
void inline_sethistorylength(inline_editor *edit, int maxlen) {
    edit->max_history_length=maxlen;

    if (maxlen > 0) { // Remove excess entries if necessary
        while (edit->history.count > maxlen) inline_stringlist_popfront(&edit->history);
    } else if (maxlen == 0) { // Clear history entirely
        inline_stringlist_clear(&edit->history);
    }
}

/** Adds an entry to the history list */
bool inline_addhistory(inline_editor *edit, const char *entry) {
    if (!entry || !*entry || !edit->max_history_length) return false; // Skip empty buffers

    if (edit->history.count > 0) { // Avoid duplicate consecutive entries
        const char *last = edit->history.items[edit->history.count - 1];
        if (strcmp(last, entry) == 0) return false;
    }

    inline_stringlist_add(&edit->history, entry);
    if (edit->max_history_length > 0 && edit->history.count > edit->max_history_length) inline_stringlist_popfront(&edit->history);
    return true;
}

/** Advances the current history */
static void inline_advancehistory(inline_editor *edit, int delta) {
    int count = inline_stringlist_count(&edit->history);
    if (count == 0) return;

    // Enter history mode if we're not in it
    if (edit->history.index == INLINE_INVALID) edit->history.index = count - 1;
    else inline_stringlist_advance(&edit->history, delta, false); // No wrap

    // Load entry or exit history mode
    const char *s = inline_stringlist_current(&edit->history);
    inline_clear(edit);
    if (s) {
        inline_insert(edit, s, strlen(s));
    } else edit->history.index = INLINE_INVALID; // Exit history mode
}

/** End browsing */
static void inline_endhistorybrowsing(inline_editor *edit) {
    edit->history.index = INLINE_INVALID;
}

/* ----------------------------------------
 * Reset
 * ---------------------------------------- */

/** Resets the editor before a new session */
static void inline_reset(inline_editor *edit) {
    inline_clear(edit);
    inline_clearselection(edit);
    inline_endhistorybrowsing(edit);
    inline_stringlist_clear(&edit->suggestions);
    edit->rawmode_enabled = false;
    edit->term_cursor_row = 0;
    edit->term_lines_drawn = 0;
}

/* ----------------------------------------
 * Viewport
 * ---------------------------------------- */

/** Initialize the viewport */
static void inline_initviewport(inline_editor *edit) {
    edit->viewport.first_visible_line = 0;
    edit->viewport.first_visible_col  = 0;
    edit->viewport.screen_rows = 1; // Will adjust for multiline editing later
    inline_updateviewportwidth(edit);
}

/** Compute logical cursor position in rows and columns */
static void inline_cursorposn(inline_editor *edit, int *out_row, int *out_col) {
    size_t byte_pos = edit->graphemes[edit->cursor_posn];  // byte offset of cursor

    int row = 0; // Find the row containing the cursor
    while (row + 1 < edit->line_count && edit->lines[row + 1] <= byte_pos) row++;

    if (out_row) *out_row = row;
    // The column is found by subtracting the grapheme offset of the start of the row
    if (out_col) *out_col = edit->cursor_posn - inline_findgraphemeindex(edit, edit->lines[row]);
}

/** Check the cursor is visible */
static void inline_ensurecursorvisible(inline_editor *edit) {
    int cursor_row, cursor_col;
    inline_cursorposn(edit, &cursor_row, &cursor_col);

    int line_start_g = inline_findgraphemeindex(edit, edit->lines[cursor_row]);
    int cursor_g = line_start_g + cursor_col;

    int cursor_term_col = inline_graphemerangewidth(edit, line_start_g, cursor_g);

    int first = edit->viewport.first_visible_col;
    int end   = first + edit->viewport.screen_cols; // exclusive

    if (cursor_term_col < first) {
        edit->viewport.first_visible_col = cursor_term_col;

    } else if (cursor_term_col >= end) {
        edit->viewport.first_visible_col =
            cursor_term_col - edit->viewport.screen_cols;
    }
}

/* **********************************************************************
 * Rendering
 * ********************************************************************** */

#define TERM_RESETCOLOR         "\x1b[0m"
#define TERM_CLEAR              "\x1b[K"
#define TERM_RESETFOREGROUND    "\x1b[39m"
#define TERM_HIDECURSOR         "\x1b[?25l"
#define TERM_SHOWCURSOR         "\x1b[?25h"
#define TERM_FAINT              "\x1b[2m"
#define TERM_INVERSEVIDEO       "\x1b[7m"

/** Write an escape sequence to the terminal */
void inline_emit(const char *seq) {
    write(STDOUT_FILENO, seq, (unsigned int) strlen(seq));
}

/** Writes an escape sequence to produce a given color */
void inline_emitcolor(int color) {
    if (color < 0) return; // default
    char seq[INLINE_ESCAPECODE_MAXLENGTH];
    int n = 0;

    if (color < 16) { // ANSI 8 or bright 8
        int base = (color < 8 ? 30 : 90);
        n = snprintf(seq, sizeof(seq), "\x1b[%dm", base + (color & 7));
    } else if (color <= 255) { // 256-color palette 8–255
        n = snprintf(seq, sizeof(seq), "\x1b[38;5;%dm", color);
    } else { // Assume RGB packed as 0x01RRGGBB
        int r = (color >> 16) & 0xFF;
        int g = (color >> 8)  & 0xFF;
        int b = (color >> 0)  & 0xFF;
        n = snprintf(seq, sizeof(seq), "\x1b[38;2;%d;%d;%dm", r, g, b);
    }

    if (n > 0) write(STDOUT_FILENO, seq, n);
}

/** Clip grapheme range [*g_start, *g_end) horizontally based on viewport */
static inline void inline_clipgraphemerange(inline_editor *edit, int line_start, int *g_start, int *g_end) {
    inline_widthfn width_fn = (edit->width_fn ? edit->width_fn : inline_graphemewidth);

    int start_col = edit->viewport.first_visible_col;
    int end_col   = start_col + edit->viewport.screen_cols;

    int col = inline_graphemerangewidth(edit, line_start, *g_start);
    int start = -1;
    int end   = *g_start;

    for (int i = *g_start; i < *g_end; i++) {
        size_t s, e;
        inline_graphemerange(edit, i, &s, &e);
        int w = width_fn(edit->buffer + s, e - s);

        if ((col >= start_col) && (col < end_col)) {
            if (start < 0) start = i; // First visible grapheme
            end = i + 1; // extend visible range
        }

        if (col + w > end_col) break;
        col += w;
    }

    if (start < 0) start = *g_end; // Clamp if line is empty or viewport is beyond end
    if (end < start) end = start;
    else if (end > start && edit->buffer[edit->graphemes[end-1]] == '\n') end--;

    *g_start = start;
    *g_end   = end;
}

/** Move terminal cursor to the editor's origin */
static inline void inline_movetoorigin(inline_editor *edit) {
    write(STDOUT_FILENO, "\r", 1); // Move to start of current line

    if (edit->term_cursor_row > 0) { // Move up cursor_row lines
        char seq[INLINE_ESCAPECODE_MAXLENGTH];
        int n = snprintf(seq, sizeof(seq), "\x1b[%dA", edit->term_cursor_row);
        write(STDOUT_FILENO, seq, n);
    }
}

/** Move the cursor by a specified delta; down is positive dy */
static inline void inline_moveby(int dx, int dy) {
    char seq[INLINE_ESCAPECODE_MAXLENGTH];

    if (dy<0) { // Up
        int n = snprintf(seq, sizeof(seq), "\x1b[%dA", abs(dy));
        write(STDOUT_FILENO, seq, n);
    } else {
        for (int i = 0; i < dy; i++) inline_emit("\n"); // Ensure scroll
    }

    if (dx!=0) { // Horizontal
        int n = snprintf(seq, sizeof(seq), "\x1b[%d%c", abs(dx), (dx < 0 ? 'D' : 'C'));
        write(STDOUT_FILENO, seq, n);
    }
}

/** Render a single line of text
 * @param[in] - edit        - the editor
 * @param[in] - prompt      - prompt for this line
 * @param[in] - byte_start  - byte offset for the start of the line
 * @param[in] - byte_end    - byte offset for the end of the line
 * @param[in] - logical_cursor_col - column the cursor should be displayed in logical coordinates, or -1 if not on this line
 * @param[in] - is_last     - whether this is the last line
 * @param[out] - rendered_cursor_col - if logical_cursor_col indicates the cursor is on this line,
 *                                     set to logical column the cursor should be rendered on, incuding clipping
 *                                     and prompt widt, or -1 if outside clipping window; otherwise not changed. */
static void inline_renderline(inline_editor *edit, const char *prompt, size_t byte_start, size_t byte_end,
                               int logical_cursor_col, bool is_last, int *rendered_cursor_col) {
    write(STDOUT_FILENO, prompt, (unsigned int) strlen(prompt)); // Write prompt
    int prompt_width = 0; // Calculate its display width
    if (!inline_stringwidth(edit, prompt, &prompt_width)) prompt_width = 0;

    int rendered_width = prompt_width; // Track rendered width
    int rendered_cursor_posn = -1;

    // Compute selection bounds, if active
    int sel_l = INLINE_INVALID, sel_r = INLINE_INVALID;
    if (edit->selection_posn != INLINE_INVALID) {
        sel_l = imin(edit->selection_posn, edit->cursor_posn);
        sel_r = imax(edit->selection_posn, edit->cursor_posn);
    }

    // Compute grapheme range for this line; remember the true start of the line
    int line_start = inline_findgraphemeindex(edit, byte_start);
    int g_start = line_start, g_end = inline_findgraphemeindex(edit, byte_end);

    // Apply horizontal clipping
    inline_clipgraphemerange(edit, line_start, &g_start, &g_end);

    int current_color = -1;
    bool selection_on = false; // Track the terminal inverse video state

    // Render syntax-colored, clipped graphemes
    int g = g_start;
    size_t off = edit->graphemes[g_start];

    inline_syntaxcolorfn syntax_fn = (edit->palette_count>0 ? edit->syntax_fn : NULL);
    inline_widthfn width_fn = (edit->width_fn ? edit->width_fn : inline_graphemewidth);

    // Render syntax-colored, clipped graphemes
    while (g < g_end && off < byte_end) {
        // Compute color span from current point
        inline_colorspan_t span = { .byte_end = off + 1, .color = 0 };
        bool ok=false;
        if (syntax_fn) ok=syntax_fn(edit->buffer, edit->syntax_ref, off, &span);
        if (!ok || span.byte_end <= off) span.byte_end = byte_end;   // treat rest of line as uncolored

        int span_color = (span.color>=0 && span.color < edit->palette_count ? edit->palette[span.color] : -1);

        // Change color only if needed
        if (span_color != current_color) {
            if (current_color != -1) {
                inline_emit(TERM_RESETCOLOR);
                selection_on = false;
            }
            if (span_color >= 0) inline_emitcolor(span_color);
            current_color = span_color;
        }

        // Print graphemes until we reach span.byte_end (clipped)
        for (; g < g_end; g++) {
            size_t gs, ge;
            inline_graphemerange(edit, g, &gs, &ge);

            if (gs >= span.byte_end) break;

            bool in_selection = (g >= sel_l && g < sel_r); // Are we in a selection?
            if (in_selection != selection_on) {            // Does terminal state match?
                if (in_selection) inline_emit(TERM_INVERSEVIDEO); // Start reverse video
                else {
                    inline_emit(TERM_RESETCOLOR);
                    if (current_color >= 0) inline_emitcolor(current_color); // Reapply syntax color
                }
                selection_on = in_selection;
            }

            if (edit->buffer[gs] == '\n') break;

            if (logical_cursor_col >= 0 &&  // Check if this grapheme was where the cursor is
                line_start + logical_cursor_col == g) rendered_cursor_posn = rendered_width;

            if (edit->buffer[gs] == '\t') {
                for (int i=0; i<INLINE_TAB_WIDTH; i++) inline_emit(" ");
            } else write(STDOUT_FILENO, edit->buffer + gs, (unsigned int) (ge - gs));
            rendered_width += width_fn(edit->buffer + gs, ge - gs);
        }

        off = span.byte_end;
    }

    if (selection_on || current_color != -1) inline_emit(TERM_RESETCOLOR);

    // Ghosted suggestion suffix (only if at right edge on last line)
    if (is_last && g_end == edit->grapheme_count && logical_cursor_col >= 0) {
        const char *suffix = inline_currentsuggestion(edit);
        edit->suggestion_shown=false;
        if (suffix && *suffix) {
            int remaining_cols = edit->viewport.screen_cols - rendered_width;

            // Width of suggestion
            int ghost_width = 0;
            if (!inline_stringwidth(edit, suffix, &ghost_width)) ghost_width = 0;

            if (ghost_width <= remaining_cols) { // Show suggestion as faint text
                edit->suggestion_shown=true;
                inline_emit(TERM_FAINT);
                write(STDOUT_FILENO, suffix, (unsigned int) strlen(suffix));
                inline_emit(TERM_RESETCOLOR);
            }
        }
    }

    if (logical_cursor_col >= 0) { // Update cursor position if on this line
        if (rendered_cursor_posn >= 0) *rendered_cursor_col = rendered_cursor_posn;
        else *rendered_cursor_col = rendered_width; // cursor at end
    }

    if (rendered_width < edit->viewport.screen_cols) inline_emit(TERM_CLEAR); // Clear to end of line
}

/** Redraw the entire buffer in multiline mode */
static void inline_redraw(inline_editor *edit) {
    inline_emit(TERM_HIDECURSOR); // Prevent flickering
    inline_movetoorigin(edit);

    int cursor_row, cursor_col; // Compute logical cursor column and row (pre-clipping)
    inline_cursorposn(edit, &cursor_row, &cursor_col);

    int rendered_cursor_col = -1;  // To be filled out by inline_renderline
    for (int i = 0; i < edit->line_count; i++) {
        size_t byte_start = edit->lines[i]; // Render lines
        size_t byte_end   = edit->lines[i+1];
        bool is_last = (i == edit->line_count - 1);

        inline_emit("\r");        // Move cursor to start of line

        inline_renderline(edit, (i==0 ? edit->prompt : edit->continuation_prompt), // prompt
                          byte_start, byte_end,
                          (cursor_row == i ? cursor_col : -1), // cursor column if on this line
                          is_last, // whether we're on the last line or not
                          &rendered_cursor_col );
                          
        if (i + 1 < edit->line_count) inline_emit("\n"); // Move to next line if not at end
    }

    int extra = (edit->term_lines_drawn > edit->line_count ? edit->term_lines_drawn - edit->line_count : 0);
    for (int i = 0; i < extra; i++) {
        inline_emit("\n\r");
        inline_emit(TERM_CLEAR);
    }

    write(STDOUT_FILENO, "\r", 1); // Move to start of line
    inline_moveby(rendered_cursor_col, cursor_row - edit->line_count - extra + 1);
    edit->term_cursor_row = cursor_row; // Record cursor row
    edit->term_lines_drawn = edit->line_count; // Record no. of lines drawn
    inline_emit(TERM_SHOWCURSOR);
}
  
/** API function to print a syntax colored string */
void inline_displaywithsyntaxcoloring(inline_editor *edit, const char *string) {
    if (!edit || !string) return;
    fflush(stdout);
    size_t len = strlen(string);

    if (!edit->syntax_fn || !edit->palette_count) { // Syntax highlighting not configured, fallback to plain
        write(STDOUT_FILENO, string, (unsigned int) len);
        return;
    }

    size_t offset = 0;
    while (offset < len) { //
        inline_colorspan_t span = { .byte_end = offset, .color=-1};

        bool ok = edit->syntax_fn(string, edit->syntax_ref, offset, &span); // Obtain next span
        if (!ok || span.byte_end <= offset) { // No more spans or broken callback; print the rest uncolored
            write(STDOUT_FILENO, string + offset, (unsigned int) (len - offset));
            return;
        }

        if (span.color < edit->palette_count && span.color >= 0) inline_emitcolor(edit->palette[span.color]);
        for (size_t i = offset; i < span.byte_end; i++) {
            if (string[i] == '\t') {
                for (int t = 0; t < INLINE_TAB_WIDTH; t++) inline_emit(" ");
            } else write(STDOUT_FILENO, &string[i], 1);
        }

        inline_emit(TERM_RESETFOREGROUND);

        offset = span.byte_end;
    }
    fflush(stdout);
}

/* **********************************************************************
 * Keypress decoding
 * ********************************************************************** */

/* ----------------------------------------
 * Raw input layer
 * ---------------------------------------- */

/** Type that represents a single unit of input */
typedef unsigned char rawinput_t;

#ifdef _WIN32
static bool inline_readkeyevent(KEY_EVENT_RECORD *k) {
    INPUT_RECORD rec;
    DWORD nread;
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);

    for (;;) {
        if (!ReadConsoleInputW(hIn, &rec, 1, &nread)) return false;

        if (rec.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            resize_pending = 1;
            if (inline_lasteditor) inline_lasteditor->refresh = true;
            continue;
        }

        if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown) {
            *k = rec.Event.KeyEvent;
            return true;
        }
    }
}

/** Helper to emit an escape sequence */
static int _emitstr(const char *s, unsigned char out[8]) {
    int n = 0;
    while (s[n] && n < 8) out[n] = (unsigned char)s[n++];
    return n;
}

/** Mapping from Windows VK codes to POSIX escape sequences */
typedef struct {
    WORD vk;
    const char *seq;
} vkmap_t;

static const vkmap_t vk_table[] = {
    { VK_RETURN, "\n"     },
    { VK_BACK,   "\b"     },
    { VK_DELETE, "\x7f"   },
    { VK_UP,     "\x1b[A" },
    { VK_DOWN,   "\x1b[B" },
    { VK_RIGHT,  "\x1b[C" },
    { VK_LEFT,   "\x1b[D" },
    { VK_HOME,   "\x1b[H" },
    { VK_END,    "\x1b[F" },
    { VK_PRIOR,  "\x1b[5~" }, // Page Up
    { VK_NEXT,   "\x1b[6~" }, // Page Down
};

/** Convert windows keypress event to POSIX */
static int inline_translatekeypress(const KEY_EVENT_RECORD *k, unsigned char out[8]) {
    WORD vk = k->wVirtualKeyCode;
    WCHAR wc = k->uChar.UnicodeChar;
    DWORD mods = k->dwControlKeyState;

    // Shift-arrows
    if ((mods & SHIFT_PRESSED) && (vk == VK_LEFT || vk == VK_RIGHT)) {
        return _emitstr( (vk == VK_LEFT ? "\x1b[1;2D" : "\x1b[1;2C"), out );
    }

    // Search table of mappings
    for (size_t i = 0; i < sizeof(vk_table)/sizeof(vk_table[0]); i++) {
        if (vk_table[i].vk == vk) return _emitstr(vk_table[i].seq, out);
    }

    // Ctrl + char
    if (mods & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
        if (vk >= 'A' && vk <= 'Z') {
            out[0] = (unsigned char) (vk - 'A') + 1;
            return 1;
        }
    }

    // Alt characters
    int i=0; 
    if (mods & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) {
        out[i++] = '\x1b'; // Prefix character with esc
    }

    if (wc != 0) { // Unicode
        if (wc < 0x80) {
            out[i++] = (unsigned char)wc;
        } else if (wc < 0x800) {
            out[i++] = (unsigned char)(0xC0 | ((unsigned int)wc >> 6));
            out[i++] = (unsigned char)(0x80 | ((unsigned int)wc & 0x3F));
        } else if (wc < 0xD800 || wc > 0xDFFF) {
            out[i++] = 0xE0 | (wc >> 12);
            out[i++] = 0x80 | ((wc >> 6) & 0x3F);
            out[i++] = 0x80 | (wc & 0x3F);
        } else if (wc >= 0xD800 && wc <= 0xDBFF) { // high surrogate
            // Need the next KEY_EVENT for the low surrogate
            KEY_EVENT_RECORD next;
            if (!inline_readkeyevent(&next)) return 0;

            WCHAR wc2 = next.uChar.UnicodeChar;
            if (wc2 >= 0xDC00 && wc2 <= 0xDFFF) {
                uint32_t cp = 0x10000 + (((wc - 0xD800) << 10) | (wc2 - 0xDC00));
                out[i++] = (unsigned char) (0xF0 | (cp >> 18));
                out[i++] = (unsigned char) (0x80 | ((cp >> 12) & 0x3F));
                out[i++] = (unsigned char) (0x80 | ((cp >> 6) & 0x3F));
                out[i++] = (unsigned char) (0x80 | (cp & 0x3F));
            }
        }
    }

    return i; // Return 
}

#endif

/** Await a single raw unit of input and store in a rawinput_t */
static bool inline_readraw(rawinput_t *out) {
#ifdef _WIN32
    static unsigned char buf[16]; // local ring buffer
    static int len = 0;
    static int pos = 0;

    if (pos < len) { // Return remaining bytes
        *out = buf[pos++];
        return true;
    }

    KEY_EVENT_RECORD k; // Get new key event
    do {
        if (!inline_readkeyevent(&k)) return false;
        len = inline_translatekeypress(&k, buf); // Translate it to POSIX
        pos = 0;
    } while (len == 0);

    *out = buf[pos++]; // Return first byte
    return true;
#else
    int n = (int) read(STDIN_FILENO, out, 1);
    return n == 1;
#endif
}

/* ----------------------------------------
 * Keypress decoding layer
 * ---------------------------------------- */

/** Identifies the type of keypress */
typedef enum {
    KEY_UNKNOWN, KEY_CHARACTER,
    KEY_RETURN, KEY_CTRL_RETURN, KEY_TAB, KEY_SHIFT_TAB, KEY_DELETE,
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,   // Arrow keys
    KEY_HOME, KEY_END,               // Home and End
    KEY_PAGE_UP, KEY_PAGE_DOWN,      // Page up and page down
    KEY_SHIFT_LEFT, KEY_SHIFT_RIGHT, // Shift+arrow key
    KEY_CTRL, KEY_ALT                // Ctrl, meta keys
} keytype_t;

/** A single keypress event obtained and processed by the terminal */
typedef struct {
    keytype_t type; /** Type of keypress */
    unsigned char c[5]; /** Up to four bytes of utf8 encoded unicode plus null terminator */
    int nbytes; /** Number of bytes */
} keypress_t;

static void inline_keypressunknown(keypress_t *keypress) {
    keypress->type=KEY_UNKNOWN;
    keypress->c[0]='\0';
    keypress->nbytes=0;
}

static void inline_keypresswithchar(keypress_t *keypress, keytype_t type, char c) {
    keypress->type=type;
    keypress->c[0]=c; keypress->c[1]='\0';
    keypress->nbytes=1;
}

/** Decode sequence of characters into a utf8 character */
static void inline_decode_utf8(unsigned char first, keypress_t *out) {
    out->nbytes = inline_utf8length(first);

    if (!out->nbytes) return; // Invalid first byte or stray continuation

    out->c[0] = first;
    for (int i=1; i<out->nbytes; i++) {
        if (!inline_readraw(&out->c[i])) { out->c[i] = '\0'; return; }
    }

    out->c[out->nbytes] = '\0';
    out->type = KEY_CHARACTER;
}

/** Map from terminal codes to keytype_t  */
typedef struct {
    const char *seq;
    keytype_t type;
} escmap_t;

static const escmap_t esc_table[] = {
    { "[A",    KEY_UP },
    { "[B",    KEY_DOWN },
    { "[C",    KEY_RIGHT },
    { "[D",    KEY_LEFT },
    { "[H",    KEY_HOME },
    { "[F",    KEY_END },
    { "[Z",    KEY_SHIFT_TAB },
    { "[5~",   KEY_PAGE_UP },
    { "[6~",   KEY_PAGE_DOWN },
    { "[1;2C", KEY_SHIFT_RIGHT },
    { "[1;2D", KEY_SHIFT_LEFT },
};

static void inline_decode_escape(keypress_t *out) {
    unsigned char seq[INLINE_ESCAPECODE_MAXLENGTH+1];
    int i = 0;
    out->type = KEY_UNKNOWN;

    if (!inline_readraw(&seq[i])) return; // Read byte after esc

    if (seq[0] !='[') { // Is this an alt + char combo?
        inline_decode_utf8(seq[0], out);
        out->type=KEY_ALT; // Override type
        return;
    }

    // It's an escape code, so read until alpha terminator
    for (i = 1; i < INLINE_ESCAPECODE_MAXLENGTH - 1; i++) {
        if (!inline_readraw(&seq[i])) break;
        if (isalpha(seq[i]) || seq[i] == '~') break;
    }
    seq[i + 1] = '\0'; // Ensure null terminated

    // Lookup escape code
    for (size_t j = 0; j < sizeof(esc_table)/sizeof(esc_table[0]); j++) {
        if (strcmp((const char *)seq, esc_table[j].seq) == 0) {
            out->type = esc_table[j].type;
            return;
        }
    }
}

/** Raw control codes produced by POSIX terminals */
enum keycodes {
    BACKSPACE_CODE = 8,   // Backspace (Ctrl+H)
    TAB_CODE       = 9,   // Tab
    LF_CODE        = 10,  // Line feed
    RETURN_CODE    = 13,  // Enter / Return (CR)
    ESC_CODE       = 27,  // Escape
    DELETE_CODE    = 127  // Delete (DEL)
};

/** Decode raw input units into a keypress */
static void inline_decode(const rawinput_t *raw, keypress_t *out) {
    inline_keypressunknown(out); // Initially UNKNOWN
    unsigned char b = *raw;

    if (b < 32 || b == DELETE_CODE) { // Control keys (ASCII control range or DEL)
        switch (b) {
            case TAB_CODE:    out->type = KEY_TAB; return;
            case LF_CODE:     out->type = KEY_CTRL_RETURN; return;
            case RETURN_CODE: out->type = KEY_RETURN; return;
            case BACKSPACE_CODE: // v fallthrough
            case DELETE_CODE: out->type = KEY_DELETE; return;
            case ESC_CODE:
                inline_decode_escape(out);
                return;

            default: // Control codes are Ctrl+A → 1, Ctrl+Z → 26
                if (b >= 1 && b <= 26) inline_keypresswithchar(out, KEY_CTRL, 'A' + (b - 1));
                return;
        }
    }

    if (b < 128) { // ASCII regular character
        inline_keypresswithchar(out, KEY_CHARACTER, b);
        return;
    }

    inline_decode_utf8(b, out); // UTF8
}

/** Obtain a keypress event */
static bool inline_readkeypress(inline_editor *edit, keypress_t *out) {
    (void) edit;
    rawinput_t raw;
    if (!inline_readraw(&raw)) return false;
    inline_decode(&raw, out);
    return true;
}

/* **********************************************************************
 * Input loop
 * ********************************************************************** */

/** Update the cursor position */
static inline void inline_setcursorposn(inline_editor *edit, int new_posn) {
    if (new_posn < 0) new_posn = 0;
    if (new_posn > edit->grapheme_count) new_posn = edit->grapheme_count;
    if (edit->cursor_posn == new_posn) return;
    edit->refresh = true;

    int old_row;
    inline_cursorposn(edit, &old_row, NULL);

    edit->cursor_posn = new_posn;
    inline_ensurecursorvisible(edit);
}

/** Insert text into the buffer */
static bool inline_insert(inline_editor *edit, const char *bytes, size_t nbytes) {
    if (!inline_extendbufferby(edit, nbytes)) return false; // Ensure capacity

    size_t offset = 0; // Obtain the byte offset of the current cursor position
    if (edit->cursor_posn < edit->grapheme_count) offset = edit->graphemes[edit->cursor_posn];
    else offset = edit->buffer_len;

    // Move contents after the insertion point to make room for the inserted text
    memmove(edit->buffer + offset + nbytes, edit->buffer + offset, edit->buffer_len - offset);

    memcpy(edit->buffer + offset, bytes, nbytes); // Copy new text into buffer
    edit->buffer_len += nbytes;
    edit->buffer[edit->buffer_len] = '\0'; // Ensure null-terminated
    
    inline_recomputegraphemes(edit);
    inline_recomputelines(edit);

    // Move cursor to end of inserted text
    int newpos = inline_findgraphemeindex(edit, offset + nbytes);
    inline_setcursorposn(edit, newpos);

    edit->refresh = true; // Redraw
    return true;
}

/** Helper to delete bytes [ start, end ) in the buffer */
static void inline_deletebytes(inline_editor *edit, size_t start, size_t end) {
    if (start >= end || end > edit->buffer_len) return;

    size_t bytes = end - start;
    memmove(edit->buffer + start, edit->buffer + end, edit->buffer_len - end); // Move subsequent text
    edit->buffer_len -= bytes;

    edit->buffer[edit->buffer_len] = '\0'; // Ensure null termination

    inline_recomputegraphemes(edit);
    inline_recomputelines(edit);
    edit->refresh = true;
}

/** Helper to delete a grapheme at a given index */
static void inline_deletegrapheme(inline_editor *edit, int index) {
    if (index < 0 || index >= edit->grapheme_count) return;

    size_t start, end;
    inline_graphemerange(edit, index, &start, &end);
    inline_deletebytes(edit, start, end);
}

/** Deletes selected text */
static void inline_deleteselection(inline_editor *edit) {
    int sel_l;
    size_t start, end;
    if (!inline_selectionrange(edit, &sel_l, NULL, &start, &end)) return;

    inline_deletebytes(edit, start, end); // Delete the selection

    edit->selection_posn = INLINE_INVALID; // Clear selection
    inline_setcursorposn(edit, sel_l); // Cursor moves to start of deleted region
}

/** Delete character under cursor */
static void inline_deletecurrent(inline_editor *edit) {
    if (edit->cursor_posn < edit->grapheme_count) { // Delete grapheme under cursor if at start of line
        inline_deletegrapheme(edit, edit->cursor_posn);
    }
}

/** Delete text from the buffer */
static void inline_delete(inline_editor *edit) {
    if (edit->selection_posn != INLINE_INVALID) {
        inline_deleteselection(edit);
    } else if (edit->cursor_posn > 0) { // Delete grapheme before cursor
        inline_deletegrapheme(edit, edit->cursor_posn - 1);
        inline_setcursorposn(edit, edit->cursor_posn - 1);
    } else inline_deletecurrent(edit);
}

/** Clear the buffer */
static void inline_clear(inline_editor *edit) {
    edit->buffer_len = 0; // Clear text buffer
    edit->buffer[0] = '\0';
    inline_recomputegraphemes(edit);
    inline_recomputelines(edit);
    inline_setcursorposn(edit, 0); // Reset cursor
    edit->refresh = true;
    edit->suggestion_shown = false;
}

/** Navigation keys */
static void inline_navigatetolineboundary(inline_editor *edit, bool end) {
    int row;
    inline_cursorposn(edit, &row, NULL);
    inline_setcursorposn(edit, inline_findgraphemeindex(edit, edit->lines[row + end]));
}

static void inline_home(inline_editor *edit) {
    inline_navigatetolineboundary(edit, false);
}

static void inline_end(inline_editor *edit) {
    inline_navigatetolineboundary(edit, true);
}

static void inline_pageup(inline_editor *edit) {
    inline_setcursorposn(edit, 0);
}

static void inline_pagedown(inline_editor *edit) {
    inline_setcursorposn(edit, edit->grapheme_count);
}

static void inline_left(inline_editor *edit) {
    if (edit->cursor_posn > 0)
        inline_setcursorposn(edit, edit->cursor_posn - 1);
}

static void inline_right(inline_editor *edit) {
    if (edit->cursor_posn < edit->grapheme_count)
        inline_setcursorposn(edit, edit->cursor_posn + 1);
}

/** Selection */
static void inline_beginselection(inline_editor *edit) {
    if (edit->selection_posn==INLINE_INVALID) edit->selection_posn = edit->cursor_posn;
}

static void inline_clearselection(inline_editor *edit) {
    edit->selection_posn=INLINE_INVALID;
}

/** Copy selected text */
static void inline_copyselection(inline_editor *edit) {
    size_t start, end;
    if (inline_selectionrange(edit, NULL, NULL, &start, &end))
        inline_copytoclipboard(edit, edit->buffer + start, end - start);
}

/** Cut selected text */
static void inline_cutselection(inline_editor *edit) {
    inline_copyselection(edit);
    inline_deleteselection(edit);
}

/** Cut part of a line */
static void inline_cutline(inline_editor *edit, bool before) {
    int row;
    inline_cursorposn(edit, &row, NULL);
    size_t b_line = edit->lines[row + (before ? 0 : 1)]; // line break position before or after
    size_t b_cursor = edit->graphemes[edit->cursor_posn]; // Cursor position

    size_t b_start = imin(b_line, b_cursor), b_end = imax(b_line, b_cursor);
    if (!before && b_end>0 && edit->buffer[b_end-1]=='\n') b_end--; // Don't include newline
    if (b_start==b_end) return; // Nothing to copy

    inline_copytoclipboard(edit, edit->buffer + b_start, b_end - b_start);
    inline_deletebytes(edit, b_start, b_end);
    inline_setcursorposn(edit, inline_findgraphemeindex(edit, b_start)); // Cursor moves to start of deleted region
}

/** Paste from clipboard */
static void inline_paste(inline_editor *edit) {
    if (edit->clipboard && edit->clipboard_len>0) {
        if (edit->selection_posn != INLINE_INVALID) inline_deleteselection(edit); // Replace selection
        inline_insert(edit, edit->clipboard, edit->clipboard_len);
    }
}

/** Process a history keypress */
static void inline_historykey(inline_editor *edit, int delta) {
    inline_advancehistory(edit, delta);
    inline_setcursorposn(edit, edit->grapheme_count); // Move to end
    inline_clearselection(edit);
    inline_clearsuggestions(edit);
}

/** Transpose two graphemes */
static void inline_transpose(inline_editor *edit) {
    int n = edit->grapheme_count, cur = edit->cursor_posn;
    if (n < 2 || cur == 0) return;

    int a = (cur >= n ? n-2 : cur-1), b = a + 1; // The two graphemes to swap
    size_t a_start, a_end, b_start, b_end; // Their byte bounds
    inline_graphemerange(edit, a, &a_start, &a_end);
    inline_graphemerange(edit, b, &b_start, &b_end);

    size_t a_len = a_end - a_start, b_len = b_end - b_start; // Temporary buffer
    char *tmp = malloc(a_len);
    if (!tmp) return;

    memcpy(tmp, edit->buffer + a_start, a_len); // Copy a into temporary buffer
    memmove(edit->buffer + a_start, edit->buffer + b_start, b_len); // Copy b overwriting a
    memcpy(edit->buffer + a_start + b_len, tmp, a_len); // Copy a from the temporary buffer

    free(tmp);

    inline_recomputegraphemes(edit);
    if (cur < n) inline_setcursorposn(edit, edit->cursor_posn+1);
}

/** Apply current suggestion */
static void inline_applysuggestion(inline_editor *edit) {
    const char *suffix = inline_currentsuggestion(edit);
    if (suffix && *suffix) inline_insert(edit, suffix, strlen(suffix));
    inline_clearsuggestions(edit);
}

/** Handle Ctrl+_ shortcuts */
static bool inline_processshortcut(inline_editor *edit, char c) {
    switch (c) {
        case 'A': inline_home(edit); break;
        case 'B': inline_left(edit); break;
        case 'C': inline_clear(edit); return false; // exit on Ctrl-C
        case 'D':
            inline_clearselection(edit);
            inline_deletecurrent(edit); 
            break;
        case 'E': inline_end(edit); break;
        case 'F': inline_right(edit); break;
        case 'G': return false; // exit on Ctrl-G
        case 'K': inline_cutline(edit, false); break; // Cut to end of line
        case 'L': inline_clear(edit); break;
        case 'N': inline_historykey(edit, 1); break; // Next history
        case 'O': inline_copyselection(edit); break;
        case 'P': inline_historykey(edit, -1); break; // Previous history
        case 'T': inline_transpose(edit); break;
        case 'U': inline_cutline(edit, true); break; // Cut to start of line
        case 'X': inline_cutselection(edit); break;
        case 'Y': // v fallthrough
        case 'V': inline_paste(edit); break;
        default: break;
    }
    edit->refresh = true;
    return true;
}

/** Handle Meta + _ shortcuts; upper case versions indicate Shift + Meta + _ */
static bool inline_processmeta(inline_editor *edit, const unsigned char *c, int nbytes) {
    (void) nbytes; 
    switch (*c) {
        case 'w': case 'W': inline_copyselection(edit); break;
        default: break;
    }
    edit->refresh = true;
    return true;
}

/** Process a keypress */
static bool inline_processkeypress(inline_editor *edit, const keypress_t *key) {
    bool generatesuggestions=true, clearselection=true, endbrowsing=true;
    switch (key->type) {
        case KEY_RETURN:
            if (!edit->multiline_fn ||
                !edit->multiline_fn(edit->buffer, edit->multiline_ref)) return false;
        case KEY_CTRL_RETURN: // v fallthrough
            if (!inline_insert(edit, "\n", 1)) return false;
            generatesuggestions = false;  // newline shouldn't trigger suggestion
            break;
        case KEY_LEFT:   inline_left(edit); break;
        case KEY_RIGHT:
            if (edit->suggestion_shown) {
                inline_applysuggestion(edit);
                generatesuggestions = false;
                break;
            }
            inline_right(edit);
            break;
        case KEY_SHIFT_LEFT:
            inline_beginselection(edit);
            inline_left(edit);
            clearselection=false;
            break;
        case KEY_SHIFT_RIGHT:
            inline_beginselection(edit);
            inline_right(edit);
            clearselection=false;
            break;
        case KEY_UP:
            inline_historykey(edit, -1);
            endbrowsing=false;
            break;
        case KEY_DOWN:
            inline_historykey(edit, +1);
            endbrowsing=false;
            break;
        case KEY_HOME:      inline_home(edit);       break;
        case KEY_END:       inline_end(edit);        break;
        case KEY_PAGE_UP:   inline_pageup(edit);     break;
        case KEY_PAGE_DOWN: inline_pagedown(edit);   break;
        case KEY_DELETE:    inline_delete(edit);     break;
        case KEY_TAB:
            if (inline_havesuggestions(edit)) {
                inline_advancesuggestions(edit, 1);
                generatesuggestions=false;
            } else if (!inline_insert(edit, "\t", 1)) return false;
            break;
        case KEY_SHIFT_TAB:
            if (inline_havesuggestions(edit)) {
                inline_advancesuggestions(edit, -1);
                generatesuggestions=false;
            }
            break;
        case KEY_CTRL:  return inline_processshortcut(edit, key->c[0]);
        case KEY_ALT:   return inline_processmeta(edit, key->c, key->nbytes);
        case KEY_CHARACTER:
            if (!inline_insert(edit, (char *) key->c, key->nbytes)) return false;
            break;
        case KEY_UNKNOWN:
            break;
    }

    if (clearselection) inline_clearselection(edit);
    if (generatesuggestions) inline_generatesuggestions(edit);
    if (endbrowsing) inline_endhistorybrowsing(edit);
    edit->refresh = true;
    return true;
}

/* **********************************************************************
 * Interface
 * ********************************************************************** */

/** If we're not attached to a terminal, e.g. a pipe, simply read the file in. */
static void inline_noterminal(inline_editor *edit) {
    int c;

    while ((c = fgetc(stdin)) != EOF && c != '\n') {
        if (!inline_extendbufferby(edit, 1)) break; // Buffer could not be extended
        edit->buffer[edit->buffer_len++] = (char)c;
    }

    edit->buffer[edit->buffer_len] = '\0'; // Ensure null termination
}

/** If the terminal is unsupported, display a prompt and read the line normally. */
static void inline_unsupported(inline_editor *edit) {
    fputs(edit->prompt, stdout);
    fflush(stdout);  // Ensure prompt appears

    inline_noterminal(edit);

    int length = (int)edit->buffer_len - 1; // Strip trailing control characters
    while (length >= 0 && iscntrl((unsigned char)edit->buffer[length])) {
        edit->buffer[length--] = '\0';
    }

    edit->buffer_len = length + 1;
}

/** Normal interface if terminal recognized */
static void inline_supported(inline_editor *edit) {
    inline_reset(edit);
    inline_setutf8();
    if (!inline_enablerawmode(edit)) return;  // Could not enter raw mode
    inline_updateterminalwidth(edit);
    inline_initviewport(edit);
    inline_redraw(edit);

    keypress_t key;
    while (inline_readkeypress(edit, &key)) {
        if (!inline_processkeypress(edit, &key)) break;

        if (resize_pending) {
            /* Update terminal width and viewport on resize */
            inline_updateterminalwidth(edit);
            inline_updateviewportwidth(edit);
            edit->refresh = true; // Ensure we redraw after resize
            resize_pending = 0;
        }
        
        if (edit->refresh) {
            inline_redraw(edit);
            edit->refresh = false;
        }
    }

    inline_clearselection(edit);
    inline_clearsuggestions(edit);
    inline_redraw(edit);
    inline_disablerawmode(edit);

    if (edit->buffer_len > 0) inline_addhistory(edit, edit->buffer); // Add to history if non-empty
    write(STDOUT_FILENO, "\r\n", 2);
}

/** API function to read a line of text from the user.
 *  @param   edit - an inline_editor that has been created with inline_new.
 *  @returns a heap-allocated copy of the string input by the user (caller must free),
 *           or NULL on error. */
char *inline_readline(inline_editor *edit) {
    if (!edit) return NULL;

    edit->buffer_len = 0; // Reset buffer
    edit->buffer[0] = '\0';

    if (!inline_checktty()) {
        inline_noterminal(edit);
    } else if (inline_checksupported()) {
        inline_supported(edit);
    } else {
        inline_unsupported(edit);
    }

    return (edit->buffer ? inline_strdup(edit->buffer) : NULL);
}
