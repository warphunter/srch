# srch

For a long time I wasn't happy with the speed and functionality of the standard find(1) utility included in Unix/Linux, and the syntax is sadly quite different from most other Unix utilities.  Since find(1) was first written about 45 years ago, in 1979 for Unix V7, computer hardware has evolved almost miraculously.  In the first decades after Unix' invention, CPU resources were scarce and disks were small and slow.

Nowadays we have a completely different setting, where there are no longer any single-CPU computers around, and disks almost as fast as RAM are mainstream, and files are more numerous than trees in the forests.  Managing file servers with tens of millions of files, I needed a tool faster and more flexible than find(1), preferably with more desirable syntax, easy to compile on all available Unix platforms, so I decided to write it myself (although I'm not a professional programmer :).

Here is a fun story about the origin of find(1): http://doc.cat-v.org/unix/find-history

After having a simple file listing tool ready for production early 2020, srch is now my Swiss Army knife for exploring, and possibly modifying, file tree structures quickly. It is written to be a fast, multi-threaded alternative to find(1), with simplified syntax and extended functionality.  While  find(1)  is  single-threaded, srch will by default use up to 8 CPU cores to search for files in parallel.  The basic idea is to handle each subdirectory as an independent unit, and feed a number of threads with these units.  Provided the underlying storage system is fast enough, this scheme will speed up file search considerably, and ultimately minimize the need for locate(1).

Examples of srch' extended functionality:

-Dx for listing the x directories containing most files.  Useful if a file system is running out of inodes, and you need to find out where all of them are located, possibly to clean up.

-Fx for listing the x biggest files.  Useful when a file system is filling up, and you don't know which files are causing it.

-Mx for listing the x most recently updated files/directories.  Useful for finding hot spots in the file system.

-H for summarizing disk usage, like du -hs.

Excluding comments and blank lines, srch consists of an 800 line re-usable library (commonlib.h) of functions written in C, in addition to the 2000 lines specific part in srch.c.  Srch can be compiled for common Unix/Linux versions out of the box, but should be easy to tailor to Unix versions I don't have access to.  Building it for Windows requires a little more, and the necessary source code is located in the "win" subdirectory.  

To build it for Unix/Linux, you just need gcc(1) or clang(1), and make(1).  Default compiler in the Makefile is gcc, but you may switch to clang instead.  Note that in my experience, gcc produces the fastest code.  Just try running "make".  If your Unix version isn't directly supported, you may try compiling it manually running "gcc -O2 srch.c -o srch -l pthread".

To build it for Windows (using a Linux machine), run "make win".  You need to have mingw-w64, mingw-w64-common and mingw-w64-x86-64-dev installed to be able to compile for 64-bit Windows, and additionally mingw-w64-i686-dev to compile for 32-bit.  Srch can also be directly compiled on Windows using Cygwin.

You may run "make test" to perform a few tests where output from srch and find(1) are compared.

You may run "make install" to copy the binary to /usr/local/bin and the man page to /usr/local/share/man/man1 or to /usr/local/man/man1 if the first folder doesn't exist.

In the manual page srch.1 (or in srch.man which is preformatted), you will find lots of examples and speed comparisons with find(1).

Srch is tested and supported on Linux, FreeBSD, OpenBSD, MacOS, AIX, HP-UX, Solaris, Windows.  As mentioned, you can use MinGW on Linux to compile it for Windows, or Cygwin directly on Windows.  You can even compile it on a matching FreeBSD release and run it under the hood (in the system shell) of a NetApp cDOT node.  To find the right FreeBSD version, you can run "file /bin/cat" in the NetApp system shell. After compilation and copying the resulting FreeBSD srch binary to the /var/home/diag/bin folder (which are already included in the PATH) on your NetApp node, you are ready to srch through the various virtual file servers (SVMs) under the /clus mount point.

Srch may actually be the fastest publicly available file search tool in this solar system :D- Just test it and compare run-times with other tools (like fd-find) to see for yourself :)

You may also check out my other muti-threaded tools: rmtree, chmodtree and chowntree
