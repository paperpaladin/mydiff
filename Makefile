CC      = gcc
# -ffile-prefix-map keeps the build reproducible: without it -g writes this
# build directory into DWARF .debug_line_str and two trees differ by path.
CFLAGS  = -std=c11 -O2 -g -Wall -Wextra -Wpedantic \
          -ffile-prefix-map=$(CURDIR)=.

mydiff: mydiff.c
	$(CC) $(CFLAGS) -o $@ $<

test: mydiff
	t/run.sh ./mydiff

# A diff tool parses two files chosen by somebody else and a patch file
# written by somebody else, so the same suite has to pass with the sanitizers
# on. Separate binary so `make test` stays fast.
#
# exitcode=86 is load-bearing: by default a sanitizer abort exits 1, and
# t/run.sh compares our exit status against GNU diff's, where 1 means
# "files differ" -- so an ASan abort on a differing pair would have looked
# like the correct answer. UBSan needs halt_on_error too, since it otherwise
# prints and carries on.
# detect_leaks=0 is deliberate, not a cover-up. mydiff reads two files,
# writes a diff and exits; nothing is freed because there is nothing to free
# it for, so LeakSanitizer reports every single allocation and drowns out the
# bounds and use-after-free findings that are the actual reason to run this.
# Turning it on reports ~8 "leaks" per run, all of them live-until-exit.
SANOPT = ASAN_OPTIONS=exitcode=86:detect_leaks=0 \
	 UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:exitcode=86

ASAN_CFLAGS = -std=c11 -g -O1 -fsanitize=address,undefined \
	      -fno-omit-frame-pointer -Wall -Wextra -Wpedantic \
	      -ffile-prefix-map=$(CURDIR)=.

# MYDIFF_ASAN tells the suite it is running against a sanitizer build, so it
# can also check that the sanitizer is not blind: see the overread case in
# t/run.sh. CC and ASAN_CFLAGS go with it because that case compiles a
# deliberately broken copy of mydiff.c and has to build it the same way.
test-asan: mydiff.c
	$(CC) $(ASAN_CFLAGS) -o mydiff-asan mydiff.c
	$(SANOPT) MYDIFF_ASAN=1 CC='$(CC)' ASAN_CFLAGS='$(ASAN_CFLAGS)' \
		t/run.sh ./mydiff-asan

bench: mydiff
	t/run.sh --bench ./mydiff

clean:
	rm -f mydiff mydiff-asan
	rm -rf t/scratch

.PHONY: test test-asan bench clean
