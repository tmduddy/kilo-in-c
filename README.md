# kilo-in-c
Following a [tutorial article](https://viewsourcecode.org/snaptoken/kilo/) to try and learn some C basics.

The objective of this tutorial is to make a terminal text editor with basic functionality.


## running:
Ensure you have a valid C compiler and make installed (I'm using the versions bundled with MacOS Sequoia via xcode-select.

```shell
make
```
will run `cc kilo.c -o kilo -Wall -Wextra -pedantic -std=c99`

and then the program can be run with 
```shell
./kilo
```

## testing:
There are no real tests as such (yet), but you can still validate different parts.

Error handling:

You can check the tcgetattr error handling by passing or piping in from STDIN like:
```shell
echo 'test' | ./kilo
./kilo <kilo.c
```
both of which should throw errors.


## formatting:
I've decicded to use LLVM's `clang-format` for this project, following these installation steps:
- Downloaded the latest Apple Silicon release binary from their [Github](https://github.com/llvm/llvm-project/releases/tag/llvmorg-18.1.8)
- saved the tar file in `~/clang-format/`
- unwrapped it to `~/clang-format/clang-format/` with `tar xvfJ clang-format/clang+llvm-18.1.8-arm64-apple-macos11.tar.xz -C ./clang-format`
- added a symlink to /usr/local/bin/clang-format with `sudo ln -s $(pwd)/$(find clang-format | grep bin/clang-format$) /usr/local/bin/clang-format`
- verified installation with `clang-format --version`

Note: I initially tried this the "easy way" with `brew install clang-format`, which works, but didn't expose the `clang-format.py` file required for the Vim integration anywhere I could find, so I deleted that and re-did it from scratch like this.

With `clang-format` installed, I followed the instructions on their [docs page](https://clang.llvm.org/docs/ClangFormat.html) to generate an LLVM standard format file in the project root
```shell
clang-format -style=llvm -dump-config > .clang-format
```

And then was finally able to add the auto-save function to my `~/.vimrc` like:
```vim
" --------------------------
" language specific settings
" --------------------------
" C/C++
"  Autoformatter on save (via brew install clang-format)
function! Formatonsave()
    let l:formatdiff = 1
    pyf clang-format/clang-format/share/clang/clang-format.py
endfunction
autocmd BufWritePre *.h,*.cc,*.cpp,*.c call Formatonsave()
```

where the path to `clang-format.py` might be specific to my machine.


## Future Work
note: some of these things may be coming later on in the tutorial, but I'm listing these ideas as I have them.
- [x] configurable row/column counts
- [ ] config file support
- [ ] use different makefile command for prod with fewer flags 
- [ ] refactor out into different sections
