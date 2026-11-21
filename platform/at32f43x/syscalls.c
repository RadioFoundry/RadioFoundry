// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// newlib hooks: no file system or console, no heap (FreeRTOS has its own). Only libc's growing string buffers
// (asprintf) would call malloc, and nothing here uses them: it gets ENOMEM rather than RAM set aside for it
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
