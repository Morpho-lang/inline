/** @file inline.h
 *  @author T J Atherton
 *
 *  @brief A simple UTF8 aware line editor with history, completion, multiline editing and syntax highlighting */

#ifndef INLINE_H
#define INLINE_H

#include <stdbool.h>
#include <stddef.h>

/* Forward declaration of the line editor structure */
typedef struct inline_editor inline_editor;

/* **********************************************************************
 * Callback functions
 * ********************************************************************** */

/* -----------------------
 * Autocomplete
 * ----------------------- */

/** @brief Autocomplete callback function.
 *
 *  Called repeatedly by the line editor to obtain completion
 *  suggestions for the given prefix.
 *  @param[in] prefix   The current word prefix to complete.
 *  @param[in] ref      User-supplied reference pointer.
 *  @param[in] index    Zero-based index of the suggestion to return.
 *
 *  @returns A UTF-8 string containing the completion suggestion,
 *           or NULL if no more suggestions exist.
 *
 *  @note The returned string is owned by the callback; inline copies the
 *        string immediately. */
typedef char *(*inline_completefn)(const char *prefix, void *ref, int index);

/* -----------------------
 * Syntax coloring
 * ----------------------- */

/** @brief A single colored span of text. */
typedef struct {
    size_t byte_start; /* inclusive start of color span */
    size_t byte_end; /* exclusive end of color span */
    int color;     /* Index into color palette */
} inline_color_span;

/** @brief Syntax coloring callback function, called repeatedly by the editor
 *         to obtain the next colored span.
 *  @param[in]  utf8    The full buffer encoded as UTF-8 to analyze.
 *  @param[in]  ref     User-supplied reference pointer.
 *  @param[in]  offset  Byte offset at which to begin scanning.
 *  @param[out] out     Filled with the next colored span, if any.
 *
 *  @returns true if a span was found, false if no more spans exist. */
typedef bool (*inline_syntaxcolorfn)(const char *utf8, void *ref, size_t offset, inline_color_span *out);

/* -----------------------
 * Multiline editing
 * ----------------------- */

/** @brief Multiline callback function
 * Called when inline wants to know whether it should enter multiline mode. 
 * The callback should parse the input and return true if inline should go to 
 * multiline mode or false otherwise.
 *  @param[in] utf8   The full buffer encoded as UTF-8.
 *  @param[in] ref    User-supplied reference pointer.
 *
 *  @returns true if more lines are required, false otherwise. */
typedef bool (*inline_multilinefn)(const char *utf8, void *ref);

/* **********************************************************************
 * Public API
 * ********************************************************************** */

/** @brief Create a new line editor.
 *  @param[in] prompt   The prompt string to display.
 *  @returns A newly allocated line editor.*/
inline_editor *inline_new(const char *prompt);

/** @brief Free a line editor and all associated resources.
 *  @param[in] edit   Line editor to free. */
void inline_free(inline_editor *edit);

/** @brief Read a line of input from the terminal.
 *  @param[in] edit   Line editor to use.
 *  @returns A malloc'd UTF-8 string containing the user's input,
 *           or NULL on EOF. Caller is responsible for freeing the returned string.
 */
char *inline_readline(inline_editor *edit);

/** @brief Enable syntax coloring.
 *  @param[in] edit   Line editor to configure.
 *  @param[in] fn     Syntax coloring callback.
 *  @param[in] ref    User-supplied reference pointer. */
void inline_syntaxcolor(inline_editor *edit, inline_syntaxcolorfn fn, void *ref);

/** @brief Set the color palette used for syntax highlighting.
 *
 *  Color indices returned by a inline_syntaxcolorfn are mapped
 *  through this palette to a final color value. The palette is copied by 
 *  the inline_editor. Color values are interpreted:
 *
 *      -1            → default color
 *       0–7          → ANSI basic colors
 *       8–255        → 256-color palette
 *       >=0x01000000 → RGB packed as 0x01RRGGBB
 *
 *  @param[in] edit     Line editor to configure.
 *  @param[in] count    Number of entries in the palette.
 *  @param[in] palette  Array mapping semantic color indices to color ints.
 */
void inline_setpalette(inline_editor *edit, int count, const int *palette);

/** @brief Enable autocomplete.
 *  @param[in] edit   Line editor to configure.
 *  @param[in] fn     Completion callback.
 *  @param[in] ref    User-supplied reference pointer. */
void inline_autocomplete(inline_editor *edit, inline_completefn fn, void *ref);

/** @brief Enable multiline editing.
 *  @param[in] edit                 Line editor to configure.
 *  @param[in] fn                   Multiline callback.
 *  @param[in] ref                  User-supplied reference pointer.
 *  @param[in] continuation_prompt  Prompt to use for continuation lines. */
void inline_multiline(inline_editor *edit, inline_multilinefn fn, void *ref, const char *continuation_prompt);

/** @brief Display a UTF-8 string using syntax coloring.
 *  @param[in] edit     Line editor to use.
 *  @param[in] string   UTF-8 string to display.*/
void inline_displaywithsyntaxcoloring(inline_editor *edit, const char *string);

/** @brief Check whether stdin and stdout are TTYs.
 *  @returns true if both stdin and stdout are terminals. */
bool inline_checktty(void);

#endif /* INLINE_H */
