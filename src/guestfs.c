/* libguestfs
 * Copyright (C) 2009-2010 Red Hat Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <config.h>

#define _BSD_SOURCE /* for mkdtemp, usleep */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <dirent.h>
#include <assert.h>

#include <rpc/types.h>
#include <rpc/xdr.h>

#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#ifdef HAVE_SYS_WAIT_H
#include <sys/wait.h>
#endif

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif

#include <arpa/inet.h>
#include <netinet/in.h>

#include "c-ctype.h"
#include "glthread/lock.h"
#include "hash.h"
#include "hash-pjw.h"

#include "guestfs.h"
#include "guestfs-internal.h"
#include "guestfs-internal-actions.h"
#include "guestfs_protocol.h"

static void default_error_cb (guestfs_h *g, void *data, const char *msg);
static void close_handles (void);

gl_lock_define_initialized (static, handles_lock);
static guestfs_h *handles = NULL;
static int atexit_handler_set = 0;

guestfs_h *
guestfs_create (void)
{
  guestfs_h *g;
  const char *str;

  g = malloc (sizeof (*g));
  if (!g) return NULL;

  memset (g, 0, sizeof (*g));

  g->state = CONFIG;

  g->fd[0] = -1;
  g->fd[1] = -1;
  g->sock = -1;

  g->abort_cb = abort;
  g->error_cb = default_error_cb;
  g->error_cb_data = NULL;

  g->recovery_proc = 1;

  str = getenv ("LIBGUESTFS_DEBUG");
  g->verbose = str != NULL && STREQ (str, "1");

  str = getenv ("LIBGUESTFS_TRACE");
  g->trace = str != NULL && STREQ (str, "1");

  str = getenv ("LIBGUESTFS_PATH");
  g->path = str != NULL ? strdup (str) : strdup (GUESTFS_DEFAULT_PATH);
  if (!g->path) goto error;

  str = getenv ("LIBGUESTFS_QEMU");
  g->qemu = str != NULL ? strdup (str) : strdup (QEMU);
  if (!g->qemu) goto error;

  str = getenv ("LIBGUESTFS_APPEND");
  if (str) {
    g->append = strdup (str);
    if (!g->append) goto error;
  }

  /* Choose a suitable memory size.  Previously we tried to choose
   * a minimal memory size, but this isn't really necessary since
   * recent QEMU and KVM don't do anything nasty like locking
   * memory into core any more.  Thus we can safely choose a
   * large, generous amount of memory, and it'll just get swapped
   * on smaller systems.
   */
  str = getenv ("LIBGUESTFS_MEMSIZE");
  if (str) {
    if (sscanf (str, "%d", &g->memsize) != 1 || g->memsize <= 256) {
      fprintf (stderr, "libguestfs: non-numeric or too small value for LIBGUESTFS_MEMSIZE\n");
      goto error;
    }
  } else
    g->memsize = 500;

  /* Start with large serial numbers so they are easy to spot
   * inside the protocol.
   */
  g->msg_next_serial = 0x00123400;

  /* Link the handles onto a global list. */
  gl_lock_lock (handles_lock);
  g->next = handles;
  handles = g;
  if (!atexit_handler_set) {
    atexit (close_handles);
    atexit_handler_set = 1;
  }
  gl_lock_unlock (handles_lock);

  if (g->verbose)
    fprintf (stderr, "new guestfs handle %p\n", g);

  return g;

 error:
  free (g->path);
  free (g->qemu);
  free (g->append);
  free (g);
  return NULL;
}

void
guestfs_close (guestfs_h *g)
{
  int i;
  char filename[256];
  guestfs_h *gg;

  if (g->state == NO_HANDLE) {
    /* Not safe to call 'error' here, so ... */
    fprintf (stderr, _("guestfs_close: called twice on the same handle\n"));
    return;
  }

  if (g->verbose)
    fprintf (stderr, "closing guestfs handle %p (state %d)\n", g, g->state);

  /* Run user close callback before anything else. */
  if (g->close_cb)
    g->close_cb (g, g->close_cb_data);

  guestfs___free_inspect_info (g);

  /* Try to sync if autosync flag is set. */
  if (g->autosync && g->state == READY) {
    guestfs_umount_all (g);
    guestfs_sync (g);
  }

  /* Remove any handlers that might be called back before we kill the
   * subprocess.
   */
  g->log_message_cb = NULL;

  if (g->state != CONFIG)
    guestfs_kill_subprocess (g);

  /* Close sockets. */
  if (g->fd[0] >= 0)
    close (g->fd[0]);
  if (g->fd[1] >= 0)
    close (g->fd[1]);
  if (g->sock >= 0)
    close (g->sock);
  g->fd[0] = -1;
  g->fd[1] = -1;
  g->sock = -1;

  /* Wait for subprocess(es) to exit. */
  waitpid (g->pid, NULL, 0);
  if (g->recoverypid > 0) waitpid (g->recoverypid, NULL, 0);

  /* Remove tmpfiles. */
  if (g->tmpdir) {
    snprintf (filename, sizeof filename, "%s/sock", g->tmpdir);
    unlink (filename);

    rmdir (g->tmpdir);

    free (g->tmpdir);
  }

  if (g->cmdline) {
    for (i = 0; i < g->cmdline_size; ++i)
      free (g->cmdline[i]);
    free (g->cmdline);
  }

  /* Mark the handle as dead before freeing it. */
  g->state = NO_HANDLE;

  gl_lock_lock (handles_lock);
  if (handles == g)
    handles = g->next;
  else {
    for (gg = handles; gg->next != g; gg = gg->next)
      ;
    gg->next = g->next;
  }
  gl_lock_unlock (handles_lock);

  if (g->pda)
    hash_free (g->pda);
  free (g->last_error);
  free (g->path);
  free (g->qemu);
  free (g->append);
  free (g->qemu_help);
  free (g->qemu_version);
  free (g);
}

/* Close all open handles (called from atexit(3)). */
static void
close_handles (void)
{
  while (handles) guestfs_close (handles);
}

const char *
guestfs_last_error (guestfs_h *g)
{
  return g->last_error;
}

static void
set_last_error (guestfs_h *g, const char *msg)
{
  free (g->last_error);
  g->last_error = strdup (msg);
}

static void
default_error_cb (guestfs_h *g, void *data, const char *msg)
{
  fprintf (stderr, _("libguestfs: error: %s\n"), msg);
}

void
guestfs_error (guestfs_h *g, const char *fs, ...)
{
  va_list args;
  char *msg;

  va_start (args, fs);
  int err = vasprintf (&msg, fs, args);
  va_end (args);

  if (err < 0) return;

  if (g->error_cb) g->error_cb (g, g->error_cb_data, msg);
  set_last_error (g, msg);

  free (msg);
}

void
guestfs_perrorf (guestfs_h *g, const char *fs, ...)
{
  va_list args;
  char *msg;
  int errnum = errno;

  va_start (args, fs);
  int err = vasprintf (&msg, fs, args);
  va_end (args);

  if (err < 0) return;

#if !defined(_GNU_SOURCE) || defined(__APPLE__)
  char buf[256];
  strerror_r (errnum, buf, sizeof buf);
#else
  char _buf[256];
  char *buf;
  buf = strerror_r (errnum, _buf, sizeof _buf);
#endif

  msg = safe_realloc (g, msg, strlen (msg) + 2 + strlen (buf) + 1);
  strcat (msg, ": ");
  strcat (msg, buf);

  if (g->error_cb) g->error_cb (g, g->error_cb_data, msg);
  set_last_error (g, msg);

  free (msg);
}

void *
guestfs_safe_malloc (guestfs_h *g, size_t nbytes)
{
  void *ptr = malloc (nbytes);
  if (nbytes > 0 && !ptr) g->abort_cb ();
  return ptr;
}

/* Return 1 if an array of N objects, each of size S, cannot exist due
   to size arithmetic overflow.  S must be positive and N must be
   nonnegative.  This is a macro, not an inline function, so that it
   works correctly even when SIZE_MAX < N.

   By gnulib convention, SIZE_MAX represents overflow in size
   calculations, so the conservative dividend to use here is
   SIZE_MAX - 1, since SIZE_MAX might represent an overflowed value.
   However, malloc (SIZE_MAX) fails on all known hosts where
   sizeof (ptrdiff_t) <= sizeof (size_t), so do not bother to test for
   exactly-SIZE_MAX allocations on such hosts; this avoids a test and
   branch when S is known to be 1.  */
# define xalloc_oversized(n, s) \
    ((size_t) (sizeof (ptrdiff_t) <= sizeof (size_t) ? -1 : -2) / (s) < (n))

/* Technically we should add an autoconf test for this, testing for the desired
   functionality, like what's done in gnulib, but for now, this is fine.  */
#if defined(__GLIBC__)
#define HAVE_GNU_CALLOC (__GLIBC__ >= 2)
#else
#define HAVE_GNU_CALLOC 0
#endif

/* Allocate zeroed memory for N elements of S bytes, with error
   checking.  S must be nonzero.  */
void *
guestfs_safe_calloc (guestfs_h *g, size_t n, size_t s)
{
  /* From gnulib's calloc function in xmalloc.c.  */
  void *p;
  /* Test for overflow, since some calloc implementations don't have
     proper overflow checks.  But omit overflow and size-zero tests if
     HAVE_GNU_CALLOC, since GNU calloc catches overflow and never
     returns NULL if successful.  */
  if ((! HAVE_GNU_CALLOC && xalloc_oversized (n, s))
      || (! (p = calloc (n, s)) && (HAVE_GNU_CALLOC || n != 0)))
    g->abort_cb ();
  return p;
}

void *
guestfs_safe_realloc (guestfs_h *g, void *ptr, int nbytes)
{
  void *p = realloc (ptr, nbytes);
  if (nbytes > 0 && !p) g->abort_cb ();
  return p;
}

char *
guestfs_safe_strdup (guestfs_h *g, const char *str)
{
  char *s = strdup (str);
  if (!s) g->abort_cb ();
  return s;
}

char *
guestfs_safe_strndup (guestfs_h *g, const char *str, size_t n)
{
  char *s = strndup (str, n);
  if (!s) g->abort_cb ();
  return s;
}

void *
guestfs_safe_memdup (guestfs_h *g, void *ptr, size_t size)
{
  void *p = malloc (size);
  if (!p) g->abort_cb ();
  memcpy (p, ptr, size);
  return p;
}

void
guestfs_set_out_of_memory_handler (guestfs_h *g, guestfs_abort_cb cb)
{
  g->abort_cb = cb;
}

guestfs_abort_cb
guestfs_get_out_of_memory_handler (guestfs_h *g)
{
  return g->abort_cb;
}

void
guestfs_set_error_handler (guestfs_h *g, guestfs_error_handler_cb cb, void *data)
{
  g->error_cb = cb;
  g->error_cb_data = data;
}

guestfs_error_handler_cb
guestfs_get_error_handler (guestfs_h *g, void **data_rtn)
{
  if (data_rtn) *data_rtn = g->error_cb_data;
  return g->error_cb;
}

int
guestfs__set_verbose (guestfs_h *g, int v)
{
  g->verbose = !!v;
  return 0;
}

int
guestfs__get_verbose (guestfs_h *g)
{
  return g->verbose;
}

int
guestfs__set_autosync (guestfs_h *g, int a)
{
  g->autosync = !!a;
  return 0;
}

int
guestfs__get_autosync (guestfs_h *g)
{
  return g->autosync;
}

int
guestfs__set_path (guestfs_h *g, const char *path)
{
  free (g->path);
  g->path = NULL;

  g->path =
    path == NULL ?
    safe_strdup (g, GUESTFS_DEFAULT_PATH) : safe_strdup (g, path);
  return 0;
}

const char *
guestfs__get_path (guestfs_h *g)
{
  return g->path;
}

int
guestfs__set_qemu (guestfs_h *g, const char *qemu)
{
  free (g->qemu);
  g->qemu = NULL;

  g->qemu = qemu == NULL ? safe_strdup (g, QEMU) : safe_strdup (g, qemu);
  return 0;
}

const char *
guestfs__get_qemu (guestfs_h *g)
{
  return g->qemu;
}

int
guestfs__set_append (guestfs_h *g, const char *append)
{
  free (g->append);
  g->append = NULL;

  g->append = append ? safe_strdup (g, append) : NULL;
  return 0;
}

const char *
guestfs__get_append (guestfs_h *g)
{
  return g->append;
}

int
guestfs__set_memsize (guestfs_h *g, int memsize)
{
  g->memsize = memsize;
  return 0;
}

int
guestfs__get_memsize (guestfs_h *g)
{
  return g->memsize;
}

int
guestfs__set_selinux (guestfs_h *g, int selinux)
{
  g->selinux = selinux;
  return 0;
}

int
guestfs__get_selinux (guestfs_h *g)
{
  return g->selinux;
}

int
guestfs__get_pid (guestfs_h *g)
{
  if (g->pid > 0)
    return g->pid;
  else {
    error (g, "get_pid: no qemu subprocess");
    return -1;
  }
}

struct guestfs_version *
guestfs__version (guestfs_h *g)
{
  struct guestfs_version *r;

  r = safe_malloc (g, sizeof *r);
  r->major = PACKAGE_VERSION_MAJOR;
  r->minor = PACKAGE_VERSION_MINOR;
  r->release = PACKAGE_VERSION_RELEASE;
  r->extra = safe_strdup (g, PACKAGE_VERSION_EXTRA);
  return r;
}

int
guestfs__set_trace (guestfs_h *g, int t)
{
  g->trace = !!t;
  return 0;
}

int
guestfs__get_trace (guestfs_h *g)
{
  return g->trace;
}

int
guestfs__set_direct (guestfs_h *g, int d)
{
  g->direct = !!d;
  return 0;
}

int
guestfs__get_direct (guestfs_h *g)
{
  return g->direct;
}

int
guestfs__set_recovery_proc (guestfs_h *g, int f)
{
  g->recovery_proc = !!f;
  return 0;
}

int
guestfs__get_recovery_proc (guestfs_h *g)
{
  return g->recovery_proc;
}

int
guestfs__set_network (guestfs_h *g, int v)
{
  g->enable_network = !!v;
  return 0;
}

int
guestfs__get_network (guestfs_h *g)
{
  return g->enable_network;
}

void
guestfs_set_log_message_callback (guestfs_h *g,
                                  guestfs_log_message_cb cb, void *opaque)
{
  g->log_message_cb = cb;
  g->log_message_cb_data = opaque;
}

void
guestfs_set_subprocess_quit_callback (guestfs_h *g,
                                      guestfs_subprocess_quit_cb cb, void *opaque)
{
  g->subprocess_quit_cb = cb;
  g->subprocess_quit_cb_data = opaque;
}

void
guestfs_set_launch_done_callback (guestfs_h *g,
                                  guestfs_launch_done_cb cb, void *opaque)
{
  g->launch_done_cb = cb;
  g->launch_done_cb_data = opaque;
}

void
guestfs_set_close_callback (guestfs_h *g,
                            guestfs_close_cb cb, void *opaque)
{
  g->close_cb = cb;
  g->close_cb_data = opaque;
}

void
guestfs_set_progress_callback (guestfs_h *g,
                               guestfs_progress_cb cb, void *opaque)
{
  g->progress_cb = cb;
  g->progress_cb_data = opaque;
}

/* Note the private data area is allocated lazily, since the vast
 * majority of callers will never use it.  This means g->pda is
 * likely to be NULL.
 */
struct pda_entry {
  char *key;                    /* key */
  void *data;                   /* opaque user data pointer */
};

static size_t
hasher (void const *x, size_t table_size)
{
  struct pda_entry const *p = x;
  return hash_pjw (p->key, table_size);
}

static bool
comparator (void const *x, void const *y)
{
  struct pda_entry const *a = x;
  struct pda_entry const *b = y;
  return STREQ (a->key, b->key);
}

static void
freer (void *x)
{
  if (x) {
    struct pda_entry *p = x;
    free (p->key);
    free (p);
  }
}

void
guestfs_set_private (guestfs_h *g, const char *key, void *data)
{
  if (g->pda == NULL) {
    g->pda = hash_initialize (16, NULL, hasher, comparator, freer);
    if (g->pda == NULL)
      g->abort_cb ();
  }

  struct pda_entry *new_entry = safe_malloc (g, sizeof *new_entry);
  new_entry->key = safe_strdup (g, key);
  new_entry->data = data;

  struct pda_entry *old_entry = hash_delete (g->pda, new_entry);
  freer (old_entry);

  struct pda_entry *entry = hash_insert (g->pda, new_entry);
  if (entry == NULL)
    g->abort_cb ();
  assert (entry == new_entry);
}

static inline char *
bad_cast (char const *s)
{
  return (char *) s;
}

void *
guestfs_get_private (guestfs_h *g, const char *key)
{
  if (g->pda == NULL)
    return NULL;                /* no keys have been set */

  const struct pda_entry k = { .key = bad_cast (key) };
  struct pda_entry *entry = hash_lookup (g->pda, &k);
  if (entry)
    return entry->data;
  else
    return NULL;
}

/* When tracing, be careful how we print BufferIn parameters which
 * usually contain large amounts of binary data (RHBZ#646822).
 */
void
guestfs___print_BufferIn (FILE *out, const char *buf, size_t buf_size)
{
  size_t i;
  size_t orig_size = buf_size;

  if (buf_size > 256)
    buf_size = 256;

  fputc ('"', out);

  for (i = 0; i < buf_size; ++i) {
    if (c_isprint (buf[i]))
      fputc (buf[i], out);
    else
      fprintf (out, "\\x%02x", (unsigned char) buf[i]);
  }

  fputc ('"', out);

  if (orig_size > buf_size)
    fprintf (out,
             _("<truncated, original size %zu bytes>"), orig_size);
}
