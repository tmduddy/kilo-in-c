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

Note: I initially tried this the "easy way" with `brew install clang-format`, which worked for the purposes of installation and running manually in the CLI, but didn't expose the `clang-format.py` file required for the Vim integration anywhere I could find, so I deleted that and re-did it from scratch like this.

Note: After my later update to use the vim-clang-format extension, I probably could have / should have just used the brew installation. I might go back to that.

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

However I was getting repeated python errors, and seeing that perhaps a lot more tinkering would be required to get this to work with a modern, python3 enabled vim. 

After much trial and tribulation I installed the vim-clang-format plugin from rhysd [here](https://github.com/rhysd/vim-clang-format/) and was able to get this to work as an autoformatter:

```vim
" --------------------------
" language specific settings
" --------------------------
" Enable auto formatting from the clang-format-vim plugin
g:clang_format#code_style='llvm'
autocmd FileType c ClangFormatAutoEnable
```

and finally now the code formats on save


## Future Work
Note: some of these things may be coming later on in the tutorial, but I'm listing these ideas as I have them.
- [x] configurable row/column counts
- [ ] config file support
- [ ] use different makefile command for prod with fewer flags 
- [ ] refactor out into different sections


## Some musings on learnings
This has been a really fun project for me despite it just being a line-for-line copy from a tutorial. Working to add (overly?) verbose comments to every new section was really helpful in learning, and around chapter 4 or definitely chapter 5 I was able to write those comments quite accurately without having to read the explanation from the tutorial, which felt like a real Learning Goal Achievement.

Another piece of this that's been really enjoyable for me is the amount of tinkering I've been free to do. A sort of secondary goal of this whole thing, and a pattern I've found I tend to follow in a lot of similar side projects, is getting better and better with Vim. I've been interested in Vim since it was first introduced to me in college, and while I've played around with a handful of configuration options and even a plugin or two over the years, it's only in the past year or so that I feel like I've gotten "proficient." 
This project in particular has helped with that - I set a rule for myself to only use Vim for editing all of these files, but allowed myself the freedome to spend time finding the "right" solutions to all of the painpoints that introduced.
- Autoformatting took a while (see above)
- Intellisense-like was easier than expected using CoC, but took a lot of mostly-undocumented configuration options to get into a state I like. 

It still isn't a VSCode like experience, but I didn't really want it to be, I want a Vim experience, and I think I've gotten that.















