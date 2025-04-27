/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

/*** defines ***/

// Bitwise AND of the input key (in ASCII) with 0001 1111
// to cast the first 3 bits to 0, which is how ASCII maps
// characters and their CTRL+<character> variants.
// q in ASCII     = 113 = 0111 0001
// <c-q> in ASCII =  17 = 0001 0001
// q & 0x1f = 0001 0001 = <c-q>
#define CTRL_KEY(key) ((key) & 0x1f)

/*** data ***/


struct editorConfig {
  int screenrows;
  int screencols;
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
char editorReadKey(void) {
  int nread;
  char c;
  while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
    if (nread == -1 && errno != EAGAIN) {
      die("read");
    }
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
  if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    // If that fails to read, or returns a col of 0, fall back on 
    // moving the cursor position to the bottom right corner and using
    // getCursorPosition to load the current position to rows + cols.
    if(write(STDOUT_FILENO, "\x1b[999C\x1b[998B", 12) != 12) {
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
void abFree(struct abuf *ab) {
  free(ab->b);
}

/*** output ***/

/*
 * Draw a column of ~ to distinguish rows
 */
void editorDrawRows(struct abuf *ab) {
  int y;
  for (y=0; y < E.screenrows; y++) {
    abAppend(ab, "~", 1);

    // Don't newline the last row
    if (y < E.screenrows - 1) {
      abAppend(ab, "\r\n", 2);
    }
  }
}

/*
 * Clears the screen and repositions the cursor.
 * Note: this function takes advantage of VT100 Escape Sequences.
 *   [J - Erase In Display (using 2 to set erase entire display.
 *   [H - Reposition Cursor (using default arg 1 to send the cursor to col 1).
 *   [?l / [?h - toggle off/on terminal "modes" (using 25 for cursor vis).
 */
void editorRefreshScreen(void) {
  struct abuf ab = ABUF_INIT;

  // Hide the cursor.
  abAppend(&ab, "\x1b[?25l", 6);
  // Clear the display and reset cursor position.
  abAppend(&ab, "\x1b[2J", 4);
  abAppend(&ab, "\x1b[H", 3);
  // Un-hide the cursor.
  abAppend(&ab, "\x1b[?25h", 6);

  editorDrawRows(&ab);
  abAppend(&ab, "\x1b[H", 3);

  write(STDOUT_FILENO, ab.b, ab.len);
  abFree(&ab);
}

/*** input ***/

/*
 * Checks the most recently pressed key against special handling cases
 */
void editorProcessKeyPress(void) {
  char c = editorReadKey();

  switch(c) {
    case CTRL_KEY('q'):
      // Clear the screen.
      write(STDOUT_FILENO, "\x1b[2J", 4);
      write(STDOUT_FILENO, "\x1b[H", 3);
     
      // Exit with success code. 
      exit(0);
      break;
  }
}

/*** init ***/

void initEditor(void) {
  if(getWindowSize(&E.screenrows, &E.screencols) == -1) die("getWindowSize");
}

int main(void) {
  enableRawMode();
  initEditor();

  // Loop until user exits.
  while (1) {
    editorRefreshScreen();
    editorProcessKeyPress();
  }
  
  return 0;
}
