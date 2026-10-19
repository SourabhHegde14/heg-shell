# heg shell

`heg` is a Unix shell written from scratch in C (~3,700 lines, libc only, no readline).
It is a real scripting language and an interactive shell with job control and its own
line editor, built directly on `fork`, `execvp`, `waitpid`, `pipe`, `dup2`, `setpgid`,
`tcsetpgrp`, signals and `termios`.

It also ships **hegterm**, a Matrix-style terminal window of its own (SDL2), and **heg-lite** (`kernel/`), a 250-line freestanding core that runs on a hobby
kernel with only 12 syscalls. See [kernel/KERNEL_PORT.md](kernel/KERNEL_PORT.md).

```
heg:~/projects$ fact() { if [ $1 -le 1 ]; then echo 1; else echo $(( $1 * $(fact $(($1-1))) )); fi; }
heg:~/projects$ fact 10
3628800
heg:~/projects$ seq 1 100000 | grep 7 | sort -r | head -n 3
99997
99987
99979
heg:~/projects$ sleep 60 &
[1] 4821
heg:~/projects$ fg
sleep 60
^Z
[1]+  Stopped    sleep 60
```

## Features

| Area | Supported |
|---|---|
| Pipelines | `a \| b \| c` (up to 32 stages) |
| Redirection | `<` `>` `>>` `N>` `N>>` `N<` `N>&M` `&>`, applied strictly left to right; here-documents `<<EOF`, `<<'EOF'` |
| Lists | `;` `&&` `\|\|` with bash short-circuit semantics |
| Control flow | `if/elif/else/fi`, `while`, `until`, `for ... in`, `break [n]`, `continue [n]` |
| Functions | `name() { ...; }`, `return`, `local`, positional parameters `$1 $@ $* $# shift` |
| Grouping | subshells `( ... )`, groups `{ ...; }` |
| Expansion | quotes, `$VAR`, `${VAR}`, `${VAR:-x} ${VAR:=x} ${VAR:+x} ${#VAR}`, `$(cmd)`, backticks, `$((arith))`, `$? $$ $!`, `~`, globbing, field splitting |
| Assignments | `VAR=value`, one-shot `VAR=x cmd` |
| Background / jobs | `&`, `$!`, `wait`, process groups, Ctrl-C, Ctrl-Z, `jobs`, `fg`, `bg`, `kill %n` |
| Line editor | arrows, Home/End, Delete, Ctrl-A/E/K/U/W/L, history (persistent, `~/.heg_history`), **Tab completion** of commands, files, directories |
| Builtins | `cd` `pwd` `exit` `echo` `export` `unset` `jobs` `fg` `bg` `wait` `kill` `history` `source`/`.` `help` `true` `false` `:` `test`/`[` `read` `eval` `type` `shift` `local` `return` `break` `continue` |
| Scripting | `heg script.heg`, `heg -c 'cmd'`, stdin, `#` comments, multi-line input with `> ` continuation prompt |

## Build and run

```sh
make                 # builds ./heg
./heg                # interactive shell
./heg -c 'ls | wc -l'
make test            # differential tests vs bash + pty job-control and editor tests
make test-asan       # same suites under AddressSanitizer + UBSan
make hegl            # build the kernel-portable heg-lite
make test-lite       # heg-lite vs bash
make hegterm         # build the Matrix terminal window (needs libsdl2-dev libsdl2-ttf-dev)
make test-term       # screen-model tests + heg running inside a pty
make dist            # create heg.zip with everything
```

## hegterm: heg's own window

`./hegterm` opens a black window with neon-green text, falling digital rain, a soft glow and
CRT scanlines, and runs heg inside it. Everything is written from scratch except the font
rasteriser (SDL_ttf).

```sh
sudo apt install -y libsdl2-dev libsdl2-ttf-dev    # once
make hegterm && ./hegterm
./hegterm -s 22                # bigger font     ./hegterm --true-colors   # real ANSI colours
./hegterm --no-rain            # F2 toggles rain, F3 toggles glow + scanlines while running
./hegterm -- bash              # it is a general terminal: run any program inside it
```

Keys: Ctrl +/- zoom, mouse wheel or Shift+PgUp/PgDn scrollback (5,000 lines), Ctrl+Shift+V paste.

How it works:

```
keyboard --SDL--> bytes --> pty master ==== pty slave <-- heg
screen   <-- draw <-- cell grid <-- escape-sequence parser <-- bytes <-- pty master
```

- `term/term.c` is the **screen model**: a grid of cells, a byte-at-a-time state machine for
  ANSI/VT sequences (cursor movement, erase, insert/delete, scroll regions, 16/256/true colour,
  alternate screen, UTF-8, cursor position reports) and a scrollback ring. It has no SDL or OS
  calls, so it is tested headlessly, including 20,000 random byte strings that must never move
  the cursor off screen.
- `term/hegterm.c` is the **front-end**: `forkpty`, key-to-escape-sequence translation, glyph
  texture cache, resize handling (`TIOCSWINSZ` makes the kernel signal heg), and the Matrix theme.
  The theme maps every ANSI colour to a green with the same brightness order, so `ls --color`
  stays readable.
- This is the same job a kernel console driver does, which is why it pairs with the toy-kernel plan.

Not implemented yet: mouse text selection/copy, wide (CJK) characters, bold fonts beyond
SDL_ttf's synthetic bold, mouse reporting for full-screen apps.

## How it works

```
text --tokenize--> tokens --parse--> AST --expand + execute--> processes
```

1. **Tokenizer** splits words and operators, honours quotes, and reports *incomplete* input
   (open quote, trailing `|`, unclosed `if`, pending heredoc) so the REPL asks for more lines.
2. **Recursive-descent parser** builds lists, pipelines and compound commands (`if`, loops,
   functions, groups). It returns *ok / syntax error / incomplete*.
3. **Expansion happens at execution time** into a fresh `Exec` struct, never into the AST.
   That is why loop and function bodies can run again and `cd /tmp && echo $PWD` is right.
4. **Executor** forks one child per command, wires pipes, applies redirections in order.
   Builtins run in-process with fds saved and restored. Subshells and `$(...)` fork a child
   with job control switched off, so Ctrl-C reaches the processes inside.

**Pipelines:** `n-1` pipes, `n` forks; each child dups its ends onto 0/1, closes *every*
pipe fd (forgetting this makes pipelines hang), then execs. `yes | head -3` is in the tests
to catch fd leaks and SIGPIPE bugs.

**Job control:** one process group per pipeline; the foreground group owns the terminal
(`tcsetpgrp`); the shell ignores SIGINT/SIGTSTP/SIGTTIN/SIGTTOU while children reset them;
`waitpid(WUNTRACED)` detects stops; `setpgid` is called in both parent and child to avoid
the startup race; finished background jobs are reaped before each prompt.

**Line editor:** raw-mode `termios`, redraw from a single buffer, escape-sequence decoder,
history ring, completion by scanning `$PATH`/directory entries with longest-common-prefix
insertion. On dumb terminals or non-ttys it falls back to plain line reads.

## Testing

| Suite | What it checks |
|---|---|
| `test_shell.sh` | 90 checks, mostly differential: each snippet runs in bash and heg in a fresh temp dir, stdout + exit code must match. Plus scripts, stdin, `source`, syntax errors, exit codes 2/126/127. |
| `test_jobs.py` | Real pseudo-terminal: background job, `jobs`, `fg`, Ctrl-Z, `bg`, `kill %1`, Ctrl-C status 130, stopped multi-process pipeline. |
| `test_editor.py` | Real pty with `TERM=xterm`: history arrows, cursor editing, Ctrl shortcuts, Tab completion (unique, ambiguous, command, directory), Ctrl-C/Ctrl-D, continuation prompt. |
| `kernel/test_hegl.sh` | heg-lite vs bash. |
| `make test-term` | 33 checks on `term.c` (escape sequences, scrollback, colours, UTF-8, resize, random-byte fuzzing) plus heg running inside a pty and rendered by the screen model. |
| `make test-asan` | All of the above under AddressSanitizer + UBSan (clean). |

## Known limitations

- No `case`, `select`, arrays, `[[ ]]`, aliases, `set -e`, process substitution.
- `"$@"` does not yet expand to separate quoted words.
- No Ctrl-R reverse search; heredocs limited to 16 KiB.
- `a && b &` backgrounds only `b` (bash backgrounds the whole list).
- Variables live in the process environment (no shell-only variables).

## Next steps

1. **Fuzz it**: build with AFL++ and fuzz `tokenize`/`parse` (a natural bridge to security work).
2. `case`/`esac`, `"$@"`, aliases, `set -e`, Ctrl-R.
3. Boot heg-lite on your own kernel (see `kernel/KERNEL_PORT.md`).

## Files

```
heg.c                  the shell
Makefile               build, test, dist targets
test_shell.sh          differential tests vs bash
test_jobs.py           pty job-control tests
test_editor.py         pty line-editor tests
term/term.c            screen model + escape-sequence parser (headless)
term/hegterm.c         SDL2 window front-end, Matrix theme
term/test_term.c       tests for the above
kernel/hegl.c          freestanding kernel-portable shell core
kernel/hegl_sys.h      the 12-operation OS interface (POSIX + xv6)
kernel/test_hegl.sh    heg-lite tests
kernel/KERNEL_PORT.md  porting guide
```
