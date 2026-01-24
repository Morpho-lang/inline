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

#ifdef _WIN32
typedef DWORD termstate_t;
#else
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
    size_t buffer_len;                    // Length of contents
    size_t buffer_size;                   // Size of buffer allocated

    size_t *graphemes;                    // Offset to each grapheme
    int grapheme_count;                   // Number of graphemes
    
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

    termstate_t termstate;                // Preserve terminal state 
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
#ifdef _WIN32
    return _isatty(_fileno(stdin)) && _isatty(_fileno(stdout));
#else
    return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
#endif
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

/** Exit handler called on crashes */
static void inline_emergencyrestore(void) {
    if (inline_lasteditor) inline_disablerawmode(inline_lasteditor);
}

#ifdef _WIN32
static BOOL WINAPI inline_consolehandler(DWORD ctrl) {
    inline_emergencyrestore();
    return FALSE; // allow default behavior 
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
bool inline_enablerawmode(inline_editor *edit) {
    if (edit->rawmode_enabled) return true;

#ifdef _WIN32
    HANDLE hConsole = GetStdHandle(STD_INPUT_HANDLE);
    if (!GetConsoleMode(hConsole, &edit->termstate)) return false; 
    if (!SetConsoleMode(hConsole, (edit->termstate & ~(ENABLE_LINE_INPUT |
                                      ENABLE_ECHO_INPUT | 
                                      ENABLE_PROCESSED_INPUT) |
                                      ENABLE_VIRTUAL_TERMINAL_INPUT ))) return false;

    SetConsoleOutputCP(CP_UTF8); // Enable utf8 input/output
    SetConsoleCP(CP_UTF8);
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
void inline_disablerawmode(inline_editor *edit) {
    if (!edit || !edit->rawmode_enabled) return;

#ifdef _WIN32
    HANDLE hConsole = GetStdHandle(STD_INPUT_HANDLE);
    SetConsoleMode(hConsole, edit->termstate);
#else
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

/* **********************************************************************
 * Rendering
 * ********************************************************************** */

/* **********************************************************************
 * Keypress decoding
 * ********************************************************************** */

/* ----------------------------------------
 * Raw input layer
 * ---------------------------------------- */

/** Type that represents a single unit of input */
#ifdef _WIN32
typedef INPUT_RECORD rawinput_t;
#else 
typedef unsigned char rawinput_t;
#endif

/** Await a single raw unit of input and store in a rawinput_t */
bool inline_readraw(rawinput_t *out) {
#ifdef _WIN32
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    DWORD count = 0;

    while (true) {
        if (!ReadConsoleInputW(hIn, out, 1, &count)) return false;

        switch (out->EventType) {
            case KEY_EVENT:
                if (out->Event.KeyEvent.bKeyDown) return true;
                break;

            case WINDOW_BUFFER_SIZE_EVENT:
                if (inline_lasteditor) inline_lasteditor->refresh = true;
                break;

            default: // Ignore mouse, menu, focus events
                break;
        }
    }
#else
    ssize_t n = read(STDIN_FILENO, out, 1);
    return n == 1;
#endif
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

void linedit_keypressunknown(keypress_t *keypress) {
    keypress->type=KEY_UNKNOWN; 
    keypress->c[0]='\0'; 
    keypress->nbytes=0; 
}

void linedit_keypresswithchar(keypress_t *keypress, keytype_t type, char c) {
    keypress->type=type; 
    keypress->c[0]=c; keypress->c[1]='\0';
    keypress->nbytes=1; 
}

#ifdef _WIN32
/** Windows version: */

/** Map from windows codes to keytype_t  */
typedef struct {
    WORD vk;          /* Virtual-key code */
    keytype_t type;   /* Our internal key type */
} winmap_t;

static const winmap_t win_table[] = {
    { VK_LEFT,      KEY_LEFT },
    { VK_RIGHT,     KEY_RIGHT },
    { VK_UP,        KEY_UP },
    { VK_DOWN,      KEY_DOWN },
    { VK_HOME,      KEY_HOME },
    { VK_END,       KEY_END },
    { VK_PRIOR,     KEY_PAGE_UP },
    { VK_NEXT,      KEY_PAGE_DOWN },
    { VK_TAB,       KEY_TAB },
    { VK_RETURN,    KEY_RETURN },
    { VK_BACK,      KEY_DELETE }
};

/** Decode raw input units into a keypress */
static void inline_decode(const rawinput_t *raw, keypress_t *out) {
    linedit_keypressunknown(out);

    const KEY_EVENT_RECORD *ev = &raw->Event.KeyEvent;
    if (!ev->bKeyDown) return; // Ignore key-up events 

    WCHAR wc = ev->uChar.UnicodeChar;

    // 1. Unicode characters
    if (wc >= 32 && wc != 127) {
        int n = WideCharToMultiByte(CP_UTF8, 0, &wc, 1, out->c, sizeof(out->c), NULL, NULL);
        if (n > 0) {
            out->c[n] = '\0';
            out->nbytes = n;
            out->type = KEY_CHARACTER;
        }
        return;
    }

    // 2. "Virtual" keys incl. arrow keys, home, end, etc. 
    for (size_t i = 0; i < sizeof(win_table)/sizeof(win_table[0]); i++) {
        if (win_table[i].vk == ev->wVirtualKeyCode) {
            out->type = win_table[i].type;
            return;
        }
    }

    // 3. Shift+Arrows 
    if (ev->dwControlKeyState & SHIFT_PRESSED) {
        switch (ev->wVirtualKeyCode) {
            case VK_LEFT:
                out->type = KEY_SHIFT_LEFT;
                return;
            case VK_RIGHT:
                out->type = KEY_SHIFT_RIGHT;
                return;
        }
    }

    // 4. Ctrl+A → Ctrl+Z 
    if (ev->dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
        WCHAR c = ev->uChar.UnicodeChar;
        if (c >= L'a' && c <= L'z') c -= 32; // Normalize to upper case
        if (c >= L'A' && c <= L'Z') linedit_keypresswithchar(out, KEY_CTRL, (char) c);
    }
}

#else 
/** POSIX version:  */

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

/** Determine length of utf8 character from the first byte */
static inline int inline_utf8length(unsigned char b) {
    if ((b & 0x80) == 0x00) return 1;      // 0xxxxxxx
    if ((b & 0xE0) == 0xC0) return 2;      // 110xxxxx
    if ((b & 0xF0) == 0xE0) return 3;      // 1110xxxx
    if ((b & 0xF8) == 0xF0) return 4;      // 11110xxx
    return 0;                              // Invalid or continuation
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
    linedit_keypressunknown(out); // Initially UNKNOWN
    unsigned char b = *raw;

    // 1. Control keys (ASCII control range or DEL)
    if (b < 32 || b == DELETE_CODE) {
        switch (b) {
            case TAB_CODE:    out->type = KEY_TAB; return;
            case RETURN_CODE: out->type = KEY_RETURN; return;
            case BACKSPACE_CODE: // v fallthrough
            case DELETE_CODE: out->type = KEY_DELETE; return;
            case ESC_CODE:
                inline_decode_escape(out);
                return;

            default: // Control codes are Ctrl+A → 1, Ctrl+Z → 26 
                if (b >= 1 && b <= 26) linedit_keypresswithchar(out, KEY_CTRL, 'A' + (b - 1));
                return;
        }
    }

    // 2. ASCII regular characters
    if (b < 128) {
        linedit_keypresswithchar(out, KEY_CHARACTER, b);
        return;
    }

    // 3. UTF‑8 multi‑byte 
    inline_decode_utf8(b, out);
}
#endif // _WIN32 / POSIX decoder

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

/*
bool inline_processkeypress(inline_editor *edit) {
    key_event key;
    bool regen_suggestions = true;

    inline_keyinit(&key);

    do {
        if (!inline_readkey(edit, &key))
            continue;

        switch (key.type) {

        case KEY_CHARACTER:
            inline_handle_character(edit, &key);
            break;

        case KEY_DELETE:
            inline_handle_delete(edit);
            break;

        case KEY_LEFT:
        case KEY_RIGHT:
        case KEY_SHIFT_LEFT:
        case KEY_SHIFT_RIGHT:
            inline_handle_arrow(edit, key.type);
            break;

        case KEY_UP:
        case KEY_DOWN:
            inline_handle_vertical(edit, key.type, &regen_suggestions);
            break;

        case KEY_RETURN:
            if (!inline_handle_return(edit))
                return false;
            break;

        case KEY_TAB:
            inline_handle_tab(edit, &regen_suggestions);
            break;

        case KEY_CTRL:
            if (!inline_handle_ctrl(edit, &key))
                return false;
            break;

        default:
            break;
        }

    } while (inline_keypressavailable());

    if (regen_suggestions)
        inline_generatesuggestions(edit);

    return true;
}*/

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
    DWORD mode;
    BOOL ok = GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode);
    printf("GetConsoleMode = %d\n", ok);

    if (!inline_enablerawmode(edit)) return;  // Could not enter raw mode 

    DWORD type = GetFileType(GetStdHandle(STD_INPUT_HANDLE));
    printf("FileType = %lu\n", type);

    inline_updateterminalwidth(edit);

    while (true) {
        keypress_t k;
        if (!inline_readkeypress(edit, &k))
            continue;

        if (k.type == KEY_CTRL && k.c[0] == 'C') {
            printf("Ctrl-C detected, exiting.\n");
            break;
        }

        printf("type=%d", k.type);

        if (k.type == KEY_CHARACTER || k.type == KEY_CTRL) {
            printf(" char=\"%s\" bytes=%d", k.c, k.nbytes);
        }

        printf("\n");
    }

    //edit->cursor_grapheme = 0;   /* Start at beginning */
    //edit->cursor_byte     = 0;

    /*inline_redraw(edit);

    for (;;) {
        int key = inline_read_keypress();
        if (!inline_process_keypress(edit, key)) break; // User pressed Enter, Ctrl-D, etc. 

        inline_redraw(edit);
    }*/

    inline_disablerawmode(edit);

    /* Add to history if non-empty */
    // if (edit->buffer_len > 0) inline_history_add(edit, edit->buffer);

    //inline_write_newline();
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
