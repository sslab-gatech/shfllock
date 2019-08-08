#define JEMALLOC_BACKGROUND_THREAD_C_
#include "jemalloc/internal/jemalloc_preamble.h"
#include "jemalloc/internal/jemalloc_internal_includes.h"

#include "jemalloc/internal/assert.h"

JEMALLOC_DIAGNOSTIC_DISABLE_SPURIOUS

/******************************************************************************/
/* Data. */

/* This option should be opt-in only. */
#define BACKGROUND_THREAD_DEFAULT false
/* Read-only after initialization. */
bool opt_background_thread = BACKGROUND_THREAD_DEFAULT;
size_t opt_max_background_threads = MAX_BACKGROUND_THREAD_LIMIT + 1;

/* Used for thread creation, termination and stats. */
malloc_mutex_t background_thread_lock;
/* Indicates global state.  Atomic because decay reads this w/o locking. */
atomic_b_t background_thread_enabled_state;
size_t n_background_threads;
size_t max_background_threads;
/* Thread info per-index. */
background_thread_info_t *background_thread_info;

/******************************************************************************/

#ifdef JEMALLOC_PTHREAD_CREATE_WRAPPER

static int (*pthread_create_fptr)(pthread_t *__restrict, const pthread_attr_t *,
    void *(*)(void *), void *__restrict);

static void
pthread_create_wrapper_init(void) {
#ifdef JEMALLOC_LAZY_LOCK
	if (!isthreaded) {
		isthreaded = true;
	}
#endif
}

int
pthread_create_wrapper(pthread_t *__restrict thread, const pthread_attr_t *attr,
    void *(*start_routine)(void *), void *__restrict arg) {
	pthread_create_wrapper_init();

	return pthread_create_fptr(thread, attr, start_routine, arg);
}
#endif /* JEMALLOC_PTHREAD_CREATE_WRAPPER */

/* #ifndef JEMALLOC_BACKGROUND_THREAD */
#define NOT_REACHED { not_reached(); }
bool background_thread_create(tsd_t *tsd, unsigned arena_ind) NOT_REACHED
bool background_threads_enable(tsd_t *tsd) NOT_REACHED
bool background_threads_disable(tsd_t *tsd) NOT_REACHED
void background_thread_interval_check(tsdn_t *tsdn, arena_t *arena,
    arena_decay_t *decay, size_t npages_new) NOT_REACHED
void background_thread_prefork0(tsdn_t *tsdn) NOT_REACHED
void background_thread_prefork1(tsdn_t *tsdn) NOT_REACHED
void background_thread_postfork_parent(tsdn_t *tsdn) NOT_REACHED
void background_thread_postfork_child(tsdn_t *tsdn) NOT_REACHED
bool background_thread_stats_read(tsdn_t *tsdn,
    background_thread_stats_t *stats) NOT_REACHED
void background_thread_ctl_init(tsdn_t *tsdn) NOT_REACHED
/* #undef NOT_REACHED */

bool
background_thread_boot0(void) {
	if (!have_background_thread && opt_background_thread) {
		malloc_printf("<jemalloc>: option background_thread currently "
		    "supports pthread only\n");
		return true;
	}
	return false;
}

bool
background_thread_boot1(tsdn_t *tsdn) {
	return false;
}
