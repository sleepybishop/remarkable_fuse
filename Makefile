LIBOBJ=\
deps/cJSON/cJSON.o\
deps/struct/src/struct.o\
deps/struct/src/struct_endian.o\
deps/sds/sds.o\
deps/plutovg/plutovg-blend.o\
deps/plutovg/plutovg-canvas.o\
deps/plutovg/plutovg-font.o\
deps/plutovg/plutovg-ft-math.o\
deps/plutovg/plutovg-ft-raster.o\
deps/plutovg/plutovg-ft-stroker.o\
deps/plutovg/plutovg-matrix.o\
deps/plutovg/plutovg-paint.o\
deps/plutovg/plutovg-path.o\
deps/plutovg/plutovg-rasterize.o\
deps/plutovg/plutovg-surface.o\
remfs.o\
remfmt.o\
rm_parser.o\
template_renderer.o\
render_svg.o\
render_png.o\
render_pdf.o\
render_xoj.o\
pdfoverlay.o\
cache.o\
path_utils.o\
generators.o\
remfuse.o

CPPFLAGS := -D_DEFAULT_SOURCE -D_FILE_OFFSET_BITS=64
CFLAGS   := -O0 -g -std=c99 -Wall -I. -Ideps -Ideps/cJSON -Ideps/struct/include/struct -Ideps/plutovg
LDLIBS   := -lm -lpng -lz

all: remfs remfmt pdfoverlay

libremfs.a: $(LIBOBJ)
	$(AR) rcs $@ $^

remfs: main.o libremfs.a
	$(CC) $(LDFLAGS) -o $@ main.o libremfs.a $(LDLIBS) -lfuse

remfmt: remfmt_cli.o libremfs.a
	$(CC) $(LDFLAGS) -o $@ remfmt_cli.o libremfs.a $(LDLIBS)

pdfoverlay: pdfoverlay_cli.o deps/sds/sds.o
	$(CC) $(LDFLAGS) -o $@ pdfoverlay_cli.o deps/sds/sds.o $(LDLIBS) -lz

pdfoverlay_cli.o: pdfoverlay.c
	$(CC) $(CFLAGS) $(CPPFLAGS) -DPDFOVERLAY_CLI -c -o $@ $<

check: remfs remfmt pdfoverlay t/test_remfs
	prove -v t/

t/test_remfs: t/test_remfs.c libremfs.a
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ t/test_remfs.c libremfs.a $(LDLIBS)

clean:
	$(RM) *.o deps/cJSON/*.o deps/struct/src/*.o deps/sds/*.o deps/*.o deps/plutovg/*.o libremfs.a remfs remfmt t/test_remfs

indent:
	clang-format -style=LLVM -i *.c *.h

scan:
	scan-build $(MAKE) clean all

.PHONY: all clean indent scan check test

test: check

