/*
 * LD_PRELOAD fault injector for fsync(2)/fdatasync(2).
 *
 * Environment:
 *   INJECT_EIO_AFTER_COUNT=N   The first N calls behave normally; call N+1
 *                              fails. Unset or negative: never inject.
 *   INJECT_EIO_ERRNO=EIO       Errno to report: EIO (default), ENOSPC, or a
 *                              number.
 *   INJECT_EIO_STICKY=1        Keep failing every later call too. The
 *                              default fails exactly once, which mirrors the
 *                              Linux behaviour behind "fsyncgate": the error
 *                              is reported once, the dirty pages are marked
 *                              clean, and a retry then "succeeds" without the
 *                              data ever reaching the disk.
 *   INJECT_EIO_CALL_REAL=1     Still perform the real sync before reporting
 *                              the error (data may or may not be durable).
 *                              Default 0: the failed sync is skipped.
 *   INJECT_EIO_VERBOSE=1       Log every intercepted call to stderr.
 *
 * Only the libc wrappers are intercepted; IORING_OP_FSYNC is not, which is
 * why lsmkv always issues its durability barriers through libc.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dlfcn.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef int (*sync_fn)(int);

static atomic_long g_calls;
static long g_after = -1;
static int g_errno = EIO;
static int g_sticky = 0;
static int g_call_real = 0;
static int g_verbose = 0;

static long env_long(const char *name, long dflt) {
  const char *v = getenv(name);
  if (v == NULL || *v == '\0') return dflt;
  char *end = NULL;
  long r = strtol(v, &end, 10);
  return (end != NULL && *end == '\0') ? r : dflt;
}

__attribute__((constructor)) static void eio_shim_init(void) {
  g_after = env_long("INJECT_EIO_AFTER_COUNT", -1);
  g_sticky = env_long("INJECT_EIO_STICKY", 0) != 0;
  g_call_real = env_long("INJECT_EIO_CALL_REAL", 0) != 0;
  g_verbose = env_long("INJECT_EIO_VERBOSE", 0) != 0;
  const char *e = getenv("INJECT_EIO_ERRNO");
  if (e != NULL) {
    if (strcmp(e, "EIO") == 0) g_errno = EIO;
    else if (strcmp(e, "ENOSPC") == 0) g_errno = ENOSPC;
    else if (strcmp(e, "EDQUOT") == 0) g_errno = EDQUOT;
    else g_errno = (int)env_long("INJECT_EIO_ERRNO", EIO);
  }
}

static void say(const char *what, int fd, long n, int injected) {
  if (!g_verbose) return;
  char buf[128];
  int len = snprintf(buf, sizeof(buf), "[eio_shim] %s(fd=%d) call #%ld%s\n", what, fd, n,
                     injected ? " -> INJECTED FAILURE" : "");
  if (len > 0) (void)!write(STDERR_FILENO, buf, (size_t)len);
}

static sync_fn resolve(const char *name) {
  /* ISO C has no object->function pointer cast; copy the bits instead. */
  void *sym = dlsym(RTLD_NEXT, name);
  sync_fn fn;
  memcpy(&fn, &sym, sizeof(fn));
  return fn;
}

static int intercept(const char *what, sync_fn real, int fd) {
  const long n = atomic_fetch_add(&g_calls, 1) + 1;
  const int inject = g_after >= 0 && (n == g_after + 1 || (g_sticky && n > g_after + 1));
  say(what, fd, n, inject);
  if (!inject) return real(fd);
  if (g_call_real) (void)real(fd);
  errno = g_errno;
  return -1;
}

int fsync(int fd) {
  static sync_fn real;
  if (real == NULL) real = resolve("fsync");
  return intercept("fsync", real, fd);
}

int fdatasync(int fd) {
  static sync_fn real;
  if (real == NULL) real = resolve("fdatasync");
  return intercept("fdatasync", real, fd);
}

/* Lets a harness ask how many syncs the process has issued so far. */
long eio_shim_call_count(void) { return atomic_load(&g_calls); }
