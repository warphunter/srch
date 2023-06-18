CC = gcc # - need GCC2.5 or higher
CFLAGS = -pipe -O2 -funroll-loops -Wall -Winline -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64
OLDTIMERCFLAGS = --param inline-unit-growth=100 --param large-function-growth=110000 # - for gcc 3.4.3, 3.4.6
BESTCFLAGSONLINUX = -pipe -O3 -march=native -funroll-loops -Wall -Winline -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64
CFLAGSWIN = -pipe -O2 -Wall -Winline -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -I. -D_POSIX_C_SOURCE
LIBS = -lpthread -lrt
ALTLIBS = -lpthread

SRC = srch.c
BIN = $(SRC:.c=)
BINWIN64 = ${SRC:.c=.exe}
BINWIN32 = ${SRC:.c=32.exe}
SRCWINX = regex.c

all: $(BIN)

win: $(BINWIN64) $(BINWIN32)

$(BIN): $(SRC)
	@export PATH; PATH=${PATH}:/usr/local/bin:/opt/freeware/bin; \
	case `uname -s` in \
	    SunOS) \
		set -x; \
		$(CC) $(CFLAGS) -pthreads -m64 $(OLDTIMERCFLAGS) -D_POSIX_PTHREAD_SEMANTICS $(SRC) -o $@ $(LIBS) \
		;; \
	    AIX) \
		set -x; \
		$(CC) $(CFLAGS) -pthread -maix64 -D_LARGE_FILES $(SRC) -o $@ $(LIBS) \
		;; \
	    HP-UX) \
		case `uname -m` in \
		    ia64) \
			set -x; \
			$(CC) $(CFLAGS) -pthread -mlp64 -D_INCLUDE_POSIX_SOURCE -D_XOPEN_SOURCE_EXTENDED $(SRC) -o $@ $(LIBS) \
			;; \
		    9000/800) \
			set -x; \
			$(CC) $(CFLAGS) -pthread -munix=98 -D_INCLUDE_POSIX_SOURCE -D_XOPEN_SOURCE_EXTENDED $(SRC) -o $@ $(LIBS); \
		esac \
		;; \
	    Linux) \
		set -x; \
		$(CC) $(CFLAGS) $(OLDTIMERCFLAGS) -pthread $(SRC) -o $@ $(LIBS) || $(CC) $(CFLAGS) -march=native -pthread $(SRC) -o $@ $(LIBS) \
		;; \
	    FreeBSD) \
		set -x; \
		$(CC) $(CFLAGS) -march=native $(SRC) -o $@ $(LIBS) \
		;; \
	    OpenBSD|Darwin) \
		set -x; \
		$(CC) $(CFLAGS) -march=native $(SRC) -o $@ $(ALTLIBS) \
		;; \
	    *) \
		echo `uname -s` has not been tested.; \
		echo You might try: gcc -pthread -O2 srch.c -o srch -lpthread -lrt \
		;; \
	esac

$(BINWIN64): $(SRC)
	@cp -p srch.c win/; \
	cd win; \
	x86_64-w64-mingw32-gcc --version >/dev/null 2>&1 || { echo =\> You need to install mingw-w64, mingw-w64-common and mingw-w64-x86-64-dev to be able to compile for 64-bit Windows.; exit 1; }; \
	set -x; \
	x86_64-w64-mingw32-gcc $(CFLAGSWIN) $(SRC) $(SRCWINX) -o $@ -lpthread -static; \
	test -f $(BINWIN64) && cp -p $(BINWIN64) ..

$(BINWIN32): $(SRC)
	@cp -p srch.c win/; \
	cd win; \
	i686-w64-mingw32-gcc --version >/dev/null 2>&1 || { echo =\> You need to install mingw-w64, mingw-w64-common and mingw-w64-i686-dev to be able to compile for 32-bit Windows.; exit 1; }; \
	set -x; \
	i686-w64-mingw32-gcc $(CFLAGSWIN) $(SRC) $(SRCWINX) -o $@ -lpthread -static; \
	test -f $(BINWIN32) && cp -p $(BINWIN32) ..

test: $(BIN)
	./testsrch -c

install: $(BIN)
	mkdir -p /usr/local/bin && cp -p srch /usr/local/bin && chmod 755 /usr/local/bin/srch; \
	mkdir -p /usr/local/man/man1 && cp -p srch.1 /usr/local/man/man1 && chmod 644 /usr/local/man/man1/srch.1

clean:
	-rm -f $(BIN)

.PHONY : all test clean
