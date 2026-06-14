LIBOBJ=\
deps/cJSON/cJSON.o\
deps/struct/src/struct.o\
deps/struct/src/struct_endian.o\
deps/sds/sds.o\
remfs.o\
remfmt.o

CPPFLAGS := -D_DEFAULT_SOURCE -D_FILE_OFFSET_BITS=64
CFLAGS   := -O0 -g -std=c99 -Wall -I. -Ideps -Ideps/cJSON -Ideps/struct/include/struct
LDLIBS   := -lm

all: remfs remfmt

libremfs.a: $(LIBOBJ)
	$(AR) rcs $@ $^

remfs: main.o libremfs.a
	$(CC) $(LDFLAGS) -o $@ main.o libremfs.a $(LDLIBS) -lfuse

remfmt: remfmt_cli.o libremfs.a
	$(CC) $(LDFLAGS) -o $@ remfmt_cli.o libremfs.a $(LDLIBS)

check: remfmt
	prove -v t/

clean:
	$(RM) *.o deps/cJSON/*.o deps/struct/src/*.o deps/*.o libremfs.a remfs remfmt

indent:
	clang-format -style=LLVM -i *.c *.h

scan:
	scan-build $(MAKE) clean all

.PHONY: all clean indent scan check

