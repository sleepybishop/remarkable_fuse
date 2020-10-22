OBJ=\
deps/cJSON/cJSON.o\
deps/struct/src/struct.o\
deps/struct/src/struct_endian.o\
kstr.o\
remfs.o\
remfmt.o

CPPFLAGS := -D_DEFAULT_SOURCE -D_FILE_OFFSET_BITS=64
CFLAGS   := -O0 -g -std=c99 -Wall -I. -Ideps/cJSON -Ideps/struct/include/struct
LDFLAGS  := -lm

all: remfs remfmt

remfs: LDFLAGS+= -lfuse
remfs: main.o $(OBJ)

remfmt: remfmt_cli.o $(OBJ)

clean:
	$(RM) *.o remfs remfmt 

indent:
	clang-format -style=LLVM -i *.c *.h

scan:
	scan-build $(MAKE) clean all

