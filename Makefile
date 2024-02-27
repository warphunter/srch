CC = gcc # - need GCC2.5 or higher or clang
CFLAGS = -pipe -O2 -funroll-loops -Wall -Winline -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64
OLDTIMERCFLAGS = --param inline-unit-growth=100 --param large-function-growth=110000 # - for gcc 3.4.3, 3.4.6
BESTCFLAGSONLINUX = -pipe -O3 -march=native -funroll-loops -Wall -Winline -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64
CFLAGSWIN = -pipe -O2 -Wall -Winline -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -I. -D_POSIX_C_SOURCE
LIBS = -lpthread -lrt
ALTLIBS = -lpthread

SRC = srch.c
INC = commonlib.h
BIN = $(SRC:.c=)
MAN = $(SRC:c=1)
BINWIN64 = ${SRC:.c=.exe}
BINWIN32 = ${SRC:.c=32.exe}
SRCWINX = regex.c

all: $(BIN)

win64: $(BINWIN64)

win32: $(BINWIN32)

win: $(BINWIN64) $(BINWIN32)

$(BIN): $(SRC) $(INC)
	@export PATH; PATH=${PATH}:/usr/local/bin:/opt/freeware/bin; \
	case `uname -s` in \
	    SunOS) \
		set -x; \
		$(CC) -DCC_USED="\"`gcc --version|awk '/gcc/ {print $$0 \", flags:\"}'` $(CFLAGS) -pthreads -m64 $(OLDTIMERCFLAGS) -D_POSIX_PTHREAD_SEMANTICS\"" $(CFLAGS) -pthreads -m64 $(OLDTIMERCFLAGS) -D_POSIX_PTHREAD_SEMANTICS $(SRC) -o $@ $(LIBS) \
		;; \
	    AIX) \
		set -x; \
		$(CC) -DCC_USED="\"`gcc --version|awk '/gcc/ {sub(\")\",\"\",$$NF); print \"gcc version \" $$NF \", flags:\"}'` $(CFLAGS) -pthread -maix64 -D_LARGE_FILES\"" $(CFLAGS) -pthread -maix64 -D_LARGE_FILES $(SRC) -o $@ $(LIBS) \
		;; \
	    HP-UX) \
		case `uname -m` in \
		    ia64) \
			set -x; \
			$(CC) -DCC_USED="\"`gcc --version|awk '/gcc/ {sub(\")\",\"\",$$NF); print \"gcc version \" $$NF \", flags:\"}'` $(CFLAGS) -pthread -mlp64 -D_INCLUDE_POSIX_SOURCE -D_XOPEN_SOURCE_EXTENDED\"" $(CFLAGS) -pthread -mlp64 -D_INCLUDE_POSIX_SOURCE -D_XOPEN_SOURCE_EXTENDED $(SRC) -o $@ $(LIBS) \
			;; \
		    9000/800) \
			set -x; \
			$(CC) -DCC_USED="\"`gcc --version|awk '/gcc/ {sub(\")\",\"\",$$NF); print \"gcc version \" $$NF \", flags:\"}'` $(CFLAGS) -pthread -munix=98 -D_INCLUDE_POSIX_SOURCE -D_XOPEN_SOURCE_EXTENDED\"" $(CFLAGS) -pthread -munix=98 -D_INCLUDE_POSIX_SOURCE -D_XOPEN_SOURCE_EXTENDED $(SRC) -o $@ $(LIBS); \
		esac \
		;; \
	    Linux) \
		set -x; \
		$(CC) -DCC_USED="\"`gcc --version|awk '/gcc/ {print $$0 \", flags:\"}'` -mcpu=native $(CFLAGS) $(OLDTIMERCFLAGS) -pthread\"" -march=native $(CFLAGS) $(OLDTIMERCFLAGS) -pthread $(SRC) -o $@ $(LIBS) \
		;; \
	    FreeBSD) \
		set -x; \
		$(CC) -DCC_USED="\"`$(CC) --version|awk '/gcc|clang/ {print $$0 \", flags:\"}'` $(CFLAGS) $(OLDTIMERCFLAGS) -pthread -march=native\"" $(CFLAGS) -march=native $(SRC) -o $@ $(LIBS) \
		;; \
	    OpenBSD|Darwin) \
		set -x; \
		$(CC) -DCC_USED="\"`gcc --version|awk '/gcc/ {sub(\")\",\"\",$$NF); print \"gcc version \" $$NF \", flags:\"}'` $(CFLAGS) $(OLDTIMERCFLAGS) -pthread -march=native\"" $(CFLAGS) -march=native $(SRC) -o $@ $(ALTLIBS) \
		;; \
	    *) \
		echo `uname -s` has not been tested.; \
		echo You might try: gcc -pthread -O2 srch.c -o srch -lpthread -lrt \
		;; \
	esac

$(BINWIN64): $(SRC) $(INC)
	@cp -p $(SRC) $(INC) win/; \
	cd win; \
	x86_64-w64-mingw32-gcc --version >/dev/null 2>&1 || { echo =\> You need to install mingw-w64, mingw-w64-common and mingw-w64-x86-64-dev to be able to compile for 64-bit Windows.; exit 1; }; \
	set -x; \
	x86_64-w64-mingw32-gcc -D__USE_MINGW_ANSI_STDIO -DCC_USED="\"`x86_64-w64-mingw32-gcc --version|head -1`\"" $(CFLAGSWIN) $(SRC) $(SRCWINX) -o $@ -lpthread -static -s; \
	set +x; \
	test -f $(BINWIN64) && mv -f $(BINWIN64) ..

$(BINWIN32): $(SRC) $(INC)
	@cp -p $(SRC) $(INC) win/; \
	cd win; \
	i686-w64-mingw32-gcc -D__USE_MINGW_ANSI_STDIO -DCC_USED="\"`i686-w64-mingw32-gcc --version|head -1`\"" $(CFLAGSWIN) $(SRC) $(SRCWINX) -o $@ -lpthread -static -s; \
	set +x; \
	test -f $(BINWIN32) && mv -f $(BINWIN32) ..

test: $(BIN)
	@-ksh=`which ksh`; ksh93=`which ksh93`; \
	if [ "$$ksh" = "" -a "$$ksh93" = "" ]; then echo Note: This test won\'t run since you don\'t have ksh installed or not in your PATH.; exit 0; elif [ "$$ksh" != "" -a "$$ksh" != /bin/ksh ]; then ksh ./testsrch -c; elif [ "$$ksh93" != "" ]; then ksh93 ./testsrch -c; else ./testsrch -c; fi

install: $(BIN)
	mkdir -p /usr/local/bin && cp -p $(BIN) /usr/local/bin; \
	test -d /usr/local/share/man/man1 && cp -p $(MAN) /usr/local/share/man/man1 || test -d /usr/local/man/man1 && cp -p $(MAN) /usr/local/man/man1; \
	exit 0

uninstall:
	rm -f /usr/local/bin/$(BIN); \
	test -f /usr/local/share/man/man1/$(MAN) && rm -f /usr/local/share/man/man1/$(MAN) || test -f /usr/local/man/man1/$(MAN) && rm -f /usr/local/man/man1/$(MAN); \
	exit 0

clean:
	-rm -f $(BIN) $(BINWIN64) $(BINWIN32)

.PHONY : all win64 win32 win test install uninstall clean
