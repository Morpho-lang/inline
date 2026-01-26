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

    int cursor_posn;                      // Position of cursor in graphemes
    int selection_posn;                   // Selection posn in graphemes 

    inline_syntaxcolorfn syntax_fn;       // Syntax coloring callback
    void *syntax_ref;                     // User reference

    inline_color_span *spans;             // Array of color spans
    int span_count;                       // Number of spans
    int span_capacity;                    // Capacity of span array 

    int *palette;                         // Palette: list of colors
    int palette_count;                    // Length of palette list

    inline_completefn complete_fn;        // Autocomplete callback
    void *complete_ref;                   // User reference 

    inline_stringlist_t suggestions;      // List of suggestions from autocompleter

    inline_multilinefn multiline_fn;      // Multiline callback
    void *multiline_ref;                  // User reference

    inline_stringlist_t history;          // List of history entries 

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
static bool inline_insert(inline_editor *edit, const char *bytes, size_t nbytes);
static void inline_clear(inline_editor *edit);
static void inline_clearselection(inline_editor *edit);

/* -----------------------
 * New/free API
 * ----------------------- */

/** Create a new line editor */
inline_editor *inline_new(const char *prompt) {
    inline_editor *editor = calloc(1, sizeof(*editor)); // All contents are zero'd
    if (!editor) return NULL;

    editor->prompt = inline_strdup(prompt ? prompt : INLINE_DEFAULT_PROMPT);
    if (!editor->prompt) goto inline_new_cleanup;

    editor->buffer_size = INLINE_DEFAULT_BUFFER_SIZE; // Allocate initial buffer
    editor->buffer = malloc(editor->buffer_size);
    if (!editor->buffer) goto inline_new_cleanup;

    editor->buffer[0] = '\0'; // Ensure zero terminated
    editor->buffer_len = 0;

    editor->selection_posn = INLINE_INVALID; // No selection

    inline_stringlist_init(&editor->suggestions);
    inline_stringlist_init(&editor->history);

    return editor;

inline_new_cleanup:
    inline_free(editor);
    return NULL; 
}

void inline_clearsuggestions(inline_editor *edit);

/** Free a line editor and associated resources */
void inline_free(inline_editor *editor) {
    if (!editor) return;

    free(editor->prompt);
    free(editor->continuation_prompt);

    free(editor->buffer);
    free(editor->graphemes);
    free(editor->clipboard);

    inline_clearsuggestions(editor);

    free(editor->spans);
    free(editor->palette);

    if (inline_lasteditor==editor) inline_lasteditor = NULL; 

    free(editor);
}

/* -----------------------
 * Configuration API
 * ----------------------- */

/** Enable syntax coloring */
void inline_syntaxcolor(inline_editor *editor, inline_syntaxcolorfn fn, void *ref) {
    editor->syntax_fn = fn;
    editor->syntax_ref = ref;
}

/** Set the color palette */
void inline_setpalette(inline_editor *editor, int count, const int *palette) {
    free(editor->palette); // Clear any old palette data
    editor->palette = NULL;
    editor->palette_count = 0;

    if (count <= 0 || palette == NULL) return;

    editor->palette = malloc(sizeof(int) * count);
    if (!editor->palette) return;

    memcpy(editor->palette, palette, sizeof(int) * count);
    editor->palette_count = count;
}

/** Enable autocomplete */
void inline_autocomplete(inline_editor *editor, inline_completefn fn, void *ref) {
    editor->complete_fn = fn;
    editor->complete_ref = ref;
}

/** Enable multiline editing */
void inline_multiline(inline_editor *editor, inline_multilinefn fn, void *ref, const char *continuation_prompt) {
    editor->multiline_fn = fn;
    editor->multiline_ref = ref;

    free(editor->continuation_prompt);
    editor->continuation_prompt = inline_strdup(continuation_prompt ? continuation_prompt : "");
}

/* **********************************************************************
 * Platform-dependent code 
 * ********************************************************************** */

/* ----------------------------------------
 * Check terminal features
 * ---------------------------------------- */

/** Check whether stdin and stdout are TTYs. */
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
 * Grapheme buffer
 * ---------------------------------------- */

/** Determine length of utf8 character from the first byte */
static inline int inline_utf8length(unsigned char b) {
    if ((b & 0x80) == 0x00) return 1;      // 0xxxxxxx
    if ((b & 0xE0) == 0xC0) return 2;      // 110xxxxx
    if ((b & 0xF0) == 0xE0) return 3;      // 1110xxxx
    if ((b & 0xF8) == 0xF0) return 4;      // 11110xxx
    return 0;                              // Invalid or continuation
}

/** Compute grapheme locations - Temporary implementation */
static void inline_recomputegraphemes(inline_editor *edit) {
    int needed = (int) edit->buffer_len; // Assume 1 byte per character as a worst case. 

    size_t required_bytes = needed * sizeof(size_t); // Ensure capacity
    if (required_bytes > edit->grapheme_size) {
        size_t newsize = edit->grapheme_size ? edit->grapheme_size : INLINE_DEFAULT_BUFFER_SIZE;
        while (newsize < required_bytes) newsize *= 2;

        size_t *new = realloc(edit->graphemes, newsize);
        if (!new) {
            edit->grapheme_count = 0;
            return;
        }

        edit->graphemes = new;
        edit->grapheme_size = newsize;
    }

    // Walk the buffer and record codepoint boundaries  
    int count = 0;
    for (size_t i = 0; i < edit->buffer_len; ) {
        edit->graphemes[count++] = i;
        size_t len = inline_utf8length((unsigned char)edit->buffer[i]);
        if (len <= 0) len = 1; // Recover from malformed utf8 
        i+=len; 
    }

    edit->grapheme_count = count;
}

/** Finds the start and end of grapheme i in bytes */
static inline void inline_graphemerange(inline_editor *edit, int i, size_t *start, size_t *end) {
    if (i < 0 || i >= edit->grapheme_count) { // Handle out of bounds access (incl. i representing end of line)
        if (start) *start = edit->buffer_len;
        if (end)   *end   = edit->buffer_len;
        return; 
    }

    if (start) *start = edit->graphemes[i];
    if (end) *end = ((i + 1 < edit->grapheme_count) ? edit->graphemes[i + 1] : edit->buffer_len);
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
static const int inline_stringlist_count(inline_stringlist_t *list) {
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
static char *inline_currentsuggestion(inline_editor *edit) {
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

    edit->span_count = 0;
    edit->rawmode_enabled = false;
}

/* **********************************************************************
 * Rendering
 * ********************************************************************** */

/** Redraw the line */
static void inline_redraw(inline_editor *edit) {
    write(STDOUT_FILENO, "\r", 1); // Move cursor to start of line

    size_t prompt_len = strlen(edit->prompt); // Write prompt
    write(STDOUT_FILENO, edit->prompt, (unsigned int) prompt_len);

    // Compute selection bounds, if a selection is active
    int sel_l = INLINE_INVALID, sel_r = INLINE_INVALID;
    if (edit->selection_posn != INLINE_INVALID) {
        sel_l = imin(edit->selection_posn, edit->cursor_posn);
        sel_r = imax(edit->selection_posn, edit->cursor_posn);
    }

    for (int i = 0; i < edit->grapheme_count; i++) {
        if (i == sel_l) write(STDOUT_FILENO, "\x1b[7m", 4); // Turn on inverse video
        if (i == sel_r) write(STDOUT_FILENO, "\x1b[0m", 4); // Turn off inverse video 

        // Write grapheme
        size_t start, end;
        inline_graphemerange(edit, i, &start, &end);
        write(STDOUT_FILENO, edit->buffer + start, (unsigned int) end - start);
    }

    // If selection extends to end, ensure attributes reset
    if (sel_l != INLINE_INVALID && sel_r == edit->grapheme_count)
        write(STDOUT_FILENO, "\x1b[0m", 4);

    // Ghosted suggestion suffix 
    const char *suffix = inline_currentsuggestion(edit);
    if (suffix && *suffix) { // Ensure suggestion is present and has nonzero length
        write(STDOUT_FILENO, "\x1b[2m", 4);              // faint
        write(STDOUT_FILENO, suffix, strlen(suffix));    // ghost text
        write(STDOUT_FILENO, "\x1b[0m", 4);              // reset
    }

    // Clear to end of line (in case previous render was longer)
    write(STDOUT_FILENO, "\x1b[K", 3);

    // Move cursor to correct position
    size_t col = prompt_len;
    for (int i = 0; i < edit->cursor_posn; i++) {
        col += 1; // minimal version: assume width=1
    }

    // Move cursor to column `col`
    char seq[32];
    int n = snprintf(seq, sizeof(seq), "\r\x1b[%zuC", col);
    write(STDOUT_FILENO, seq, n);
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

#define INLINE_ESCAPECODE_MAXLENGTH 16
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

    int inserted_count = edit->grapheme_count - old_count; 
    edit->cursor_posn += (inserted_count > 0? inserted_count : 0); // Move cursor forward by number of graphemes
    if (edit->cursor_posn > edit->grapheme_count) edit->cursor_posn = edit->grapheme_count;

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

    edit->cursor_posn = sel_l; // Cursor moves to start of deleted region
    edit->selection_posn = INLINE_INVALID; // Clear selection
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
        edit->cursor_posn -= 1;
    } else inline_deletecurrent(edit);
}

/** Clear the buffer */
static void inline_clear(inline_editor *edit) {
    edit->buffer_len = 0; // Clear text buffer 
    edit->buffer[0] = '\0';
    edit->grapheme_count = 0; // Reset graphemes
    edit->cursor_posn = 0; // Reset cursor
    edit->refresh = true;
}

/** Navigation keys */
static void inline_home(inline_editor *edit) {
    if (edit->cursor_posn != 0) {
        edit->cursor_posn = 0;
        edit->refresh = true;
    }
}

static void inline_end(inline_editor *edit) {
    if (edit->cursor_posn != edit->grapheme_count) {
        edit->cursor_posn = edit->grapheme_count;
        edit->refresh = true;
    }
}

static void inline_left(inline_editor *edit) {
    if (edit->cursor_posn > 0) {
        edit->cursor_posn -= 1;
        edit->refresh = true;
    }
}

static void inline_right(inline_editor *edit) {
    if (edit->cursor_posn < edit->grapheme_count) {
        edit->cursor_posn += 1;
        edit->refresh = true;
    }
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
    const char *s = inline_currentsuggestion(edit);
    if (!s) return;

    inline_insert(edit, s, strlen(s));
    inline_clearsuggestions(edit);
}

/** History */
static void inline_historyprev(inline_editor *edit) {
}

static void inline_historynext(inline_editor *edit) {
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
        case KEY_RETURN: return false; 
        case KEY_LEFT:   inline_left(edit); break;
        case KEY_RIGHT:  
            if (inline_havesuggestions(edit)) {
                const char *suffix = inline_currentsuggestion(edit);
                if (suffix && *suffix) inline_insert(edit, suffix, strlen(suffix));

                inline_clearsuggestions(edit);
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
            endbrowsing=false;
            break;
        case KEY_DOWN:
            inline_advancehistory(edit, 1);
            endbrowsing=false;
            break;
        case KEY_HOME:   inline_home(edit);        break;
        case KEY_END:    inline_end(edit);         break;
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
    if (!inline_enablerawmode(edit)) return;  // Could not enter raw mode 
    inline_updateterminalwidth(edit);
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
    //write(STDOUT_FILENO, "\r\n", 2);
}

/** Public interface to the line editor.
 *  @param   edit - a line editor that has been created with inline_new.
 *  @returns the string input by the user. */
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
