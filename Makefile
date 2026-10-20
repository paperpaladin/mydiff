CC      = gcc
# -ffile-prefix-map keeps the build reproducible: without it -g writes this
# build directory into DWARF .debug_line_str and two trees differ by path.
CFLAGS  = -std=c11 -O2 -g -Wall -Wextra -Wpedantic \
          -ffile-prefix-map=$(CURDIR)=.

mydiff: mydiff.c
	$(CC) $(CFLAGS) -o $@ $<

test: mydiff
	t/run.sh ./mydiff
	@$(MAKE) --no-print-directory overread

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

# A buffer larger than its data turns the sanitizer off, and slurp's was: the
# file buffer is the one that holds the input nobody validated, so a blind spot
# there costs more than anywhere else in the program. t/run.sh already plants a
# read past the undiscarded sequence; this plants one at buf[len] and requires
# the sanitizer to report it, which it did not before slurp trimmed the buffer
# to the file.
#
# Two cases, because they were not equally bad and a gate on either one alone
# would have missed the other. A normal file was allocated st_size + 1, so a
# one-past read sat in exactly one byte of slack and was invisible while a
# two-past read was caught. A file whose st_size reads 0 (/proc and friends,
# which is why the 65536 fallback exists) started at 65536 and doubled, so the
# same read landed 65 KB inside a live allocation.
#
# The st_size check on the second input is not ceremony: if that file ever
# reported a real size, case two would silently become a second copy of case
# one and this gate would be half of what it claims.
OVERREAD_D = t/scratch/overread
PROCFILE   = /proc/self/status

overread: mydiff.c
	@mkdir -p $(OVERREAD_D)
	@$(CC) $(ASAN_CFLAGS) -DMYDIFF_PLANT_OVERREAD \
		-o $(OVERREAD_D)/probe mydiff.c
	@printf 'alpha\nbeta\ngamma\n' > $(OVERREAD_D)/a
	@printf 'alpha\nBETA\ngamma\n' > $(OVERREAD_D)/b
	@test -r $(PROCFILE) || { \
		echo 'FAIL [overread] cannot read $(PROCFILE): the zero-st_size'; \
		echo '     case has no input and would check nothing'; exit 1; }
	@test "`stat -c %s $(PROCFILE)`" = 0 || { \
		echo 'FAIL [overread] $(PROCFILE) reports a real st_size, so the'; \
		echo '     zero-st_size case is a second copy of the first one'; exit 1; }
	@rc=0; for c in 'one-byte-slack:$(OVERREAD_D)/a $(OVERREAD_D)/b' \
	                'zero-st_size:$(PROCFILE) $(PROCFILE)'; do \
		n=$${c%%:*}; f=$${c#*:}; \
		$(SANOPT) $(OVERREAD_D)/probe $$f \
			> /dev/null 2> $(OVERREAD_D)/$$n.err; \
		s=$$?; \
		if [ $$s = 86 ]; then \
			echo "[overread] $$n: the sanitizer sees the read at buf[len]"; \
		else \
			echo "FAIL [overread] $$n: the planted read at buf[len] exited $$s, not 86."; \
			echo '     slurp is handing back a buffer larger than the file in it, so'; \
			echo '     every sanitizer result about the two inputs is worth less than'; \
			echo '     it looks. Expected a heap-buffer-overflow in slurp; got:'; \
			sed -n 1,4p $(OVERREAD_D)/$$n.err; \
			rc=1; \
		fi; \
	done; exit $$rc

bench: mydiff
	t/run.sh --bench ./mydiff

clean:
	rm -f mydiff mydiff-asan
	rm -rf t/scratch

.PHONY: test test-asan overread bench clean
