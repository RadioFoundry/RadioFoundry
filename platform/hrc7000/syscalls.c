// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// newlib hooks: no file system or console, no heap (FreeRTOS has its own). The csky-elf newlib from
// tools/build-csky-toolchain.sh calls the plain names (close, write, ...) from its reentrant wrappers, the newlib of
// the ARM toolchain the underscore names, so both are defined; without them snprintf's stdio pulls in libnosys,
// whose stubs warn at link time.
#include <errno.h>
#include <stddef.h>
#include <sys/stat.h>

int _close(int fd) { (void)fd; return -1; }
int _lseek(int fd, int off, int whence) { (void)fd; (void)off; (void)whence; return 0; }
int _read(int fd, char *buf, int len) { (void)fd; (void)buf; (void)len; return 0; }
int _write(int fd, const char *buf, int len) { (void)fd; (void)buf; return len; }
int _fstat(int fd, struct stat *st) { (void)fd; st->st_mode = S_IFCHR; return 0; }
int _isatty(int fd) { (void)fd; return 1; }
void *_sbrk(ptrdiff_t incr) { (void)incr; errno = ENOMEM; return (void *)-1; }
int _kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }
int _getpid(void) { return 1; }

int close(int fd);
int lseek(int fd, int off, int whence);
int read(int fd, char *buf, int len);
int write(int fd, const char *buf, int len);
int fstat(int fd, struct stat *st);
int isatty(int fd);
void *sbrk(ptrdiff_t incr);
int kill(int pid, int sig);
int getpid(void);
void _exit(int status);

int close(int fd) { return _close(fd); }
int lseek(int fd, int off, int whence) { return _lseek(fd, off, whence); }
int read(int fd, char *buf, int len) { return _read(fd, buf, len); }
int write(int fd, const char *buf, int len) { return _write(fd, buf, len); }
int fstat(int fd, struct stat *st) { return _fstat(fd, st); }
int isatty(int fd) { return _isatty(fd); }
void *sbrk(ptrdiff_t incr) { return _sbrk(incr); }
int kill(int pid, int sig) { return _kill(pid, sig); }
int getpid(void) { return _getpid(); }

// abort() ends here (newlib's assert from its own dtoa); stop in place for the debugger
void _exit(int status)
{
	(void)status;
	for (;;) {
	}
}
