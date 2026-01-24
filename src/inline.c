/** @file inline.c
 *  @author T J Atherton
 *
 *  @brief A simple UTF8 aware line editor with history, completion, multiline editing and syntax highlighting */

#include "inline.h"
#include <stdlib.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <conio.h>
#define read _read
#define isatty _isatty
#define STDIN_FILENO _fileno(stdin)
#define STDOUT_FILENO _fileno(stdout)
#else
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#endif

#define INLINE_DEFAULT_BUFFER_SIZE 128
#define INLINE_DEFAULT_PROMPT ">"

// Forward declarations
static char *inline_strdup(const char *s); 
void inline_disablerawmode(inline_editor *edit);

#ifndef _WIN32
typedef struct termios termstate_t;
#endif

/* **********************************************************************
 * Line editor data structure and configuration
 * ********************************************************************** */

typedef struct inline_editor {
    char *prompt; 
    char *continuation_prompt; 

    int ncols;                            // Number of columns

    char *buffer;                         // Buffer holding UTF8
    size_t buffer_len;                    // Length of contents in bytes
    size_t buffer_size;                   // Size of buffer allocated in bytes

    size_t *graphemes;                    // Offset to each grapheme
    int grapheme_count;                   // Number of graphemes
    size_t grapheme_size;                 // Size of grapheme buffer in bytes
    
    int cursor_posn;                      // Position of cursor in graphemes

    inline_syntaxcolorfn syntax_fn;       // Syntax coloring callback
    void *syntax_ref;                     // User reference

    inline_color_span *spans;             // Array of color spans
    int span_count;                       // Number of spans
    int span_capacity;                    // Capacity of span array 

    int *palette;                         // Palette: list of colors
    int palette_count;                    // Length of palette list

    inline_completefn complete_fn;        // Autocomplete callback
    void *complete_ref;                   // User reference 

    inline_multilinefn multiline_fn;      // Multiline callback
    void *multiline_ref;                  // User reference
#ifndef _WIN32
    termstate_t termstate;                // Preserve terminal state 
#endif 
    bool rawmode_enabled;                 // Record if rawmode has already been enabled

    int cursor_col;                       // current cursor column on screen
    int render_cols;                      // width of last render

    bool refresh;                         // Set to refresh on next redraw
} inline_editor; 

static inline_editor *inline_lasteditor = NULL;

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

    return editor;

inline_new_cleanup:
    inline_free(editor);
    return NULL; 
}

/** Free a line editor and associated resources */
void inline_free(inline_editor *editor) {
    if (!editor) return;

    free(editor->prompt);
    free(editor->continuation_prompt);

    free(editor->buffer);
    free(editor->graphemes);

    free(editor->spans);
    free(editor->palette);

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

/** Update the terminal width */
void inline_updateterminalwidth(inline_editor *edit) {
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

#ifndef _WIN32
/** Exit handler called on crashes */
static void inline_emergencyrestore(void) {
    if (inline_lasteditor) inline_disablerawmode(inline_lasteditor);
}

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
#ifndef _WIN32 
static bool registered = false; 
    if (registered) return; 
    registered=true;
    atexit(inline_emergencyrestore);

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
bool inline_enablerawmode(inline_editor *edit) {
    if (edit->rawmode_enabled) return true;

#ifndef _WIN32 // Note modern windows with ConPTY is in raw mode by default. 
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
void inline_disablerawmode(inline_editor *edit) {
    if (!edit || !edit->rawmode_enabled) return;

#ifndef _WIN32
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &edit->termstate);
#endif

    fputs("\r", stdout); // Print a carriage return to ensure we're back on the left hand side
    edit->rawmode_enabled = false;
}

/* **********************************************************************
 * Utility functions
 * ********************************************************************** */

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
bool inline_extendbufferby(inline_editor *edit, size_t extra) {
    size_t required = edit->buffer_len + extra + 1;  // +1 for null terminator

    if (required <= edit->buffer_size) return true;  // Sufficient space already

    size_t newcap = edit->buffer_size ? edit->buffer_size : INLINE_DEFAULT_BUFFER_SIZE;
    while (newcap < required) newcap *= 2; // Grow exponentially

    edit->buffer = realloc(edit->buffer, newcap);
    if (!edit->buffer) return false;  
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
void inline_recomputegraphemes(inline_editor *edit) {
    int needed = (int)edit->buffer_len; // Assume 1 byte per character as a worst case. 

    size_t required_bytes = needed * sizeof(size_t); // Ensure capacity
    if (required_bytes > edit->grapheme_size) {
        size_t newsize = edit->grapheme_size ? edit->grapheme_size : INLINE_DEFAULT_BUFFER_SIZE;
        while (newsize < required_bytes) newsize *= 2;

        size_t *new = realloc(edit->graphemes, newsize);
        if (!new) {
            edit->graphemes = NULL;
            edit->grapheme_count = 0;
            edit->grapheme_size = 0;
            return;
        }

        edit->graphemes = new;
        edit->grapheme_size = newsize;
    }

    // Walk the buffer and record codepoint boundaries  
    int count = 0;
    for (size_t i = 0; i < edit->buffer_len; ) {
        edit->graphemes[count++] = i;
        i += inline_utf8length((unsigned char)edit->buffer[i]);
    }

    edit->grapheme_count = needed;
}

/* **********************************************************************
 * Rendering
 * ********************************************************************** */

void inline_redraw(inline_editor *edit) {
    // 1. Move cursor to start of line
    write(STDOUT_FILENO, "\r", 1);

    // 2. Write prompt
    int prompt_len = strlen(edit->prompt);
    write(STDOUT_FILENO, edit->prompt, prompt_len);

    // 3. Write buffer
    write(STDOUT_FILENO, edit->buffer, edit->buffer_len);

    // 4. Clear to end of line (in case previous render was longer)
    write(STDOUT_FILENO, "\x1b[K", 3);

    // 5. Move cursor to correct position
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
bool inline_readraw(rawinput_t *out) {
    size_t n = read(STDIN_FILENO, out, 1);
    return n == 1;
}

/* ----------------------------------------
 * Keypress decoding layer
 * ---------------------------------------- */

/** Identifies the type of keypress */
typedef enum {
    KEY_UNKNOWN, KEY_CHARACTER,
    KEY_RETURN, KEY_TAB, KEY_DELETE,
    KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT,   // Arrow keys
    KEY_HOME, KEY_END,               // Home and End
    KEY_PAGE_UP, KEY_PAGE_DOWN,      // Page up and page down
    KEY_SHIFT_LEFT, KEY_SHIFT_RIGHT, // Shift+arrow key
    KEY_CTRL
} keytype_t;

/** A single keypress event obtained and processed by the terminal */
typedef struct {
    keytype_t type; /** Type of keypress */
    char c[5]; /** Up to four bytes of utf8 encoded unicode plus null terminator */
    int nbytes; /** Number of bytes */
} keypress_t;

void inline_keypressunknown(keypress_t *keypress) {
    keypress->type=KEY_UNKNOWN; 
    keypress->c[0]='\0'; 
    keypress->nbytes=0; 
}

void inline_keypresswithchar(keypress_t *keypress, keytype_t type, char c) {
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
        if (isalpha(seq[i])) break;
    }
    seq[i + 1] = '\0'; // Ensure null terminated

    // Lookup escape code 
    for (size_t j = 0; j < sizeof(esc_table)/sizeof(esc_table[0]); j++) {
        if (strcmp(seq, esc_table[j].seq) == 0) {
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
bool inline_readkeypress(inline_editor *edit, keypress_t *out) {
    rawinput_t raw;
    if (!inline_readraw(&raw)) return false;
    inline_decode(&raw, out);
    return true; 
}

/* **********************************************************************
 * Input loop
 * ********************************************************************** */

/** Insert text into the buffer */
bool inline_insert(inline_editor *edit, const char *bytes, size_t nbytes) {
    if (!inline_extendbufferby(edit, nbytes)) return false; // Ensure capacity 

    size_t offset = 0; // Obtain the byte offset of the current cursor position
    if (edit->cursor_posn < edit->grapheme_count) offset = edit->graphemes[edit->cursor_posn];
    else offset = edit->buffer_len; 

    // Move contents after the insertion point to make room for the inserted dat
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

void inline_delete(inline_editor *edit) {
    if (edit->cursor_posn >= edit->grapheme_count) return; // End of line, so nothing to do

    // Byte offset of the grapheme to delete
    size_t start = edit->graphemes[edit->cursor_posn];

    // Byte offset of the next grapheme (or end of buffer)
    size_t end;
    if (edit->cursor_posn + 1 < edit->grapheme_count) end = edit->graphemes[edit->cursor_posn + 1];
    else end = edit->buffer_len;

    size_t bytes_to_delete = end - start;

    // Shift the remaining bytes left
    memmove(edit->buffer + start, edit->buffer + end, edit->buffer_len - end);

    edit->buffer_len -= bytes_to_delete;
    edit->buffer[edit->buffer_len] = '\0';

    inline_recomputegraphemes(edit);
    edit->refresh = true;
}

void inline_home(inline_editor *edit) {
    if (edit->cursor_posn != 0) {
        edit->cursor_posn = 0;
        edit->refresh = true;
    }
}

void inline_end(inline_editor *edit) {
    if (edit->cursor_posn != edit->grapheme_count) {
        edit->cursor_posn = edit->grapheme_count;
        edit->refresh = true;
    }
}

void inline_left(inline_editor *edit) {
    if (edit->cursor_posn > 0) {
        edit->cursor_posn -= 1;
        edit->refresh = true;
    }
}

void inline_right(inline_editor *edit) {
    if (edit->cursor_posn < edit->grapheme_count) {
        edit->cursor_posn += 1;
        edit->refresh = true;
    }
}

void inline_historyprev(inline_editor *edit) {
}

void inline_historynext(inline_editor *edit) {
}

bool inline_processshortcut(inline_editor *edit, char c) {
    switch (c) {
        case 'A': inline_home(edit); break;
        case 'E': inline_end(edit); break;
        case 'B': inline_left(edit); break;
        case 'F': inline_right(edit); break;
        /*case 'K': inline_kill_to_end(edit); break;
        case 'U': inline_kill_to_start(edit); break;
        case 'W': inline_delete_prev_word(edit); break;*/
        case 'C': return false; // exit on Ctrl-C
        default: break;
    }
    edit->refresh = true;
    return true;
}

bool inline_processkeypress(inline_editor *edit, const keypress_t *key) {
    switch (key->type) {
        case KEY_RETURN: return false; 
        case KEY_LEFT:   inline_left(edit);         break;
        case KEY_RIGHT:  inline_right(edit);        break;
        case KEY_UP:     inline_historyprev(edit);  break;
        case KEY_DOWN:   inline_historynext(edit);  break;
        case KEY_HOME:   inline_home(edit);         break;
        case KEY_END:    inline_end(edit);          break;
        case KEY_DELETE: inline_delete(edit);       break;
        case KEY_CTRL:   
            return inline_processshortcut(edit, key->c[0]);
        case KEY_CHARACTER: 
            if (!inline_insert(edit, key->c, key->nbytes)) return false; 
            break;
        default:
            break;
    }

    edit->refresh = true;
    return true;
}

/* **********************************************************************
 * Interface
 * ********************************************************************** */

/** If we're not attached to a terminal, e.g. a pipe, simply read the file in. */
void inline_noterminal(inline_editor *edit) {
    int c;

    while ((c = fgetc(stdin)) != EOF && c != '\n') {
        if (!inline_extendbufferby(edit, 1)) break; // Buffer could not be extended
        edit->buffer[edit->buffer_len++] = (char)c;
    }

    edit->buffer[edit->buffer_len] = '\0'; // Ensure null termination
}

/** If the terminal is unsupported, display a prompt and read the line normally. */
void inline_unsupported(inline_editor *edit) {
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
void inline_supported(inline_editor *edit) {
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

    /* Add to history if non-empty */
    // if (edit->buffer_len > 0) inline_history_add(edit, edit->buffer);

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

    return edit->buffer;
}
