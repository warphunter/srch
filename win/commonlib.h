/*
   srch - parallel file search with simplified and extended find(1) functionality

   Copyright (C) 2020 - 2023 by Jorn I. Viken <jornv@1337.no>

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

/////////////////////////////////////////////////////////////////////////////

// For LIFO queue - default
static inline __attribute__((always_inline)) void lifodirlist_insert(
        dirlist_t *newdir)
{
	pthread_mutex_lock(&dirlist_lock);
        if (! dirlist_head) {
                dirlist_head = newdir;
                dirlist_head->next = NULL;
        } else {
		dirlist_t *old_head = dirlist_head;
		dirlist_head = newdir;
                dirlist_head->next = old_head;
        }
	queuesize++;
	pthread_mutex_unlock(&dirlist_lock);
}

/////////////////////////////////////////////////////////////////////////////

// For LIFO queue - default
static inline __attribute__((always_inline)) dirlist_t *lifodirlist_extract()
{
	pthread_mutex_lock(&dirlist_lock);
        if (! dirlist_head) {
		pthread_mutex_unlock(&dirlist_lock);
		return NULL;
	}
	dirlist_t *first = dirlist_head;
	dirlist_head = dirlist_head->next;
	queuesize--;
	pthread_mutex_unlock(&dirlist_lock);
	return first;
}

/////////////////////////////////////////////////////////////////////////////

// For FIFO queue - used if option -q is selected
static inline __attribute__((always_inline)) void fifodirlist_insert(
	dirlist_t *newdir)
{
	pthread_mutex_lock(&dirlist_lock);
	if (! dirlist_head) {
		dirlist_head = dirlist_tail = newdir;
		dirlist_head->next = NULL;
	} else {
		dirlist_tail->next = newdir;
		dirlist_tail = dirlist_tail->next;
		dirlist_tail->next = NULL;
	}
	queuesize++;
	pthread_mutex_unlock(&dirlist_lock);
}

/////////////////////////////////////////////////////////////////////////////

// For FIFO queue - used if option -q is selected
static inline __attribute__((always_inline)) dirlist_t *fifodirlist_extract()
{
	dirlist_t *first;

	pthread_mutex_lock(&dirlist_lock);
	if (! dirlist_head) {
		pthread_mutex_unlock(&dirlist_lock);
		return NULL;
	}
	first = dirlist_head;
	if (dirlist_head == dirlist_tail)
		dirlist_head = dirlist_tail = NULL;
	else
		dirlist_head = dirlist_head->next;
	queuesize--;
	pthread_mutex_unlock(&dirlist_lock);
	return first;
}

/////////////////////////////////////////////////////////////////////////////

// For inode queue - used if option -Q is selected
static inline __attribute__((always_inline)) void inodirlist_bintreeinsert(
	dirlist_t *newdir)
{
	dirlist_t *current;
	dirlist_t *prev = NULL;
	newdir->next = NULL;
	newdir->prev = NULL;

	pthread_mutex_lock(&dirlist_lock);
	current = dirlist_head;
	while (current) {
		prev = current;
		inolist_bypasscount++;

		if (newdir->st_ino < current->st_ino) {
			current = current->prev;
		} else {
			current = current->next;
		}
	}

	if (! prev) {
       		dirlist_head = newdir;
    	} else if (newdir->st_ino < prev->st_ino) {
       		prev->prev = newdir;
	} else {
		prev->next = newdir;
	}
        queuesize++;
	pthread_mutex_unlock(&dirlist_lock);
}

/////////////////////////////////////////////////////////////////////////////

// For inode queue - used if option -Q is selected
static inline __attribute__((always_inline)) dirlist_t *inodirlist_bintreeextract()
{
	dirlist_t *current;
	dirlist_t *previous = NULL;

       	pthread_mutex_lock(&dirlist_lock);
	current = dirlist_head;
	if (! queuesize) {
       		pthread_mutex_unlock(&dirlist_lock);
		return NULL;
	}
	while (current->prev) {
		// We do have at least a child to the left
		previous = current;
		current = current->prev;
	}
	if (previous)
		previous->prev = current->next;
	else
		dirlist_head = current->next;

       	queuesize--;
       	pthread_mutex_unlock(&dirlist_lock);
	return current;
}

/////////////////////////////////////////////////////////////////////////////

static boolean regex_init(
	regex_t **recomp,
	char *optarg,
	int flags)
{
	*recomp = malloc(sizeof(**recomp));
	size_t errcode = regcomp(*recomp, optarg, flags);
	char errbuf[LINE_MAX];
	if (errcode) {
		regerror(errcode, *recomp, errbuf, sizeof errbuf);
		fprintf(stderr, "%s: Compiling regular expression specified with -n/-i failed with message:\n%s\n\n", progname, errbuf);
		return FALSE;
	}
	return TRUE;
}

/////////////////////////////////////////////////////////////////////////////

#if defined(SRCH)
// This routine is only correct for pure ASCII A - Z (and a - z)

static void str_to_lc(
	char *str)
{
	while (*str) {
               	*str = tolower((int)*str);
                str++;
        }
}

/////////////////////////////////////////////////////////////////////////////

static boolean str_is_ascii(
	char *str)
{
	while (*str) {
		if (! isascii(*str))
			return FALSE;
		str++;
	}
	return TRUE;
}
#endif

/////////////////////////////////////////////////////////////////////////////

static inline __attribute__((always_inline)) char *printable_time(
	time_t modtime)
{
	char timestr[] = "year-mm-dd hh:mm:ss";
	struct tm ret;
	(void) localtime_r(&modtime, &ret);
	snprintf(timestr, sizeof(timestr), "%4d-%02d-%02d %02d:%02d:%02d",
			1900+ret.tm_year%1000u, (ret.tm_mon+1)%100u, ret.tm_mday%100u,
			ret.tm_hour%100u, ret.tm_min%100u, ret.tm_sec%100u);
	return strdup(timestr);
}

/////////////////////////////////////////////////////////////////////////////

static inline __attribute__((always_inline)) void dirlist_add_dir(
	const char *dirpath,
	int depth,
	struct stat *st)
{
	dirlist_t *new_dir = malloc(sizeof(dirlist_t));
	assert(new_dir);

	new_dir->dirpath 	= strdup(dirpath);
	assert(new_dir->dirpath);
	new_dir->depth   	= depth;
	new_dir->inlined	= 0;
	new_dir->filecnt	= 0;
#     if defined(SRCH)
	new_dir->du		= 0;
#     elif defined(RMTREE)
	new_dir->all_inlined    = TRUE;
#     elif defined(CHOWNTREE)
        new_dir->st_uid         = st->st_uid;
        new_dir->st_gid         = st->st_gid;
#     endif

	assert(st); // st should always be filled at this point

	new_dir->st_nlink = simulate_posix_compliance ? DIRTY_CONSTANT : st->st_nlink; // - simulate POSIX compliant link count for BTRFS a.o.
	new_dir->st_dev = st->st_dev;
#     if defined(SRCH)
	new_dir->modtime = st->st_mtime;
#     elif defined(CHMODTREE)
	new_dir->st_mode = st->st_mode;
#     endif

	if (lifo_queue) {
                lifodirlist_insert(new_dir);
        } else if (fifo_queue) {
                fifodirlist_insert(new_dir);
	} else if (ino_queue) {
		new_dir->st_ino = st->st_ino;
		inodirlist_bintreeinsert(new_dir);
        } else {
		fprintf(stderr, "Queue type not implemented - bailing out.\n");
		exit(1);
	}

#if ! defined(__APPLE__)
	if (sem_post(&threads_sem)) {
#	      if defined(PR_ATOMIC_ADD)
		PR_ATOMIC_ADD(&sem_val_max_exceeded_cnt, 1);
#	      else
		pthread_mutex_lock(&sem_val_max_exceeded_cnt_lock);
		sem_val_max_exceeded_cnt++;
		pthread_mutex_unlock(&sem_val_max_exceeded_cnt_lock);
#	      endif
	}
#else
        if (dispatch_semaphore_signal(threads_sem) == 0) {
#	      if defined(PR_ATOMIC_ADD)
		PR_ATOMIC_ADD(&sem_val_max_exceeded_cnt, 1);
#	      else
                pthread_mutex_lock(&sem_val_max_exceeded_cnt_lock);
                sem_val_max_exceeded_cnt++;
                pthread_mutex_unlock(&sem_val_max_exceeded_cnt_lock);;
#	      endif
        }
#endif

#     if defined(PR_ATOMIC_ADD)
	PR_ATOMIC_ADD(&queued_dirs, 1);
#     else
	pthread_mutex_lock(&queued_dirs_lock);
	queued_dirs++;
	pthread_mutex_unlock(&queued_dirs_lock);
#     endif

	return;
}

/////////////////////////////////////////////////////////////////////////////

static inline __attribute__((always_inline)) void incr_sleepers()
{
#     if defined(PR_ATOMIC_ADD)
        PR_ATOMIC_ADD(&sleeping_thread_cnt, 1);
#     else
	pthread_mutex_lock(&sleeping_thread_cnt_lock);
	sleeping_thread_cnt++;
	pthread_mutex_unlock(&sleeping_thread_cnt_lock);
#     endif
}

/////////////////////////////////////////////////////////////////////////////

static inline __attribute__((always_inline)) void decr_sleepers()
{
#     if defined(PR_ATOMIC_ADD)
        PR_ATOMIC_ADD(&sleeping_thread_cnt, -1);
#     else
	pthread_mutex_lock(&sleeping_thread_cnt_lock);
	sleeping_thread_cnt--;
	pthread_mutex_unlock(&sleeping_thread_cnt_lock);
#     endif
}

/////////////////////////////////////////////////////////////////////////////

static inline __attribute__((always_inline)) dirlist_t *dirlist_pull_dir()
{
	dirlist_t *nextdir;

#     if ! defined(__APPLE__)
	incr_sleepers();
	if (sleeping_thread_cnt == thread_cnt)
		sem_post(&master_sem);  // - wake up master and let it decide if the show is over
	sem_wait(&threads_sem);
	decr_sleepers();
#     else // __APPLE__
	incr_sleepers();
        if (sleeping_thread_cnt == thread_cnt)
                dispatch_semaphore_signal(master_sem);
        dispatch_semaphore_wait(threads_sem, DISPATCH_TIME_FOREVER);
	decr_sleepers();
#     endif

	// Invariant: There is at least one entry in the Q here:
	if (lifo_queue) {
		nextdir = lifodirlist_extract();
	} else if (fifo_queue) {
		nextdir = fifodirlist_extract();
	} else if (ino_queue) {
		nextdir = inodirlist_bintreeextract();
	} else {
		fprintf(stderr, "Queue type not implemented - bailing out.\n");
		exit(1);
	}

	if (! nextdir)
		return NULL;

	return nextdir;
}

/////////////////////////////////////////////////////////////////////////////

#if defined(__linux__) || defined(__FreeBSD__) || defined(__OpenBSD__)

// The framework for this code is borrowed from the getdents(2) Linux man page.
static void readdir_extreme(
	unsigned fd,
	char *buf,
	unsigned buf_size,
	char *dirpath,
	unsigned *pos,
	struct dirent *dent,
	unsigned *returned)
{
	struct dirent *d;

#if defined (__OpenBSD__)
	start:
#endif
	if (*pos == *returned) {
		getdents_calls++;
#	      if defined(__linux__)
		*returned = syscall(SYS_getdents, fd, buf, buf_size);
#	      else
		*returned = getdents(fd, buf, buf_size);
#	      endif
		if (*returned == -1) {
			//perror("getdents()");
			fprintf(stderr, "%s: Unable to read directory %s\n", progname, dirpath);
			*returned = 0;
			return;
		}

		if (*returned == 0) // - nothing more left
			return;
		*pos = 0;
	}
	d = (struct dirent *) (buf + *pos);
	//dent->d_ino = d->d_ino;
	//dent->d_reclen = d->d_reclen;
#     if defined(__linux__)
	dent->d_type = *(buf + *pos + d->d_reclen-1);
	strcpy(dent->d_name, d->d_name-1);
#     elif defined(__OpenBSD__)
	if (! d->d_fileno) { // - bogus entry
		*pos += d->d_reclen;
		goto start;
	}
	dent->d_type = d->d_type;
	//printf("<%s> (%i)\n", d->d_name, d->d_fileno);
	strcpy(dent->d_name, d->d_name);
#     else
	dent->d_type = d->d_type;
	strcpy(dent->d_name, d->d_name);
#     endif
	*pos += d->d_reclen;
	return;
}

#endif

/////////////////////////////////////////////////////////////////////////////

static inline __attribute__((always_inline)) time_t get_mtime(
	char *path)
{
	struct stat st;
	int rc;
        // assuming -j is rarely used - only perform an lstat when actually needed...
        rc = lstat(path, &st);
        if (rc) {
        	st.st_mtime = 0;
                pthread_mutex_lock(&perror_lock);
		fprintf(stderr, "%s: ", progname);
                perror(path);
                pthread_mutex_unlock(&perror_lock);
	}

#     if defined(PR_ATOMIC_ADD)
        PR_ATOMIC_ADD(&statcount, 1);
#     else
        pthread_mutex_lock(&statcount_lock);
        statcount++;
        pthread_mutex_unlock(&statcount_lock);
#     endif

	return st.st_mtime;
}

/////////////////////////////////////////////////////////////////////////////

#if defined(RMTREE)
static char *gettime() {
        time_t now;
        time(&now);
        char *timestr = ctime(&now);
        *(timestr+strlen(timestr)-1) = 0;
        return timestr;
}
#endif

/////////////////////////////////////////////////////////////////////////////

static void traverse_trees(
	char **dirpaths,
	int dirpathcount)
{
	int rc = 0, i;
	struct stat st;
	int verified_startdircount = dirpathcount;

	for (i = 0; i < dirpathcount; i++) {
		if (! dirpaths[i]) {
			verified_startdircount--;
			continue;
		}

		rc = stat(dirpaths[i], &st);
		statcount++;
		if (rc) {
			errno = ENOENT;
			fprintf(stderr, "%s: ", progname);
			perror(dirpaths[i]);
			verified_startdircount--;
			dirpaths[i] = NULL;
			continue;
		}
		if (! S_ISDIR(st.st_mode)) {
			errno = ENOTDIR;
			fprintf(stderr, "%s: ", progname);
			perror(dirpaths[i]);
			verified_startdircount--;
			dirpaths[i] = NULL;
			continue;
		}

#	      if defined(__sun__)
		struct statvfs stfs;
		if (statvfs(dirpaths[i], &stfs) < 0) {
			fprintf(stderr, "%s: ", progname);
			perror("statfs()");
			return;
		}
		if (strcmp(stfs.f_basetype, "hsfs") == 0
			|| strcmp(stfs.f_basetype, "udfs") == 0
			|| strcmp(stfs.f_basetype, "proc") == 0)
				simulate_posix_compliance = TRUE;
		// Solaris file system is assumed to be POSIX compliant if fstype != hsfs && fstype != udfs && fstype != proc

		if (debug)
			fprintf(stderr, "FStype(%s) = %s, POSIX compliance = %s\n",
				dirpaths[i], stfs.f_basetype, simulate_posix_compliance ? "FALSE" : "TRUE");
#	      endif

		if (st.st_nlink < 2) // - assumed to be the case for btrfs, fuse, ntfs, fat
			simulate_posix_compliance = TRUE;

#	      if defined(_AIX)
		if (debug)
			fprintf(stderr, "Startdir: %s, POSIX compliance = %s\n",
				dirpaths[i], simulate_posix_compliance ? "FALSE" : "TRUE");
		// - for AIX, see /usr/include/sys/vmount.h for supported f_vfstype
#	      else
		if (debug)
			fprintf(stderr, "%s, POSIX compliance = %s\n",
				dirpaths[i], simulate_posix_compliance ? "FALSE" : "TRUE");
#	      endif

		//char *rind = rindex(dirpaths[i], '/');
		size_t slen = strlen(dirpaths[i]);
		char *rightmost = dirpaths[i] + slen - 1;
		while (*rightmost == '/' && rightmost != dirpaths[i]) {
			*rightmost = '\0';
			rightmost--;
		}
		dirlist_add_dir(dirpaths[i], 1, &st);
	}

#     if defined(RMTREE) || defined(CHMODTREE) || defined(CHOWNTREE)
        if (! verified_startdircount) {
                fprintf(stderr, "No valid path given - bailing out!\n");
                exit(1);
        }
#     endif

	do {
#             if ! defined(__APPLE__)
		sem_wait(&master_sem);
#             else
		dispatch_semaphore_wait(master_sem, DISPATCH_TIME_FOREVER);
#             endif

#	     if defined(DEBUG3)
		if (getenv("DEBUG3"))
			fprintf(stderr, "traverse_trees(): MASTER woken up - sleepers = %i\n", sleeping_thread_cnt);
#	     endif
		pthread_mutex_lock(&sem_val_max_exceeded_cnt_lock);
		while (sem_val_max_exceeded_cnt) {
#                     if ! defined(__APPLE__)
			if (sem_post(&threads_sem))
				break;
#		      else
                        if (dispatch_semaphore_signal(threads_sem) == 0)
                                break;
#		      endif
			sem_val_max_exceeded_cnt--;
		}
		pthread_mutex_unlock(&sem_val_max_exceeded_cnt_lock);
	} while (queuesize > 0 || sleeping_thread_cnt < thread_cnt);

#     if defined(RMTREE)
        if (! dryrun) {
                // Final phase starts here.
                // All remaining empty directories will be deleted.
                final_pass = TRUE;
                for (i = 0; i < dirpathcount; i++) {
                        if (! dirpaths[i])
                                continue;

                        if (debug)
                                fprintf(stderr, "\n%s - starting final cleanup for directory: %s\n", gettime(), dirpaths[i]);

                        struct stat st;
                        rc = lstat(dirpaths[i], &st);
                        if (rc < 0)
                                continue;
                        statcount++;
#            	      if defined(DEBUG3)
			if (getenv("DEBUG3"))
                        	fprintf(stderr, "traverse_trees() - running FINAL dirlist_add_dir()\n");
#		      endif
                        dirlist_add_dir(dirpaths[i], 1, &st);
                }

                while (queuesize > 0 || sleeping_thread_cnt < thread_cnt) {
#                     if ! defined(__APPLE__)
                        sem_wait(&master_sem);
#                     else
                        dispatch_semaphore_wait(master_sem, DISPATCH_TIME_FOREVER);
#                     endif

#            if defined(DEBUG3)
                        if (getenv("DEBUG3"))
                                fprintf(stderr, "traverse_trees(): MASTER woken up - sleepers = %i\n", sleeping_thread_cnt);
#            endif
                        pthread_mutex_lock(&sem_val_max_exceeded_cnt_lock);
                        while (sem_val_max_exceeded_cnt) {
#			      if ! defined(__APPLE__)
                                if (sem_post(&threads_sem))
                                        break;
#			      else
				if (dispatch_semaphore_signal(threads_sem) == 0)
					break;
#			      endif
                                sem_val_max_exceeded_cnt--;
                        }
                        pthread_mutex_unlock(&sem_val_max_exceeded_cnt_lock);
                }
        }
#     endif

	master_finished = TRUE;
	if (debug)
		fprintf(stderr, "traverse_trees() - MASTER loop FINISHED\n");

	for (i = 0; i < thread_cnt; i++) {
#if ! defined(__APPLE__)
		sem_post(&threads_sem);
#else
		dispatch_semaphore_signal(threads_sem);
#endif
	}

	if (debug)
		fprintf(stderr, "traverse_trees() - waiting for threads to finish\n");

	for (i = 0; i < thread_cnt; i++) {
#             if ! defined(__APPLE__)
		sem_wait(&finished_threads_sem);
#             else
		dispatch_semaphore_wait(finished_threads_sem, DISPATCH_TIME_FOREVER);
#             endif
	}

#     if defined(RMTREE)
        for (i = 0; i < dirpathcount; i++) {
                if (! dirpaths[i] || (*dirpaths[i] == '.' && *(dirpaths[i]+1) == '\0'))
                        continue;
                rc = lstat(dirpaths[i], &st);
                if (rc < 0) {
			if (debug)
				fprintf(stderr, "Startdir %s already removed\n", dirpaths[i]);
                        continue;
		}
                statcount++;
                if (debug)
                        fprintf(stderr, "Removing startdir: %s\n", dirpaths[i]);
                if (dryrun)
                        puts(dirpaths[i]);
                else
                        do_rmdir(dirpaths[i]);
	}
#     elif defined(CHMODTREE)
        for (i = 0; i < dirpathcount; i++) {
                if (! dirpaths[i])
                        continue;
                if (dryrun
                    && (! filetypemask || (filetypemask & FILETYPE_DIR)))
                        puts(dirpaths[i]);
		else if (! filetypemask || (filetypemask & FILETYPE_DIR)) {
			if (debug)
				fprintf(stderr, "Chmod'ing startdir: %s\n", dirpaths[i]);
                	rc = stat(dirpaths[i], &st);
                	if (rc < 0) {
				continue;
			}
                	statcount++;
			if (numeric_dirmode) { // - when filetypemask = FILETYPE_DIR
				// - just set the new mode (if different).
				if ((st.st_mode&S_IRWXA) != (dir_mode&S_IRWXA))
					do_chmod(dirpaths[i], dir_mode);
			} else if (numeric_commonmode) { // - when filetypemask is not set.
				// - just set the new mode (if different).
				if ((st.st_mode&S_IRWXA) != (common_mode&S_IRWXA))
					do_chmod(dirpaths[i], common_mode);
			} else {
				// - symbolic mode is handled here.
				// - read the old mask and merge with the new one.
				mode_t updated_mode = getmode(new_dirmodeset, st.st_mode);
				if ((st.st_mode&S_IRWXA) != (updated_mode&S_IRWXA))
					do_chmod(dirpaths[i], updated_mode);
			}
		}
        }
#     endif

	if (debug)
		fprintf(stderr, "traverse_trees() - FINISHED waiting for threads\n");

	return;
}

/////////////////////////////////////////////////////////////////////////////

static void walk_dir(dirlist_t *); // - used by pthread_routine()

/////////////////////////////////////////////////////////////////////////////

// This is the routine used in every created thread.
static void *pthread_routine(
	void *id) // - has to be void *
{
	dirlist_t *curdir;

	do {
		if ((curdir = dirlist_pull_dir())) {
			walk_dir(curdir);
#		      if defined(SRCH)
			if (summarize_diskusage && curdir->du) {
#			      if defined(PR_ATOMIC_ADD)
				PR_ATOMIC_ADD(&accum_du, curdir->du);
#			      else
				pthread_mutex_lock(&accum_du_lock);
				accum_du += curdir->du;
				pthread_mutex_unlock(&accum_du_lock);
#			      endif
			}
#		      endif
			if (just_count || verbose_count) {
#                             if defined(PR_ATOMIC_ADD)
				PR_ATOMIC_ADD(&accum_filecnt, curdir->filecnt);
#                             else
				pthread_mutex_lock(&accum_filecnt_lock);
				accum_filecnt += curdir->filecnt;
				pthread_mutex_unlock(&accum_filecnt_lock);
#			      endif

				if (verbose_count) {
					pthread_mutex_lock(&last_accum_filecnt_lock);
					unsigned int n = last_accum_filecnt + verbose_count;
					if (accum_filecnt > n) {
						time_t t, timediff;
						t = time(NULL);
						timediff = t - last_t;
						fprintf(stderr, "About %d files processed ", accum_filecnt);
						if (timediff > 0)
							fprintf(stderr, "(%i files/s)...\n", (int)((accum_filecnt - last_accum_filecnt)/timediff));
						else
							fprintf(stderr, "(currently running too fast to calculate files/s)...\n");
						last_accum_filecnt = accum_filecnt;
						last_t = t;
					}
					pthread_mutex_unlock(&last_accum_filecnt_lock);
				}
			}
			free(curdir);
		}
	} while (! master_finished);

#     if ! defined(__APPLE__)
	sem_post(&finished_threads_sem);
#     else
	dispatch_semaphore_signal(finished_threads_sem);
#     endif

	if (debug)
		fprintf(stderr, "Ending pthread_routine(%lu)\n", (unsigned long)id);

	return NULL;
}

/////////////////////////////////////////////////////////////////////////////

static void thread_prepare()
{
	int rc;
	unsigned long i;

        rc = pthread_mutex_init(&dirlist_lock, NULL);
        assert(rc == 0);

	thread_arr = calloc(thread_cnt, sizeof(pthread_t));
	assert(thread_arr);

#if ! defined(__APPLE__)
	int rc1 = sem_init(&master_sem, 0, 0);
	int rc2 = sem_init(&threads_sem, 0, 0);
	int rc3 = sem_init(&finished_threads_sem, 0, 0);
	assert(! rc1 && ! rc2 && ! rc3);
#else
	master_sem = dispatch_semaphore_create(0);
	threads_sem = dispatch_semaphore_create(0);
	finished_threads_sem = dispatch_semaphore_create(0);
	assert(master_sem && threads_sem && finished_threads_sem);
#endif

	pthread_attr_t attr;
	pthread_attr_init(&attr);
	for (i = 0; i < thread_cnt; i++) {
		int rc = pthread_create(&thread_arr[i], &attr, pthread_routine, (void *)i);
		assert(rc == 0);
	}
	pthread_attr_destroy(&attr);
}

/////////////////////////////////////////////////////////////////////////////

static void thread_cleanup()
{
	int i;

	if (debug)
		fprintf(stderr, "START thread_cleanup()\n");

#     if ! defined(PR_ATOMIC_ADD)
	pthread_mutex_destroy(&accum_filecnt_lock);
#     endif
	pthread_mutex_destroy(&perror_lock);

#     if defined(SRCH)
	pthread_mutex_destroy(&heap_lock);
#     endif

	pthread_mutex_destroy(&dirlist_lock);

	// Wait for threads to finish:
	for (i = 0; i < thread_cnt; i++)
		pthread_join(thread_arr[i], NULL);

	free(thread_arr);

	if (excludelist_count)
		free(excludelist);

	if (excluderecomp) {
		for (i = 0; i < excludelist_count; i++) {
			regfree(excluderecomp[i]);
			free(excluderecomp[i]);
		}
		free(excluderecomp);
	}

#     if defined(SRCH)
	if (regex_opt) {
		regfree(regexcomp);
		free(regexcomp);
	} else if (fast_match_opt)
		free(fast_match_arg);
#     endif

#if ! defined(__APPLE__)
	sem_destroy(&threads_sem);
	sem_destroy(&master_sem);
	sem_destroy(&finished_threads_sem);
#else
	dispatch_release(threads_sem);
	dispatch_release(master_sem);
	dispatch_release(finished_threads_sem);
#endif
	if (debug)
		fprintf(stderr, "END thread_cleanup()\n");
}

/////////////////////////////////////////////////////////////////////////////

#if ! defined(__MINGW32__) && ! defined(CHMODTREE)
static inline __attribute__((always_inline)) uid_t username_to_uid_or_exit(
	const char *user)
{
	struct passwd *pw = getpwnam(user);
	if (pw)
		return pw->pw_uid;
	else {
                fprintf(stderr, "User %s not found - bailing out.\n", user);
		exit(1);
	}
}

/////////////////////////////////////////////////////////////////////////////

static inline __attribute__((always_inline)) gid_t groupname_to_gid_or_exit(
	const char *group)
{
	struct group *gr = getgrnam(group);
	if (gr)
		return gr->gr_gid;
	else {
		fprintf(stderr, "Group %s not found - bailing out.\n", group);
		exit(1);
	}
}
#endif
