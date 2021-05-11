## malt

A malt-flavored terminal multiplexer.  Built to better understand how [tmux](http://github.com/tmux/tmux) and [screen](https://www.gnu.org/software/screen/) work, and implements a simplified VT100 terminal emulator.

In its current state, `malt` is not ready for heavy use.  While the internals are there, it still needs many small improvements to usability.

### Demo

![Demo](https://github.com/Cubified/malt/blob/main/malt.png)

### Compiling and Running

`malt` has no dependencies other than the C standard library, meaning it can be compiled and run with:

     $ make
     $ ./malt

### Using

`malt`'s magic key is `Ctrl-A`, meaning it can be followed by:
 - `c`: Create a new window
 - `q`: Exit

### To-Do

- Terminal scrollback
- Fix escape code handling on full buffer
- Fix off-by-one errors due to 0- versus 1-indexed arrays
- More keybinds
