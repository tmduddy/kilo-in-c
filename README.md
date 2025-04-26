# kilo-in-c
following a [tutorial article](https://viewsourcecode.org/snaptoken/kilo/) to try and learn some C basics

The objective of this tutorial is to make a terminal text editor with basic functionality


### running:
ensure you have a valid C compiler and make installed (I'm using the versions bundled with MacOS Sequoia via xcode-select

```shell
make
```
will run `cc kilo.c -o kilo -Wall -Wextra -pedantic -std=c99`

and then the program can be run with 
```shell
./kilo
```

Soon I'll learn more about `make` files and set up a different command for compiling for production (fewer flags)
