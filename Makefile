CC ?= clang
CFLAGS ?= -Os -fno-asynchronous-unwind-tables -ffunction-sections -fdata-sections -Wall -Wextra -pedantic -Werror -std=c99 -Isrc
LDFLAGS ?= -Wl,--gc-sections
LIBS = -lz
PREFIX ?= /usr

OBJS = src/main.o src/index.o src/db.o src/extractor.o src/port.o src/sha256.o src/spawn_util.o src/tar.o

all: drop

drop: $(OBJS)
	$(CC) $(LDFLAGS) $(OBJS) $(LIBS) -o $@
	strip -s $@

clean:
	rm -f drop $(OBJS)

install: drop
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 drop $(DESTDIR)$(PREFIX)/bin/drop

.PHONY: all clean install
