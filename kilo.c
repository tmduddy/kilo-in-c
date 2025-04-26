#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

struct termios orig_termios;

/* 
 * Reset all terminal configs to their original state.
*/
void disableRawMode(void) {
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

/* 
 * Edit terminal settings to enable "raw" mode,
 * where raw mode means inputs are "submitted" after every
 * character, not when the user hits enter.
*/
void enableRawMode(void) {
  // Store all initial terminal configs in global struct orig_termios.
  tcgetattr(STDIN_FILENO, &orig_termios);

  // Ensure that disableRawMode is always called at program exit.
  atexit(disableRawMode);
  
  // Create a struct raw to hold updated terminal config.
  struct termios raw = orig_termios;

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
  // CS8 is a bitmask that sets the Character Size (CS) to 8
  //   the article indicates this is likely already set
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

  // Persist the change:
  // TCSAFLUSH waits for all pending output to be written 
  // to the terminal, and also discards any input that 
  // hasn’t been read.
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);  
}

int main(void) {
  enableRawMode();

  // Infinitely read from STDIN, saving typed character in char c
  // until a 'q' is entered.
  char c;
  while (read(STDIN_FILENO, &c, 1) == 1 && c != 'q') {
    if(iscntrl(c)) {
      // iscntrl checks for non-printable ASCII control characters.
      printf("%d\r\n", c);
    } else {
      // Print the ASCII code and the printable byte.
      printf("%d ('%c')\r\n", c, c);
    }
  }
  
  return 0;
}
