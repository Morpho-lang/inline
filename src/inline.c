/** @file inline.c
 *  @author T J Atherton
 *
 *  @brief A simple UTF8 aware line editor with history, completion, multiline editing and syntax highlighting */

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
    int screen_cols;         // Viewport width
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
void inline_setpalette(inline_editor *edit, int count, const int *palette) {
    free(edit->palette); // Clear any old palette data
    edit->palette = NULL;
    edit->palette_count = 0;

    if (count <= 0 || palette == NULL) return;

    edit->palette = malloc(sizeof(int) * count);
    if (!edit->palette) return;

    memcpy(edit->palette, palette, sizeof(int) * count);
    edit->palette_count = count;
}

/** API function to enable autocomplete */
void inline_autocomplete(inline_editor *edit, inline_completefn fn, void *ref) {
    edit->complete_fn = fn;
    edit->complete_ref = ref;
}

/** API function to enable multiline editing */
void inline_multiline(inline_editor *edit, inline_multilinefn fn, void *ref, const char *continuation_prompt) {
    edit->multiline_fn = fn;
    edit->multiline_ref = ref;

    free(edit->continuation_prompt);
    edit->continuation_prompt = inline_strdup(continuation_prompt ? continuation_prompt : edit->prompt);
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
static bool inline_checksupported(void) {
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

/** Update the terminal width */
static void inline_updateterminalwidth(inline_editor *edit) {
    int width = 80; // fallback 

#ifdef _WIN32
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;

    if (GetConsoleScreenBufferInfo(h, &csbi)) {
        width = csbi.srWindow.Right - csbi.srWindow.Left + 1;
    }
#else
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != -1 && ws.ws_col > 0) {
        width = ws.ws_col;
    }
#endif

    edit->ncols = width;
}

/* ----------------------------------------
 * Handle crashes
 * ---------------------------------------- */

/** Exit handler called on crashes */
static void inline_emergencyrestore(void) {
    if (inline_lasteditor) inline_disablerawmode(inline_lasteditor);
}

#ifdef _WIN32
static BOOL WINAPI inline_consolehandler(DWORD ctrl) {
    inline_emergencyrestore(); // Restore on any console event
    return FALSE; // Allow default behavior
}
#else 
static void inline_signalhandler(int sig, siginfo_t *info, void *ucontext) {
    if (sig == SIGWINCH) {
        if (inline_lasteditor) inline_lasteditor->refresh = true;
        return; 
    } 
    inline_emergencyrestore();
    signal(sig, SIG_DFL);
    raise(sig);
}
#endif

/** Register emergency exit and signal handlers */
static void inline_registeremergencyhandlers(void) {
    static bool registered = false; 
    if (registered) return; 
    registered=true;

    atexit(inline_emergencyrestore);
#ifdef _WIN32 
    SetConsoleCtrlHandler(inline_consolehandler, TRUE);
#else 
    const int sigs[] = { // List of signals to respond to
        SIGINT, SIGTERM, SIGQUIT, SIGHUP, SIGSEGV, SIGABRT, SIGBUS, SIGILL, SIGFPE, SIGWINCH
    };

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sigemptyset(&sa.sa_mask);
    sa.sa_sigaction = inline_signalhandler;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;

    for (size_t i = 0; i < sizeof(sigs)/sizeof(sigs[0]); i++) sigaction(sigs[i], &sa, NULL);
#endif
}

/* ----------------------------------------
 * Switch to/from raw mode
 * ---------------------------------------- */

/** Enable utf8 */
static void inline_setutf8(void) {
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
}

/* **********************************************************************
 * Utility functions
 * ********************************************************************** */

/** Min and max */
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

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
    size_t required = edit->buffer_len + extra + 1;  // +1 for null terminator

    if (required <= edit->buffer_size) return true;  // Sufficient space already

    size_t newcap = edit->buffer_size ? edit->buffer_size : INLINE_DEFAULT_BUFFER_SIZE;
    while (newcap < required) newcap *= 2; // Grow exponentially

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

/** Minimal grapheme splitter */
static size_t inline_graphemesplit(const char *in, const char *end) {
    const unsigned char *p =  (const unsigned char *) in,
                        *uend = (const unsigned char *) end;
    if (p >= uend) return 0; // At end already

    // Read first codepoint
    size_t len = inline_utf8length(*p);
    if (len == 0) len = 1; // Recover from malformed utf8 codepoint
    if ((size_t)(uend - p) < len) return (size_t)(uend - p); 
    p += len;

    // Combining diacritical marks U+0300–U+036F (accents, etc.)
    while (p < uend && *p >= 0xCC && *p <= 0xCF) {
        len = inline_utf8length(*p);
        if (len == 0 || (size_t)(uend - p) < len) break;
        p += len;
    }

    do { // Skip past suffix extenders 
        len = inline_matchcodepoint(suffix_count, suffix_extenders, p, uend);
        p += len;
    } while (len!=0);

    for (;;) { // Joiners (ZWJ sequences)
        len = inline_matchcodepoint(joiners_count, joiners, p, uend);
        if (len == 0) break;
        p += len;

        if (p >= uend) break;

        len = inline_utf8length((unsigned char)*p); // Process joined codepoint
        if (len == 0 || (size_t)(uend - p) < len) break;
        p += len;
    }

    return (size_t)(p - (const unsigned char *)in);
}

/* ----------------------------------------
 * Grapheme buffer
 * ---------------------------------------- */

/** Compute grapheme locations */
static void inline_recomputegraphemes(inline_editor *edit) {
    int needed = (int) edit->buffer_len + 1; // Assume 1 byte per character as a worst case + sentinel

    size_t required_bytes = needed * sizeof(size_t); // Ensure capacity
    if (required_bytes > edit->grapheme_size) {
        size_t newsize = (edit->grapheme_size ? edit->grapheme_size : INLINE_DEFAULT_BUFFER_SIZE);
        while (newsize < required_bytes) newsize *= 2;

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
        while (newsize < needed) newsize *= 2;

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
static void inline_addsuggestion(inline_editor *edit, char *s) {
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
        char *s;
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

/** Adds an entry to the history list */
static void inline_addhistory(inline_editor *edit, const char *buffer) {
    if (!buffer || !*buffer) return; // Skip empty buffers

    if (edit->history.count > 0) { // Avoid duplicate consecutive entries
        const char *last = edit->history.items[edit->history.count - 1];
        if (strcmp(last, buffer) == 0) return;
    }

    inline_stringlist_add(&edit->history, buffer);
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
static void inline_endbrowsing(inline_editor *edit) {
    edit->history.index = INLINE_INVALID;
}

/* ----------------------------------------
 * Reset
 * ---------------------------------------- */

/** Resets the editor before a new session */
static void inline_reset(inline_editor *edit) {
    inline_clear(edit);
    inline_clearselection(edit);
    inline_endbrowsing(edit);
    inline_stringlist_clear(&edit->suggestions);
    edit->rawmode_enabled = false;
}

/* ----------------------------------------
 * Viewport
 * ---------------------------------------- */

/** Initialize the viewport */
static void inline_initviewport(inline_editor *edit) {
    edit->viewport.first_visible_line = 0;
    edit->viewport.first_visible_col  = 0;
    edit->viewport.screen_rows = 1; // Will adjust for multiline editing later
    int prompt_width; 
    if (!inline_stringwidth(edit, edit->prompt, &prompt_width)) prompt_width = 0; 
    edit->viewport.screen_cols = edit->ncols - prompt_width; // Terminal width already known.
}

/** Compute logical cursor position in rows and columns */
static void inline_cursorposn(inline_editor *edit, int *out_row, int *out_col) {
    size_t byte_pos = edit->graphemes[edit->cursor_posn];  // byte offset of cursor

    int row = 0; // Find the row containing the cursor
    while (row + 1 < edit->line_count && edit->lines[row + 1] <= byte_pos) row++;

    *out_row = row; 

    // The column is found by subtracting the grapheme offset of the start of the row
    *out_col = edit->cursor_posn - inline_findgraphemeindex(edit, edit->lines[row]);
}

/** Check the cursor is visible */
static void inline_ensurecursorvisible(inline_editor *edit) {
    int cursor_row, cursor_col;
    inline_cursorposn(edit, &cursor_row, &cursor_col); // Logical indices

    int line_start_g = inline_findgraphemeindex(edit, edit->lines[cursor_row]); 
    int cursor_g = line_start_g + cursor_col; // Grapheme index relative to start of buffer

    int cursor_term_col = inline_graphemerangewidth(edit, line_start_g, cursor_g); // terminal width up to cursor

    int first = edit->viewport.first_visible_col;
    int last  = first + edit->viewport.screen_cols - 1;

    if (cursor_term_col < first) { // Cursor is left of viewport
        edit->viewport.first_visible_col = cursor_term_col;
    } else if (cursor_term_col > last) { // Cursor is right of viewport
        edit->viewport.first_visible_col = cursor_term_col - edit->viewport.screen_cols + 1;
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
static inline void inline_emit(const char *seq) {
    write(STDOUT_FILENO, seq, (unsigned int) strlen(seq));
}

/** Writes an escape sequence to produce a given color */
static void inline_emitcolor(int color) {
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

/** Compute visible grapheme range [g_start, g_end) */
static inline void inline_visiblegraphemerange(inline_editor *edit, int *g_start, int *g_end) {
    inline_widthfn width_fn = edit->width_fn ? edit->width_fn : inline_graphemewidth;

    int start_col = edit->viewport.first_visible_col;
    int end_col   = start_col + edit->viewport.screen_cols;

    int col = 0;
    int start = -1;   // < 0 means "not found yet"
    int end   = 0;

    for (int i = 0; i < edit->grapheme_count; i++) {
        size_t s, e;
        inline_graphemerange(edit, i, &s, &e);
        int w = width_fn(edit->buffer + s, e - s);

        if (start < 0) { // Haven't entered visible region yet
            if (col + w > start_col) start = i;   // first visible grapheme
        }

        if (start >= 0) { // Inside visible region
            if (col >= end_col) break;       // past right edge
            end = i + 1;
        }

        col += w;
    }

    if (start < 0) start = edit->grapheme_count; // Clamp if line is empty or viewport is beyond end
    if (end   < start) end = start;

    *g_start = start;
    *g_end   = end;
}

/** Clip grapheme range [*g_start, *g_end) horizontally based on viewport */
static inline void inline_clipgraphemerange(inline_editor *edit, int prompt_width, int *g_start, int *g_end) {
    inline_widthfn width_fn = edit->width_fn ? edit->width_fn : inline_graphemewidth;

    int start_col = edit->viewport.first_visible_col;
    int end_col   = start_col + edit->viewport.screen_cols;

    int col = prompt_width;   // Line begins after prompt
    int start = -1;
    int end   = *g_start;

    for (int i = *g_start; i < *g_end; i++) {
        size_t s, e;
        inline_graphemerange(edit, i, &s, &e);
        int w = width_fn(edit->buffer + s, e - s);

        if ((col + w > start_col) && (col < end_col)) {
            if (start < 0) start = i;        // first visible grapheme
            end = i + 1;                     // extend visible range
        }

        if (col >= end_col) break;
        col += w;
    }

    if (start < 0) start = *g_end; // Clamp if line is empty or viewport is beyond end
    if (end < start) end = start;

    *g_start = start;
    *g_end   = end;
}

/** Move terminal cursor to the editor's origin given the current cursor row */
static inline void inline_movetoorigin(int cursor_row) {
    write(STDOUT_FILENO, "\r", 1); // Move to start of current line

    if (cursor_row > 0) { // Move up cursor_row lines
        char seq[INLINE_ESCAPECODE_MAXLENGTH];
        int n = snprintf(seq, sizeof(seq), "\x1b[%dA", cursor_row);
        write(STDOUT_FILENO, seq, n);
    }
}

/** Move to a given (row,col) */
static inline void inline_moveto(int row, int col) {
    char seq[INLINE_ESCAPECODE_MAXLENGTH];

    if (row > 0) { // Move down to target row
        int n = snprintf(seq, sizeof(seq), "\x1b[%dB", row);
        write(STDOUT_FILENO, seq, n);
    }

    if (col > 0) { // Move right to target column
        int n = snprintf(seq, sizeof(seq), "\x1b[%dC", col);
        write(STDOUT_FILENO, seq, n);
    }
}

/** Move the cursor by a specified delta; down is positive dy */
static inline void inline_moveby(int dx, int dy) {
    char seq[INLINE_ESCAPECODE_MAXLENGTH];

    if (dy!=0) { // Vertical 
        int n = snprintf(seq, sizeof(seq), "\x1b[%d%c", abs(dy), (dy < 0 ? 'A' : 'B'));
        write(STDOUT_FILENO, seq, n);
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
    write(STDOUT_FILENO, "\r", 1);        // Move cursor to start of line

    write(STDOUT_FILENO, prompt, (unsigned int) strlen(prompt)); // Write prompt
    int prompt_width = 0; // Calculate its display width
    if (!inline_stringwidth(edit, prompt, &prompt_width)) prompt_width = 0; 

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
    inline_clipgraphemerange(edit, prompt_width, &g_start, &g_end);

    // Determine actual rendered cursor column
    if (logical_cursor_col >= 0) { // Cursor is on this line
        int clipped_col = logical_cursor_col - (g_start-line_start);
        if (clipped_col >= 0 && clipped_col < (g_end - g_start)) {
            *rendered_cursor_col = prompt_width + inline_graphemerangewidth(edit, g_start, g_start + clipped_col);
        } else *rendered_cursor_col = -1; 
    }

    int current_color = -1;
    bool selection_on = false; // Track the terminal inverse video state

    // Render syntax-colored, clipped graphemes
    int g = g_start;
    size_t off = edit->graphemes[g_start];

    // Render syntax-colored, clipped graphemes
    while (g < g_end && off < byte_end) {
        // Compute color span from current point
        inline_colorspan_t span = { .byte_start = off, .byte_end = off + 1, .color = 0 };
        if (edit->syntax_fn) edit->syntax_fn(edit->buffer, edit->syntax_ref, off, &span);
        int span_color = (span.color < edit->palette_count ? edit->palette[span.color] : -1);

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

            write(STDOUT_FILENO, edit->buffer + gs, (unsigned int) (ge - gs));
        }

        off = span.byte_end;
    }

    if (selection_on || current_color != -1) inline_emit(TERM_RESETCOLOR);

    // Ghosted suggestion suffix (only if at right edge on last line)
    if (is_last && g_end == edit->grapheme_count && logical_cursor_col >= 0) {
        const char *suffix = inline_currentsuggestion(edit);
        edit->suggestion_shown=false; 
        if (suffix && *suffix) {
            int remaining_cols = edit->viewport.screen_cols - *rendered_cursor_col;

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

    inline_emit(TERM_CLEAR); // Clear to end of line
}

/** Redraw the entire buffer in multiline mode */
void inline_redraw(inline_editor *edit) {
    inline_emit(TERM_HIDECURSOR); // Prevent flickering

    int cursor_row, cursor_col; // Compute logical cursor column and row (pre-clipping)
    inline_cursorposn(edit, &cursor_row, &cursor_col);

    inline_movetoorigin(cursor_row); 

    int rendered_cursor_col = -1;  // To be filled out by inline_renderline

    for (int i = 0; i < edit->line_count; i++) {
        size_t byte_start = edit->lines[i]; // Render lines
        size_t byte_end   = edit->lines[i+1];
        bool is_last = (i == edit->line_count - 1);

        inline_renderline(edit, (i==0 ? edit->prompt : edit->continuation_prompt), // prompt
                          byte_start, byte_end, 
                          (cursor_row == i ? cursor_col : -1), // cursor column if on this line
                          is_last, // whether we're on the last line or not
                          &rendered_cursor_col );      
    }

    write(STDOUT_FILENO, "\r", 1); // Move to start of line

    inline_moveby(rendered_cursor_col, cursor_row - edit->line_count + 1);
    inline_emit(TERM_SHOWCURSOR);
}
  
/** API function to print a syntax colored string */
void inline_displaywithsyntaxcoloring(inline_editor *edit, const char *string) {
    if (!edit || !string) return;
    size_t len = strlen(string);

    if (!edit->syntax_fn || !edit->palette_count) { // Syntax highlighting not configured, fallback to plain
        write(STDOUT_FILENO, string, (unsigned int) len);
        return;
    }

    size_t offset = 0;
    while (offset < len) { // 
        inline_colorspan_t span;

        bool ok = edit->syntax_fn(string, edit->syntax_ref, offset, &span); // Obtain next span
        if (!ok) { // No more spans, print the rest uncolored
            write(STDOUT_FILENO, string + offset, (unsigned int) (len - offset));
            return;
        }

        // Print any uncolored text before the span
        if (span.byte_start > offset) write(STDOUT_FILENO, string + offset, (unsigned int) (span.byte_start - offset));

        if (span.color<edit->palette_count) inline_emitcolor(edit->palette[span.color]);
        write(STDOUT_FILENO, string + span.byte_start, (unsigned int) (span.byte_end - span.byte_start));
        inline_emit(TERM_RESETFOREGROUND);

        offset = span.byte_end;
    }
}

/* **********************************************************************
 * Keypress decoding
 * ********************************************************************** */

/* ----------------------------------------
 * Raw input layer
 * ---------------------------------------- */

/** Type that represents a single unit of input */
typedef unsigned char rawinput_t;

/** Await a single raw unit of input and store in a rawinput_t */
static bool inline_readraw(rawinput_t *out) {
    int n = (int) read(STDIN_FILENO, out, 1);
    return n == 1;
}

/* ----------------------------------------
 * Keypress decoding layer
 * ---------------------------------------- */

/** Identifies the type of keypress */
typedef enum {
    KEY_UNKNOWN, KEY_CHARACTER,
    KEY_RETURN, KEY_TAB, KEY_SHIFT_TAB, KEY_DELETE,
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,   // Arrow keys
    KEY_HOME, KEY_END,               // Home and End
    KEY_PAGE_UP, KEY_PAGE_DOWN,      // Page up and page down
    KEY_SHIFT_LEFT, KEY_SHIFT_RIGHT, // Shift+arrow key
    KEY_CTRL
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

    // Expect '[' 
    if (!inline_readraw(&seq[i]) || seq[0] != '[') { return; }

    // Read until alpha terminator
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
            case LF_CODE:     return; 
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
    if (new_posn > edit->grapheme_count) new_posn = edit->grapheme_count; // Clamp to [0,grapheme_count]

    if (edit->cursor_posn != new_posn) {
        edit->cursor_posn = new_posn;
        inline_ensurecursorvisible(edit);
        edit->refresh = true;
    }
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

    int old_count = edit->grapheme_count; // Save grapheme count
    
    inline_recomputegraphemes(edit); 
    inline_recomputelines(edit);

    // Move cursor forward by number of graphemes
    int inserted_count = edit->grapheme_count - old_count; 
    inline_setcursorposn(edit, edit->cursor_posn + (inserted_count > 0? inserted_count : 0)); 

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
    edit->grapheme_count = 0; // Reset graphemes
    inline_setcursorposn(edit, 0); // Reset cursor
    edit->refresh = true;
}

/** Navigation keys */
static void inline_home(inline_editor *edit) {
    if (edit->cursor_posn != 0) 
        inline_setcursorposn(edit, 0); // Reset cursor
}

static void inline_end(inline_editor *edit) {
    if (edit->cursor_posn != edit->grapheme_count) 
        inline_setcursorposn(edit, edit->grapheme_count); // Reset cursor
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

/** Paste from clipboard */
static void inline_paste(inline_editor *edit) {
    if (edit->clipboard && edit->clipboard_len>0) {
        if (edit->selection_posn != INLINE_INVALID) inline_deleteselection(edit); // Replace selection
        inline_insert(edit, edit->clipboard, edit->clipboard_len);
    }
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
        case 'C': inline_copyselection(edit); break; 
        case 'D': 
            inline_clearselection(edit);
            inline_deletecurrent(edit);
            break; 
        case 'E': inline_end(edit); break;
        case 'F': inline_right(edit); break;
        case 'G': return false; // exit on Ctrl-G
        case 'L': inline_clear(edit); break; 
        case 'X': inline_cutselection(edit); break; 
        case 'Y': // v fallthrough
        case 'V': inline_paste(edit); break; 
        default: break;
    }
    edit->refresh = true;
    return true;
}

/** Process a keypress */
static bool inline_processkeypress(inline_editor *edit, const keypress_t *key) {
    bool generatesuggestions=true, clearselection=true, endbrowsing=true;
    switch (key->type) {
        case KEY_END:
        case KEY_RETURN: 
            if (!edit->multiline_fn ||
                !edit->multiline_fn(edit->buffer, edit->multiline_ref)) return false;
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
            inline_advancehistory(edit, -1);
            inline_setcursorposn(edit, edit->grapheme_count);
            endbrowsing=false;
            break;
        case KEY_DOWN:
            inline_advancehistory(edit, 1);
            inline_setcursorposn(edit, edit->grapheme_count);
            endbrowsing=false;
            break;
        case KEY_HOME:   inline_home(edit);        break;
        //case KEY_END:    inline_end(edit);         break;
        case KEY_DELETE: inline_delete(edit);      break;
        case KEY_TAB:
            if (inline_havesuggestions(edit)) {
                inline_advancesuggestions(edit, 1);
                generatesuggestions=false; 
            }
            break; 
        case KEY_SHIFT_TAB:
            if (inline_havesuggestions(edit)) {
                inline_advancesuggestions(edit, -1);
                generatesuggestions=false; 
            }
            break; 
        case KEY_CTRL:  return inline_processshortcut(edit, key->c[0]);
        case KEY_CHARACTER: 
            if (!inline_insert(edit, (char *) key->c, key->nbytes)) return false;
            break;
        default:
            break;
    }

    if (clearselection) inline_clearselection(edit);
    if (generatesuggestions) inline_generatesuggestions(edit);
    if (endbrowsing) inline_endbrowsing(edit);
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

        if (edit->refresh) { 
            inline_redraw(edit); 
            edit->refresh = false; 
        }
    }

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
