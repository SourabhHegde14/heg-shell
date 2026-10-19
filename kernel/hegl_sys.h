/*
 * hegl_sys.h -- the ENTIRE interface between the hegl shell and its operating
 * system. If your kernel provides these ~12 operations, hegl runs on it.
 *
 *   process : hs_fork  hs_exec  hs_wait  hs_exit
 *   files   : hs_open  hs_close hs_read  hs_write hs_pipe hs_dup_to
 *   misc    : hs_chdir hs_getcwd hs_isatty hs_default_path
 *
 * hegl.c includes nothing else: no stdio, no malloc, no string.h, no signals,
 * no environment, no termios. All memory is static.
 *
 * Two implementations follow:
 *   HEGL_XV6   xv6-style user library (also the template for your own kernel)
 *   default    POSIX, so the shell can be developed and tested on Linux/macOS
 *
 * Contract:
 *   hs_fork()              child gets 0, parent gets child id, -1 on error
 *   hs_exec(path, argv)    replaces the process image; returns only on failure
 *   hs_wait(&status)       waits for ANY child; returns its id (or -1 if none)
 *                          and stores its exit status (0..255)
 *   hs_exit(status)        never returns
 *   hs_open(path, mode)    mode: HS_RDONLY, HS_TRUNC (create+truncate for
 *                          writing), HS_APPEND (create, append); returns the
 *                          LOWEST free fd, or -1
 *   hs_dup_to(src, dst)    make dst refer to what src refers to; 0 / -1
 *   hs_pipe(fds)           fds[0] = read end, fds[1] = write end; 0 / -1
 *   hs_read / hs_write     return byte count, 0 at EOF, -1 on error
 *   hs_chdir(path)         0 / -1
 *   hs_getcwd(buf, n)      0 / -1 (kernels without a cwd query may return -1)
 *   hs_isatty(fd)          1 if fd is the console (hegl then prints a prompt)
 *   hs_default_path()      initial value of $PATH, e.g. "/bin:/"
 */
#ifndef HEGL_SYS_H
#define HEGL_SYS_H

#define HS_RDONLY 0
#define HS_TRUNC 1
#define HS_APPEND 2

#ifdef HEGL_XV6
/* ------------------------------------------------------------------ */
/* xv6 (RISC-V flavour: exit(int), wait(int *)). On the original x86   */
/* xv6 change hs_exit/hs_wait as noted below.                          */
/* ------------------------------------------------------------------ */
#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"

static inline int hs_fork(void) { return fork(); }
static inline void hs_exit(int status) __attribute__((noreturn));
static inline void hs_exit(int status) { exit(status); } /* x86 xv6: exit(); */
static inline int hs_wait(int *status) { return wait(status); } /* x86 xv6: wait() has no argument; status = 0 */
static inline int hs_exec(const char *path, char *const argv[]) { return exec((char *)path, (char **)argv); }
static inline int hs_open(const char *path, int mode) {
    if (mode == HS_RDONLY) return open((char *)path, O_RDONLY);
    if (mode == HS_TRUNC) return open((char *)path, O_WRONLY | O_CREATE | O_TRUNC);
    return -1; /* xv6 has no O_APPEND and no lseek, so ">>" is unsupported */
}
static inline int hs_close(int fd) { return close(fd); }
static inline int hs_read(int fd, void *buf, int n) { return read(fd, buf, n); }
static inline int hs_write(int fd, const void *buf, int n) { return write(fd, buf, n); }
static inline int hs_pipe(int fds[2]) { return pipe(fds); }
/* xv6 has no dup2: close dst, then dup() returns the lowest free fd == dst */
static inline int hs_dup_to(int src, int dst) {
    if (src == dst) return dst;
    close(dst);
    return dup(src) == dst ? dst : -1;
}
static inline int hs_chdir(const char *path) { return chdir((char *)path); }
static inline int hs_getcwd(char *buf, int n) { (void)buf; (void)n; return -1; }
static inline int hs_isatty(int fd) { (void)fd; return 1; } /* the console is the only stdin */
static inline const char *hs_default_path(void) { return "/:/bin"; }

#else
/* ------------------------------------------------------------------ */
/* POSIX: development and testing on Linux / macOS                     */
/* ------------------------------------------------------------------ */
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

static inline int hs_fork(void) { return (int)fork(); }
static inline void hs_exit(int status) __attribute__((noreturn));
static inline void hs_exit(int status) { _exit(status); }
static inline int hs_wait(int *status) {
    int st, p;
    while ((p = (int)wait(&st)) < 0 && errno == EINTR) {}
    if (p < 0) return -1;
    *status = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    return p;
}
static inline int hs_exec(const char *path, char *const argv[]) { return execv(path, argv); }
static inline int hs_open(const char *path, int mode) {
    if (mode == HS_RDONLY) return open(path, O_RDONLY);
    if (mode == HS_TRUNC) return open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    return open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
}
static inline int hs_close(int fd) { return close(fd); }
static inline int hs_read(int fd, void *buf, int n) { return (int)read(fd, buf, (size_t)n); }
static inline int hs_write(int fd, const void *buf, int n) { return (int)write(fd, buf, (size_t)n); }
static inline int hs_pipe(int fds[2]) { return pipe(fds); }
static inline int hs_dup_to(int src, int dst) { return src == dst ? dst : (dup2(src, dst) < 0 ? -1 : dst); }
static inline int hs_chdir(const char *path) { return chdir(path); }
static inline int hs_getcwd(char *buf, int n) { return getcwd(buf, (size_t)n) ? 0 : -1; }
static inline int hs_isatty(int fd) { return isatty(fd); }
static inline const char *hs_default_path(void) {
    const char *p = getenv("PATH");
    return p ? p : "/usr/bin:/bin";
}
#endif

#endif /* HEGL_SYS_H */
