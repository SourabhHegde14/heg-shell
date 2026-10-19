# Running heg on your own kernel

heg comes in two layers:

| | `heg.c` (full shell) | `kernel/hegl.c` (heg-lite) |
|---|---|---|
| Needs | a POSIX OS (signals, termios, job control, libc) | **12 operations**, nothing else |
| Features | scripting, functions, job control, line editor, globbing | pipes, `< > >>`, `&`, `;`, quotes, PATH, `cd pwd exit help` |
| Memory | malloc | static buffers only |
| Size | ~3k lines | ~250 lines |

Develop on the full shell, bring up a kernel with heg-lite. The two share the
same fork/exec/pipe/dup structure, so everything you learn transfers.

## The contract (`kernel/hegl_sys.h`)

| Operation | Kernel syscall you must provide |
|---|---|
| `hs_fork` | `fork` |
| `hs_exec(path, argv)` | `exec` / `execve` |
| `hs_wait(&status)` | `wait` (any child) |
| `hs_exit(code)` | `exit` |
| `hs_open(path, mode)` | `open` (read / create+truncate / append), returns **lowest free fd** |
| `hs_close`, `hs_read`, `hs_write` | `close`, `read`, `write` |
| `hs_pipe(fds)` | `pipe` |
| `hs_dup_to(src, dst)` | `dup2`, or `close(dst); dup(src)` |
| `hs_chdir`, `hs_getcwd` | `chdir`, optional `getcwd` |
| `hs_isatty(fd)` | "is this the console?" |

Your toy kernel also needs: a console driver that **echoes** typed characters
and delivers input line by line (the shell reads one byte at a time and does
no editing), and a filesystem with `/bin` holding programs.

## Porting steps

1. **POSIX baseline.** `make test-lite` builds `kernel/hegl` and diffs it
   against bash. Fix anything here first, where debugging is easy.
2. **xv6 first (recommended stepping stone).** Copy `hegl.c` and `hegl_sys.h`
   into xv6's `user/`, add `_hegl` to `UPROGS`, compile with `-DHEGL_XV6`.
   `hegl_sys.h` already maps every operation to xv6 calls.
   - xv6 has no `O_APPEND`, so `>>` fails with a message (add lseek-to-end to fix).
   - On the original x86 xv6, `exit()` and `wait()` take no arguments; the two
     places to change are marked in the header.
3. **Your own kernel.** Provide the syscalls above plus a tiny user-space
   start file (`_start` calls `main`, then `exit`). Replace the `HEGL_XV6`
   block in `hegl_sys.h` with raw syscall stubs for your ABI, e.g.
   `static inline int hs_write(int fd, const void *b, int n) { return syscall3(SYS_WRITE, fd, b, n); }`
4. **Bring-up order that works:** `hs_write` (print the prompt) -> `hs_read`
   -> `hs_fork`/`hs_exec`/`hs_wait` (run `echo`) -> `hs_pipe`+`hs_dup_to`
   (pipelines) -> `hs_open` (redirects).

## Common pitfalls

- `open` must return the lowest free descriptor, or redirects land on the wrong fd.
- A pipeline hangs forever when some process still holds a **write end** open:
  the reader never sees EOF. heg-lite closes unused pipe ends in parent and
  child; your kernel must also release descriptors on `exit` and `exec`.
- `wait` must return `-1` when there are no children, or the wait loop spins.
- Reaped statuses: heg-lite expects 0..255 in `*status`; adapt in `hs_wait` if
  your kernel packs the status differently.

## Why this matters

The shell is the first program that exercises almost every part of a kernel:
process creation, address-space replacement, file descriptors, pipes, the
scheduler and blocking I/O. If heg-lite runs a three-stage pipeline on your
kernel, your process and file layers work.
