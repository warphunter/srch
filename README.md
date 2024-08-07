# srch

For a long time I wasn't happy with the speed and functionality of the standard find(1) utility included in Unix/Linux, and the syntax is sadly quite different from most other Unix utilities.  Since find(1) was first written about 45 years ago, in 1979 for Unix V7, as a kind of grep(1) for file systems, computer hardware has evolved almost miraculously.  In the first decades after Unix' invention, CPU resources were scarce and disks were small and slow.

Nowadays we have a completely different setting, where there are no longer any single-core computers around (except in my lab ;), and disks almost as fast as RAM are mainstream, and files are more numerous than trees in the forests. Waiting for find(1) to search for the needle in the haystack was getting more and more painful.  Managing file servers with tens of millions of files, I needed a tool faster and more flexible than find(1), preferably with more desirable syntax, and easy to compile on all available Unix platforms, so I decided to write it myself (although I'm not a professional programmer :).

Here is a fun story about the origin of find(1): http://doc.cat-v.org/unix/find-history

After having a fast but simple file listing tool ready for production early 2020, srch is now my Swiss Army knife for exploring, and possibly modifying, file tree structures quickly. Over the years it has evolved to be a real alternative to find(1), with simplified syntax and extended functionality, although GNU find(1) is still the king of options :D.

While  find(1)  is  single-threaded, srch will by default use up to 8 CPU cores to search for files in parallel.  The basic idea is to handle each subdirectory as an independent unit, and feed a number of threads with these units.  Provided the underlying storage system is fast enough, this scheme will speed up file search considerably, and ultimately minimize the need for locate(1).
 
Srch is not only fast because of multi-threading, but also because lstat(2) system calls are only used when absolutely needed.  On Linux, *BSD and in MacOS, the file type is included in the "dirent" struct when using readdir(3), so an extra lstat(2) is not needed to determine the type of the file at hand.

Examples of srch' extended functionality:

-Dx for listing the x directories containing most files.  Useful if a file system is running out of inodes, and you need to find out where all of them are located, possibly to clean up.

-Fx for listing the x biggest files.  Useful when a file system is filling up, and you don't know which files are causing it.

-Mx for listing the x most recently updated files/directories.  Useful for finding hot spots in the file system.

-H for summarizing disk usage, like du -hs.

Excluding comments and blank lines, srch consists of an 800 line re-usable library (commonlib.h) of functions written in C, in addition to the 2000 lines specific part in srch.c.  Srch can be compiled for common Unix/Linux versions out of the box, but should be easy to tailor to Unix versions I don't have access to.  Building it for Windows requires a little more, and the necessary source code is located in the "win" subdirectory.  

To build it for Unix/Linux, you just need gcc(1) or clang(1), and make(1).  Default compiler in the Makefile is gcc, but you may switch to clang instead.  Note that in my experience, gcc produces the fastest code.  Just try running "make".  If your Unix version isn't directly supported in the Makefile, you may try compiling it manually running "gcc -O2 srch.c -o srch -l pthread".

To build it for Windows (using a Linux machine), run "make win".  You need to have mingw-w64, mingw-w64-common and mingw-w64-x86-64-dev installed to be able to compile for 64-bit Windows, and additionally mingw-w64-i686-dev to compile for 32-bit.  Srch can also be directly compiled on Windows using Cygwin.

You may run "make test" to perform a few tests where output from srch and find(1) are compared.  If the directory being tested is dynamic, where files come and go at will, the results from find(1) and srch may differ.

You may run "make install" to copy the binary to /usr/local/bin and the man page to /usr/local/share/man/man1 or to /usr/local/man/man1 if the first folder doesn't exist.

In the manual page srch.1 (or in srch.man which is preformatted), you will find lots of examples and speed comparisons with find(1).

Srch is tested and supported on Linux, FreeBSD, OpenBSD, MacOS, AIX, HP-UX, Solaris, Windows.  As mentioned, you can use MinGW on Linux to compile it for Windows, or Cygwin directly on Windows.  You can even compile it on a matching FreeBSD release and run it under the hood (in the system shell) of a NetApp cDOT node.  NetApp has no compiler, so you will need to install FreeBSD on a VM or a physical PC to be able to compile srch. To find the right FreeBSD version, you can log in as the diag user and run "file /bin/cat" in the NetApp system shell. After compilation and copying the resulting FreeBSD srch binary to the /var/home/diag/bin folder (which is already included in the PATH) on your NetApp node, you are ready to srch through the various virtual file servers (SVMs) under the /clus mount point.  You will need to be root to do this, so what I usually do is running "sudo csh" (which is actually tcsh(1)) to have the necessary power.

Srch may actually be the fastest publicly available file search tool in this solar system :D. Just test it and compare run-times with similar tools (like fd-find) to see for yourself!

You may also check out my other muti-threaded tools: rmtree, chmodtree and chowntree
