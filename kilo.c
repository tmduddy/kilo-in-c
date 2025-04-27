/*** includes ***/

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
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

/* Get the current terminal window size */
int getWindowSize(int *rows, int *cols) {
  struct winsize ws;

  // Fetch the current window size from ioctl
  if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
    // Feturn an error if the fetch fails or the column is 0,
    // where 0 is another error state from sys/ioctl
    return -1;
  } else {
    // load the current column and row sizes into the input args
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
  }
}

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

/*** output ***/

/*
 * Draw a column of ~ to distinguish rows
*/
void editorDrawRows(void) {
  int y;
  for (y=0; y < E.screenrows; y++) {
    write(STDOUT_FILENO, "~\r\n", 3);
  }
}

/*
 * Clears the screen and repositions the cursor.
 * Note: this function takes advantage of VT100 Escape Sequences.
 *   [J - Erase In Display (using 2 to set erase entire display.
 *   [H - Reposition Cursor (using default arg 1 to send the cursor to col 1).
*/
void editorRefreshScreen(void) {
  write(STDOUT_FILENO, "\x1b[2J", 4);
  write(STDOUT_FILENO, "\x1b[H", 3);

  editorDrawRows();
  write(STDOUT_FILENO, "\x1b[H", 3);
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
