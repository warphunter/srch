/*
 *	                    OS-HACKS.H					  
 *              OS-specfic (Win32) include file for building the GPC rts:
 *              Contains certain implementations for Mingw32 and Cygwin
 *
 * This file is part of the GPC package.
 *  v1.11, May 10 2002
 *  v1.12, Nov 13 2002
 *
 * Contributors:
 *   Prof. A Olowofoyeku (The African Chief) <African_Chief@bigfoot.com>
 *   Frank Heckenbach <frank@g-n-u.de>
 *
 *
 *  THIS SOFTWARE IS NOT COPYRIGHTED
 *
 *  This source code is offered for use in the public domain. You may
 *  use, modify or distribute it freely.
 *
 *  This code is distributed in the hope that it will be useful but
 *  WITHOUT ANY WARRANTY. ALL WARRANTIES, EXPRESS OR IMPLIED ARE HEREBY
 *  DISCLAMED. This includes but is not limited to warranties of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 *
 *       1. Mingw32:
 * 	 Defines and implements:                                
 *                statfs
 *		  fsync
 *		  isinf
 *		  isnan
 *		  setpwent
 *		  endpwent
 *		  getpwent
 *		  getpwuid
 *		  getpwnam
 *		  						   
 *	 Defines/redefines:						  
 *        	  HAVE_STATFS
 *		  stat
 *        	  _sleep			 				  
 *        	  ssize_t	 					  
 *        	  realpath		 				  
 *        	  _fullpath		 				  
 *        	  mkdir
 *		  system
 * 		  WIFEXITED
 * 		  WEXITSTATUS
 *
 *									  
 *       2. Cygwin32:
 * 	 Redefines the following to use CygWin APIs:		  
 *       	  realpath						  
 *       	  getcwd
 *
 *
 *	 3. Both platforms:
 *	 Defines/Redefines:
 *		  time
 *                executable_path
 *
*/

#ifndef __OS_HACKS_H__
#define __OS_HACKS_H__

#define WIN32_LEAN_AND_MEAN  /* exclude winsock.h because of clash with select() */
#include <windows.h>         /* needed for WinAPI stuff */
#undef WIN32_LEAN_AND_MEAN 

#include <errno.h>           /* needed for vfs.h functions */
#include <lm.h>              /* needed for pwd.h functions */

#ifndef time
#include <time.h>
#endif

#ifdef __MINGW32__
/* new problem (20/5/2000): Define if you have the signal function.  */
#undef HAVE_SIGNAL

#ifndef SSIZE_MAX
  #define SSIZE_MAX INT_MAX    /* not defined elsewhere */
#endif

#ifndef ssize_t
  #define ssize_t int
  #define HAVE_SSIZE_T
#endif

#ifndef WIFEXITED
  #define WIFEXITED(S) 1
#endif

#ifndef WEXITSTATUS
  #define WEXITSTATUS(S) (S)
#endif

#define _fullpath(res,path,size) \
  (GetFullPathName ((path), (size), (res), NULL) ? (res) : NULL)

#define realpath(path,resolved_path) _fullpath(resolved_path, path, MAX_PATH)

#undef HAVE_SLEEP
#undef HAVE__SLEEP
#define HAVE__SLEEP 1
#define _sleep(seconds) Sleep(1000*seconds)

#define MKDIR_TWOARG 1
#define mkdir(path,mode) mkdir(path)

/**************** environment variables *****************/
#define HAVE_UNSETENV 1
#define SETENV_DECLARED 1
#define unsetenv(pname) SetEnvironmentVariable(pname, NULL)

#define HAVE_SETENV 1
#define UNSETENV_DECLARED 1
#define setenv(__pname,__pvalue,___overwrite) \
({ \
  int result; \
 if (___overwrite == 0 && getenv (__pname)) result = 0; \
   else \
     result = SetEnvironmentVariable (__pname,__pvalue); \
 result; \
})
 
/*************** system() ********************/
/* make sure that system() always receives back slashes */
#define system(cmdline) \
({ \
   int result; \
   char *new_cmdline = strdup (cmdline), *c; \
   for (c = new_cmdline; *c && *c != ' '; c++) \
     if (*c == '/') *c = '\\'; \
   result = system (new_cmdline); \
   free (new_cmdline); \
   result; \
})

/*********************** stat() *****************************/
/* mingw cannot correctly stat the root directory of a drive */
#define stat(_filename,_buf) \
({ \
   char *filename = (char*) (_filename), resolved_path [MAX_PATH]; \
   struct stat *buf = (_buf); \
   int result; \
   _fullpath(resolved_path, filename, MAX_PATH); \
   if (resolved_path [0] != 0 && \
       resolved_path [1] == ':' && \
       resolved_path [2] == '\\' && \
       resolved_path [3] == 0) \
     { \
       /* There's not much information available. Just fill in the most \
          important fields. Perhaps someone knows how to get more \
          information... */ \
       memset (buf, 0, sizeof (*buf)); \
       buf->st_mode = S_IFDIR | 0777; \
       buf->st_nlink = 0xffff; /* Better be safe than sorry... */ \
       result = 0; \
     } \
   else \
     result = stat (filename, buf); \
   result; \
})
/**************** end: stat *******************/

#undef HAVE_FSYNC
#define HAVE_FSYNC 1
#define fsync(__fd) ((FlushFileBuffers ((HANDLE) _get_osfhandle (__fd))) ? 0 : -1)

#undef HAVE_ISINF
#define HAVE_ISINF 1

#undef HAVE_ISNAN
#define HAVE_ISNAN 1

#define __uint32_t unsigned int

typedef union
{
  struct 
  {
    __uint32_t lsw, msw;
  } parts;
  double value;
} ieee_double_shape_type;

#define isinf(x) \
({ \
  ieee_double_shape_type e; \
  e.value = (x); \
  e.parts.lsw != 0 ? 0 : (e.parts.msw == 0xfff00000) ? -1 \
    : (e.parts.msw == 0x7ff00000) ? 1 : 0; \
})

#define isnan(x) \
({ \
  ieee_double_shape_type e; \
  e.value = (x); \
  ((e.parts.msw & 0x7fffffff) | (e.parts.lsw != 0)) > 0x7ff00000; \
})


/******************** pwd.h functions ********************/
#ifndef _PWD_H_

#ifndef HAVE_PWD_H
  #define HAVE_PWD_H 1
  #define	_PATH_PASSWD		"/etc/passwd"
  #define	_PASSWORD_LEN		128	/* max length, not counting NULL */
struct passwd {
	char	*pw_name;		/* user name */
	char	*pw_passwd;		/* encrypted password */
	int	pw_uid;			/* user uid */
	int	pw_gid;			/* user gid */
	char	*pw_comment;		/* comment */
	char	*pw_gecos;		/* Honeywell login info */
	char	*pw_dir;		/* home directory */
	char	*pw_shell;		/* default shell */
};
#endif /* HAVE_PWD_H */

#endif /* _PWD_H_ */

/******************** pwd.c *************************/

#define pwd_max_mystr 260
typedef char pwd_Mystr [pwd_max_mystr];


/*
 * use globals
 */
 struct _USER_INFO_3 *info2 = NULL;
 int uid2 = -1;
 DWORD Counter = 0;
 HINSTANCE DllHandle = 0;
 FARPROC pNetApiBufferFree = NULL;
 FARPROC pNetUserEnum = NULL;



/* macros */
#define setpwent __setpwent
#define endpwent __endpwent
#define getpwent __getpwent
#define getpwuid(_uid) __getpwuid(_uid)
#define getpwnam(_name) __getpwnam(_name)

/******************** pwd.c *************************/

#endif /* __MINGW32__ */



/******************* cygwin stuff *****************/

#ifdef __CYGWIN32__

#define realpath(path,resolved_path) \
  (getenv ("CYGWIN_USE_WIN32_PATHS") \
   ? (cygwin_conv_to_full_win32_path ((path), (resolved_path)), resolved_path) \
   : realpath ((path), (resolved_path)))

#define getcwd(_buf,_size) \
( \
  { \
    char *buf = (_buf); \
    size_t size = (_size); \
    char tmp [260]; \
    getcwd (tmp, size); \
    realpath (tmp, buf); \
    buf; \
  } \
)

#endif /* __CYGWIN32__ */


/******************* common stuff *****************/

/* we undefine HAVE_GETTIMEOFDAY because of Cygwin's date/time bug */
#undef HAVE_GETTIMEOFDAY

/* new time() that uses the WinAPI to call GetLocalTime */
#undef time
#define time(_timer) \
( \
  { \
  time_t *timer = (_timer); \
  SYSTEMTIME tmp; \
  time_t tmp2 = 0; \
  struct tm gnu; \
    memset (&gnu, 0, sizeof (gnu)); \
    GetLocalTime (&tmp); \
    gnu.tm_isdst = -1; \
    gnu.tm_year  = tmp.wYear - 1900; \
    gnu.tm_mon   = tmp.wMonth - 1; \
    gnu.tm_mday  = tmp.wDay; \
    gnu.tm_hour  = tmp.wHour; \
    gnu.tm_min   = tmp.wMinute; \
    gnu.tm_sec   = tmp.wSecond; \
    tmp2 = mktime (&gnu); \
    if (timer) *timer = tmp2; \
    tmp2; \
  } \
)

#undef HAVE_EXECUTABLE_PATH
#define HAVE_EXECUTABLE_PATH 1

#define executable_path(buffer) \
( \
 { \
   char *buf = (buffer); \
   GetModuleFileName(NULL,buf,MAX_PATH); \
   buf; \
 } \
)

/*
#define HAVE_GETTMPDIR
#define gettmpdir(Buffer, Size) (GetTempPath((Size), (Buffer)) > 0 ? (Buffer) : NULL)
*/

/**********************************************************/
/* new problem (7/8/2000) */
#undef HAVE_SYS_SIGLIST
#undef HAVE__SYS_SIGLIST
/**********************************************************/

#endif /* __OS_HACKS_H__ */

