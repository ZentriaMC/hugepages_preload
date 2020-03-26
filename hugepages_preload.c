/*
 * Copyright (c) 2020 Mark Vainomaa
 *
 * This source code is proprietary software and must not be distributed and/or copied without the express permission of Mark Vainomaa
 */
// nix-shell -p gcc --command 'gcc -shared -s -Wall -Werror hugepages_preload.c -ldl -o hugepages_preload.so'

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <mntent.h>

#include "hugepages_preload.h"

static long (*orig_sysconf)(int name) = NULL;
static FILE* (*orig_fopen)(const char *path, const char *mode) = NULL;
static uintmax_t determined_pagesize = 0;
static int DEBUGMODE = 0;

static void __attribute__((constructor)) sysconf_patch_initialize();

static inline void *get_sym(const char *name) {
  void *sym = NULL;
  if ((sym = dlsym(RTLD_NEXT, name)) == NULL) {
    fprintf(stderr, PREFIX " ERROR: failed to dlsym '%s' symbol\n", name);
    _exit(1);
  }
  return sym;
}

static int parse_humanreadable(char *s, uintmax_t *result) {
  char *endp = s;
  int sh = 0;
  errno = 0;
  uintmax_t x = strtoumax(s, &endp, 10);

  if (errno || endp == s) {
    goto error;
  }

  switch (*endp) {
    case 'K': sh = 10; break;
    case 'M': sh = 20; break;
    case 'G': sh = 30; break;
    case 0: sh = 0; break;
    default: goto error;
  }

  if (x > (SIZE_MAX >> sh))
    goto error;

  x <<= sh;
  *result = x;

  return 0;
 error:
  return -1;
}

static void sysconf_patch_initialize() {
  // Load symbols as we need them too
  orig_sysconf = get_sym("sysconf");
  orig_fopen = get_sym("fopen");

  // Debug mode
  const char *debug_envvar;
  if ((debug_envvar = getenv("HP_PRL_DEBUG")) != NULL && strcmp(debug_envvar, "1") == 0) {
    DEBUGMODE = 1;
  }

  // Determine huge page mount point and current system support
  const char *hugepage_mountpoint = NULL;
  uintmax_t hugepage_pagesize = -1;
  if ((hugepage_mountpoint = getenv("HP_PRL_HPP")) != NULL) {
    FILE *pf = setmntent("/proc/mounts", "r");
    if (pf == NULL) {
      fprintf(stderr, PREFIX " Unable to open /proc/mounts: %s\n", strerror(errno));
      _exit(1);
    }
    struct mntent *mnt;
    while ((mnt = getmntent(pf)) != NULL) {
      if (strcmp(mnt->mnt_fsname, "hugetlbfs") == 0) {
        if (DEBUGMODE) {
          fprintf(stderr, PREFIX " Found %s -> %s (%s)\n", mnt->mnt_fsname, mnt->mnt_dir, mnt->mnt_opts);
        }

        // Found the mount point, attempt to parse options
        if (strcmp(mnt->mnt_dir, hugepage_mountpoint) == 0) {
          char *dup_opts = strdup(mnt->mnt_opts);
          char *opts = dup_opts;
          char *found;

          // Search for pagesize=
          while ((found = strsep(&opts, ",")) != NULL) {
            if (DEBUGMODE) {
              fprintf(stderr, PREFIX " Option: %s\n", found);
            }
            if (strncmp(found, "pagesize=", 9) == 0) {
              char *raw_size = found + 9;
              uintmax_t parsed_size;
              if (parse_humanreadable(raw_size, &parsed_size) < 0) {
                fprintf(stderr, PREFIX " ERROR: failed to parse size '%s': %s\n", raw_size, strerror(errno));
                continue;
              }

              if (DEBUGMODE) {
                fprintf(stderr, PREFIX " Found: %s -> %lu\n", found, parsed_size);
              }
              hugepage_pagesize = parsed_size;
              break;
            }
          }

          free(dup_opts);
          opts = NULL;
          found = NULL;
          break;
        }
      }
    }
    endmntent(pf);
    mnt = NULL;
  }

  if (hugepage_pagesize == -1) {
    fprintf(stderr, PREFIX " ERROR: could not find specified hugepage mount\n");
    _exit(1);
  }

  // Check if system actually supports that page size
  char *fbuf = malloc(PATH_MAX + 1);
  snprintf(fbuf, PATH_MAX-1, "/sys/kernel/mm/hugepages/hugepages-%lukB/nr_hugepages", hugepage_pagesize >> 10);
  FILE *f = orig_fopen(fbuf, "r");
  if (!f) {
    fprintf(stderr, PREFIX " ERROR: Unable to check for given hugepage size support\n");
    _exit(1);
  }
  fclose(f);
  free(fbuf);

  // It's showtime
  fprintf(stderr, PREFIX " Initialized, reporting max page size %lu bytes\n", hugepage_pagesize);
  determined_pagesize = hugepage_pagesize;
}

static int rewrite_meminfo(uintmax_t bytes, char **result_path) {
  int ret = -1;
  char *buf = NULL, *fname = NULL;
  FILE *mi = NULL, *ti = NULL;

  if ((fname = malloc(PATH_MAX)) == NULL) {
    goto cleanup;
  }
  snprintf(fname, PATH_MAX-1, "/tmp/.rewritten-meminfoXXXXXX");
  if (mkstemp(fname) < 0) {
    goto cleanup;
  }

  if ((mi = orig_fopen("/proc/meminfo", "r")) == NULL) {
    goto cleanup;
  }
  if ((ti = orig_fopen(fname, "w")) == NULL) {
    goto cleanup;
  }

  buf = malloc(256);
  while (fgets(buf, 256, mi)) {
    if (strncmp(buf, "Hugepagesize:", 13) == 0) {
      fprintf(ti, "Hugepagesize:   %lu kB\n", bytes >> 10);
    } else {
      fprintf(ti, "%s\n", buf);
    }
  }

  ret = 0;
  *result_path = fname;

 cleanup:
  // NOTE: not freeing fname is intentional as it gets passed over
  if (buf != NULL) {
    free(buf);
  }
  if (mi != NULL) {
    fclose(mi);
  }
  if (ti != NULL) {
    fclose(ti);
  }

  return ret;
}

/* EXPORT */ long sysconf(int name) {
  if (name == _SC_PAGESIZE || name == _SC_PAGE_SIZE) {
    return (long) determined_pagesize;
  }
  return orig_sysconf(name);
}

/* EXPORT */ FILE *fopen(const char *path, const char *mode) {
  return fopen64(path, mode);
}

/* EXPORT */ FILE *fopen64(const char *path, const char *mode) {
  if (strcmp(path, "/proc/meminfo") == 0) {
    if (DEBUGMODE) {
      fprintf(stderr, PREFIX " Masking /proc/meminfo file\n");
    }

    char *filename = NULL;
    if (rewrite_meminfo(determined_pagesize, &filename) < 0) {
      fprintf(stderr, PREFIX " ERROR: failed to mask /proc/meminfo: %s\n", strerror(errno));
      return NULL;
    }

    FILE *f = orig_fopen(filename, mode);
    free(filename);
    return f;
  }
  return orig_fopen(path, mode);
}
