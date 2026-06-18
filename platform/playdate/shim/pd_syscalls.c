/** @file pd_syscalls.c
 *
 *  Minimal newlib syscall stubs for the Playdate *device* (armgcc) build.
 *
 *  open8's cart loader and stb_image reference the stdio/POSIX layer
 *  (fopen/fread/...). On Playdate we never use them — carts are loaded from an
 *  embedded byte array and file access (later) goes through pd->file, not POSIX.
 *  But newlib still pulls these symbols at link time, so provide no-op stubs.
 *  Memory (malloc/realloc/free) is supplied by the SDK's setup.c.
 *
 *  Device-only; the simulator build links the host libc instead.
 *
 *  SPDX-License-Identifier: MIT
 **/
#include <sys/stat.h>
#include <sys/times.h>
#include <errno.h>

void _exit(int code) { (void)code; for (;;) {} }
int  _close(int fd) { (void)fd; return -1; }
int  _fstat(int fd, struct stat* st) { (void)fd; st->st_mode = S_IFCHR; return 0; }
int  _isatty(int fd) { (void)fd; return 1; }
int  _lseek(int fd, int offset, int whence) { (void)fd; (void)offset; (void)whence; return 0; }
int  _read(int fd, char* ptr, int len) { (void)fd; (void)ptr; (void)len; return 0; }
int  _write(int fd, char* ptr, int len) { (void)fd; (void)ptr; return len; }
int  _open(const char* path, int flags, int mode) { (void)path; (void)flags; (void)mode; return -1; }
int  _kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }
int  _getpid(void) { return 1; }
int  _gettimeofday(struct timeval* tv, void* tz) { (void)tv; (void)tz; return 0; }
