/*
   srch - fast parallel file search with simplified and extended find(1) functionality

   Copyright (C) 2020 - 2024 by Jorn I. Viken <jornv@1337.no>

   Portions of this code are derived from software created by
   - Dmitry Yu Okunev <dyokunev@ut.mephi.ru>, https://github.com/xaionaro/libpftw (framework)
   - Martin Kunev <martinkunev@gmail.com>, https://gist.github.com/martinkunev/1365481 (heap code)
   - Shadkam I. <>, https://github.com/shadkam/recentmost (recentmost)
   - John Beppu, BusyBox (du heap code)

   This program is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define VERSION "2.41"
#define SRCH

#define _GNU_SOURCE

#include <errno.h>
#include <semaphore.h>
#if defined(__APPLE__)
#   include <dispatch/dispatch.h>
#endif
#include <signal.h>
#if defined(_AIX)
#    define _SIGSET_T	// needed at least on GCC 4.8.5 to avoid these msgs: "error: conflicting types for 'sigset_t'"
#endif
#include <pthread.h>
#include <locale.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <regex.h>
#include <stdlib.h>
#include <search.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <assert.h>
#include <limits.h>
#include <time.h>
#include <sys/time.h>
#if ! defined(__MINGW32__)
#    include <pwd.h>
#    include <grp.h>
#endif

#if defined(__hpux)
#   include <sys/pstat.h>
#endif

#if defined (__sun__)
#    include <sys/statvfs.h>
#elif defined(__hpux)
#    include <sys/vfs.h>
#elif defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__APPLE__)
#    include <sys/param.h>
#    include <sys/mount.h>
#elif defined(__MINGW32__)
#    include "os-hacks.h"
#    define LINE_MAX 2048
#    define __USE_MINGW_ANSI_STDIO 1
#    define _POSIX_SEM_VALUE_MAX 32767
#    define lstat(x,y) stat(x,y)
#endif

#undef FALSE
#undef TRUE
typedef enum {FALSE, TRUE} boolean;

#if defined (__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__)
#    include <fcntl.h>
#    include <sys/syscall.h>
#    define DEFAULT_DIRENT_COUNT 100000		// - for option -X, may be overridden using env var DIRENTS
    static boolean extreme_readdir = FALSE; 	// - set to TRUE if option -X is given
    static unsigned buf_size;			// - set if option -X is given
    static unsigned getdents_calls;		// - incremented for every syscall(SYS_getdents, ... (if option -X is given)
#endif

// Borrowed from /usr/include/nspr4/pratom.h on RH6.4:
#if ((__GNUC__ > 4) || (__GNUC__ == 4 && __GNUC_MINOR__ >= 1)) && ! defined(__hppa__)
#    define PR_ATOMIC_ADD(ptr, val) __sync_add_and_fetch(ptr, val)
#endif

#if ! defined(PR_ATOMIC_ADD)
     static pthread_mutex_t accum_filecnt_lock = PTHREAD_MUTEX_INITIALIZER; // for protecting "accum_filecnt"
     static pthread_mutex_t accum_du_lock = PTHREAD_MUTEX_INITIALIZER; // for protecting "accum_du"
#endif

#define ASCEND TRUE			// - default sort order is ascending
#define DESCEND FALSE

#define INLINE_PROCESSING_THRESHOLD  2	// - 2 subdirectories are handled in-line by default (not in a separate thread)

#define MAX_THREADS        	   512	// - max number of threads that may be created

#define DIRTY_CONSTANT		    ~0 	// - for handling non-POSIX compliant file systems
			   		// (link count should reflect the number of subdirectories, and should be 2 for empty directories)

static char *progname;
static boolean xdev = FALSE;		// - set to TRUE if option -x is given

static boolean master_finished = FALSE;	// - set to TRUE when master thread has nothing more to do (when the other threads are finished)
static unsigned statcount = 0;		// - counter for lstat calls
#if ! defined(PR_ATOMIC_ADD)
    static pthread_mutex_t statcount_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__APPLE__)
    unsigned statcount_unexp = 0;	// lstat() calls not expected
#   if ! defined(PR_ATOMIC_ADD)
        static pthread_mutex_t statcount_unexp_lock = PTHREAD_MUTEX_INITIALIZER; // - for protecting statcount_unexp
#   endif
#endif

static unsigned queued_dirs = 0;	// - the total number of dirs handled by a separate thread
#if ! defined(PR_ATOMIC_ADD)
    static pthread_mutex_t queued_dirs_lock = PTHREAD_MUTEX_INITIALIZER; // - for protecting queued_dirs
#endif

static regex_t *regexcomp = NULL;		  // - set to expression to search for if option -n or -i is given
static boolean regex_opt = FALSE;	  	  // - set if option -n or -i is given
static boolean match_all_path_elems = FALSE; 	  // - set if option -a is given (default is to match against last name in a path)
static boolean fast_match_opt = FALSE;		  // - set if option -N is given. Also set as default if no -n, -i, -N is given
static boolean negate_match = FALSE; 		  // - set if "!" is the first char after -n/-i/-N
static char *fast_match_arg = NULL;		  // - (simple) expression to search for, i.e. case insensitive *string* 
static boolean end_with_null = FALSE;		  // - for option -0, used in handle_dirent() only
static boolean simulate_posix_compliance = FALSE; // - POSIX requires the directory link count to be at least 2

static unsigned long inline_processing_threshold = INLINE_PROCESSING_THRESHOLD;

static boolean lifo_queue = TRUE;		  // - default queue of directories to be processed is of type LIFO
static boolean fifo_queue = FALSE;		  // - select a standard FIFO queue with option -q
static boolean ino_queue = FALSE;       	  // - select a sorted queue of dirents with option -Q
static unsigned long inolist_bypasscount; 	  // - total number of list elements bypassed; only used by inodirlist_bintreeinsert() 

static boolean debug = FALSE;			  // - set if env var DEBUG is set

enum filetype {
	FILETYPE_REGFILE=1,
	FILETYPE_DIR=2,
	FILETYPE_SYMLINK=4,
	FILETYPE_BLOCKDEV=8,
	FILETYPE_CHARDEV=16,
	FILETYPE_PIPE=32,
	FILETYPE_SOCKET=64
};
static unsigned filetypemask = 0;

static boolean wc = FALSE;                // - for option -w; works as wc -l
static unsigned cntbiggestdirs = 0;	  // - for option -D <count>; list the "biggest" directories
static unsigned cntlongestdepth = 0;	  // - for option -R <count>; list the dirctories furthest from the root

static unsigned accum_filecnt = 0;	  // - count the number of files seen
static off_t accum_du = 0;		  // - sum of diskusage of files seen, in 512B blocks
static unsigned verbose_count = 0;	  // - set if option -v is specified
static unsigned last_accum_filecnt = 0;	  // - used by -v
static pthread_mutex_t last_accum_filecnt_lock = PTHREAD_MUTEX_INITIALIZER; // for protecting "accum_filecnt"
static time_t last_t = 0;		  // - previous timestamp in seconds since EPOCH, used in pthread_routine()

static unsigned cntmodmostrecently = 0;	  // - set if option -M is specified
static unsigned cntaccmostrecently = 0;	  // - set if option -A is specified
static unsigned cntmodleastrecently = 0;  // - set if option -L is specified
static unsigned cntaccleastrecently = 0;  // - set if option -B is specified
static unsigned cntfattestfiles = 0;	  // - set if option -F is specified
static boolean timestamp_or_size_on_heap = FALSE; // - set if -M, -A, -L, -B or -F is specified
static boolean lstat_needed = FALSE;	  // - set if any option requiring an lstat() is specified
static boolean just_count = FALSE;        // - set if -w, -z, -D, -R is specified
static boolean summarize_diskusage = FALSE; // - set if option -H is specified
#define TWO_TB  (2LL * 1024LL * 1024LL * 1024LL * 1024LL)
static time_t olderthan;		  // - set if -o or -O is specified
static time_t youngerthan;		  // - set if -y or -Y is specified
static boolean older_and_younger = FALSE; // - set if one of both pairs -o/-O and -y/-Y are given, i.e. -o and -y
static boolean older_or_younger = FALSE;  // - set if one or both of the pairs -o/-O or -y/-Y are given, i.e. -o or -y or both
static boolean modtimelist = FALSE;	  // - set if -j is specified
static boolean zerosized = FALSE;	  // - set if -z is specified
static boolean uidsearch = FALSE;	  // - set if -u is specified, N/A for Windows
static boolean uidnegsearch = FALSE;	  // - set if -U is specified, N/A for Windows
static boolean gidsearch = FALSE;	  // - set if -g is specified, N/A for Windows
static boolean gidnegsearch = FALSE;	  // - set if -G is specified, N/A for Windows
static boolean uid_or_gid = FALSE;  	  // - set if one or both of the pairs -u/-g or -U/-G are given, e.g. -u or -g or both

static char **excludelist = NULL;	  // - set if -e/-E is specified
static unsigned excludelist_count = 0;	  // - set if -e/-E is specified
static regex_t **excluderecomp = NULL;	  // - set if -e/-E is specified

#if defined(__MINGW32__)
	typedef int uid_t;		  // - just a placeholder, and never used on Windows
	typedef int gid_t;		  // - just a placeholder, and never used on Windows
#endif
static uid_t *uidlist = NULL;	  	  // - set if -u/-U is specified - never used on Windows since there is no uid
static unsigned int uidlist_count = 0;	  // - set if -e/-E is specified
static gid_t *gidlist = NULL;	  	  // - set if -g/-G is specified - never used on Windows since there is no gid
static unsigned int gidlist_count = 0;	  // - set if -g/-G is specified
static unsigned long filesize = 0;
static unsigned long lowfilesize = 0;
static boolean filesizesearch = FALSE;
static boolean filesizeabove = FALSE;
static boolean filesizebelow = FALSE;

static pthread_mutex_t perror_lock = PTHREAD_MUTEX_INITIALIZER;	// - perror() calls should be allowed to complete in one go

typedef struct dirlist dirlist_t;

struct dirlist {
	char		*dirpath;	  // - full path to current directory
	time_t		 modtime;
	unsigned	 depth;		  // - current directory depth
	unsigned	 inlined;  	  // - how many subdirs are processed inline so far;
					  //   only needed for btrfs and other file systems where st_nlink is not useful
	unsigned	 filecnt;    	  // - sum of files in this dir and inline processed subdirs
	off_t		 du;		  // - sum of disk usage of files in this dir and inline processed subdirs, in 512B blocks
	dirlist_t	*next;	    	  // - pointer to next directory in queue
	dirlist_t	*prev;	    	  // - pointer to previous directory in queue
	unsigned	 st_nlink;	  // - link count for current directory; note that it will be decreased for
					  //   each handled subdirectory
	unsigned long	 st_dev;	  // - file system id for current directory

	ino_t		 st_ino;	  // - directory inode number
};

// This is the global list of directories to be processed, malloc'ed later:
dirlist_t	       *dirlist_head = NULL;      // - first directory in queue
dirlist_t	       *dirlist_tail = NULL;      // - last directory in queue - only for FIFO queue (option -q)
unsigned	 	queuesize    = 0;         // - current number of queued directories
unsigned	 	maxdepth     = 0;         // - max directory depth, if option -m is specified
pthread_mutex_t	 	dirlist_lock = PTHREAD_MUTEX_INITIALIZER; // - for protecting dirlist_head, dirlist_tail, queuesize

static pthread_t	*thread_arr  = NULL;      // - an array of all the thread_cnt threads doing real work, allocated in thread_prepare()
static unsigned		 thread_cnt  = 0;         // - set by main(), used by thread_prepare(), thread_cleanup(), dirlist_pull_dir(), traverse_trees()
static unsigned		 sleeping_thread_cnt = 0; // - how many threads are sleeping with nothing to do
#if ! defined(PR_ATOMIC_ADD)
	static pthread_mutex_t sleeping_thread_cnt_lock = PTHREAD_MUTEX_INITIALIZER; // - for protecting "sleeping_thread_cnt"
#endif

#if ! defined(__APPLE__)
	static sem_t	 master_sem;	       // - main thread initially goes to sleep on this semaphore, but sooner or later it will be signalled by dirlist_pull_dir()
	static sem_t	 threads_sem;	       // - signalled by dirlist_add_dir() when there is (more) work to do for a thread
	static sem_t	 finished_threads_sem; // - main thread will wait for a signal from each thread that it has finished
#else
	static dispatch_semaphore_t
			 master_sem;
	static dispatch_semaphore_t
			 threads_sem;
	static dispatch_semaphore_t
			 finished_threads_sem;
#endif
static unsigned		 sem_val_max_exceeded_cnt = 0;	// - _POSIX_SEM_VALUE_MAX == 32767, so this variable counts all above this value
static pthread_mutex_t	 sem_val_max_exceeded_cnt_lock = PTHREAD_MUTEX_INITIALIZER; // - for protecting "sem_val_max_exceeded_cnt"

/////////////////////////////////////////////////////////////////////////////

#if ! defined(__MINGW32__)
// Code to assert that hard linked inodes are only counted once when option -H is given - borrowed from busybox' du:

typedef struct ino_dev_hash_bucket ino_dev_hash_bucket_t;
struct ino_dev_hash_bucket {
        ino_t ino;
        dev_t dev;
        ino_dev_hash_bucket_t *next;
        /*
         * Reportedly, on cramfs a file and a dir can have same ino.
         * Need to also remember "file/dir" bit:
         */
        boolean isdir;
};

#define HASH_SIZE      311u   /* Should be prime */
#define hash_inode(i)  ((unsigned)(i) % HASH_SIZE)

/* array of [HASH_SIZE] elements */
static ino_dev_hash_bucket_t **ino_dev_hashtable;

/* Add statbuf to statbuf hash table */
static inline __attribute__((always_inline)) void add_to_ino_dev_hashtable(const struct stat *statbuf)
{
        int i;
        ino_dev_hash_bucket_t *bucket;

        bucket = malloc(sizeof(ino_dev_hash_bucket_t));
	assert(bucket);
        bucket->ino = statbuf->st_ino;
        bucket->dev = statbuf->st_dev;
        bucket->isdir = !!S_ISDIR(statbuf->st_mode);

        if (! ino_dev_hashtable)
                ino_dev_hashtable = calloc(HASH_SIZE, sizeof(*ino_dev_hashtable));

        i = hash_inode(statbuf->st_ino);
        bucket->next = ino_dev_hashtable[i];
        ino_dev_hashtable[i] = bucket;
}

/////////////////////////////////////////////////////////////////////////////

/*
 * Return name if statbuf->st_ino && statbuf->st_dev are recorded in
 * ino_dev_hashtable, else return NULL
 */
static inline __attribute__((always_inline)) boolean is_in_ino_dev_hashtable(const struct stat *statbuf)
{
        ino_dev_hash_bucket_t *bucket;

        if (! ino_dev_hashtable)
                return FALSE;

        bucket = ino_dev_hashtable[hash_inode(statbuf->st_ino)];
        while (bucket) {
                if ((bucket->ino == statbuf->st_ino)
                 && (bucket->dev == statbuf->st_dev)
                 && (bucket->isdir == !!S_ISDIR(statbuf->st_mode))
                ) {
                        return TRUE;
                }
                bucket = bucket->next;
        }
        return FALSE;
}
#endif

/////////////////////////////////////////////////////////////////////////////

// Thanks to https://gist.github.com/martinkunev/1365481 from which basic heap related code was originally taken
// and to https://github.com/shadkam/recentmost for pieces of code from recentmost.c

typedef struct filedata heap_elem_t;
struct filedata {
	char *name;
	long long metadata;
};

typedef struct heap heap_t;
struct heap {
	size_t		  heapsize;
	size_t		  heapcount;
	heap_elem_t	**data;
};

static heap_t		  dtslist;	// - date, time and size list, used for options -M / -A / -F
static pthread_mutex_t	  heap_lock = PTHREAD_MUTEX_INITIALIZER; // - for protecting dtslist in heap_push() only.
								 // - heap_pop() is just used while being protected inside heap_push(),
								 //   and outside threads, so this lock is not needed in heap_pop().

/////////////////////////////////////////////////////////////////////////////

static int heap_init(
	int size)
{
	dtslist.heapsize  = size;
	dtslist.heapcount = 0;
	dtslist.data      = malloc(sizeof(heap_elem_t) * size);
	assert(dtslist.data);
	return 0;
}

/////////////////////////////////////////////////////////////////////////////

#define heap_term() free(dtslist.data)

/////////////////////////////////////////////////////////////////////////////

static inline __attribute__((always_inline)) heap_elem_t *heap_new_elem(
	long long metadata,
	char *filepath)
{
	heap_elem_t *elem = malloc(sizeof(heap_elem_t));
	assert(elem);
	elem->metadata = metadata;
	elem->name = strdup(filepath);
	assert(elem->name);
	return elem;
}

/////////////////////////////////////////////////////////////////////////////

static inline __attribute__((always_inline)) void heap_free_elem(
	heap_elem_t *elem)
{
	if (! elem)
		return;
	if (elem->name)
		free(elem->name);
	free(elem);
}

/////////////////////////////////////////////////////////////////////////////

static inline __attribute__((always_inline)) heap_elem_t *heap_pop(
	boolean ascend)
{
	unsigned int index, swap, other;

	if (dtslist.heapcount == 0)
		return NULL;

	heap_elem_t *popped_elem = *dtslist.data;

	heap_elem_t *temp = dtslist.data[--dtslist.heapcount];
	for (index = 0; TRUE; index = swap) {
		swap = (index << 1) + 1;
		if (swap >= dtslist.heapcount)
			break;
		other = swap + 1;
		if (ascend) {
			if (other < dtslist.heapcount && dtslist.data[other]->metadata < dtslist.data[swap]->metadata)
				swap = other;
			if (temp->metadata < dtslist.data[swap]->metadata)
				break;
		} else {
			if (other < dtslist.heapcount && dtslist.data[other]->metadata > dtslist.data[swap]->metadata)
				swap = other;
			if (temp->metadata > dtslist.data[swap]->metadata)
				break;
		}
		dtslist.data[index] = dtslist.data[swap];
	}
	dtslist.data[index] = temp;

	return popped_elem;
}

/////////////////////////////////////////////////////////////////////////////

static inline __attribute__((always_inline)) boolean heap_push(
	heap_elem_t *value,
	boolean ascend)
{
	unsigned int index, parent;

	pthread_mutex_lock(&heap_lock);

	if (dtslist.heapcount) {
		if (dtslist.heapcount >= dtslist.heapsize) {
			heap_elem_t *min_in_top = *dtslist.data;
			if (ascend && min_in_top->metadata < value->metadata)
				heap_free_elem(heap_pop(ASCEND));
			else if (! ascend && min_in_top->metadata > value->metadata)
				heap_free_elem(heap_pop(DESCEND));
			else {
				pthread_mutex_unlock(&heap_lock);
				return FALSE;
			}
		}
	}
	for (index = dtslist.heapcount++; index; index = parent) {
		parent = (index - 1) >> 1;
		if ((ascend && dtslist.data[parent]->metadata < value->metadata)
		    || (! ascend && dtslist.data[parent]->metadata > value->metadata))
			break;
		dtslist.data[index] = dtslist.data[parent];
	}
	dtslist.data[index] = value;

	pthread_mutex_unlock(&heap_lock);

	return TRUE;
}

/////////////////////////////////////////////////////////////////////////////

// Algorithm based on https://github.com/embeddedartistry/embedded-resources/blob/master/examples/libc/string/strstr.c

static inline __attribute__((always_inline)) char *strstr_ignorecase(
        char *haystack,
        char *needle)
{
        char *hayptr, *needleptr;

        // No need to test if haystack == NULL initially since a file name is never NULL...
        // We also assume that the pattern to serach for (needle) is never NULL...
        for (needleptr = needle; *haystack; haystack++, needleptr = needle) {
                hayptr = haystack;
                while (TRUE) {
                        if (! *needleptr)
                                return haystack;

                        if (tolower((int)*hayptr++) != *needleptr++)
                                break;
                }
        }
        return NULL;
}

/////////////////////////////////////////////////////////////////////////////

static inline boolean filename_match(
	char *name)
{
	if (fast_match_opt) {
		if (! strstr_ignorecase(name, fast_match_arg))
			return negate_match ? TRUE : FALSE;
	} else if (regex_opt) {
		if (regexec(regexcomp, name, 0, NULL, 0))
			return negate_match ? TRUE : FALSE;
	}
	return negate_match ? FALSE : TRUE;
}

/////////////////////////////////////////////////////////////////////////////

static inline __attribute__((always_inline)) boolean uid_match(
	uid_t uid)
{
	unsigned i;
	for (i = 0; i < uidlist_count; i++)
		if (uid == uidlist[i])
			return TRUE;
	return FALSE;
}

/////////////////////////////////////////////////////////////////////////////

static inline __attribute__((always_inline)) boolean gid_match(
	gid_t gid)
{
	unsigned i;
	for (i = 0; i < gidlist_count; i++)
		if (gid == gidlist[i])
			return TRUE;
	return FALSE;
}

/////////////////////////////////////////////////////////////////////////////

#include "commonlib.h"

/////////////////////////////////////////////////////////////////////////////

static inline __attribute__((always_inline)) boolean modtimecheck(
        time_t mtime)
{
        return ! older_or_younger
            || (olderthan && mtime < olderthan && ! youngerthan)
            || (youngerthan && mtime > youngerthan && ! olderthan)
            || (older_and_younger && mtime < olderthan && mtime > youngerthan);
}

/////////////////////////////////////////////////////////////////////////////

static inline __attribute__((always_inline)) boolean uidgidcheck(
	uid_t uid,
	gid_t gid)
{
	return ! uid_or_gid
	    || (uidsearch && uid_match(uid))
	    || (uidnegsearch && ! uid_match(uid))
            || (gidsearch && gid_match(gid))
	    || (gidnegsearch && ! gid_match(gid));
}

/////////////////////////////////////////////////////////////////////////////

static inline __attribute__((always_inline)) boolean sizecheck(
	size_t size)
{
	if (! filesizesearch)
		return TRUE;

	if (filesizeabove && filesizebelow)
		return size >= lowfilesize && size <= filesize;
	else
		return (filesizeabove && size >= filesize)
	            || (filesizebelow && size <= filesize)
	    	    || size == filesize;
}

/////////////////////////////////////////////////////////////////////////////

static inline void handle_dirent(dirlist_t *, struct dirent *); // - used by walk_dir()

/////////////////////////////////////////////////////////////////////////////

static void walk_dir(
	dirlist_t *curdir)
{
	DIR *dir = NULL;
#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__)
	int fd = 0;    			// - only used on Linux/*BSD if option -X is given
	char *buf = NULL;   		// - same
	unsigned bpos = 0, nread = 0;	// - same
#endif
	struct dirent *dent = NULL;

	//assert(curdir->dirpath);

#    if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__)
	if (extreme_readdir) {
		if ((fd = open(curdir->dirpath, O_RDONLY | O_DIRECTORY)) < 0) {
			pthread_mutex_lock(&perror_lock);
			fprintf(stderr, "%s: ", progname);
			perror(curdir->dirpath);
			pthread_mutex_unlock(&perror_lock);
			return;
		}
		dent = malloc(sizeof(struct dirent));
		assert(dent);
		buf = malloc(buf_size);
		assert(buf);
	} else
#    endif
	if (! (dir = opendir(curdir->dirpath))) {
			pthread_mutex_lock(&perror_lock);
			fprintf(stderr, "%s: ", progname);
			perror(curdir->dirpath);
			pthread_mutex_unlock(&perror_lock);
			return;
	}

	if (curdir->st_nlink < 2 && ! simulate_posix_compliance) {
		if (debug)	
			fprintf(stderr, "POSIX non-compliance detected on %s - setting simulate_posix_compliance = TRUE\n", curdir->dirpath);
		simulate_posix_compliance = TRUE;
		curdir->st_nlink = DIRTY_CONSTANT;
	}
	//assert(curdir->st_nlink);   // - lstat() for curdir->dirpath should already have been executed at this point

	while (TRUE) {
		//assert(dir); // - something is seriously wrong if dir == 0 here...
#	      if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__)
		if (extreme_readdir) {
			readdir_extreme(fd, buf, buf_size, curdir->dirpath, &bpos, dent, &nread);
			if (! nread) {
				close(fd);
				free(buf);
				free(dent);
				dent = NULL;
			}
		} else
#	      endif
			dent = readdir(dir);

		if (dent == NULL)
			break;

		if (dent->d_name[0] == '.' && 
			(dent->d_name[1] == 0 ||
			(dent->d_name[1] == '.' && dent->d_name[2] == 0)))
				continue;       // Skip "." and ".."

		handle_dirent(curdir, dent);
	}

#     if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__)
	if (! extreme_readdir)
#     endif
		closedir(dir);

	if (cntbiggestdirs > 0) {
		heap_elem_t *elem = heap_new_elem(curdir->filecnt, curdir->dirpath);
		if (! heap_push(elem, ASCEND))
			heap_free_elem(elem);
	} else if (cntlongestdepth > 0) {
		heap_elem_t *elem = heap_new_elem(curdir->depth-1, curdir->dirpath);
		if (! heap_push(elem, ASCEND))
			heap_free_elem(elem);
	} else if (zerosized && curdir->filecnt == 0 && modtimecheck(curdir->modtime)) {
		if (end_with_null)
			printf("%s%c", curdir->dirpath, '\0');
		else if (modtimelist) {
			char *timestr = printable_time(curdir->modtime);
			printf("%s %s\n", timestr, curdir->dirpath);
			free(timestr);
		} else
			puts(curdir->dirpath);
	}

	if (curdir->dirpath)
		free(curdir->dirpath);

	return;
}

/////////////////////////////////////////////////////////////////////////////

static inline __attribute__((always_inline)) void handle_dirent(
	dirlist_t *curdir,
	struct dirent *dent)
{
	boolean dive_into_subdir = FALSE;
	int ftype = 0;
	int lstaterror = 0;
	struct stat st;
	st.st_dev = 0;

	// Getting path
	size_t path_len = strlen(curdir->dirpath) + 1 + strlen(dent->d_name);
	char *path = malloc(path_len+1);
	assert(path);
	strcpy(path, curdir->dirpath);
#     if ! defined(__MINGW32__)
	if (! (path[0] == '/' && path[1] == 0)) strcat(path, "/"); // - only add / if path != /
#     else
	if (! (path[0] == '\\' && path[1] == 0)) strcat(path, "\\"); // - only add '\' if path != '\'
#     endif
	strcat(path, dent->d_name);

	// Running lstat() if and only if needed...
#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__APPLE__)
	if (dent->d_type == DT_DIR
	    || (dent->d_type == DT_UNKNOWN && curdir->st_nlink > 2)
	    || lstat_needed) {
		// We might get d_type == DT_UNKNOWN (0):
		// - on directories we don't own ourselves.
		// - on NFS shares.
		lstaterror = lstat(path, &st);
		if (lstaterror) {
			st.st_mode = st.st_mtime = st.st_size = st.st_blocks = 0;
			st.st_uid = st.st_gid = -1;
			st.st_nlink = 1;
			pthread_mutex_lock(&perror_lock);
			fprintf(stderr, "%s: %s: ", progname, path);
			perror(NULL);
			pthread_mutex_unlock(&perror_lock);
		}

		if (dent->d_type == DT_UNKNOWN) {
#                     if defined(PR_ATOMIC_ADD)
                        PR_ATOMIC_ADD(&statcount_unexp, 1);
#                     else
                        pthread_mutex_lock(&statcount_unexp_lock);
                        statcount_unexp++;
                        pthread_mutex_unlock(&statcount_unexp_lock);
#                     endif
			switch (st.st_mode & S_IFMT) {
				case S_IFREG:
					dent->d_type = DT_REG;
					break;
				case S_IFDIR:
					dent->d_type = DT_DIR;
					break;
#			if ! defined(__MINGW32__)
				case S_IFBLK:
					dent->d_type = DT_BLK;
					break;
				case S_IFCHR:
					dent->d_type = DT_CHR;
					break;
				case S_IFIFO:
					dent->d_type = DT_FIFO;
					break;
				case S_IFLNK:
					dent->d_type = DT_LNK;
					break;
				case S_IFSOCK:
					dent->d_type = DT_SOCK;
					break;
#			endif
			}
		} else {
#                     if defined(PR_ATOMIC_ADD)
                        PR_ATOMIC_ADD(&statcount, 1);
#                     else
                        pthread_mutex_lock(&statcount_lock);
                        statcount++;
                        pthread_mutex_unlock(&statcount_lock);
#                     endif
		}
	}

	if (dent->d_type == DT_DIR) {
		ftype = S_IFDIR;
		curdir->st_nlink--;
		curdir->modtime = st.st_mtime;
		dive_into_subdir = TRUE;


		if (xdev && curdir->st_dev != st.st_dev)
			dive_into_subdir = FALSE;
	} else if (filetypemask) { // - we don't care what file type it is if not asked for through options...
		//assert(dent->d_type != DT_UNKNOWN); // - happens on NFS shares
		switch (dent->d_type) {
			case DT_REG:
				ftype = S_IFREG;
				break;
#		if ! defined(__MINGW32__)
			case DT_LNK:
				ftype = S_IFLNK;
				break;
			case DT_BLK:
				ftype = S_IFBLK;
				break;
			case DT_CHR:
				ftype = S_IFCHR;
				break;
			case DT_FIFO:
				ftype = S_IFIFO;
				break;
			case DT_SOCK:
				ftype = S_IFSOCK;
				break;
#		endif
			case DT_UNKNOWN:
			default:
				ftype = 0;
				break;
		}
	}
#else // - non-Linux/BSD goes here:
	if (curdir->st_nlink > 2
	    || lstat_needed) {
#	      if ! defined(__MINGW32__)
		lstaterror = lstat(path, &st);
		if (lstaterror) {
			st.st_mode = st.st_mtime = st.st_size = st.st_blocks = 0;
			st.st_uid = st.st_gid = -1;
			st.st_nlink = 1;
			pthread_mutex_lock(&perror_lock);
			fprintf(stderr, "%s: Couldn't stat %s\n", progname, path);
			pthread_mutex_unlock(&perror_lock);
		}
#	      else // __MINGW32__ follows:
		WIN32_FIND_DATA ffd;    
		HANDLE handle = FindFirstFile(path, &ffd);
		if (handle == INVALID_HANDLE_VALUE) {
			st.st_mtime = st.st_atime = st.st_ino = 0;
			st.st_mode = st.st_size = 0;
			st.st_nlink = 1;
			pthread_mutex_lock(&perror_lock);
			fprintf(stderr, "%s: GetFileAttributesEx() failed on %s\n", progname, path);
			pthread_mutex_unlock(&perror_lock); 
		} else {
			FindClose(handle);

			st.st_mode = ffd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY ? S_IFDIR : 0;
			st.st_size = (((unsigned long long)ffd.nFileSizeHigh << 32) | ffd.nFileSizeLow) / 512ULL;
#		      define EPOCH_AS_FILETIME 116444736000000000  // January 1, 1970 as MS file time
#		      define HUNDREDS_OF_NANOSECONDS 10000000
			st.st_mtime = (((long long)ffd.ftLastWriteTime.dwHighDateTime << 32) | ((unsigned long)ffd.ftLastWriteTime.dwLowDateTime&0xffffffff));
			st.st_mtime = (st.st_mtime - EPOCH_AS_FILETIME) / HUNDREDS_OF_NANOSECONDS;
			st.st_atime = (((long long)ffd.ftLastAccessTime.dwHighDateTime << 32) | ((unsigned long)ffd.ftLastAccessTime.dwLowDateTime&0xffffffff));
			st.st_atime = (st.st_atime - EPOCH_AS_FILETIME) / HUNDREDS_OF_NANOSECONDS;
		}
#	      endif
#             if defined(PR_ATOMIC_ADD)
                PR_ATOMIC_ADD(&statcount, 1);
#             else
                pthread_mutex_lock(&statcount_lock);
                statcount++;
                pthread_mutex_unlock(&statcount_lock);
#             endif


		if (S_ISDIR(st.st_mode)) {
			curdir->st_nlink--;
			curdir->modtime = st.st_mtime;
			dive_into_subdir = TRUE;

			if (xdev && curdir->st_dev != st.st_dev)
				dive_into_subdir = FALSE;
		}
		ftype = st.st_mode & S_IFMT;
	} else {
		st.st_mtime = st.st_atime = st.st_size = 0;
	}
#endif // - non-Linux/BSD

	if (excludelist_count > 0 && dive_into_subdir) {
		unsigned i;
		for (i = 0; i < excludelist_count; i++)
			if (excluderecomp) {
				if (regexec(excluderecomp[i], dent->d_name, 0, NULL, 0) == 0) {
					if (debug)
						fprintf(stderr, "==> Skipping dir %s (%s)\n", path, excludelist[i]);
					free(path);
					return;		// - skip directories specified through -e
				}
			} else {
				if (strcmp(excludelist[i], dent->d_name) == 0) {
					if (debug)
						fprintf(stderr, "==> Skipping dir %s (%s)\n", path, excludelist[i]);
					free(path);
					return;		// - skip directories specified through -E
				}
			}
	}

	// List the file or not - that is the big question at this point:
	if (filename_match(match_all_path_elems ? path : dent->d_name)
	    && (
		! filetypemask
		|| ((filetypemask & FILETYPE_REGFILE) && ftype == S_IFREG)
		|| ((filetypemask & FILETYPE_DIR) && ftype == S_IFDIR)
#	    if ! defined(__MINGW32__)
		|| ((filetypemask & FILETYPE_BLOCKDEV) && ftype == S_IFBLK)
		|| ((filetypemask & FILETYPE_CHARDEV) && ftype == S_IFCHR)
		|| ((filetypemask & FILETYPE_SYMLINK) && ftype == S_IFLNK)
		|| ((filetypemask & FILETYPE_SOCKET) && ftype == S_IFSOCK)
		|| ((filetypemask & FILETYPE_PIPE) && ftype == S_IFIFO)
#	    endif
	    )
	   ) {
		if (timestamp_or_size_on_heap) {
			if (lstaterror) {
				free(path);
				return;
			}

			if (cntmodmostrecently > 0) {
				heap_elem_t *elem = heap_new_elem(st.st_mtime, path);
				if (! heap_push(elem, ASCEND))
					 heap_free_elem(elem);
			} else if (cntaccmostrecently > 0) {
				heap_elem_t *elem = heap_new_elem(st.st_atime, path);
				if (! heap_push(elem, ASCEND))
					 heap_free_elem(elem);
			} else if (cntmodleastrecently > 0) {
				heap_elem_t *elem = heap_new_elem(st.st_mtime, path);
				if (! heap_push(elem, DESCEND))
					 heap_free_elem(elem);
			} else if (cntaccleastrecently > 0) {
				heap_elem_t *elem = heap_new_elem(st.st_atime, path);
				if (! heap_push(elem, DESCEND))
					 heap_free_elem(elem);
			} else if (cntfattestfiles > 0) {
#			      if defined(__sun__)
				if (S_ISBLK(st.st_mode))
					st.st_size = 0;
#			      endif
				if (modtimecheck(st.st_mtime)
				    && uidgidcheck(st.st_uid, st.st_gid)
				    && sizecheck(st.st_size)) {
						heap_elem_t *elem = heap_new_elem((unsigned long long)st.st_size, path);
						if (! heap_push(elem, ASCEND))
							heap_free_elem(elem);
				}
			}
		} else if (modtimecheck(st.st_mtime)
			   && uidgidcheck(st.st_uid, st.st_gid)
			   && sizecheck(st.st_size)) {
#		      if defined(__CYGWIN__)
			if (summarize_diskusage && st.st_blocks) {
				curdir->du += st.st_blocks * 2;
			}
#		      elif defined(__MINGW32__)
			if (summarize_diskusage && st.st_size) {
				curdir->du += st.st_size;
			}
#		      else
			if (summarize_diskusage && st.st_size) {
				if (st.st_nlink == 1 || (st.st_nlink > 1 && ! is_in_ino_dev_hashtable(&st))) {
					if (st.st_size < TWO_TB)
						curdir->du += st.st_blocks;
					else
						curdir->du += st.st_size / 512ULL;
					if (st.st_nlink > 1 && ! dive_into_subdir)
						add_to_ino_dev_hashtable(&st);
				}
			}
#		      endif
			if (just_count || verbose_count)
			 	curdir->filecnt++;
			else if (end_with_null)
				printf("%s%c", path, '\0');
			else if (modtimelist) {
				time_t mtime = lstat_needed ? st.st_mtime : get_mtime(path);
				char *timestr = printable_time(mtime);
				printf("%s %s\n", timestr, path);
				free(timestr);
			} else
				puts(path);
		} else if (zerosized) {
		 	curdir->filecnt++;
		}
	}

	if (dive_into_subdir) {
		if (maxdepth) {
			if (curdir->depth >= maxdepth) {
				free(path);
				return;
			}
		}

		// fprintf(stderr, "%s: curdir->st_nlink = %i\n", curdir->dirpath, curdir->st_nlink);
		if (inline_processing_threshold &&
		    (curdir->st_nlink < inline_processing_threshold + 2 ||				// - posix compliant
		    (simulate_posix_compliance && curdir->inlined < inline_processing_threshold))) {	// - non-compliant (btrfs)
			// Process up to n subdirs inline, n = inline_processing_threshold.
			curdir->inlined++;

			dirlist_t subdirentry;

			subdirentry.dirpath = strdup(path);
			assert(subdirentry.dirpath);
			subdirentry.depth = curdir->depth+1;
			subdirentry.inlined = 0;
			subdirentry.st_nlink = simulate_posix_compliance ? DIRTY_CONSTANT : st.st_nlink;
			subdirentry.modtime = st.st_mtime;
			subdirentry.st_dev = st.st_dev;
			subdirentry.filecnt = 0;
			subdirentry.du = 0;

			walk_dir(&subdirentry);

			if (summarize_diskusage) {
				curdir->du += subdirentry.du;
				curdir->filecnt += subdirentry.filecnt;
			} else if (wc || verbose_count)
				curdir->filecnt += subdirentry.filecnt;
		} else {
                        // - The first n subdirs, n <= inline_processing_threshold, will be enqueued and processed when a thread is available.
			dirlist_add_dir(path, curdir->depth+1, &st);
		}
	}

	free(path);
	return;
}

/////////////////////////////////////////////////////////////////////////////

static int usage()
{
#if defined(__MINGW32__)
	printf("Usage: %s [-t <count>] [[-n|-i [!]<re1|re2|...> | -N [!]<name>] [-a]] [-e <dir> ... | -E <dir> ...]\n", progname);
	printf("\t    [-f] [-d] [-m <maxdepth>] [-x] [-z] [-j] [-0] [-w] [-H] [-v <count>]\n");
	printf("\t    [-o <days> | -O <minutes> | -P <tstamp-file>] [-y <days> | -Y <minutes> | -W <tstamp-file>]\n");
	printf("\t    [-s [+|-]<size>[k|m|g|t] | +<size>[k|m|g|t]:-<size>[k|m|g|t]] [-w] [-H] [-v <count>]\n");
	printf("\t    [-D <count> | -F <count> | -M <count> | -A <count> | -L <count> | -B <count>]\n");
	printf("\t    [-I <count>] [-q | -Q] [-S] [-T] [-V] [-h] [arg1 [arg2] ...]\n\n");
#else
	printf("Usage: %s [-t <count>] [[-n|-i [!]<re1|re2|...> | -N [!]<name>] [-a]] [-e <dir> ... | -E <dir> ... | -Z]\n", progname);
	printf("\t    [-f] [-d] [-l] [-b] [-c] [-p] [-k]\n\t    [-m <maxdepth>] [-x] [-z] [-j] [-0] [-w] [-H] [-v <count>]\n");
	printf("\t    [-u <user> ... | -U <user> ...] [-g <group> ... | -G <group> ...]\n");
	printf("\t    [-o <days> | -O <minutes> | -P <tstamp-file>] [-y <days> | -Y <minutes>] | -W <tstamp-file>]\n");
	printf("\t    [-s [+|-]<size>[k|m|g|t] | +<size>[k|m|g|t]:-<size>[k|m|g|t]]\n");
	printf("\t    [-D <count> | -F <count> | -M <count> | -A <count> | -L <count> | -B <count> | -R <count>]\n");
	printf("\t    [-I <count>] [-q | -Q] [-X] [-S] [-T] [-V] [-h] [arg1 [arg2] ...]\n\n");
#endif
	printf("-t <count>\t Run up to <count> threads in parallel.\n");
	printf("\t\t * Must be a non-negative integer between 1 and %i.\n", MAX_THREADS);
	printf("\t\t * Defaults to (virtual) CPU count on host, up to 8.\n");
	printf("\t\t * Note that <count> threads will be created in addition to the main thread, so the total thread\n");
	printf("\t\t   count will be <count+1>, but the main thread won\'t do any hard work, and will be mostly idle.\n\n");

	printf("-n [!]<re1|re2|...>\n");
	printf("\t\t Search for file names matching regular expression re1 OR re2 etc.\n");
	printf("\t\t * Use '!' to negate, i.e. search for file names NOT matching regular expression re1 OR re2 etc.\n");
	printf("\t\t * Extended regular expressions (REG_EXTENDED) are supported.\n");
	printf("\t\t * For an exact match, '^filename$' can be specified (\"^filename$\" on Windows).\n");
	printf("\t\t * To search for files containing a dot, '\\.' or \\\\. can be specified (\".\" on Windows).\n");
	printf("\t\t * Also consider option -N <name> which gives better performance, especially on large directory structures.\n");
	printf("\t\t * Only one -n option is supported.\n\n");

        printf("-i [!]<re1|re2|...>\n");
	printf("\t\t Same as -n, but perform case insensitive pattern matching.\n");
        printf("\t\t * If both -n and -i are given, the last one on the command line takes presedence.\n\n");

	printf("-N [!]<name>\t Search for file names matching shell pattern *name* (case insensitive).\n");
	printf("\t\t * Use '!' to negate, i.e. search for file names NOT matching shell pattern *name*.\n");
	printf("\t\t * This is usually much faster than -n/-i when searching through millions of files.\n");
	printf("\t\t * This option is implemented to be simple and portable, and supports pure ASCII characters only.\n");
	printf("\t\t * Option -N is default if -n/-i is not given.\n");
	printf("\t\t * If both option -N and -n/-i are given, -n or -i is silently ignored.\n");
	printf("\t\t * Only one -N option is supported.\n\n");

        printf("-a\t\t Together with option -n/-i/-N, match <re> or <name> against all of the directory elements\n");
	printf("\t\t in the tree structure being traversed, treating '/' as an ordinary character, like locate(1).\n\n");

	printf("-e <dir>\t Exclude directories matching <dir> from traversal.\n");
	printf("\t\t * Extended regular expressions are supported.\n");
	printf("\t\t * Any number of -e options are supported, up to command line limit.\n\n");

	printf("-E <dir>\t Exclude directory <dir> from traversal.\n");
	printf("\t\t * For simplicity, only exact matches are excluded with this option.\n");
	printf("\t\t * Any number of -E options are supported, up to command line limit.\n");
	printf("\t\t * Hint: Excluding .snapshot is usually desired on (the root of) NFS shares from NAS\n");
	printf("\t\t         where visible snapshots are enabled.\n\n");

#if ! defined(__MINGW32__)
	printf("-Z\t\t Equivalent to -E.snapshot.\n");
	printf("\t\t * Just to save some typing since it is commonly needed on a NAS NFS share.\n");
	printf("\t\t * Not implemented for Windows.\n\n");
#endif

	printf("-f\t\t Print only ordinary files to stdout.\n");
	printf("-d\t\t Print only directories to stdout.\n");
#if ! defined(__MINGW32__)
	printf("-l\t\t Print only symlinks to stdout.\n");
	printf("-b\t\t Print only block special files to stdout.\n");
	printf("-c\t\t Print only character special files to stdout.\n");
	printf("-p\t\t Print only named pipes/fifos to stdout.\n");
	printf("-k\t\t Print only sockets to stdout.\n");
#endif

	printf("\n-m <maxdepth>\t Descend at most <maxdepth> (a positive integer) levels below the start point(s).\n");
	printf("\t\t * This equals the -maxdepth option to GNU find(1).\n\n");

	printf("-x\t\t Only traverse the file system(s) containing the directory/directories specified.\n");
	printf("\t\t * This equals the -xdev option to find(1).\n\n");

	printf("-z\t\t Print out empty directories only.\n");
	printf("\t\t * This equals options \"-type d -empty\" to GNU find(1).\n");
	printf("\t\t * Apart from selection related options, -z can only be combined with -o/-O/-y/-Y.\n\n");

	printf("-j\t\t Prepend filename with modtime details.\n");
	printf("\t\t * Note that an extra lstat(2) call will be needed on non-directories.\n\n");

	printf("-0\t\t Print file names followed by a null character instead of the default newline.\n");
	printf("\t\t * This equals the -print0 option to GNU find(1).\n");
	printf("\t\t * Whatever characters in the file names, we can search the tree starting at\n");
	printf("\t\t   the current directory by running something like (using GNU xargs(1))\n");
	printf("\t\t   `%s -0 pattern | xargs -0P rm -f'\n", progname);
	printf("\t\t   to delete the matched files.\n\n");

        printf("-w\t\t Print out the total number of files/directories in the selected tree structure(s).\n");
        printf("\t\t * Equivalent to running `%s <args> | wc -l' as long as there is no file name containing a newline.\n", progname);
        printf("\t\t * This option may not be combined with -z (for implementation simplicity/execution speed).\n\n");

        printf("-H\t\t Print out the sum of the file sizes, in powers of 1024, of all the files encountered.\n");
        printf("\t\t * Output is on a human-readable format, like `du -hs'.\n\n");

        printf("-v <count>\t Print out a progress line after every <count> files have been processed.\n\n");

#if ! defined(__MINGW32__)
	printf("-u <user>\t Search for files owned by specific user name or uid.\n");
	printf("\t\t * Any number of -u options are supported, up to command line limit.\n\n");
	printf("-U <user>\t Search for files NOT owned by specific user name or uid.\n");
	printf("\t\t * Any number of -U options are supported, up to command line limit.\n\n");
	printf("-g <group>\t Search for files owned by specific group name or gid.\n");
	printf("\t\t * Any number of -g options are supported, up to command line limit.\n\n");
	printf("-G <group>\t Search for files NOT owned by specific group name or gid.\n");
	printf("\t\t * Any number of -G options are supported, up to command line limit.\n\n");
#endif

	printf("-o <days>\t Only print out files older than <days>, i.e. modified more than <days> ago.\n\n");
	printf("-O <minutes>\t Only print out files older than <minutes>, i.e. modified more than <minutes> ago.\n\n");
	printf("-P <tstamp-file> Only print out files older than <tstamp-file>, i.e. modified before <tstamp-file>.\n\n");
	printf("-y <days>\t Only print out files younger than <days>, i.e. modified less than <days> ago.\n\n");
	printf("-Y <minutes>\t Only print out files younger than <minutes>, i.e. modified less than <minutes> ago.\n\n");
	printf("-W <tstamp-file> Only print out files younger than <tstamp-file>, i.e. modified after <tstamp-file>.\n\n");

	printf("-s [+|-]<size>[k|m|g|t] | +<size>[k|m|g|t]:-<size>[k|m|g|t]\n");
	printf("\t\t Only print out files with size equal to, bigger than (+) or smaller than (-) <size> bytes,\n");
	printf("\t\t kibibytes (k), mebibytes (m), gibibytes (g) or tebibytes (t).\n");
	printf("\t\t * No space between [+|-] and <size> and [k|m|g|t] are allowed.\n");
	printf("\t\t * Modifiers +/- include the <size> given, e.g. -s+0 includes files of zero size, i.e. all files.\n");
	printf("\t\t * option -s+1 lists all non-zero files.\n");
	printf("\t\t * An interval can be specified using +<size>[k|m|g|t]:-<size>[k|m|g|t].\n");
	printf("\t\t * Only one -s option is currently supported.\n\n");

	printf("-D <count>\t Print out the path to the <count> directories containing the highest number of files together with this number.\n");
	printf("\t\t * Note that if the highest number of files is found in several directories, and <count> is 1, the\n");
	printf("\t\t   printed path is randomly chosen between these directories.  The same goes for any <count>.\n\n");
	printf("-F <count>\t Print out the path to the <count> biggest files together with the file size in bytes.\n");
	printf("\t\t * May be combined with options -o/-O/-y/-Y.\n");
	printf("\t\t   Particularly, -o may be useful to find the biggest files older than a chosen number of days.\n");
	printf("\t\t * May also be combined with options -u/-U/-g/-G.\n");
	printf("\t\t   Particularly, '-U root' may be useful to exclude files owned by root.\n");
	printf("\t\t * Note that if the biggest file size (default incl directory sizes) is found several times, and <count> is 1,\n");
	printf("\t\t   the printed path is randomly chosen between the equally sized files.  The same goes for any <count>.\n\n");
	printf("-M <count>\t Print out the path to the <count> most recently modified files/directories together with the time stamp.\n");
	printf("\t\t * Note that if the same time stamp is found on several files, and <count> is 1, the\n");
	printf("\t\t   printed path is randomly chosen between these.  The same goes for any <count> and identical time stamps.\n\n");
	printf("-A <count>\t Print out the path to the <count> most recently accessed files/directories together with the time stamp.\n");
	printf("\t\t * Same comment as for -M <count>.\n\n");
	printf("-L <count>\t Print out the path to the <count> least recently modified files/directories together with the time stamp.\n");
	printf("\t\t * Same comment as for -M <count>.\n\n");
	printf("-B <count>\t Print out the path to the <count> least recently accessed files/directories together with the time stamp.\n");
	printf("\t\t * Same comment as for -M <count>.\n\n");
	printf("-R <count>\t Print out the path to the <count> dirctories furthest from the root(s) together with the depth.\n");
	printf("\t\t * Note that if there are several directories at an equal depth, and <count> is 1, the\n");
	printf("\t\t   printed path is randomly chosen between these.  The same goes for any <count>.\n\n");
	printf("-I <count>\t Use <count> as number of subdirectories in a directory, that should\n");
	printf("\t\t be processed in-line instead of processing them in separate threads.\n");
	printf("\t\t * Default is to process up to two subdirectories in a directory in-line.\n");
	printf("\t\t * If there are no more than <count> subdirectories, all will be processed in-line.\n");
	printf("\t\t * If there are more than <count> subdirectories, say n, the first n - <count>\n");
	printf("\t\t   will be enqueued to avoid thread starvation.\n");
	printf("\t\t * This is a performance option to possibly squeeze out even faster run-times.\n");
	printf("\t\t * Use 0 for processing every subdirectory in a separate thread, and no in-line processing.\n\n");

	printf("-q\t\t Organize the queue of directories as a FIFO which may be faster in some cases (default is LIFO).\n");
	printf("\t\t * The speed difference between a LIFO and a FIFO queue is usually small.\n");
	printf("\t\t * Note that this option will make '%s' use more memory.\n\n", progname);
	printf("-Q\t\t Organize the queue of directories as a list sorted on inode number.\n");
	printf("\t\t * Using this option with a file system on a single (or mirrored) spinning disk is recommended.\n");
	printf("\t\t * Using it on a storage array or on SSD or FLASH disk is probably pointless.\n\n");

#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__)
	printf("-X\t\t May be used to speed up %s'ing eXtremely big directories containing millions of files.\n", progname);
	printf("\t\t * This option is probably only useful when the big directories being traversed are cached in memory.\n");
	printf("\t\t * With this option, default maximum number of dirents read in one go is 100000.\n");
	printf("\t\t * Environment variable DIRENTS may be set to override the default.\n");
	printf("\t\t * This option is only supported on Linux and *BSD flavors.\n\n");
#endif

	printf("-S\t\t Print some stats to stderr when finished.\n");
	printf("-T\t\t Print the elapsed real time between invocation and termination of the program on stderr, like time(1).\n");
	printf("-V\t\t Print out version and exit.\n");
	printf("-h\t\t Print this help text.\n");

	printf("\n* If no argument is specified, current directory (.) will be traversed, and all file and directory names found,\n");
	printf("  will be printed in no particular order.\n\n");
	printf("* If one argument (arg1) is specified, and this is a directory or a symlink to a directory, it will be traversed,\n");
	printf("  and all file and directory names found, will be printed in no particular order.\n\n");
	printf("* If one argument (arg1) is specified, and this is not a directory nor a symlink to a directory, option -N is assumed,\n");
	printf("  and file names matching shell pattern \"*arg1*\" (ignoring case) are searched for in current directory (including subdirs).\n\n");
	printf("* If more than one argument (arg1 arg2 ...) is specified, and the first is not a directory, option -N is assumed,\n");
	printf("  and file names matching shell pattern \"*arg1*\" (ignoring case) are searched for in remaining arguments (dirs) \"arg2\", ....\n\n");
	printf("* Ambiguity warning: If something like `%s pat pat' is executed, and \"pat\" is a directory,\n", progname);
	printf("  the files in the \"pat\" tree structure will silently be listed twice.  Use option -N, -n or -i if\n");
	printf("  the intention is to search for files/dirs matching \"pat\" in directory \"pat\".\n\n");
	printf("* Options [-F <count> | -M <count> | -A <count> | -L <count> | -B <count>] could slow down execution considerably\n");
	printf("  because they require an lstat(2) call for every file/directory in the specified directory tree(s).\n\n");
	printf("* Options [-f] [-d] [-l] [-b] [-c] [-p] [-s] may be combined in any order.\n");
	printf("  Note that using any of these might slow down the program considerably,\n");
	printf("  at least on AIX/HP-UX/Solaris because lstat(2) has to be called for every file.\n");
	printf("  These options may be combined with one of [-D <count> | -F <count> | -M <count> | -A <count> | -L <count> | -B <count>]\n");
	printf("  to list out only files, directories etc.\n\n");
	printf("* The program has been tested on these file systems:\n");
	printf("  - Linux: ext2, ext3, ext4, xfs, jfs, btrfs, nilfs2, f2fs, zfs, tmpfs\n");
	printf("           reiserfs, hfs plus, minix, bfs, ntfs (fuseblk), vxfs, gpfs\n");
	printf("  - FreeBSD: ufs, zfs, devfs, ms-dos/fat\n");
	printf("  - OpenBSD: ffs\n");
	printf("  - NetApp (systemshell@FreeBSD): clusfs\n");
	printf("  - MacOS: apfs\n");
	printf("  - AIX: jfs, jfs2, ahafs\n");
	printf("  - HP-UX: vxfs, hfs\n");
	printf("  - Solaris: zfs, ufs, udfs\n");
	printf("  - Windows (MinGW, Cygwin): ntfs\n");
	printf("  - All: nfs\n\n");
	printf("* The program contains code inspired by https://github.com/xaionaro/libpftw.\n");
	printf("* The program makes use of heap algorithms derived from https://gist.github.com/martinkunev/1365481.\n");
	printf("* The program makes direct use of heap struct and a couple of routines from BusyBox' `du' code, https://busybox.net.\n");
	printf("* The program makes use of a slightly modified version of https://github.com/coapp-packages/libgnurx when being built for Windows.\n\n");
	printf("* Note that symlinks below the start point(s), pointing to directories, are never followed.\n\n");
	printf("* Warning: This program may impose a very high load on your storage systems when utilizing many CPU cores.\n\n");
	printf("* The '%s' program comes with ABSOLUTELY NO WARRANTY.  This is free software, and you are welcome\n", progname);
	printf("  to redistribute it under certain conditions.  See the GNU General Public Licence for details.\n");

	printf("\nCopyright (C) 2020 - 2024 by Jorn I. Viken, jornv@1337.no.\n");
	return -1;
}

/////////////////////////////////////////////////////////////////////////////

int main(
	int argc,
	char *argv[])
{
	char **startdirs;
	unsigned startdircount;
	int ch, i;
	struct stat st;
	boolean stats = FALSE;
	boolean e_option = FALSE, E_option = FALSE;
	struct timeval starttime;
	boolean timer = FALSE;
	unsigned threads = 1;
#    if defined(__hpux)
	struct pst_dynamic psd;

	if (pstat_getdynamic(&psd, sizeof(psd), (size_t)1, 0))
		threads = (unsigned) psd.psd_proc_cnt;
#    elif defined(__MINGW32__)
	SYSTEM_INFO sysinfo;
	GetSystemInfo(&sysinfo);
	threads = sysinfo.dwNumberOfProcessors;
#    else
	threads = (unsigned) sysconf(_SC_NPROCESSORS_ONLN);
#    endif
	if (threads > 8)
		threads = 8; // - using 8 as default max number of threads

	progname = strrchr(argv[0], '/');
	if (progname)
		progname++; // - move pointer past the found '/'
	else
		progname = argv[0];

	debug = getenv("DEBUG") != NULL;
	tzset(); // - core dumps on Ubuntu 16.04.6 LTS with kernel 4.4.0-174-generic when executed through localtime() at the end of main()

#    if defined(__MINGW32__)
	while ((ch = getopt(argc, argv, "ht:I:e:E:n:N:i:afdm:wv:D:F:M:A:L:B:R:o:O:P:y:Y:W:xzju:U:0qQs:STVH")) != -1)
#    else
	setlocale(LC_ALL, "");
	while ((ch = getopt(argc, argv, "ht:I:e:E:n:N:i:afdlbcpkm:wv:D:F:M:A:L:B:R:o:O:P:y:Y:W:xzZju:U:g:G:0qQs:STVXH")) != -1)
#    endif
	switch (ch) {
		case 't':
			threads = atoi(optarg);
			if (threads < 1 || threads > MAX_THREADS)
				return usage();
			break;
		case 'I':
			inline_processing_threshold = atoi(optarg);
			break;
		case 'e':
			if (E_option) {
				fprintf(stderr, "Option -e can not be combined with -E.\n");
				exit(1);
			}
			excludelist = realloc(excludelist, (excludelist_count + 1) * sizeof(excludelist));
			assert(excludelist);
			excludelist[excludelist_count] = optarg;
			excluderecomp = realloc(excluderecomp, (excludelist_count + 1) * sizeof(excluderecomp));
			assert(excluderecomp);
			if (! regex_init(&excluderecomp[excludelist_count], optarg, REG_EXTENDED|REG_NOSUB))
				exit(1);
			excludelist_count++;
			e_option = TRUE;
			break;
		case 'E':
		case 'Z':
			if (e_option) {
				fprintf(stderr, "Option -E / -Z can not be combined with -e.\n");
				exit(1);
			}
			excludelist = realloc(excludelist, (excludelist_count + 1) * sizeof(excludelist));
			assert(excludelist);
			if (ch == 'E')
				excludelist[excludelist_count++] = optarg;
			else
				excludelist[excludelist_count++] = ".snapshot";
			E_option = TRUE;
			break;
		case 'n':
			if (zerosized) {
				fprintf(stderr, "Searching for empty directories in combination with a pattern is not supported.\n");
				exit(1);
			}
			if (regex_opt || fast_match_opt) {
				fprintf(stderr, "Option -n can't be specified multiple times nor combined with -i or -N.\n");
				fprintf(stderr, "Use re1|re2|... for multiple patterns.\n");
				exit(1);
			}
			if (*optarg == '!') {
				negate_match = TRUE;
				optarg++;
				if (! *optarg) {
					fprintf(stderr, "Need a pattern after '!' - bailing out.\n");
					exit(1);
				}
			}

			if (! regex_init(&regexcomp, optarg, REG_EXTENDED|REG_NOSUB))
				exit(1);

			regex_opt = TRUE;
			break;
		case 'N':
			if (zerosized) {
				fprintf(stderr, "Searching for empty directories in combination with a pattern is not supported.\n");
				exit(1);
			}
			if (regex_opt || fast_match_opt) {
				fprintf(stderr, "Option -N can't be specified multiple times nor combined with -n or -i.\n");
				exit(1);
			}
			if (! str_is_ascii(optarg)) {
				fprintf(stderr, "Only pure ASCII characters supported for option -N.\n");
				fprintf(stderr, "Please use -n or -i instead for localized characters.\n");
				exit(1);
			}
                        if (*optarg == '!') {
                                negate_match = TRUE;
                                optarg++;
                                if (! *optarg) {
                                        fprintf(stderr, "Need a pattern after '!' - bailing out.\n");
                                        exit(1);
                                }
                        }

			fast_match_opt = TRUE;
			fast_match_arg = strdup(optarg);
			str_to_lc(fast_match_arg);
			break;
		case 'i':
			if (zerosized) {
				fprintf(stderr, "Searching for empty directories in combination with a pattern is not supported.\n");
				exit(1);
			}
			if (regex_opt || fast_match_opt) {
				fprintf(stderr, "Option -i can't be specified multiple times nor combined with -n or -N.\n");
				fprintf(stderr, "Use re1|re2|... for multiple patterns.\n");
				exit(1);
			}
			if (*optarg == '!') {
				negate_match = TRUE;
				optarg++;
				if (! *optarg) {
					fprintf(stderr, "Need a pattern after '!' - bailing out.\n");
					exit(1);
				}
			}

			if (! regex_init(&regexcomp, optarg, REG_EXTENDED|REG_NOSUB|REG_ICASE))
				return usage();
			regex_opt = TRUE;
			break;
		case 'a':
			match_all_path_elems = TRUE;
			break;
		case 'f':
			filetypemask |= FILETYPE_REGFILE;
			lstat_needed = TRUE;
			break;
		case 'd':
			filetypemask |= FILETYPE_DIR;
#		      if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__APPLE__)
			lstat_needed = FALSE;
#		      else
			lstat_needed = TRUE;
#		      endif
			break;
		case 'l':	// Not for Windows
			filetypemask |= FILETYPE_SYMLINK;
			lstat_needed = TRUE;
			break;
		case 'b':
			filetypemask |= FILETYPE_BLOCKDEV;
			lstat_needed = TRUE;
			break;
		case 'c':
			filetypemask |= FILETYPE_CHARDEV;
			lstat_needed = TRUE;
			break;
		case 'p':
			filetypemask |= FILETYPE_PIPE;
			lstat_needed = TRUE;
			break;
		case 'k':	// Not for Windows
			filetypemask |= FILETYPE_SOCKET;
			lstat_needed = TRUE;
			break;
		case 'm':
			if (atoi(optarg) < 1)
				return usage();
			maxdepth = atoi(optarg);
			break;
		case 'w':
			wc = TRUE;
			if (zerosized) {
				fprintf(stderr, "Option -w can not be combined with -z.\n");
				exit(1);
			}
			just_count = TRUE;
			break;
		case 'H':
			summarize_diskusage = TRUE;
			just_count = TRUE;
			lstat_needed = TRUE;
			break;
		case 'v':
			if (atoi(optarg) < 1)
				return usage();
			verbose_count = atoi(optarg);
			last_t = time(NULL);
			break;
		case 'D':
			if (timestamp_or_size_on_heap || cntlongestdepth > 0) {
				fprintf(stderr, "Option -D can not be combined with -M, -A, -L, -B, -F, -R.\n");
				exit(1);
			}
			if (atoi(optarg) < 1)
				return usage();
			cntbiggestdirs = atoi(optarg);
			just_count = TRUE;
			break;
		case 'F':
			if (timestamp_or_size_on_heap || cntbiggestdirs > 0 || cntlongestdepth > 0) {
				fprintf(stderr, "Option -F can not be combined with -M, -A, -L, -B, -D, -R.\n");
				exit(1);
			}
			if (atoi(optarg) < 1)
				return usage();
			cntfattestfiles = atoi(optarg);
			timestamp_or_size_on_heap = TRUE;
			lstat_needed = TRUE;
			break;
		case 'x':
			xdev = TRUE;
			break;
		case 'z':
			zerosized = TRUE;
			if (wc || regex_opt || fast_match_opt) {
				fprintf(stderr, "Option -z can not be combined with -w, -n, -N, -i.\n");
				exit(1);
			}
			just_count = TRUE;
			break;
#if ! defined(__MINGW32__)
		case 'u':
			if (zerosized) {
				fprintf(stderr, "Option -u can not be combined with -z.\n");
				exit(1);
			}
			if (uidnegsearch) {
				fprintf(stderr, "Option -u can not be combined with -U.\n");
				exit(1);
			}
			uidlist = realloc(uidlist, (uidlist_count + 1) * sizeof(uidlist));
			assert(uidlist);
			uidlist[uidlist_count++] = isdigit((unsigned char)*optarg) ? (uid_t) atol(optarg) : username_to_uid_or_exit(optarg);
			uidsearch = TRUE;
			uid_or_gid = TRUE;
			lstat_needed = TRUE;
			break;
		case 'U':
			if (zerosized) {
				fprintf(stderr, "Option -U can not be combined with -z.\n");
				exit(1);
			}
			if (uidsearch) {
				fprintf(stderr, "Option -U can not be combined with -u.\n");
				exit(1);
			}
			uidlist = realloc(uidlist, (uidlist_count + 1) * sizeof(uidlist));
			assert(uidlist);
			uidlist[uidlist_count++] = isdigit((unsigned char)*optarg) ? (uid_t) atol(optarg) : username_to_uid_or_exit(optarg);
			uidnegsearch = TRUE;
			uid_or_gid = TRUE;
			lstat_needed = TRUE;
			break;
		case 'g':
			if (zerosized) {
				fprintf(stderr, "Option -g can not be combined with -z.\n");
				exit(1);
			}
			if (gidnegsearch) {
				fprintf(stderr, "Option -g can not be combined with -G.\n");
				exit(1);
			}
			gidlist = realloc(gidlist, (gidlist_count + 1) * sizeof(gidlist));
			assert(gidlist);
			gidlist[gidlist_count++] = isdigit((unsigned char)*optarg) ? (gid_t) atol(optarg) : groupname_to_gid_or_exit(optarg);
			gidsearch = TRUE;
			uid_or_gid = TRUE;
			lstat_needed = TRUE;
			break;
		case 'G':
			if (zerosized) {
				fprintf(stderr, "Option -G can not be combined with -z.\n");
				exit(1);
			}
			if (gidsearch) {
				fprintf(stderr, "Option -G can not be combined with -g.\n");
				exit(1);
			}
			gidlist = realloc(gidlist, (gidlist_count + 1) * sizeof(gidlist));
			assert(gidlist);
			gidlist[gidlist_count++] = isdigit((unsigned char)*optarg) ? (gid_t) atol(optarg) : groupname_to_gid_or_exit(optarg);
			gidnegsearch = TRUE;
			uid_or_gid = TRUE;
			lstat_needed = TRUE;
			break;
#endif
		case 'j':
			modtimelist = TRUE;
			break;
		case '0':
			end_with_null = TRUE;
			break;
		case 'M':
			if (timestamp_or_size_on_heap || cntbiggestdirs > 0 || cntlongestdepth > 0) {
				fprintf(stderr, "Option -M can not be combined with -A, -L, -N, -D, -F, -R.\n");
				exit(1);
			}
			if (atoi(optarg) < 1)
				return usage();
			cntmodmostrecently = atoi(optarg);
			timestamp_or_size_on_heap = TRUE;
			lstat_needed = TRUE;
			break;
		case 'A':
			if (timestamp_or_size_on_heap || cntbiggestdirs > 0 || cntlongestdepth > 0) {
				fprintf(stderr, "Option -A can not be combined with -M, -L, -N, -D, -F, -R.\n");
				exit(1);
			}
			if (atoi(optarg) < 1)
				return usage();
			cntaccmostrecently = atoi(optarg);
			timestamp_or_size_on_heap = TRUE;
			lstat_needed = TRUE;
			break;
		case 'L':
			if (timestamp_or_size_on_heap || cntbiggestdirs > 0 || cntlongestdepth > 0) {
				fprintf(stderr, "Option -A can not be combined with -M, -A, -N, -D, -F, -R.\n");
				exit(1);
			}
			if (atoi(optarg) < 1)
				return usage();
			cntmodleastrecently = atoi(optarg);
			timestamp_or_size_on_heap = TRUE;
			lstat_needed = TRUE;
			break;
		case 'B':
			if (timestamp_or_size_on_heap || cntbiggestdirs > 0 || cntlongestdepth > 0) {
				fprintf(stderr, "Option -A can not be combined with -M, -A, -L, -D, -F, -R.\n");
				exit(1);
			}
			if (atoi(optarg) < 1)
				return usage();
			cntaccleastrecently = atoi(optarg);
			timestamp_or_size_on_heap = TRUE;
			lstat_needed = TRUE;
			break;
		case 'R':
			if (timestamp_or_size_on_heap || cntbiggestdirs > 0) {
				fprintf(stderr, "Option -A can not be combined with -M, -A, -L, -N, -F, -D.\n");
				exit(1);
			}
			if (atoi(optarg) < 1)
				return usage();
			cntlongestdepth = atoi(optarg);
			just_count = TRUE;
			break;
		case 'o':
			if (atoi(optarg) < 1 || olderthan)
				return usage();
			olderthan = time(NULL) - atoi(optarg) * 24 * 60 * 60; // - days => seconds
			older_or_younger = TRUE;
			if (youngerthan) {
				if (olderthan <= youngerthan) {
					fprintf(stderr, "Derived argument to -o (%lu) must be greater than derived argument to -y / -Y / -W (%lu).\n",
						(unsigned long)olderthan, (unsigned long)youngerthan);
					exit(1);
				}
				older_and_younger = TRUE;
			}
			if (! zerosized)
				lstat_needed = TRUE;
			break;
		case 'O':
			if (atoi(optarg) < 1 || olderthan)
				return usage();
			olderthan = time(NULL) - atoi(optarg) * 60; // - minutes => seconds
			older_or_younger = TRUE;
			if (youngerthan) {
				if (olderthan <= youngerthan) {
					fprintf(stderr, "Derived argument to -O (%lu) must be greater than derived argument to -y / -Y / -W (%lu).\n",
						(unsigned long)olderthan, (unsigned long)youngerthan);
					exit(1);
				}
				older_and_younger = TRUE;
			}
			if (! zerosized)
				lstat_needed = TRUE;
			break;
		case 'P':
			if (lstat(optarg, &st)) {
				perror(optarg);
				exit(1);
			}
			olderthan = st.st_mtime;
			older_or_younger = TRUE;
			if (youngerthan) {
                                if (olderthan <= youngerthan) {
                                        fprintf(stderr, "Derived argument to -P (%lu) must be greater than derived argument to -y / -Y / -W (%lu).\n",
                                                (unsigned long)olderthan, (unsigned long)youngerthan);
                                        exit(1);
                                }
                                older_and_younger = TRUE;
                        }
			if (! zerosized)
				lstat_needed = TRUE;
	
			break;
		case 'y':
			if (atoi(optarg) < 1 || youngerthan)
				return usage();
			youngerthan = time(NULL) - atoi(optarg) * 24 * 60 * 60; // - days => seconds
			older_or_younger = TRUE;
			if (olderthan) {
				if (olderthan <= youngerthan) {
					fprintf(stderr, "Derived argument to -y (%lu) must be less than derived argument to -o / -O / -P (%lu).\n",
						(unsigned long)youngerthan, (unsigned long)olderthan);
					exit(1);
				}
				older_and_younger = TRUE;
			}
			if (! zerosized)
				lstat_needed = TRUE;
			break;
		case 'Y':
			if (atoi(optarg) < 1 || youngerthan)
				return usage();
			youngerthan = time(NULL) - atoi(optarg) * 60; // - minutes => seconds
			older_or_younger = TRUE;
			if (olderthan) {
				if (olderthan <= youngerthan) {
					fprintf(stderr, "Derived argument to -Y (%lu) must be less than derived argument to -o / -O / -P (%lu).\n",
						(unsigned long)youngerthan, (unsigned long)olderthan);
					exit(1);
				}
				older_and_younger = TRUE;
			}
			if (! zerosized)
				lstat_needed = TRUE;
			break;
		case 'W':
			if (lstat(optarg, &st)) {
				perror(optarg);
				exit(1);
			}
			youngerthan = st.st_mtime;
			older_or_younger = TRUE;
			if (olderthan) {
                                if (olderthan <= youngerthan) {
                                        fprintf(stderr, "Derived argument to -W (%lu) must be less than derived argument to -o / -O / -P (%lu).\n",
                                                (unsigned long)youngerthan, (unsigned long)olderthan);
                                        exit(1);
                                }
                                older_and_younger = TRUE;
                        }
			if (! zerosized)
				lstat_needed = TRUE;
			break;
		case 's':
			// Analyze the first character:
			if (! isdigit((int)*optarg)) {
				if (*optarg == '+')
					filesizeabove = TRUE;
				else if (*optarg == '-')
					filesizebelow = TRUE;
				else {
					fprintf(stderr, "Argument to -s (%s) must start with a digit or '+' or '-'.\n", optarg);
					exit(1);
				}
				optarg++;
			}
			// Second character must be a digit:
			if (! isdigit((int)*optarg)) {
				fprintf(stderr, "Second char (%c) in argument to -s must be a digit.\n", *optarg);
				exit(1);
			}

			// Check for ':' separator for interval search:
			char *lowhighseparator = strchr(optarg, ':');
			if (lowhighseparator) {
				// Then filesizeabove must be TRUE:
				if (! filesizeabove) {
					fprintf(stderr,
						"When choosing an interval for -s, the first value must be preceded by a '+'\n");
					exit(1);
				}
				unsigned long lowfactor = 1;
				char *lastcharbeforeseparator = lowhighseparator - 1;
				if (! isdigit((int)*lastcharbeforeseparator)) {
					switch(*lastcharbeforeseparator) {
                                        	case 'k':
                                        	case 'K': lowfactor = 1024; break;
                                        	case 'm':
                                        	case 'M': lowfactor = 1024*1024; break;
                                        	case 'g':
                                        	case 'G': lowfactor = 1024*1024*1024; break;
                                        	case 't':
                                        	case 'T': lowfactor = 1024*1024*1024*1024UL; break;
                                        	default:
                                                	fprintf(stderr,
								"Last char '%c' preceding ':' in argument to -s is not supported.\n",
                                                        	*lastcharbeforeseparator);
                                                	fprintf(stderr, "- must be a digit or one of k/K/m/M/g/G/t/T\n");
                                                	exit(1);

					}
					*lastcharbeforeseparator = '\0';
				}
				*lowhighseparator = '\0';
				char *lowendptr = NULL;
				lowfilesize = strtoul(optarg, &lowendptr, 10) * lowfactor;
				if (*lowendptr) {
					fprintf(stderr,
						"Argument to -s is not on a supported format - please try again using +<digit(s)>[k|K|m|M|g|G|t|T]\n");
                               		exit(1);
				}
				optarg = lowhighseparator + 1;
				if (*optarg == '-')
                                       	filesizebelow = TRUE;
				else {
					fprintf(stderr, "When choosing an interval for -s, the last value must be preceded by a '-'.\n");
	                                exit(1);
				}
				optarg++;
				if (! isdigit((int)*optarg)) {
					fprintf(stderr, "The upper value in the interval must be numeric, optionally followed by k/m/g/t\n");
					exit(1);
				}
			}

		        // Handle the last char in the argument:
			char *optarglastchar = strchr(optarg, '\0') - 1;
			unsigned long factor = 1;
			if (! isdigit((int)*optarglastchar)) {
				switch (*optarglastchar) {
					case 'k':
					case 'K': factor = 1024; break;
					case 'm':
					case 'M': factor = 1024*1024; break;
					case 'g':
					case 'G': factor = 1024*1024*1024; break;
					case 't':
					case 'T': factor = 1024*1024*1024*1024UL; break;
					default:
						fprintf(stderr, "Last char '%c' in argument to -s is not supported.\n",
							*optarglastchar);
						fprintf(stderr, "- must be a digit or one of [k|K|m|M|g|G|t|T].\n");
						exit(1);
				}
				*optarglastchar = '\0';
				optarglastchar--;
				if (! isdigit((int)*optarglastchar)) {
					fprintf(stderr, "Argument to -s is not supported - a digit must precede [k|K|m|M|g|G|t|T].\n");
					exit(1);
				}
			}
			char *endptr = NULL;
			filesize = strtoul(optarg, &endptr, 10) * factor;
			if (*endptr) {
				fprintf(stderr,
				"Argument to -s is not on a supported format - please try again using [+|-]<digit(s)>[k|K|m|M|g|G|t|T].\n");
				exit(1);
			}
			if (lowfilesize > filesize) {
				fprintf(stderr, "When an interval is specified with -s, the last value can't be lower than the first.\n");
				exit(1);
			}
			if (debug)
				fprintf(stderr, "lowfilesize=%lu, filesize=%lu\n", lowfilesize, filesize);
			filesizesearch = TRUE;
			lstat_needed = TRUE;
			break;
		case 'q':
			fifo_queue = TRUE;
			lifo_queue = FALSE;
			ino_queue = FALSE;
			break;
		case 'Q':
			ino_queue = TRUE;
			lifo_queue = FALSE;
			fifo_queue = FALSE;
			break;
		case 'S':
			stats = TRUE;
			break;
		case 'T':
			timer = TRUE;
			(void) gettimeofday(&starttime, NULL);
			break;
		case 'V':
			printf("%s\n", VERSION);
			exit(0);
			break;
		case 'X':
#		      if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__)
			extreme_readdir = TRUE;
			if (getenv("DIRENTS"))
				buf_size = atoi(getenv("DIRENTS")) * sizeof(struct dirent);
			else
				buf_size = DEFAULT_DIRENT_COUNT * sizeof(struct dirent);
#		      else
			fprintf(stderr, "Option -X is not implemented for this OS.\n");
			exit(1);
#		      endif
			break;
		case 'h':
		case '?':
		default:
			return usage();
	}

	argc -= optind;
	argv += optind;

	static char *buf[2]; // both bytes initialized to 0 by "static"
	startdirs = buf;
	if (argc == 0) {
		startdirs[0] = ".";
		startdircount = 1;
	} else {
		int rc;
		rc = stat(argv[0], &st); // - this is just a test to find out if the first arg is a directory - a symlink to a dir is accepted as a dir
		statcount++;

		if (! rc && S_ISDIR(st.st_mode)) {
			if (debug)
				fprintf(stderr, "%s is a directory which will be traversed (if possible).\n", argv[0]);
			if (st.st_nlink < 2)
				simulate_posix_compliance = TRUE;
			startdirs = argv;
			startdircount = argc;
		} else {
			if (zerosized) {
				fprintf(stderr, "Searching for empty directories in combination with a pattern is not supported.\n");
				exit(1);
			}
			if (strchr(argv[0], '/') && ! match_all_path_elems) {
				// - Something looking like a directory path was given, but not identified as such.
				fprintf(stderr, "Directory %s not found - bailing out.\n", argv[0]);
				exit(1);
			}
			if (debug)
				fprintf(stderr, "%s is NOT interpreted as a directory, but as a file name to search for.\n", argv[0]);
			fast_match_opt = TRUE;
                        if (! str_is_ascii(argv[0])) {
                                fprintf(stderr, "Only pure ASCII characters supported for (silently selected) option -N.\n");
                                fprintf(stderr, "Please use -n or -i instead for localized characters.\n");
                                exit(1);
                        }
			if (*argv[0] == '!') {
				negate_match = TRUE;
				argv[0]++;
				if (! *argv[0]) {
					fprintf(stderr, "Need a pattern after '!' - bailing out.\n");
					exit(1);
				}
			}
			fast_match_arg = strdup(argv[0]);
			str_to_lc(fast_match_arg);
			if (argc == 1) {
				startdirs[0] = ".";
				startdircount = 1;
				if (summarize_diskusage)
					maxdepth = 1;
			} else {
				if (summarize_diskusage) {
					fprintf(stderr, "Only one single non-directory argument is supported for option -H.\n");
					fprintf(stderr, "(Multiple directories are supported when directories are the only arguments.)\n");
					exit(1);
				}
				startdirs = argv+1;
				startdircount = argc-1;
			}
		}
	}

	if (cntbiggestdirs > 0) {
		if (heap_init(cntbiggestdirs)) {
			perror("option -D");
			exit(1);
		}
	} else if (cntlongestdepth > 0) {
		if (heap_init(cntlongestdepth)) {
			perror("option -R");
			exit(1);
		}
	} else if (cntmodmostrecently > 0) {
		if (heap_init(cntmodmostrecently)) {
			perror("option -M");
			exit(1);
		}
	} else if (cntaccmostrecently > 0) {
		if (heap_init(cntaccmostrecently)) {
			perror("option -A");
			exit(1);
		}
	} else if (cntmodleastrecently > 0) {
		if (heap_init(cntmodleastrecently)) {
			perror("option -L");
			exit(1);
		}
	} else if (cntaccleastrecently > 0) {
		if (heap_init(cntaccleastrecently)) {
			perror("option -N");
			exit(1);
		}
	} else if (cntfattestfiles > 0) {
		if (heap_init(cntfattestfiles)) {
			perror("option -F");
			exit(1);
		}
	}

	if (! filetypemask || filetypemask & FILETYPE_DIR) {
		for (i = 0; i < startdircount; i++) {
		    char *dirname = strrchr(startdirs[i], '/'); // remove final / if any
		    if (dirname && dirname[1]) // - don't remove / if startdirs[i] equals /
			dirname++;
		    else
			dirname = startdirs[i];

		    if (filename_match(dirname)) {
			if (debug)
				fprintf(stderr, "filename_match() returned TRUE for startdir = %s\n", dirname);

			if (stat(startdirs[i], &st) < 0 || ! S_ISDIR(st.st_mode)) {
				fprintf(stderr, "Directory %s not found - bailing out.\n", startdirs[i]);
				exit(1);
			}
			statcount++;

			if ((just_count || verbose_count)
			    && modtimecheck(st.st_mtime)
			    && uidgidcheck(st.st_uid, st.st_gid)
			    && sizecheck(st.st_size))
				accum_filecnt++;
			else if (timestamp_or_size_on_heap) {
				if (cntmodmostrecently > 0) {
					heap_elem_t *elem = heap_new_elem(st.st_mtime, startdirs[i]);
					if (! heap_push(elem, ASCEND))
						heap_free_elem(elem);
				} else if (cntaccmostrecently > 0) {
					heap_elem_t *elem = heap_new_elem(st.st_atime, startdirs[i]);
					if (! heap_push(elem, ASCEND))
						heap_free_elem(elem);
				} else if (cntmodleastrecently > 0) {
					heap_elem_t *elem = heap_new_elem(st.st_mtime, startdirs[i]);
					if (! heap_push(elem, DESCEND))
						heap_free_elem(elem);
				} else if (cntaccleastrecently > 0) {
					heap_elem_t *elem = heap_new_elem(st.st_atime, startdirs[i]);
					if (! heap_push(elem, DESCEND))
						heap_free_elem(elem);
				} else if (cntfattestfiles > 0) {
#                             	      if defined(__sun__)
                                	if (S_ISBLK(st.st_mode))
                                        	st.st_size = 0;
#                             	      endif
                                	if (modtimecheck(st.st_mtime)
					    && uidgidcheck(st.st_uid, st.st_gid)
					    && sizecheck(st.st_size)) {
						heap_elem_t *elem = heap_new_elem((unsigned long long)st.st_size, startdirs[i]);
						if (! heap_push(elem, ASCEND))
							heap_free_elem(elem);
					}
				}
			} else if (modtimecheck(st.st_mtime)
				   && uidgidcheck(st.st_uid, st.st_gid)
				   && sizecheck(st.st_size)
				   && ! zerosized) {
				if (end_with_null)
					printf("%s%c", startdirs[i], '\0');
				else if (modtimelist) {
					char *timestr = printable_time(st.st_mtime);
					printf("%s %s\n", timestr, startdirs[i]);
					free(timestr);
				} else
					puts(startdirs[i]);
			}
		    } // if (filename_match(dirname)
		} // for (i = 0; i < startdircount; i++)
	} // if (! filetypemask || filetypemask & FILETYPE_DIR)

	if (threads == 1)
		inline_processing_threshold = DIRTY_CONSTANT; // - process everything inline if we have just 1 CPU...
	thread_cnt = threads;
	thread_prepare();

	traverse_trees(startdirs, startdircount);

	thread_cleanup();

	if (timestamp_or_size_on_heap) {
		i = cntmodmostrecently + cntaccmostrecently + cntmodleastrecently + cntaccleastrecently + cntfattestfiles;
		while (i--) {
			heap_elem_t *popped = cntmodleastrecently | cntaccleastrecently ?
						heap_pop(DESCEND) : heap_pop(ASCEND);
			if (! popped)
				break;
			if (cntfattestfiles > 0) {
#			      if defined(__MINGW32__)
				printf("%-19ld %s\n", (long) popped->metadata, popped->name);
#			      else
				printf("%-19lld %s\n", popped->metadata, popped->name);
#			      endif
			} else {
				char *timestr = printable_time(popped->metadata);
				printf("%s %s\n", timestr, popped->name);
				free(timestr);
			}
			heap_free_elem(popped);
		}
		heap_term();
	} else if (cntbiggestdirs > 0) {
		while (cntbiggestdirs--) {
			heap_elem_t *popped = heap_pop(ASCEND);
			if (! popped)
				break;
#		      if defined(__MINGW32__)
			printf("%-19ld %s\n", (long) popped->metadata, popped->name);
#		      else
			printf("%-19lld %s\n", popped->metadata, popped->name);
#		      endif
			heap_free_elem(popped);
		}
		heap_term();
	} else if (cntlongestdepth > 0) {
		while (cntlongestdepth--) {
			heap_elem_t *popped = heap_pop(ASCEND);
			if (! popped)
				break;
#		      if defined(__MINGW32__)
			printf("%-19ld %s\n", (long) popped->metadata, popped->name);
#		      else
			printf("%-19lld %s\n", popped->metadata, popped->name);
#		      endif
			heap_free_elem(popped);
		}
		heap_term();
	}

	if (summarize_diskusage) {
		off_t KiB = accum_du / 2UL;

		if (KiB < 1024)
			printf("%-.1lFK", (double) KiB);
		else if (KiB < 1024 * 1024)
			printf("%-.1lFM", (double) KiB/1024);
		else if (KiB < 1024 * 1024 * 1024)
			printf("%-.1lFG", (double) KiB/1024/1024);
		else
			printf("%-.1lFT", (double) KiB/1024/1024/1024);
		for (i = 0; i < startdircount; i++) {
			printf("\t%s", startdirs[i]);
		}
		printf("\n");
	}
	if (wc)
		printf("%u\n", accum_filecnt);

	if (timer) {
		struct timeval endtime;
		(void) gettimeofday(&endtime, NULL);
		fflush(stdout);
		fprintf(stderr, "Real: %.2f seconds\n",
			(double)(((endtime.tv_sec-starttime.tv_sec)*1000 + (endtime.tv_usec-starttime.tv_usec) / 1000)) / 1000);
	}

	if (stats) {
		fprintf(stderr, "+------------------------------+\n");
		fprintf(stderr, "| Some final tidbits from \"-S\" |\n");
		fprintf(stderr, "+------------------------------+\n");
		fprintf(stderr, "- Version: %s\n", VERSION);
		fprintf(stderr, "- Number of threads used: %i\n", threads);
		fprintf(stderr, "- Max number of subdirectories that could be processed in-line per directory\n");
		fprintf(stderr, "  (and not in a separate thread): %lu\n", inline_processing_threshold);
#if 	      defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__)
		if (extreme_readdir) {
			fprintf(stderr, "- Number of SYS_getdents system calls = %u\n", getdents_calls);
			fprintf(stderr, "- Used DIRENTS = %lu\n", (unsigned long)buf_size / sizeof(struct dirent));
		}
#	      endif
		fprintf(stderr, "- Mandatory lstat calls (at least 1 per directory): %i\n", statcount);
#	      if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__APPLE__)
		fprintf(stderr, "- Unexpected lstat calls (when returned d_type is DT_UNKNOWN): %i\n", statcount_unexp);
#	      endif
		fprintf(stderr, "- Number of %s enqueued directories: %i\n", fifo_queue ? "FIFO" : (ino_queue ? "INODE" : "LIFO"), queued_dirs);
		if (ino_queue) {
			fprintf(stderr, "- INO queue insert bypasscount: %lu\n", inolist_bypasscount);
		}
#             if defined(PR_ATOMIC_ADD)
		fprintf(stderr, "- Program compiled with support for __sync_add_and_fetch\n");
#             endif
#	      if defined(CC_USED)
		fprintf(stderr, "- Compiled using: %s\n", CC_USED);
#	      endif
	}
	return 0;
}
