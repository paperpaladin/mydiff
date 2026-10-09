# mydiff

Myers' diff algorithm in one C11 file. Computes a shortest edit script between
two text files, prints it as a unified diff, and applies one back again. The
only test oracle is GNU diffutils and GNU patch.

## make test

```
$ make clean && make test
rm -f mydiff mydiff-asan
rm -rf t/scratch
gcc -std=c11 -O2 -g -Wall -Wextra -Wpedantic -ffile-prefix-map=. -o mydiff mydiff.c
t/run.sh ./mydiff

corpus: 93 pairs

93 agree with GNU diff byte-for-byte, 0 differ

fuzz: 400 adversarial pairs
388 agree with plain `diff -U N`, 12 differ
of the 12, 12 are GNU disagreeing with its own
--minimal (discard_confusing_lines) and 0 are unexplained

995 passed, 0 failed
```

`make test-asan` runs the same 995 checks under
`-fsanitize=address,undefined`, and also passes.

## Usage

```sh
mydiff A B                  # unified diff, 3 lines of context
mydiff -U 0 A B             # no context
mydiff --minimal A B        # disable the non-minimal heuristics
mydiff -p PATCH ORIG > NEW  # apply a unified diff
```

Exit status is GNU diff's: 0 same, 1 differ, 2 trouble.

```sh
$ mydiff a b | patch -F0 a      # works, which is half the point
$ diff -u a b > p; mydiff -p p a | cmp - b   # works, which is the other half
```

Four extra modes exist only because the test suite uses them: `--stat`
(line, class and changed-line counts), `--runs` (the raw edit script with no
formatting), `--hunks` (re-print the `@@` headers a patch file contains).
They are oracle hooks rather than features -- each one exposes an
intermediate result so that an independent GNU output format can check it in
isolation.

## Why the oracle is unusually good here

Most reimplementations can only be checked loosely: you can test that a
decompressor round-trips, or that a parser accepts valid input, but
"byte-identical to the reference" is usually off the table because formatting
is a matter of taste. Diff is different. Myers' algorithm produces a *minimal*
edit script, GNU diffutils implements the same algorithm, and the unified
format is fully specified down to where the newline goes. So exact agreement
is achievable, and the bar can be set at it:

**`mydiff A B` and `diff -u A B` must produce the same bytes**, including the
two header lines with their nanosecond mtimes, which this program formats the
way GNU's `time_format` does precisely so that there is nothing to normalise
away before comparing. Any disagreement is then a real difference in
behaviour that can be chased down, not a formatting quibble to be argued
about.

And the loop closes in both directions against two independent tools:

1. `diff -u A B` equals `mydiff A B`, byte for byte.
2. GNU patch applied to **our** diff reproduces B from A.
3. Our `-p` applied to **GNU's** diff reproduces B from A.

(3) is not redundant with (1): a shared misunderstanding of the format would
pass (1) and fail (2) or (3). Only the self round-trip, `mydiff -p` on
`mydiff`'s own output, is weak, and the suite says so where it runs it.

## What does not work

This is the honest list. Several of these are deliberate and marked
`shortcut:` in the source with their ceiling and upgrade path; the rest are
simply absent.

**We differ from `diff -u`'s default output on a measurable class of inputs.**
GNU diff, by default, runs `discard_confusing_lines` before comparing: it
throws away lines it judges unhelpful, which is a heuristic and which changes
*which* of several equally short edit scripts comes out. This program does not
implement it. The consequence, measured rather than guessed:

- On the 93-pair corpus: no difference at all, 93 of 93 byte-identical.
- On 400 deliberately adversarial pairs (line alphabets of 1 to 3 distinct
  values, dense changes, randomised context widths): 388 byte-identical to
  plain `diff -u`, 400 byte-identical to `diff -u --minimal`.
- All 12 differences are cases where GNU's *own* default output differs from
  GNU's own `--minimal` output. The suite checks that for every one of them
  and fails if any difference is left unexplained. That number is currently 0.

Both scripts cost the same number of edits in every case observed, so this is
a tie broken differently, not a worse answer. It is still a difference from
the reference, and it is the first unchecked item in `PLAN.md`.

**`-p` is not a replacement for patch(1), and is not trying to be.**

- No fuzz and no offset search. Context and deleted lines must match the
  original exactly, at exactly the line the `@@` header names. GNU patch will
  cheerfully apply a hunk 40 lines away from where the header says; that is
  the right behaviour for a human applying a stale patch and the wrong
  behaviour for a closed test loop, because a patch whose line numbers were
  all one off would still apply and the loop would stop detecting it.
- Hunks must appear in increasing order of original line number.
- Single-file patches only. A patch containing several `--- / +++` sections
  will be applied as if it were all one file, and will almost certainly
  produce nonsense rather than an error.
- No reversed-patch detection, no file creation or deletion, no `Index:` or
  git extended headers, no `.orig`/`.rej` files, no backups.
- **Output is streamed.** A patch that fails on its third hunk has already
  written the first two hunks' worth of lines to stdout. The exit status is 2
  and the output must be thrown away; nothing cleans up for you. Buffering it
  is the second unchecked item in `PLAN.md`.
- Only the unified format is parsed. Context, normal and ed-style diffs are
  not.

**Only unified output.** No `-c` context diffs, no normal or ed or RCS output,
no `--side-by-side`, no if-then-else formats.

**Almost none of diff's other options.** No `-b`, `-w`, `-i`, `-B`
(whitespace and case insensitivity), no `-r` for directories, no `--label`, no
colour, no binary-file detection, no `-N`/`--new-file`. Two files, both
regular, both text, is the whole input domain.

**`-p` means something different here than in GNU diff.** In `diff`, `-p`
shows which C function each hunk falls in. Here it selects patch mode. If you
have muscle memory for `diff -p`, this will surprise you.

**Both files are read entirely into memory**, plus an equivalence-class table
and two frontier arrays sized N+M. 400000 lines is comfortable; a file larger
than RAM will not work and there is no streaming mode.

**A line is bytes.** No CRLF normalisation -- a file with `\r\n` endings
differs from the same file with `\n` endings on every line, which is what
`diff` also reports. No character-encoding awareness of any kind. Trailing
whitespace is a real difference, since there is no `-b`.

**Timestamps are local time**, read from `st_mtim` and formatted with
`localtime_r`, so the two header lines depend on `TZ`. GNU diff does the same,
which is why they still compare equal, but it means neither tool's output is
reproducible across time zones.

**No stdin.** Both arguments must be paths. `-` is not special, and neither is
`/dev/null`.

**Line counts are `long`.** A file with more than `LONG_MAX` lines is
unsupported, which on any machine that could hold such a file is not the
binding constraint.

## Performance

Myers is O((N+M)D) where D is the edit distance, so the interesting claim is
that cost tracks D rather than file size. Measured (`make bench`, best of five,
20000 lines on each side for the first table):

```
edit distance at fixed size (20000 lines each side)
         D    mydiff_ms      diff_ms    ratio
         2            9            3    3.00x
        20            9            5    1.80x
       200            9            5    1.80x
      2000           12            6    2.00x
     10000          139            6   23.17x
     20000          336            7   48.00x

file size at fixed edit distance (100 edits)
     lines    mydiff_ms      diff_ms    ratio
      1000            1            1    1.00x
      5000            3            2    1.50x
     20000            9            5    1.80x
    100000           45           20    2.25x
    400000          224          101    2.22x

like for like at large D: both sides forced minimal, so the
comparison is of the same algorithm and not of GNU's heuristics
         D    mydiff_ms      diff_ms    ratio
     10000          165          138    1.20x
     20000          637          524    1.22x
```

The claim holds: between D=2 and D=2000 the time barely moves, because the
whole 9ms is the fixed cost of reading and hashing 1.8MB. The `(N+M)D` term
only becomes visible past D=10000, where it goes quadratic-ish as expected.

**Where we lose, which is the part worth reading.** Against plain `diff` at
D=20000 we are 48 times slower. That number is almost entirely GNU's
heuristics and almost nothing to do with the algorithm: force both programs
to compute a genuinely minimal script and the same case closes to 1.22x. GNU
wins there by not solving the problem -- `discard_confusing_lines` plus the
big-snake shortcut let it return a non-minimal answer quickly, and on a pair
of 20000-line files with every line changed that is enormously cheaper. Our
own copy of the big-snake heuristic does fire (it is why 336ms is not the
637ms that `--minimal` costs), but without the line-discarding pass it cannot
close the gap.

On ordinary inputs -- a handful of changes in a file of any size -- we are
1.5x to 2.5x slower, and that is constant-factor cost in reading and hashing,
not asymptotics. It is a fair summary to say this is within a small multiple
of GNU diff on the inputs people actually diff, and badly beaten on inputs
where GNU gives up on minimality and we do not.

## Build

```sh
make            # gcc -std=c11 -O2 -g, no dependencies
make test       # the suite, needs diff, patch, python3
make test-asan  # the same suite under ASan + UBSan
make bench      # the tables above
make clean
```

`-ffile-prefix-map=$(CURDIR)=.` is in `CFLAGS` so that `-g` does not write
the build directory into DWARF `.debug_line_str`; the build is reproducible
across directory, timezone, locale, umask, hostname, user and `-j`.

Scratch files go in `t/scratch/`, under the repo, never `/tmp`. The suite
keys that directory on the binary name so `make test` and `make test-asan`
can run at the same time.

`detect_leaks=0` is set for `test-asan`, deliberately: this program reads two
files, writes a diff and exits, so nothing is freed and LeakSanitizer reports
every allocation in it. Eight per run, all live until exit, drowning out the
bounds findings that are the reason to run ASan at all.

## Notes from building it

Three things cost real time and are worth writing down.

**Boundary normalisation is most of the difficulty, not the algorithm.** The
D-path search is about 120 lines and worked almost immediately. Making it
agree with GNU took two more rounds, because a shortest edit script is not
unique and the ambiguity is common rather than exotic: given `a b` against
`a b a b`, inserting the new pair before or after the old one are both
minimal. GNU resolves it afterwards by sliding runs of changes forward and
merging them, and reproducing that exactly is where the bugs were.

**GNU diff's analysis depends on `-U`.** `diff -U0` and `diff -u` on the same
pair of files can report an insertion at different line numbers. This looks
like a bug in whichever program you trust less, and it is not: GNU leaves
`context` lines of the common prefix and suffix inside the region it
normalises, so a run can slide N lines further with `-U N` than with `-U 0`.
It is measurable from the outside -- feed GNU an insertion into a run of
identical lines next to the common tail and the reported line number moves by
exactly N -- and until that was found, five corpus pairs disagreed for
reasons that made no sense.

**Two of the harness's checks could not fail when first written**, which is
the failure mode that matters more than any bug in the code:

- The equivalence-class check compared our class count against
  `cat a b | sort -u | wc -l`. Every fixture ended in a newline, so deleting
  the trailing-newline flag from both the hash and the comparator still
  printed "5 passed". Worse, `sort -u` *cannot* check that flag at all, since
  `cat` welds the unterminated tail of the first file onto the first line of
  the second and the distinction is destroyed before sort runs. The check now
  says so in a comment instead of implying coverage it does not have.
- Every comparison in the suite was `[ "$(cmd)" = "$(cmd)" ]`. Command
  substitution strips trailing newlines from both sides, so a diff that ended
  without its final newline -- exactly what this project exists to get right
  -- compared equal. Everything goes through files and `cmp` now.
- And a third, found later: `--runs` printed changed line *contents*, so a run
  marked at the wrong offset with the same text compared equal. It prints line
  numbers now, which is how the `-U`-dependent behaviour above was finally
  pinned down.
