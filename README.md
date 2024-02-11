# srch

For a long time I wasn't happy with the speed and functionality of the standard, single-threaded find(1) included in Unix/Linux.  Since find(1) was first written almost 60 years ago, computer hardware has evolved almost miraculously.  In the first decades after Unix' invention, CPU resources were scarce and disks were small and slow.  Nowadays we have a completely different setting, where there are no more single-CPU computers, and disks almost as fast as RAM are mainstream.  Managing file servers with tens of millions of files, I needed a tool faster and more flexible than find(1), and decided to write it myself (although I'm not a professional programmer :).

In my eyes, srch is now the Swiss Army knife for exploring file tree structures quickly. It is written to be a fast, multi-threaded alternative to find(1), with simplified syntax and extended functionality.  While  find(1)  is  single-threaded, srch will by default use up to 8 CPU cores to search for files in parallel.  The basic idea is to handle each subdirectory as an independent unit, and feed a number of threads with these units.  Provided the underlying storage system is fast enough, this scheme will speed up file search considerably, and ultimately minimize the need for locate(1).

Srch might actually be the fastest publicly available file search tool - just test it and compare run-times with other tools (like fd-find) to see for yourself :).

Srch consists of a 900 line re-usable C library (commonlib.h) in addition to the 2000 lines specific part in srch.c.  Srch can be compiled for almost any Unix/Linux out of the box, but should be easy to tailor to Unix versions I don't have access to.  Building it for Windows requires a little more, and the necessary source code is located in the "win" subdirectory.  

To build it for Unix/Linux, you just need gcc(1) or clang(1), and make(1).  Default compiler in the Makefile is gcc, but you may switch to clang instead.  Note that in my experience, gcc produces the fastest code.  Just try running "make".  If your Unix version isn't directly supported, you may try compiling it running "gcc -O2 srch.c -o srch -l pthread".

To build it for Windows (on a Linux machine), run "make win".  You need to have mingw-w64, mingw-w64-common and mingw-w64-x86-64-dev installed to be able to compile for 64-bit Windows, and additionally mingw-w64-i686-dev to compile for 32-bit.  Srch can also be directly compiled on Windows using Cygwin.

You can run "make test" to perform a few tests where output from srch and find(1) are compared.

You can run "make install" to copy the binary to /usr/local/bin and the man page to /usr/local/share/man/man1 or to /usr/local/man/man1 if the first folder doesn't exist.

In the manual page srch.1 (or in srch.man which is preformatted), you will find lots of examples and speed comparisons with find(1).

Srch is supported on Linux, FreeBSD, OpenBSD, MacOS, AIX, HP-UX, Solaris, Windows.  You can use MinGW on Linux to compile it for Windows, or Cygwin directly on Windows.  You can even compile it on a matching FreeBSD release and run it under the hood (in the system shell) of a NetApp cDOT node.  To find the right FreeBSD version, you can run "file /bin/cat" in the NetApp system shell. After copying the resulting FreeBSD binary version of srch to e.g. /var/home/diag/bin which are already included in the PATH, you are ready to search through the various virtual file servers (SVMs) under the /clus mount point.
