/* -*- mode: C; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: expandtab:ts=8:sw=4:softtabstop=4:
#ident "Copyright (c) 2007-2011 Tokutek Inc.  All rights reserved."

#include <config.h>

#include <toku_portability.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#if defined(HAVE_MALLOC_H)
# include <malloc.h>
#elif defined(HAVE_SYS_MALLOC_H)
# include <sys/malloc.h>
#endif
#include <dlfcn.h>
#include <valgrind/helgrind.h>
#include <sys/mman.h>
#include "memory.h"
#include "toku_assert.h"

static malloc_fun_t  t_malloc  = 0;
static malloc_fun_t  t_xmalloc = 0;
static free_fun_t    t_free    = 0;
static realloc_fun_t t_realloc = 0;
static realloc_fun_t t_xrealloc = 0;

enum which_mallocator { M_LIBC, M_JEMALLOC, M_DARWIN } which_mallocator;
static LOCAL_MEMORY_STATUS_S status;
int toku_memory_do_stats = 0;

static size_t pagesize = 0;

typedef size_t (*malloc_usable_size_fun_t)(const void *);
static malloc_usable_size_fun_t malloc_usable_size_f;

static bool memory_startup_complete;

int
toku_memory_startup(void) {
    if (memory_startup_complete) {
        return 0;
    }
    memory_startup_complete = true;

    pagesize = sysconf(_SC_PAGESIZE);

    int result = 0;

#if defined(HAVE_M_MMAP_THRESHOLD)
    // initialize libc malloc
    size_t mmap_threshold = 64 * 1024; // 64K and larger should be malloced with mmap().
    int success = mallopt(M_MMAP_THRESHOLD, mmap_threshold);
    if (success) {
        status.mallocator_version = "libc";
        status.mmap_threshold = mmap_threshold;
        which_mallocator = M_LIBC;
    } else
        result = EINVAL;
#else
    // just a guess
    status.mallocator_version = "darwin";
    status.mmap_threshold = 16 * 1024;
#endif

    // jemalloc has a mallctl function, while libc malloc does not.  we can check if jemalloc 
    // is loaded by checking if the mallctl function can be found.  if it can, we call it 
    // to get version and mmap threshold configuration.
    typedef int (*mallctl_fun_t)(const char *, void *, size_t *, void *, size_t);
    mallctl_fun_t mallctl_f;
    mallctl_f = (mallctl_fun_t) dlsym(RTLD_DEFAULT, "mallctl");
    if (mallctl_f) { // jemalloc is loaded
        which_mallocator = M_JEMALLOC;
        size_t version_length = sizeof status.mallocator_version;
        result = mallctl_f("version", &status.mallocator_version, &version_length, NULL, 0);
        if (result == 0) {
            size_t lg_chunk; // log2 of the mmap threshold
            size_t lg_chunk_length = sizeof lg_chunk;
            result  = mallctl_f("opt.lg_chunk", &lg_chunk, &lg_chunk_length, NULL, 0);
            if (result == 0)
                status.mmap_threshold = 1 << lg_chunk;
        }
    }

    malloc_usable_size_f = (malloc_usable_size_fun_t) dlsym(RTLD_DEFAULT, "malloc_usable_size");
    if (!malloc_usable_size_f) {
        malloc_usable_size_f = (malloc_usable_size_fun_t) dlsym(RTLD_DEFAULT, "malloc_size"); // darwin
        if (!malloc_usable_size_f) {
            result = EINVAL; // couldn't find a malloc size function
        }
    }

    return result;
}

static bool memory_shutdown_complete;

void
toku_memory_shutdown(void) {
    if (memory_shutdown_complete) {
        return;
    }
    memory_shutdown_complete = true;
}

void 
toku_memory_get_status(LOCAL_MEMORY_STATUS s) {
    *s = status;
}

// jemalloc's malloc_usable_size does not work with a NULL pointer, so we implement a version that works
static size_t
my_malloc_usable_size(void *p) {
    return p == NULL ? 0 : malloc_usable_size_f(p);
}

// Note that max_in_use may be slightly off because use of max_in_use is not thread-safe.
// It is not worth the overhead to make it completely accurate, but
// this logic is intended to guarantee that it increases monotonically.
// Note that status.sum_used and status.sum_freed increase monotonically
// and that status.max_in_use is declared volatile.
static inline void 
set_max(uint64_t sum_used, uint64_t sum_freed) {
    if (sum_used >= sum_freed) {
	uint64_t in_use = sum_used - sum_freed;
	uint64_t old_max;
	do {
	    old_max = status.max_in_use;
	} while (old_max < in_use &&
		 !__sync_bool_compare_and_swap(&status.max_in_use, old_max, in_use));
    }
}

size_t 
toku_memory_footprint(void * p, size_t touched) {
    size_t rval = 0;
    if (!pagesize)
	pagesize = sysconf(_SC_PAGESIZE);
    if (p) {
	size_t usable = my_malloc_usable_size(p);
	if (usable >= status.mmap_threshold) {
            int num_pages = (touched + pagesize) / pagesize;
            rval = num_pages * pagesize;
	}
	else {
	    rval = usable;
	}
    }
    return rval;
}

void *
toku_malloc(size_t size) {
    void *p = t_malloc ? t_malloc(size) : os_malloc(size);
    if (p) {
	ANNOTATE_NEW_MEMORY(p, size); // see #4671 and https://bugs.kde.org/show_bug.cgi?id=297147
        if (toku_memory_do_stats) {
            size_t used = my_malloc_usable_size(p);
            __sync_add_and_fetch(&status.malloc_count, 1);
            __sync_add_and_fetch(&status.requested,size);
            __sync_add_and_fetch(&status.used, used);
            set_max(status.used, status.freed);
        }
    } else {
        __sync_add_and_fetch(&status.malloc_fail, 1);
    }
  return p;
}

void *
toku_calloc(size_t nmemb, size_t size) {
    size_t newsize = nmemb * size;
    void *p = toku_malloc(newsize);
    if (p) memset(p, 0, newsize);
    return p;
}

void *
toku_realloc(void *p, size_t size) {
    size_t used_orig = p ? my_malloc_usable_size(p) : 0;
    void *q = t_realloc ? t_realloc(p, size) : os_realloc(p, size);
    if (q) {
        if (toku_memory_do_stats) {
            size_t used = my_malloc_usable_size(q);
            __sync_add_and_fetch(&status.realloc_count, 1);
            __sync_add_and_fetch(&status.requested, size);
            __sync_add_and_fetch(&status.used, used);
            __sync_add_and_fetch(&status.freed, used_orig);
            set_max(status.used, status.freed);
        }
    } else {
	__sync_add_and_fetch(&status.realloc_fail, 1);
    }
    return q;
}

void *
toku_memdup(const void *v, size_t len) {
    void *p = toku_malloc(len);
    if (p) memcpy(p, v,len);
    return p;
}

char *
toku_strdup(const char *s) {
    return toku_memdup(s, strlen(s)+1);
}

void
toku_free(void *p) {
    if (p) {
        if (toku_memory_do_stats) {
            size_t used = my_malloc_usable_size(p);
            __sync_add_and_fetch(&status.free_count, 1);
            __sync_add_and_fetch(&status.freed, used);
        }
	if (t_free)
	    t_free(p);
	else
	    os_free(p);
    }
}

void
toku_free_n(void* p, size_t size __attribute__((unused))) {
    toku_free(p);
}

void *
toku_xmalloc(size_t size) {
    void *p = t_xmalloc ? t_xmalloc(size) : os_malloc(size);
    resource_assert(p);
    ANNOTATE_NEW_MEMORY(p, size); // see #4671 and https://bugs.kde.org/show_bug.cgi?id=297147
    if (toku_memory_do_stats) {
        size_t used = my_malloc_usable_size(p);
        __sync_add_and_fetch(&status.malloc_count, 1);
        __sync_add_and_fetch(&status.requested, size);
        __sync_add_and_fetch(&status.used, used);
        set_max(status.used, status.freed);
    }
    return p;
}

void *
toku_xcalloc(size_t nmemb, size_t size) {
    size_t newsize = nmemb * size;
    void *vp = toku_xmalloc(newsize);
    if (vp) memset(vp, 0, newsize);
    return vp;
}

void *
toku_xrealloc(void *v, size_t size) {
    size_t used_orig = v ? my_malloc_usable_size(v) : 0;
    void *p = t_xrealloc ? t_xrealloc(v, size) : os_realloc(v, size);
    resource_assert(p);
    if (toku_memory_do_stats) {
        size_t used = my_malloc_usable_size(p);
        __sync_add_and_fetch(&status.realloc_count, 1);
        __sync_add_and_fetch(&status.requested, size);
        __sync_add_and_fetch(&status.used, used);
        __sync_add_and_fetch(&status.freed, used_orig);
        set_max(status.used, status.freed);
    }
    return p;
}

size_t 
toku_malloc_usable_size(void *p) {
    return my_malloc_usable_size(p);
}

void *
toku_xmemdup (const void *v, size_t len) {
    void *p = toku_xmalloc(len);
    memcpy(p, v, len);
    return p;
}

char *
toku_xstrdup (const char *s) {
    return toku_xmemdup(s, strlen(s)+1);
}

void
toku_set_func_malloc(malloc_fun_t f) {
    t_malloc = f;
    t_xmalloc = f;
}

void
toku_set_func_xmalloc_only(malloc_fun_t f) {
    t_xmalloc = f;
}

void
toku_set_func_malloc_only(malloc_fun_t f) {
    t_malloc = f;
}

void
toku_set_func_realloc(realloc_fun_t f) {
    t_realloc = f;
    t_xrealloc = f;
}

void
toku_set_func_xrealloc_only(realloc_fun_t f) {
    t_xrealloc = f;
}

void
toku_set_func_realloc_only(realloc_fun_t f) {
    t_realloc = f;

}

void
toku_set_func_free(free_fun_t f) {
    t_free = f;
}

#define HUGEPAGE_SIZE (2L*1024L*1024L)
void toku_memory_dontneed_after_but_i_touched(void *malloced_object, size_t malloced_size, size_t just_touched_start, size_t just_touched_length) {
    if (which_mallocator==M_JEMALLOC) {
        if ((just_touched_length>0) && (0==((long)malloced_object & (HUGEPAGE_SIZE-1)))) {
            // If it's zero, then we didn't touch anything.
            // jemalloc aligns all large objects on 4MB boundaries, so if it's not 2MB aligned, then don't bother to continue.
            // Since we know that o is aligned, we can just do arithmetic on the offsets.
            long prevhugepage = (just_touched_start                    -1)/HUGEPAGE_SIZE;
            long thishugepage = (just_touched_start+just_touched_length-1)/HUGEPAGE_SIZE;
            if (prevhugepage != thishugepage) {
                // Crossed a hugepage boundary.
                // Take the ceiling modulo page size of the first untouched byte:
                long start_offset = (just_touched_start + just_touched_length + pagesize -1)&~(pagesize-1);
                char *madvise_start = (char*)malloced_object + start_offset;
                size_t usable        = toku_malloc_usable_size(malloced_object);
                assert(usable >= malloced_size);
                if (usable%HUGEPAGE_SIZE == 0) { // We knew it was 2MB-aligned, and now we know that it's a big block
		    long madvise_len = usable-start_offset;
		    assert(madvise_len>=0);
		    if (madvise_len>0) {
			int r = madvise(madvise_start, madvise_len, MADV_DONTNEED);
			assert(r==0);
		    }
		}
            }
        }
    }
}

#include <valgrind/helgrind.h>
void __attribute__((constructor)) toku_memory_helgrind_ignore(void);
void
toku_memory_helgrind_ignore(void) {
    VALGRIND_HG_DISABLE_CHECKING(&status, sizeof status);
}
