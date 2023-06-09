# srch

If you're happy with the speed and functionality of the standard, single-threaded find(1) included in Unix/Linux, you probably won't be interested in reading further.

Srch is the Swiss Army knife for exploring file tree structures quickly. Srch is written to be a fast, multi-threaded alternative to find(1), with simplified syntax and extended functionality.  While  find(1)  is  single-threaded, srch will by default use up to 8 CPU cores to search for files in parallel.  The basic idea is to handle each subdirectory as an independent unit, and feed a number of threads with these units.  Provided the underlying storage system is fast enough, this scheme will speed up file search considerably, and ultimately minimize the need for locate(1).

Srch consists of one single 2700 lines C source file which can be compiled for Unix/Linux.  Building it for Windows requires a little more, and the necessary source code is located in the "win" subdirectory.

To build it for Unix/Linux, you just need gcc(1) and make(1).  Just run "make".

To build it for Windows (on a Linux machine), run "make win".  You need to have mingw-w64, mingw-w64-common and mingw-w64-x86-64-dev installed to be able to compile for 64-bit Windows, and additionally mingw-w64-i686-dev to compile for 32-bit.

You can run "make test" to perform a few tests where output from srch and find(1) are compared.

There is no "make install" for the moment.

In the manual page srch.1 (or in srch.man which is preformatted) you will find lots of examples and speed comparisons with find(1).

Srch is supported on Linux, FreeBSD, OpenBSD, MacOS, AIX, HP-UX, Solaris, Windows.  You can even compile it on a matching FreeBSD release and run it under the hood (in the system shell) of NetApp to search the various virtual file servers under the /clus mount point.
