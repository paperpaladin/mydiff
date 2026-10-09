CC      = gcc
# -ffile-prefix-map keeps the build reproducible: without it -g writes this
# build directory into DWARF .debug_line_str and two trees differ by path.
CFLAGS  = -std=c11 -O2 -g -Wall -Wextra -Wpedantic \
          -ffile-prefix-map=$(CURDIR)=.

mydiff: mydiff.c
	$(CC) $(CFLAGS) -o $@ $<

test: mydiff
	t/run.sh ./mydiff

clean:
	rm -f mydiff mydiff-asan
	rm -rf t/scratch

.PHONY: test clean
