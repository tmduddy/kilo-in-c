/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*** defines ***/

// Required to allow getline() to succeed cross-platform.
// While it succeeded for me on MacOS Sequoia during development,
// I'm not sure that's really cross platform without these additional
// definitions.
#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

// Bitwise AND of the input key (in ASCII) with 0001 1111
// to cast the first 3 bits to 0, which is how ASCII maps
// characters and their CTRL+<character> variants.
// q in ASCII     = 113 = 0111 0001
// <c-q> in ASCII =  17 = 0001 0001
// q & 0x1f = 0001 0001 = <c-q>
#define CTRL_KEY(key) ((key) & 0x1f)

#define KILO_VERSION "0.0.1"

#define KILO_TAB_STOP 8

#define KILO_QUIT_TIMES 3

// Enum to map ints to key names.
// These values are outside of the standard char range to avoid conflicts
enum editorKey {
  BACKSPACE = 127,
  ARROW_LEFT = 1000,
  ARROW_RIGHT,
  ARROW_UP,
  ARROW_DOWN,
  DEL_KEY,
  HOME_KEY,
  END_KEY,
  PAGE_UP,
  PAGE_DOWN
};

/*
 * Store the possible categories of text to highlight.
 * KW1/KW2 allow for different highlighting for different types of keywords,
 * for example reserved keywords vs. common types in C
 */
enum editorHighlight {
  HL_NORMAL = 0,
  HL_COMMENT,
  HL_KEYWORD1,
  HL_KEYWORD2,
  HL_STRING,
  HL_NUMBER,
  HL_MATCH
};

#define HL_HIGHLIGHT_NUMBERS (1 << 0)
#define HL_HIGHLIGHT_STRINGS (1 << 1)

/*** data ***/

/*
 * Hold data related to syntax highlighting.
 * - filetype is the name of the filetype to display to the user.
 * - filematch is an array of strings, where each string contains a pattern to
 *   match filenames against, where a match decides which filetype to use.
 * - singleline_comment_start holds the characters that denote the beginning
 *   of a comment
 * - keywords is a NULL terminated array of strings containing all keywords
 *   to highlight. Keyword1 vs. Keyword2 HLs will be distinguished using a |
 *   ex: ["switch", "if", "int|", "long|"] will categorize switch and if as
 *   KW1s and int and long as KW2s.
 * - flags is a bit field that will contain flags for whether to highlight
 *   numbers and whether to highlight strings for that filetype.
 */
struct editorSyntax {
  char *filetype;
  char **filematch;
  char **keywords;
  char *singleline_comment_start;
  int flags;
};

/*
 * hold a single row of editor text
 */
typedef struct erow {
  int size;
  int rsize;
  char *chars;
  char *render;
  unsigned char *hl;
} erow;

struct editorConfig {
  int cx;
  int cy;
  int rx;
  int rowoff;
  int coloff;
  int screenrows;
  int screencols;
  int numrows;
  erow *row;
  int dirty;
  char *filename;
  char statusmsg[80];
  time_t statusmsg_time;
  struct editorSyntax *syntax;
  struct termios orig_termios;
};

struct editorConfig E;

char *C_HL_extensions[] = {".c", ".h", ".cpp", NULL};
char *CL_HL_keywords[] = {"switch",    "if",      "while",   "for",    "break",
                          "continue",  "return",  "else",    "struct", "union",
                          "typedef",   "static",  "enum",    "class",  "case",
                          "int|",      "long|",   "double|", "float|", "char|",
                          "unsigned|", "signed|", "void|",   NULL};

// The Highlight Database maps file extensions to filetype names and rules.
struct editorSyntax HLDB[] = {
    {"c", C_HL_extensions, CL_HL_keywords, "//",
     HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS},
};

// Store the length of the HLDB array.
#define HLDB_ENTRIES (sizeof(HLDB) / sizeof(HLDB[0]))

/*** prototypes ***/

// This prototype concept seems smelly to me, but maybe it's a C thing.
// These allow functions to be declared/shaped now, but not defined until later
// allowing them to be used before definition when compiling.

void editorSetStatusMessage(const char *fmt, ...);
void editorRefreshScreen(void);
char *editorPrompt(char *prompt, void (*callback)(char *, int));

/*** terminal ***/

/*
 * Standard error handling exit
 */
void die(const char *s) {
  // Clear the screen.
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  // Print the error.
  perror(s);
  exit(1);
}

/*
 * Reset all terminal configs to their original state.
 */
void disableRawMode(void) {
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1) {
    die("tcsetattr");
  }
}

/*
 * Edit terminal settings to enable "raw" mode,
 * where raw mode means inputs are "submitted" after every
 * character, not when the user hits enter.
 */
void enableRawMode(void) {
  // Store all initial terminal configs in global struct orig_termios.
  if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) {
    die("tcgetattr");
  }

  // Ensure that disableRawMode is always called at program exit.
  atexit(disableRawMode);

  // Create a struct raw to hold updated terminal config.
  struct termios raw = E.orig_termios;

  // Bitwise invert 'input' flags.
  // BRKINT (maybe optional) controls the handling of Break conditions
  //   Disabling it disables Break conditions sending SIGINT.
  //   (not sure what those are if not <c-c>, which we already handled)
  // ICRNL controls the conversion of carriage returns into new lines.
  //   Disabling it resets <c-m> and <enter> to
  //   ASCII 10 (\r) instead of ASCII 13 (\n).
  // INPCK (optional) controls "parity handling.
  //   The article indicates this isn't neccesary on modern terminal
  //   emulators, just a holdover of tradition.
  // ISTRIP strips the 8th bit of every byte (casts to 0)
  //   The article indicates this is surely already enabled on a modern term.
  // IXON controls the handling of XOFF/XON controls for output suppression.
  //   Disabling it overrides <c-s>/<c-q> and allows them to be read
  //   as ASCII.
  raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);

  // Bitwise invert 'output' flags.
  // OPOST controls the conversion of new lines (\n) into NL returns (\n\r).
  //   Disabling it allows/requires printing to manually specify new lines
  //   and returns separately.
  raw.c_oflag &= ~(OPOST);

  // Bitwise or to force enable character settings
  // CS8 is a bitmask that sets the Character Size (CS) to 8.
  //   The article indicates this is likely already set.
  raw.c_cflag |= (CS8);

  // Bitwise invert 'local' flags.
  // ECHO controls the output of characters.
  //   Disabling it suppresses terminal output while typing,
  //   like when entering a sudo password.
  // ICANON controls canonical mode vs raw mode.
  //   Disabling it handles inputs char by char instead of line by line.
  // ISIG controls the handling of signal inputs.
  //   Disabling it overrides <c-c>/<c-y>/<c-z> and allows them to be read
  //   as ASCII.
  // IEXTEN controls the handling of input buffering controls.
  //   Disabling it overrides <c-o>/<c-v> and allows them to be read
  //   as ASCII.
  raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);

  // cc are the control character handling settings.
  // VMIN sets the minimum # of bytes read() needs before returning.
  // VTIME sets the max wait time before read() returns (in 1/10 sec.).
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 1;

  // Persist the change:
  // TCSAFLUSH waits for all pending output to be written
  // to the terminal, and also discards any input that
  // hasn’t been read.
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) {
    die("tcsetattr");
  }
}

/*
 * Reads a single key press from STDIN, validates read success, and returns it.
 */
int editorReadKey(void) {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) {
      die("read");
    }
  }

  // If we read an <esc>, immediately read the next two bytes.
  if (c == '\x1b') {
    char seq[3];

    if (read(STDIN_FILENO, &seq[0], 1) != 1 ||
        read(STDIN_FILENO, &seq[1], 1) != 1) {
      // We didn't see another key fast enough, so just return the <esc>.
      return '\x1b';
    }

    // Map [A-D to our custom arrow keys
    // Map [5,6~ to Page Up and Page Down
    // Map [1,4,7,8 to Home and End
    // Map OH,OF to Home and End
    // Map [3 to DEL (not backspace)
    if (seq[0] == '[') {
      if (seq[1] >= '0' && seq[1] <= '9') {
        if (read(STDIN_FILENO, &seq[2], 1) != 1) {
          return '\x1b';
        }
        if (seq[2] == '~') {
          switch (seq[1]) {
          case '1':
            return HOME_KEY;
          case '3':
            return DEL_KEY;
          case '4':
            return END_KEY;
          case '5':
            return PAGE_UP;
          case '6':
            return PAGE_DOWN;
          case '7':
            return HOME_KEY;
          case '8':
            return END_KEY;
          }
        }
      } else {
        switch (seq[1]) {
        case 'A':
          return ARROW_UP;
        case 'B':
          return ARROW_DOWN;
        case 'C':
          return ARROW_RIGHT;
        case 'D':
          return ARROW_LEFT;
        case 'H':
          return HOME_KEY;
        case 'F':
          return END_KEY;
        }
      }
    } else if (seq[0] == 'O') {
      switch (seq[1]) {
      case 'H':
        return HOME_KEY;
      case 'F':
        return END_KEY;
      }
    }

    // for all other sequences just return the <esc>
    return '\x1b';
  }

  return c;
}

/*
 * Get the current cursor position
 * [n gets device information, and arg 6 returns the cursor position
 * and reports back in the form:
 *   \x1b[<row>;<col>R
 */
int getCursorPosition(int *rows, int *cols) {
  char buf[32];
  unsigned int i = 0;

  // Send the escape code to return the cursor position
  if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4) {
    return -1;
  }

  while (i < sizeof(buf) - 1) {
    if (read(STDIN_FILENO, &buf[i], 1) != 1) {
      // Break if we reach the end.
      break;
    }
    if (buf[i] == 'R') {
      // Break if we read an 'R'.
      break;
    }
    i++;
  }

  // printf expects strings to end with '\0' so we manually insert that
  buf[i] = '\0';

  // Verify that we're reading an escape sequence.
  if (buf[0] != '\x1b' || buf[1] != '[') {
    return -1;
  }

  if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) {
    return -1;
  }

  return 0;
}

/*
 * Get the current terminal window size .
 * [C sends the cursor forward (999 cols in this case)
 * [B sends the cursor down (998 rows in this case)
 */
int getWindowSize(int *rows, int *cols) {
  struct winsize ws;

  // Fetch the current window size from ioctl.
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    // If that fails to read, or returns a col of 0, fall back on
    // moving the cursor position to the bottom right corner and using
    // getCursorPosition to load the current position to rows + cols.
    if (write(STDOUT_FILENO, "\x1b[999C\x1b[998B", 12) != 12) {
      return -1;
    }
    return getCursorPosition(rows, cols);
  } else {
    // load the current column and row sizes into the input args
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

/*** syntax highlighting ***/

/*
 * Return whether or not a character is a separator for syntax highlighting.
 * strchr comes from <string.h> and returns a pointer to the first matching
 *   character in a string with args `strchr(searchArea, searchTerm)`.
 * strncmp(s1, s2, n) (from <string.h>) compares the first n characters of
 *   two strings and returns an integer indicating which one is greater:
 *   -1 for s1, 1 for s2, or 0 if the two exactly match.
 */
int is_separator(int c) {
  return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}
/*
 * Categorize the contents of a given row into syntax categories
 * for highlighting.
 */
void editorUpdateSyntax(erow *row) {
  // Allocate enough memory in hl to store the current row
  row->hl = realloc(row->hl, row->rsize);
  // copy the default highlighting category into the each memory
  // block for the row.
  memset(row->hl, HL_NORMAL, row->rsize);

  // Quit out if no syntax is defined.
  if (E.syntax == NULL)
    return;

  // Store all keywords to highlight from the current language config.
  char **keywords = E.syntax->keywords;

  // Store the comment prefix to look for from the current language config.
  char *scs = E.syntax->singleline_comment_start;
  int scs_len = scs ? strlen(scs) : 0;

  // Store whether the preceding character for a given string is a separator.
  int prev_sep = 1;

  // Store whether the character is currently inside of a string.
  int in_string = 0;

  // Iterate through the rendered characters in the row.
  // Using a while loop to allow for checking multiple characters at once.
  int i = 0;
  while (i < row->rsize) {
    char c = row->render[i];
    unsigned char prev_hl = (i > 0) ? row->hl[i - 1] : HL_NORMAL;

    // Highlight singleline comments
    if (scs_len && !in_string) {
      // If the next scs_len characters of row.render match the scs string:
      if (!strncmp(&row->render[i], scs, scs_len)) {
        // Set the entire single comment row length from i-> to HL_COMMENT
        memset(&row->hl[i], HL_COMMENT, row->rsize - i);
        break;
      }
    }

    // Highlight strings if enabled in E.syntax
    if (E.syntax->flags & HL_HIGHLIGHT_STRINGS) {
      if (in_string) {
        // If we're already in a string, just keep highlighting.
        // Set the HL for the current character to HL_STRING.
        row->hl[i] = HL_STRING;
        // Account for escaped quotes, which should not end the string.
        if (c == '\\' && i + 1 < row->rsize) {
          // Set the next char to HL_STRING
          row->hl[i + 1] = HL_STRING;
          // Skip over the next char, we already handled it.
          i += 2;
          continue;
        }
        // If c is a closing quotation (see else), stop HL.
        if (c == in_string)
          in_string = 0;
        i++;
        // Mark that the previous character was not a separator (because it was
        // a string).
        prev_sep = 1;
        continue;
      } else {
        // If we hit a closing quotation mark, stop highlighting after this one
        if (c == '"' || c == '\'') {
          in_string = c;
          row->hl[i] = HL_STRING;
          i++;
          continue;
        }
      }
    }

    // Highlight digits if enabled in E.syntax.
    if (E.syntax->flags & HL_HIGHLIGHT_NUMBERS) {
      // If the current character is a digit preceded by a separator or digit,
      // or the current character is a period preceded by a digit (decimal pt)
      if ((isdigit(c) && (prev_sep || prev_hl == HL_NUMBER)) ||
          (c == '.' && prev_hl == HL_NUMBER)) {
        // Assign HL_NUMBER to the character.
        row->hl[i] = HL_NUMBER;
        i++;
        // Mark that the previous character was not a separator (because it was
        // a digit).
        prev_sep = 0;
        continue;
      }
    }

    // Highlight Keywords (1 and 2)
    if (prev_sep) {
      // iterate through the array of Keywords to highlight.
      int j;
      for (j = 0; keywords[j]; j++) {
        // Store the length of a given keyword.
        int klen = strlen(keywords[j]);
        // check if the last character is a '|', which denotes that it is
        // a Keyword2. Otherwise, it's a Keyword1.
        int is_kw2 = keywords[j][klen - 1] == '|';
        // Strip the | off the end of any KW2s.
        if (is_kw2)
          klen--;

        // If the next klen characters of row.render match the current keyword,
        // and the character following the keyword chars is a separator:
        if (!strncmp(&row->render[i], keywords[j], klen) &&
            is_separator(row->render[i + klen])) {
          // Set the next klen chars to HL_KW1/KW2
          memset(&row->hl[i], is_kw2 ? HL_KEYWORD2 : HL_KEYWORD1, klen);
          // Jump forward to the end of the keyword.
          i += klen;
          break;
        }
      }
      if (keywords[j] != NULL) {
        prev_sep = 0;
        continue;
      }
    }

    // Check if the current character is a separator then continue iteration.
    prev_sep = is_separator(c);
    i++;
  }
}

/*
 * Assign a color (via ANSI code) to each syntax category.
 */
int editorSyntaxToColor(int hl) {
  switch (hl) {
  case HL_COMMENT:
    // Cyan
    return 36;
  case HL_KEYWORD1:
    // Green
    return 32;
  case HL_KEYWORD2:
    // Yellow
    return 33;
  case HL_STRING:
    // Magenta
    return 35;
  case HL_NUMBER:
    // Red
    return 31;
  case HL_MATCH:
    // Blue
    return 34;
  default:
    // White
    return 37;
  }
}

/*
 * Match the current filename to one of the filematch fields and set syntax
 * strrchr (from <string.h>) returns a pointer to the last occurrence of a
 *   character in a string.
 * strcmp (from <string.h>) returns 0 if 2 strings match.
 */
void editorSelectSyntaxHighlight(void) {
  // Start with an empty syntax pointer and exit early if no filename is set.
  E.syntax = NULL;
  if (E.filename == NULL)
    return;

  // Store a pointer to the last '.' in the file name to get the extension.
  char *ext = strrchr(E.filename, '.');

  // Iterate through every entry in the HLDB.
  for (unsigned int j = 0; j < HLDB_ENTRIES; j++) {
    // Pull out the syntax for the current DB element.
    struct editorSyntax *s = &HLDB[j];
    // Iterate through every file extension in the filematch array.
    unsigned int i = 0;
    while (s->filematch[i]) {
      // Validate that the array element has '.' (this is a shortcut to
      // avoid having to store the array size and works as long as the
      // filematch array always starts with file extensions and ends with NULL)
      int is_ext = (s->filematch[i][0] == '.');
      // If the filematch is an extentension, the current file name has
      // an extension, and the extensions match:
      // or
      // If this is the last element of the filematch array and the filematch
      // extension is found completely within the filename:
      if ((is_ext && ext && !strcmp(ext, s->filematch[i])) ||
          (!is_ext && strstr(E.filename, s->filematch[i]))) {
        // Set the syntax rules.
        E.syntax = s;

        // Rehighlight the entire file after setting E.syntax.
        int filerow;
        for (filerow = 0; filerow < E.numrows; filerow++) {
          editorUpdateSyntax(&E.row[filerow]);
        }
        return;
      }
      i++;
    }
  }
}

/*** row operations ***/

/*
 * Calculate the correct Render Cursor x offset
 */
int editorRowCxToRx(erow *row, int cx) {
  int rx = 0;
  int j;
  for (j = 0; j < cx; j++) {
    // Offset X position by tab stop
    // If it’s a tab, we use (rx % KILO_TAB_STOP) to find out how many columns
    // we are to the right of the last tab stop,
    // then subtract that from (KILO_TAB_STOP - 1) to find out how many
    // columns we are to the left of the next tab stop.
    if (row->chars[j] == '\t')
      rx += (KILO_TAB_STOP - 1) - (rx % KILO_TAB_STOP);

    // Regardless, add 1 to rx because we've iterated forward 1 character
    rx++;
  }
  return rx;
}

/*
 * Calculate the correct render offset for a given cursor position
 */
int editorRowRxToCx(erow *row, int rx) {
  int cur_rx = 0;
  // Iterate through every character in the row looking for tabs
  // because tabs take up more render space than byte space.
  int cx;
  for (cx = 0; cx < row->size; cx++) {
    if (row->chars[cx] == '\t') {
      // If we find a tab, offset the cur_rx by the appropriate amount.
      cur_rx = (KILO_TAB_STOP - 1) - (cur_rx % KILO_TAB_STOP);
    }
    // Either way, move it forward by 1 char.
    cur_rx++;

    // If we've passed the render X we're looking from, we're done.
    if (cur_rx > rx)
      return cx;
  }

  // This is a safety fallback in case the provided rx is out of range.
  return cx;
}

/*
 * Convert a buffered text row into a rendered row for display.
 */
void editorUpdateRow(erow *row) {
  // Count the number of tab characters in the row in order to alloc enough
  // memory.
  int tabs = 0;
  int j;
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t')
      tabs++;
  }

  // Free up space and allocate it to hold the row, accounting for \t now
  // taking up 8 space characters instead.
  free(row->render);
  row->render = malloc(row->size + (tabs * (KILO_TAB_STOP - 1)) + 1);

  // Copy over each char from row into render.
  int idx = 0;
  for (j = 0; j < row->size; j++) {
    if (row->chars[j] == '\t') {
      // If the char is a tab, loop to replace it with 8 spaces.
      row->render[idx++] = ' ';
      while (idx % KILO_TAB_STOP != 0)
        row->render[idx++] = ' ';
    } else {
      // Otherwise copy it over as-is.
      row->render[idx++] = row->chars[j];
    }
  }

  // Denote the end of the line
  row->render[idx] = '\0';
  // After the above for-loop, idx contains the number of chars that were
  // copied over and can be used to describe the render size
  row->rsize = idx;

  // Refresh the syntax highlighting assignments for this row.
  editorUpdateSyntax(row);
}

/*
 * Add a row as a string with length len as a new row in the editor at a given
 * position, 'at'.
 */
void editorInsertRow(int at, char *s, size_t len) {
  if (at < 0 || at > E.numrows)
    return;

  // Allocate space for a new erow.
  E.row = realloc(E.row, (sizeof(erow)) * (E.numrows + 1));
  // Make room at the specified index for the new row
  memmove(&E.row[at + 1], &E.row[at], sizeof(erow) * (E.numrows - at));

  // Define the length of the row to add and store a pointer to
  // the next free large enough memory address.
  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);

  // copy the len bytes from memory address s to the memory addresses starting
  // with the start-point of the new row.
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';

  // Define the size of the text to render and a NULL pointer for rendering
  // and highlighting;
  E.row[at].rsize = 0;
  E.row[at].render = NULL;
  E.row[at].hl = NULL;
  // pass the mem address of the current row's start position.
  editorUpdateRow(&E.row[at]);

  // Let the editor know how long the rows array is.
  E.numrows++;
  E.dirty++;
}

/*
 * Free the raw and rendered char arrays from a given editor row.
 */
void editorFreeRow(erow *row) {
  free(row->render);
  free(row->chars);
  free(row->hl);
}

/*
 * Completely delete a single row.
 */
void editorDelRow(int at) {
  if (at < 0 || at >= E.numrows)
    return;

  // Free the memory owned by the row to delete.
  editorFreeRow(&E.row[at]);

  // Shift all following rows into the memory previously occupied by the row
  // to delete.
  memmove(&E.row[at], &E.row[at + 1], sizeof(erow) * (E.numrows - at - 1));

  E.numrows--;
  E.dirty++;
}

/*
 * Insert a single character at a given position in an existing row.
 * Note that this function does not need to know the details of the
 * cursor positioning, it only worries about the memory operations required
 * to insert the character.
 */
void editorRowInsertChar(erow *row, int at, int c) {
  // Validate 'at', noting that it can be 1 position past the end of the row
  // in which case it needs to be moved placed at the actual end of the row
  // row->size.
  if (at < 0 || at > row->size)
    at = row->size;

  // Allocate a block with enough memory for the current row + 2,
  // 1 for the new character and 1 for the null byte.
  row->chars = realloc(row->chars, row->size + 2);

  // memmove is a lot like memcpy but safe to use in cases where the
  // to and from arrays / memory blocks overlap.
  memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);

  // Let the row know its new size.
  row->size++;

  // Insert the new character and persist it to the editor.
  row->chars[at] = c;
  editorUpdateRow(row);

  E.dirty++;
}

/*
 * Append a given string s of length len to a given editor row.
 */
void editorRowAppendString(erow *row, char *s, size_t len) {
  // Increase the memory size of the given row to account for the length of the
  // new string, plus 1 more for the EOL null byte.
  row->chars = realloc(row->chars, row->size + len + 1);

  // Copy the contents of s to the end of the row's chars array.
  memcpy(&row->chars[row->size], s, len);

  // Update the stored row size.
  row->size += len;

  // Add the EOL null byte.
  row->chars[row->size] = '\0';

  // Persist the change to the editor row.
  editorUpdateRow(row);
  E.dirty++;
}

/*
 * Delete a single character at a given position in an existing row.
 */
void editorRowDelChar(erow *row, int at) {
  // Validate 'at', noting that if its at an invalid position we can return.
  if (at < 0 || at > row->size)
    return;

  // Move the entire memory block down, including the \0 bit at the end.
  memmove(&row->chars[at], &row->chars[at + 1], row->size - at);

  // Decrement the row size and persist the change to the editor.
  row->size--;
  editorUpdateRow(row);

  E.dirty++;
}

/*** editor operations ***/

/*
 * Manage the cursor aspect of new character insertion.
 * Note that this function does not need to know about the memory implications
 * of inserting a new character, it only manages the cursor positioning.
 */
void editorInsertChar(int c) {
  if (E.cy == E.numrows) {
    // If the cursor is one past the end of the file (on the new-line-tilde)
    // add a new empty row before inserting the character
    editorInsertRow(E.numrows, "", 0);
  }

  editorRowInsertChar(&E.row[E.cy], E.cx, c);
  E.cx++;
}

/*
 * Add a new row below the current cursor position containing
 * only an empty string or a null byte.
 */
void editorInsertNewline(void) {
  // If the cursor is at the start of the line, simply insert a new empty row
  // at the current vertical position.
  if (E.cx == 0) {
    editorInsertRow(E.cy, "", 0);
  } else {
    // Copy the current row to a new memory block.
    erow *row = &E.row[E.cy];
    // Add a new row below the current row containing the characters
    // from the current X position and right.
    editorInsertRow(E.cy + 1, &row->chars[E.cx], row->size - E.cx);
    // truncate the current row at the cursor position
    row = &E.row[E.cy];
    row->size = E.cx;
    // add an EOL null byte at the cursor position.
    row->chars[row->size] = '\0';
    // Persist the updated row to the editor.
    editorUpdateRow(row);
  }
  // Move the cursor down to the beginning of the next (i.e. new) line.
  E.cy++;
  E.cx = 0;
}

/*
 * Manage the cursor aspect of character deletion.
 */
void editorDelChar(void) {
  // Cannot delete from past the last row.
  if (E.cy == E.numrows)
    return;
  // Cannot delete from the before the first row.
  if (E.cx == 0 && E.cy == 0)
    return;

  erow *row = &E.row[E.cy];

  if (E.cx > 0) {
    // If the cursor is within or at the end of a given row, delete one char.
    editorRowDelChar(row, E.cx - 1);
    E.cx--;
  } else {
    // If the cursor is at the beginning of a row, move the cursor horizontally
    // to the end of the previous row, without moving its vertical position.
    E.cx = E.row[E.cy - 1].size;

    // Append the full contents of the current row to the end of the previous
    // row's contents
    editorRowAppendString(&E.row[E.cy - 1], row->chars, row->size);

    // Delete the current row entirely
    editorDelRow(E.cy);

    // move the cursor up one row
    E.cy--;
  }
}

/*** file i/o ***/

/*
 * Convert the erow structs into a single string ready to be saved to disk.
 */
char *editorRowsToString(int *buflen) {
  int totlen = 0;
  // Add up the bytelength of all rows in the editor.
  int j;
  for (j = 0; j < E.numrows; j++)
    totlen += E.row[j].size + 1;

  // Store the result in the input *buflen to let the caller know the result.
  *buflen = totlen;

  // Allocate enough memory to hold the entire string.
  // This pointer remains stationary so that the caller can `free` it
  // afterwards.
  char *buf = malloc(totlen);

  // Store the starting location at *p, which will move as data is added.
  char *p = buf;
  for (j = 0; j < E.numrows; j++) {
    // Copy the data from each row into p
    memcpy(p, E.row[j].chars, E.row[j].size);
    // Update the pointer to the end of the array.
    p += E.row[j].size;
    // Manually insert a newline char at the end of every row.
    *p = '\n';
    // Increment p by 1 to step over the new line.
    p++;
  }

  // Return buf so the caller can free it.
  return buf;
}

/*
 * Open and read a file from disk (eventually).
 * Currently this only supports hard coding a single editor line
 */
void editorOpen(char *filename) {
  // Store the filename in the editor config.
  free(E.filename);
  E.filename = strdup(filename);

  // Enable syntax highlighting.
  editorSelectSyntaxHighlight();

  // Open a file by name.
  FILE *fp = fopen(filename, "r");
  if (!fp) {
    die("fopen");
  }

  // Load one line from the file.
  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen;
  // When getline receives a NULL *line and 0 line capacity arg, it will
  // automatically allocate the correct amount of memory and store the
  // correct *line pointer and capacity, returning the length.
  // Note: A linelen of -1 indicates the end of the file.
  while ((linelen = getline(&line, &linecap, fp)) != -1) {
    // Strip off new lines and carriage returns.
    while (linelen > 0 &&
           (line[linelen - 1] == '\r' || line[linelen - 1] == '\n')) {
      linelen--;
    }
    editorInsertRow(E.numrows, line, linelen);
  }

  // Re-free the memory allocated to the line and close the file.
  free(line);
  fclose(fp);

  // Reset the dirty flag on open to ensure we start clean.
  E.dirty = 0;
}

/*
 * Write the string generated by editorRowsToString to disk.
 */
void editorSave(void) {
  // If this is not an existing file, we don't know where to save it, so
  // prompt the user for a name, and use that.
  if (E.filename == NULL) {
    E.filename = editorPrompt("Save as: %s", NULL);
    if (E.filename == NULL) {
      // If the user exited the save as prompt with <esc>, bail out.
      editorSetStatusMessage("Save cancelled");
      return;
    }
    // Recheck for syntax highlighting.
    editorSelectSyntaxHighlight();
  }

  // Convert the editor rows to a string.
  int len;
  char *buf = editorRowsToString(&len);

  // Open the known file nam  as writable, or create a new one.
  // 0644 is a permissions object allowing the file owner to r/w
  // but limiting to read only for other users.
  int fd = open(E.filename, O_RDWR | O_CREAT, 0644);
  if (fd != -1) {
    // Set the file's size to the length set in editorRowstoString
    if (ftruncate(fd, len) != -1) {
      // Write the results to disk.
      if (write(fd, buf, len) == len) {
        close(fd);
        free(buf);
        // Reset the dirty flag on every save.
        E.dirty = 0;
        editorSetStatusMessage("%d bytes written to disk", len);
        return;
      }
    }
    close(fd);
  }

  free(buf);
  editorSetStatusMessage("Can't save! I/O error: %s", strerror(errno));
}

/*** find ***/

/*
 * Allow users to search for a string and automatically jump to the next match
 */
void editorFindCallback(char *query, int key) {
  // The index of the row containing the most recent match.
  static int last_match = -1;
  // The direction to search,
  // where 1 = forwards from the cursor and -1 = backwards
  static int direction = 1;

  // Prepare to store the row index of the match row so that highlighting can
  // be applied and removed.
  static int saved_hl_line;
  static char *saved_hl = NULL;

  // Reset any current highlighting before finding the next match.
  if (saved_hl) {
    memcpy(E.row[saved_hl_line].hl, saved_hl, E.row[saved_hl_line].rsize);
    free(saved_hl);
    saved_hl = NULL;
  }

  if (key == '\r' || key == '\x1b') {
    last_match = -1;
    direction = 1;
    return;
  } else if (key == ARROW_RIGHT || key == ARROW_DOWN) {
    direction = 1;
  } else if (key == ARROW_LEFT || key == ARROW_UP) {
    direction = -1;
  } else {
    last_match = -1;
    direction = 1;
  }

  if (last_match == -1)
    direction = 1;
  int current = last_match;

  // Iterate through every row in the editor.
  int i;
  for (i = 0; i < E.numrows; i++) {
    // move the "current" index forward or backward (depending on direction)
    // and begin the search again from there
    current += direction;
    if (current == -1)
      // If this is the first search case, start looking from the top.
      current = E.numrows - 1;
    else if (current == E.numrows)
      // If we hit the end of the file, restart at the top.
      current = 0;

    // Search the row at the 'current' index rather than the raw loop index.
    erow *row = &E.row[current];

    // Check each row to see if a match is found.
    char *match = strstr(row->render, query);
    if (match) {
      // If yes, jump to the first match instance
      last_match = current;
      E.cy = current;
      // Move the cursor horizontally to the beginning of the match
      E.cx = editorRowRxToCx(row, match - row->render);
      // Set the row offset to the bottom of the screen so that on the next
      // screen refresh the current cursor position is placed at the top of
      // the screen.
      E.rowoff = E.numrows;

      // Apply syntax highlighting to the found matches.
      saved_hl_line = current;
      saved_hl = malloc(row->rsize);
      memcpy(saved_hl, row->hl, row->rsize);

      // Set all of the found characters' HL statuses to HL_MATCH
      // for highlighting
      memset(&row->hl[match - row->render], HL_MATCH, strlen(query));
      break;
    }
  }
}

/*
 * Prompt the user to perform a search and pass the input to editorFindCallback
 */
void editorFind(void) {
  // Save the pre-search cursor position so we can return to it on cancel.
  int saved_cx = E.cx;
  int saved_cy = E.cy;
  int saved_coloff = E.coloff;
  int saved_rowoff = E.rowoff;

  // Prompt the user for a search string stored at *query
  char *query =
      editorPrompt("Search: %s (Use ESC/Arrows/Enter)", editorFindCallback);

  if (query) {
    free(query);
  } else {
    E.cx = saved_cx;
    E.cy = saved_cy;
    E.coloff = saved_coloff;
    E.rowoff = saved_rowoff;
  }
}

/*** append buffer ***/

/*
 * An appendable buffer stores a pointer to its location and its own length.
 * This will allows us to append our full screen output to a single buffer
 * then write it all at once, rather than a series of individual writes which
 * can cause a flickering effect as they execute.
 */
struct abuf {
  char *b;
  int len;
};

/*
 * Creates a new, empty appendable buffer
 */
#define ABUF_INIT {NULL, 0}

/*
 * Append a new string, s, with length len to struct abuf ab
 */
void abAppend(struct abuf *ab, const char *s, int len) {
  // Move ab to a new memory block with enough room for the current length
  // plus the new length (or extend the current block if possible).
  char *new = realloc(ab->b, ab->len + len);

  if (new == NULL) {
    return;
  }

  // Append s to the end of ab and update ab's pointers accordingly
  memcpy(&new[ab->len], s, len);
  ab->b = new;
  ab->len += len;
}

/*
 * Simple deallocator for our dynamic length buffer.
 */
void abFree(struct abuf *ab) { free(ab->b); }

/*** output ***/

/*
 * Account for the current cursor position and row offset to enable scrolling.
 */
void editorScroll(void) {
  // Use the Render Cursor X position to account for renders that don't
  // match the character count, like '\t' taking KILO_TAB_STOP spaces.
  E.rx = 0;
  if (E.cy < E.numrows) {
    E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
  }

  // Cursor is above the vertical window.
  if (E.cy < E.rowoff) {
    E.rowoff = E.cy;
  }

  // Cursor is below the vertical window.
  if (E.cy >= E.rowoff + E.screenrows) {
    E.rowoff = E.cy - E.screenrows + 1;
  }

  // Cursor is left of the horizontal window
  if (E.rx < E.coloff) {
    E.coloff = E.rx;
  }

  // Cursor is right of the horizontal window
  if (E.rx >= E.coloff + E.screencols) {
    E.coloff = E.rx - E.screencols + 1;
  }
}

/*
 * Draw a column of ~ to distinguish rows.
 * [K - Erase In Line, using default arg 0 to erase to the right of the
 * cursor.
 * [3m - Select Graphic Rendition, using 1 for red and 9 for default.
 */
void editorDrawRows(struct abuf *ab) {
  int y;
  for (y = 0; y < E.screenrows; y++) {
    int filerow = y + E.rowoff;
    if (filerow >= E.numrows) {
      // Generate a welcome message if no user rows are present.
      if (E.numrows == 0 && y == E.screenrows / 3) {
        char welcome[80];
        int welcomelen = snprintf(welcome, sizeof(welcome),
                                  "Kilo editor --- version %s", KILO_VERSION);
        if (welcomelen > E.screencols) {
          welcomelen = E.screencols;
        }
        // Center the message.
        int padding = (E.screencols - welcomelen) / 2;
        if (padding) {
          // The first padding character should be a ~
          abAppend(ab, "~", 1);
          padding--;
        }
        // The rest of the padding should be spaces
        while (padding--) {
          abAppend(ab, " ", 1);
        }

        // add the welcome message to the buffer
        abAppend(ab, welcome, welcomelen);
      } else {
        abAppend(ab, "~", 1);
      }
    } else {
      // Print the rows as-is but truncate the text to the terminal window,
      // accounting for the column offset to allow for horizontal scrolling.
      int len = E.row[filerow].rsize - E.coloff;
      if (len < 0)
        len = 0;
      if (len > E.screencols)
        len = E.screencols;

      // Store the currently visible row text in c and hl
      char *c = &E.row[filerow].render[E.coloff];
      unsigned char *hl = &E.row[filerow].hl[E.coloff];

      int current_color = -1;

      // Iterate through every character in the visible row and apply styling.
      int j;
      for (j = 0; j < len; j++) {
        if (iscntrl(c[j])) {
          // If we hit a non-printable character,
          // print Alpha ctrl chars as capital letters by adding them
          // to '@', which converts the <c-a> to A .. <c-z> to Z
          // We'll print all other non-printables as '?'.
          char sym = (c[j] <= 26) ? '@' + c[j] : '?';

          // Invert the text color to differentiate these symbols.
          abAppend(ab, "\x1b[7m", 4);
          abAppend(ab, &sym, 1);
          abAppend(ab, "\x1b[m", 3);

          // The above line resets all formatting, so we need to reapply any
          // HL present.
          if (current_color != -1) {
            char buf[16];
            int clen = snprintf(buf, sizeof(buf), "\x1b[%dm]", current_color);
            abAppend(ab, buf, clen);
          }
        } else if (hl[j] == HL_NORMAL) {
          if (current_color != -1) {
            // Only reset text coloring if it's been applied.
            abAppend(ab, "\x1b[39m", 5);
            current_color = -1;
          }
          abAppend(ab, &c[j], 1);
        } else {
          // Find the correct color for the character.
          int color = editorSyntaxToColor(hl[j]);
          // Check if the current color is already being applied.
          if (color != current_color) {
            current_color = color;
            // Create a char buf to hold the color setting string.
            char buf[16];
            int clen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
            // Add the color setting string and character to the row.
            abAppend(ab, buf, clen);
          }
          abAppend(ab, &c[j], 1);
        }
      }
      // Reset all text coloring.
      abAppend(ab, "\x1b[39m", 5);
    }

    // Clear to the right of the cursor.
    abAppend(ab, "\x1b[K", 3);

    // Add a new line to the end of every row.
    abAppend(ab, "\r\n", 2);
  }
}

/*
 * Draw a status bar on the bottom row of the screen.
 *   [m - Select Graphic Rendition affects the text rendering of the following
 *     text
 *     [7m - Invert colors
 *     [m - Reverts to normal colors (default arg 0)
 */
void editorDrawStatusBar(struct abuf *ab) {
  // invert colors
  abAppend(ab, "\x1b[7m", 4);

  // Add the filename (if available) and line count to the status bar.
  char status[80];
  // Add the current line number, right aligned
  char rstatus[80];

  int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
                     E.filename ? E.filename : "[No Name]", E.numrows,
                     E.dirty ? "(modified)" : "");
  int rlen =
      snprintf(rstatus, sizeof(rstatus), "%s | %d/%d",
               E.syntax ? E.syntax->filetype : "no ft", E.cy + 1, E.numrows);
  // Truncate the status to fit on the screen, just in case
  if (len > E.screencols)
    len = E.screencols;
  abAppend(ab, status, len);

  // Append a full row of spaces
  while (len < E.screencols) {
    if (E.screencols - len == rlen) {
      // Once we hit the right edge - the length of our right-aligned status,
      // add the right-aligned status message to the row.
      abAppend(ab, rstatus, rlen);
      break;
    } else {
      abAppend(ab, " ", 1);
      len++;
    }
  }

  // Restore colors
  abAppend(ab, "\x1b[m", 3);

  // Add a new line to suppor the status message.
  abAppend(ab, "\r\n", 2);
}

/*
 * Display a custom messsage underneath the status bar at the bottom of the
 * editor window.
 *   [K - Clear the line
 */
void editorDrawMessageBar(struct abuf *ab) {
  // Clear the message bar line.
  abAppend(ab, "\x1b[K", 3);

  int msglen = strlen(E.statusmsg);
  // Truncate the message to fit the window if necessary.
  if (msglen > E.screencols)
    msglen = E.screencols;

  // Display the message if it is less than 5 seconds old.
  // Note: it will only disappear when a key is pressed to refresh the screen.
  if (msglen && time(NULL) - E.statusmsg_time < 5)
    abAppend(ab, E.statusmsg, msglen);
}

/*
 * Clears the screen and repositions the cursor.
 * Note: this function takes advantage of VT100 Escape Sequences.
 *   [J - Erase In Display (using 2 to set erase entire display.
 *   [?l / [?h - toggle off/on terminal "modes" (using 25 for cursor vis).
 */
void editorRefreshScreen(void) {
  editorScroll();

  struct abuf ab = ABUF_INIT;

  // Hide the cursor and reset its position
  abAppend(&ab, "\x1b[?25l", 6);
  abAppend(&ab, "\x1b[H", 3);

  editorDrawRows(&ab);
  editorDrawStatusBar(&ab);
  editorDrawMessageBar(&ab);

  // Move the cursor to the stored X, Y position.
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1,
           (E.rx - E.coloff) + 1);
  abAppend(&ab, buf, strlen(buf));

  // Un-hide the cursor.
  abAppend(&ab, "\x1b[?25h", 6);

  // Display full contents of ab and free the memory.
  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

/*
 * A configurable editor status message.
 * Note that this is a variadic function, allowing any number of args
 */
void editorSetStatusMessage(const char *fmt, ...) {
  // parse N arguments using va_list
  va_list ap;
  va_start(ap, fmt);
  // A customizable print message that handles applying the printf to
  // every provided argument using va_arg.
  vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
  va_end(ap);

  // time(NULL) fetches the current time
  E.statusmsg_time = time(NULL);
}

/*** input ***/

/*
 * Display a user text prompt in the status bar.
 * Note that *prompt should be a format string using %s.
 * callback is an optional function that will be called after each keypress.
 * It takes the user input and the last key pressed.
 */
char *editorPrompt(char *prompt, void (*callback)(char *, int)) {
  // Allocate enough space to hold 128 user input characters in the prompt.
  size_t bufsize = 128;
  char *buf = malloc(bufsize);

  // Initialize the input to a single EOL null byte.
  size_t buflen = 0;
  buf[0] = '\0';

  while (1) {
    // Print the prompt to the status bar.
    editorSetStatusMessage(prompt, buf);
    editorRefreshScreen();

    // Read the user's input 1 character at a time.
    int c = editorReadKey();
    if (c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE) {
      // Support backspacing.
      if (buflen != 0) {
        buf[--buflen] = '\0';
      }
    } else if (c == '\x1b') {
      // Exit on <esc>
      editorSetStatusMessage("");
      if (callback)
        callback(buf, c);
      free(buf);
      return NULL;
    } else if (c == '\r') {
      // If the user hit enter on a non-empty input, clear the status and
      // return a pointer to the input.
      if (buflen != 0) {
        editorSetStatusMessage("");
        if (callback)
          callback(buf, c);
        return buf;
      }
    } else if (!iscntrl(c) && c < 128) {
      // If the user is typing valid ASCII characters:
      if (buflen == bufsize - 1) {
        // If the user runs out of room in the buffer, double the size and
        // reallocate it.
        bufsize *= 2;
        buf = realloc(buf, bufsize);
      }
      // Add the user's input character to the buffer and increment the
      // length counter.
      buf[buflen++] = c;
      // Add an EOL null byte at the end.
      buf[buflen] = '\0';
    }

    if (callback)
      callback(buf, c);
  }
}
/*
 * Handle cursor movement options by incrementing the stored cursor positions.
 */
void editorMoveCursor(int key) {
  // Limit the cursor veritcally to 1 past the end of the file.
  erow *row = (E.cy > E.numrows) ? NULL : &E.row[E.cy];

  switch (key) {
  case ARROW_LEFT:
    if (E.cx > 0) {
      E.cx--;
    } else if (E.cy > 0) {
      E.cy--;
      E.cx = E.row[E.cy].size;
    }
    break;
  case ARROW_DOWN:
    if (E.cy < E.numrows) {
      E.cy++;
    }
    break;
  case ARROW_RIGHT:
    if (row && E.cx < row->size) {
      E.cx++;
    } else if (row && E.cx == row->size) {
      E.cy++;
      E.cx = 0;
    }
    break;
  case ARROW_UP:
    if (E.cy > 0) {
      E.cy--;
    }
    break;
  }

  // Correct the cursor's horizontal position when vertical scrolling would
  // place the cursor in a horizontally invalid position (such as from a longer
  // line to a shorter one)
  row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
  int rowlen = row ? row->size : 0;
  if (E.cx > rowlen) {
    // If the cursor would be put in a bad spot, snap it to the end of the line
    E.cx = rowlen;
  }
}

/*
 * Checks the most recently pressed key against special handling cases
 */
void editorProcessKeyPress(void) {
  static int quit_times = KILO_QUIT_TIMES;

  int c = editorReadKey();

  switch (c) {
  case '\r':
    editorInsertNewline();
    break;

  // Quit out with <c-q>
  case CTRL_KEY('q'):
    // Require multiple <c-q> hits to exit with pending changes
    if (E.dirty && quit_times > 0) {
      editorSetStatusMessage("WARNING!!! File has unsaved changes. "
                             "Press ctrl-q %d more times to quit.",
                             quit_times);
      quit_times--;
      return;
    }
    // Clear the screen.
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    // Exit with success code.
    exit(0);
    break;

  // Save to disk with <c-s>
  case CTRL_KEY('s'):
    editorSave();
    break;

  // Support line beginning / end scrolling.
  case HOME_KEY:
    E.cx = 0;
    break;
  case END_KEY:
    if (E.cy < E.numrows)
      E.cx = E.row[E.cy].size;
    break;

  case CTRL_KEY('f'):
    editorFind();
    break;

  case BACKSPACE:
  case CTRL_KEY('h'):
  case DEL_KEY:
    // allow deleting "backwards" with DEL
    if (c == DEL_KEY)
      editorMoveCursor(ARROW_RIGHT);
    editorDelChar();
    break;

  // Support full page scrolling.
  case PAGE_UP:
  case PAGE_DOWN: {
    // Why are we doing these conditional checks in a switch? Ew.
    if (c == PAGE_UP) {
      // Scroll to the top of the page.
      E.cy = E.rowoff;
    } else if (c == PAGE_DOWN) {
      // Scroll to the bottom of the page and place the cursor at the bottom.
      E.cy = E.rowoff + E.screenrows - 1;
      if (E.cy > E.numrows)
        // If that would put us outside of the file, place us 1 line after the
        // end of the file instead.
        E.cy = E.numrows;
    }

    int times = E.screenrows;
    while (times--) {
      editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
    }
  } break;

  // Map our custom Arrow Key inputs to movement.
  case ARROW_UP:
  case ARROW_LEFT:
  case ARROW_DOWN:
  case ARROW_RIGHT:
    editorMoveCursor(c);
    break;

  // Handle <c-l> and <esc> as null operations.
  case CTRL_KEY('l'):
  case '\x1b':
    break;

  default:
    // Any non-special-case char should be inserted into the editor row.
    editorInsertChar(c);
    break;
  }

  // Reset quit_times if the user does anything other than quit.
  quit_times = KILO_QUIT_TIMES;
}

/*** init ***/

void initEditor(void) {
  // Set the cursor position to (0, 0).
  E.cx = 0;
  E.cy = 0;

  // Set the Render Cursor position to 0
  E.rx = 0;

  // Start with no row or column offset.
  E.rowoff = 0;
  E.coloff = 0;

  // Start with no text rows.
  E.numrows = 0;

  // Init the row pointer to NULL to allow for dynamic resizing.
  E.row = NULL;

  // Init the dirty flag to 0 / Off when we start
  E.dirty = 0;

  // Init the filename pointer to NULL to allow for dynamic resizing.
  E.filename = NULL;

  // Init an empty status message to show the user under the status bar.
  E.statusmsg[0] = '\0';
  E.statusmsg_time = 0;

  // Init the syntax configuration to NULL, meaning no filetype or highlighting
  E.syntax = NULL;

  // If we fail to read a screen size, exit.
  if (getWindowSize(&E.screenrows, &E.screencols) == -1)
    die("getWindowSize");

  // Reserve 2 rows at the bottom of the editor window for the
  // status bar and message.
  E.screenrows -= 2;
}

int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor();
  // If a file name is provided, pass it to editor open.
  if (argc >= 2) {
    editorOpen(argv[1]);
  }

  editorSetStatusMessage("HELP: Ctrl-s = save | Ctrl-q = quit | Ctrl-f = find");

  // Loop until user exits.
  while (1) {
    editorRefreshScreen();
    editorProcessKeyPress();
  }

  return 0;
}
