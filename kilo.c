/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
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

// Enum to map ints to key names.
// These values are outside of the standard char range to avoid conflicts
enum editorKey {
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

/*** data ***/

/*
 * hold a single row of editor text
 */
typedef struct erow {
  int size;
  char *chars;
} erow;

struct editorConfig {
  int cx;
  int cy;
  int rowoff;
  int coloff;
  int screenrows;
  int screencols;
  int numrows;
  erow *row;
  struct termios orig_termios;
};

struct editorConfig E;

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

/*** row operations ***/

/*
 * Add a row as a string with length len as a new row in the editor.
 */
void editorAppendRow(char *s, size_t len) {
  // Allocate space for a new erow.
  E.row = realloc(E.row, (sizeof(erow)) * (E.numrows + 1));

  // Define the length of the row to add and store a pointer to
  // the next free large enough memory address.
  int at = E.numrows;
  E.row[at].size = len;
  E.row[at].chars = malloc(len + 1);

  // copy the len bytes from memory address s to the memory addresses starting
  // with the start-point of the new row.
  memcpy(E.row[at].chars, s, len);
  E.row[at].chars[len] = '\0';

  // Let the editor know how long the rows array is.
  E.numrows++;
}

/*** file i/o ***/

/*
 * Open and read a file from disk (eventually).
 * Currently this only supports hard coding a single editor line
 */
void editorOpen(char *filename) {
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
    editorAppendRow(line, linelen);
  }

  // Re-free the memory allocated to the line and close the file.
  free(line);
  fclose(fp);
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
 * Account for the current cursor position and row offset to enable vertical
 * scrolling
 */
void editorScroll(void) {
  // Cursor is above the vertical window.
  if (E.cy < E.rowoff) {
    E.rowoff = E.cy;
  }

  // Cursor is below the vertical window.
  if (E.cy >= E.rowoff + E.screenrows) {
    E.rowoff = E.cy - E.screenrows + 1;
  }

  // Cursor is left of the horizontal window
  if (E.cx < E.coloff) {
    E.coloff = E.cx;
  }

  // Cursor is right of the horizontal window
  if (E.cx >= E.coloff + E.screencols) {
    E.coloff = E.cx - E.screencols + 1;
  }
}

/*
 * Draw a column of ~ to distinguish rows.
 * [K - Erase In Line, using default arg 0 to erase to the right of the
 * cursor.
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
      // truncate the text to the terminal window, accounting for the column
      // offset to allow for horizontal scrolling.
      int len = E.row[filerow].size - E.coloff;
      if (len < 0)
        len = 0;
      if (len > E.screencols)
        len = E.screencols;
      abAppend(ab, &E.row[filerow].chars[E.coloff], len);
    }

    // Clear to the right of the cursor.
    abAppend(ab, "\x1b[K", 3);

    // Don't newline the last row.
    if (y < E.screenrows - 1) {
      abAppend(ab, "\r\n", 2);
    }
  }
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

  // Move the cursor to the stored X, Y position.
  char buf[32];
  snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1,
           (E.cx - E.coloff) + 1);
  abAppend(&ab, buf, strlen(buf));

  // Un-hide the cursor.
  abAppend(&ab, "\x1b[?25h", 6);

  // Display full contents of ab and free the memory.
  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

/*** input ***/

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
    }
    break;
  case ARROW_UP:
    if (E.cy > 0) {
      E.cy--;
    }
    break;
  }
}

/*
 * Checks the most recently pressed key against special handling cases
 */
void editorProcessKeyPress(void) {
  int c = editorReadKey();

  switch (c) {
  case CTRL_KEY('q'):
    // Clear the screen.
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    // Exit with success code.
    exit(0);
    break;
  // Map our custom Arrow Key inputs to movement.
  case ARROW_UP:
  case ARROW_LEFT:
  case ARROW_DOWN:
  case ARROW_RIGHT:
    editorMoveCursor(c);
    break;
  case HOME_KEY:
    E.cx = 0;
    break;
  case END_KEY:
    E.cx = E.screenrows - 1;
    break;
  case PAGE_UP:
  case PAGE_DOWN: {
    int times = E.screenrows;
    while (times--) {
      editorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
    }
  } break;
  }
}

/*** init ***/

void initEditor(void) {
  // Set the cursor position to (0, 0).
  E.cx = 0;
  E.cy = 0;

  // Start with no row or column offset.
  E.rowoff = 0;
  E.coloff = 0;

  // Start with no text rows.
  E.numrows = 0;

  // init the row pointer to NULL to allow for dynamic resizing
  E.row = NULL;

  if (getWindowSize(&E.screenrows, &E.screencols) == -1)
    die("getWindowSize");
}

int main(int argc, char *argv[]) {
  enableRawMode();
  initEditor();
  // If a file name is provided, pass it to editor open.
  if (argc >= 2) {
    editorOpen(argv[1]);
  }

  // Loop until user exits.
  while (1) {
    editorRefreshScreen();
    editorProcessKeyPress();
  }

  return 0;
}
