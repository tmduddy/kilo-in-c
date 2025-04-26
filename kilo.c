#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

struct termios orig_termios;

/* 
 * reset all terminal configs to their original state
*/
void disableRawMode(void) {
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

/* 
 * Edit terminal settings to enable "raw" mode
 * where raw mode means inputs are "submitted" after every
 * character, not when the user hits enter
*/
void enableRawMode(void) {
  // store all initial terminal configs in global struct orig_termios
  tcgetattr(STDIN_FILENO, &orig_termios);

  // ensure that disableRawMode is always called at program exit 
  atexit(disableRawMode);
  
  // create a struct raw to hold updated terminal config
  struct termios raw = orig_termios;

  // bitwise invert 'input' flags
  // IXON controls the handling of XOFF/XON controls for output suppression
  //  disabling it overrides <c-s>/<c-q> and allows them to be read
  //  as ASCII 
  // ICRNL controls the conversion of carriage returns into new lines
  //   disabling it resets <c-m> and <enter> to 
  //   ASCII 10 (\r) instead of ASCII 13 (\n)
  raw.c_iflag &= ~(ICRNL | IXON)
  
  // bitwise invert 'local' flags
  // ECHO controls the output of characters. 
  //  disabling it suppresses terminal output while typing, 
  //  like when entering a sudo password
  // ICANON controls canonical mode vs raw mode
  //  disabling it handles inputs char by char instead of line by line
  // ISIG controls the handling of signal inputs
  //   disabling it overrides <c-c>/<c-y>/<c-z> and allows them to be read 
  //   as ASCII
  // IEXTEN controls the handling of input buffering controls
  //   disabling it overrides <c-o>/<c-v> and allows them to be read
  //   as ASCII
  raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN)

  // persist the change:
  // TCSAFLUSH waits for all pending output to be written 
  // to the terminal, and also discards any input that 
  // hasn’t been read.
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);  
}

int main(void) {
  enableRawMode();

  // infinitely read from STDIN, saving typed character in char c
  // until a 'q' is entered
  char c;
  while (read(STDIN_FILENO, &c, 1) == 1 && c != 'q') {
    if(iscntrl(c)) {
      // iscntrl = non printable ASCII control char
      // so print ASCII only
      printf("%d\n", c);
    } else {
      // print the ASCII code and the printable byte
      printf("%d ('%c')\n", c, c);
    }
  }
  
  return 0;
}
