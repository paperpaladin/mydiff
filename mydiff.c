/* mydiff -- Myers diff in one file: compute a minimal edit script between two
 * text files and print it as a unified diff, and apply one back again.
 *
 * The oracle is GNU diffutils. `diff -u A B` and `mydiff A B` are required to
 * agree byte for byte, headers and timestamps included, so the file-reading
 * and line-splitting conventions below are GNU's, not ones of our choosing:
 * a line is the bytes up to and including '\n', a final chunk with no '\n' is
 * still a line, and an empty file has zero lines.
 */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static const char *prog = "mydiff";

static void die(const char *what, const char *name)
{
	fprintf(stderr, "%s: %s%s%s: %s\n", prog, what, name ? ": " : "",
	        name ? name : "", strerror(errno));
	exit(2);
}

/* A line, with its newline split off. Two lines are equal iff both the bytes
 * and the newline flag match: that is what makes "a" and "a\n" different
 * lines, which is the whole no-trailing-newline question. */
struct line {
	const char *p;
	size_t n;	/* bytes, excluding the terminator */
	int nl;		/* 1 if the line ended with '\n' in the file */
};

struct file {
	const char *name;
	char *buf;
	size_t len;
	struct line *l;
	long n;
	struct timespec mtime;
};

static void *xmalloc(size_t n)
{
	void *p = malloc(n ? n : 1);
	if (!p)
		die("out of memory", NULL);
	return p;
}

static void *xcalloc(size_t n, size_t s)
{
	void *p = calloc(n ? n : 1, s);
	if (!p)
		die("out of memory", NULL);
	return p;
}

static void slurp(struct file *f, const char *name)
{
	FILE *fp;
	struct stat st;
	size_t cap = 0, got = 0;
	long i;

	f->name = name;
	fp = fopen(name, "rb");
	if (!fp)
		die("cannot read", name);
	if (fstat(fileno(fp), &st) != 0)
		die("cannot stat", name);
	f->mtime = st.st_mtim;

	/* st_size is a hint only: /proc and friends report 0. */
	cap = st.st_size > 0 ? (size_t)st.st_size + 1 : 65536;
	f->buf = xmalloc(cap);
	for (;;) {
		size_t r = fread(f->buf + got, 1, cap - got, fp);
		got += r;
		if (got < cap)
			break;
		cap *= 2;
		f->buf = realloc(f->buf, cap);
		if (!f->buf)
			die("out of memory", NULL);
	}
	if (ferror(fp))
		die("read error", name);
	fclose(fp);
	f->len = got;

	/* Count lines, then fill. Two passes beats growing an array. */
	f->n = 0;
	for (i = 0; (size_t)i < got; i++)
		if (f->buf[i] == '\n')
			f->n++;
	if (got && f->buf[got - 1] != '\n')
		f->n++;

	f->l = xmalloc(sizeof *f->l * (size_t)(f->n + 1));
	{
		size_t start = 0;
		long k = 0;
		for (i = 0; (size_t)i < got; i++) {
			if (f->buf[i] != '\n')
				continue;
			f->l[k].p = f->buf + start;
			f->l[k].n = (size_t)i - start;
			f->l[k].nl = 1;
			k++;
			start = (size_t)i + 1;
		}
		if (start < got) {
			f->l[k].p = f->buf + start;
			f->l[k].n = got - start;
			f->l[k].nl = 0;
			k++;
		}
		f->n = k;
	}
}

/* --- line equivalence classes ---------------------------------------------
 *
 * Myers compares elements, not strings. Hashing every line of both files into
 * a dense class id up front turns the inner loop comparison into an int
 * compare, which matters because that loop runs O((N+M)D) times. Equal class
 * id means equal line; the numeric values are meaningless and nothing may
 * depend on their order.
 */
struct eqtab {
	long *slot;		/* open addressing: index into ent[], or -1 */
	size_t mask;
	struct line *ent;
	long nent;
};

static size_t linehash(const struct line *l)
{
	size_t h = 1469598103934665603u;	/* FNV-1a 64 */
	size_t i;
	for (i = 0; i < l->n; i++) {
		h ^= (unsigned char)l->p[i];
		h *= 1099511628211u;
	}
	/* The nl flag is part of the identity, not metadata: "a" and "a\n"
	 * are different lines and must land in different classes. */
	h ^= (size_t)l->nl + 1;
	h *= 1099511628211u;
	return h;
}

static int lineeq(const struct line *x, const struct line *y)
{
	return x->nl == y->nl && x->n == y->n && memcmp(x->p, y->p, x->n) == 0;
}

static void eq_init(struct eqtab *t, long cap)
{
	size_t sz = 16;
	while ((long)sz < cap * 2 + 2)
		sz *= 2;
	t->mask = sz - 1;
	t->slot = xmalloc(sizeof *t->slot * sz);
	memset(t->slot, 0xff, sizeof *t->slot * sz);
	t->ent = xmalloc(sizeof *t->ent * (size_t)(cap + 1));
	t->nent = 0;
}

static long eq_id(struct eqtab *t, const struct line *l)
{
	size_t i = linehash(l) & t->mask;
	while (t->slot[i] >= 0) {
		if (lineeq(&t->ent[t->slot[i]], l))
			return t->slot[i];
		i = (i + 1) & t->mask;
	}
	t->ent[t->nent] = *l;
	t->slot[i] = t->nent;
	return t->nent++;
}

/* equivs[i] is the class of line i. */
static long *classify(struct eqtab *t, const struct file *f)
{
	long *e = xmalloc(sizeof *e * (size_t)(f->n + 1));
	long i;
	for (i = 0; i < f->n; i++)
		e[i] = eq_id(t, &f->l[i]);
	return e;
}

/* --- discarding confusing lines ------------------------------------------
 *
 * GNU diff does not run the comparison over the lines of the two files. It
 * throws some of them away first: a line that matches nothing at all in the
 * other file is an insertion or a deletion whatever the rest of the script
 * does, and a line that matches very many is noise that drags the search
 * around. What survives is compared; what was discarded is marked changed on
 * the spot.
 *
 * This is a heuristic and the thresholds are somebody's taste, not a
 * derivation: "five times the square root of the line count", "cancel a
 * subrun of MINIMUM or more", "three nonprovisionals in a row", "at least 8
 * lines in". So the code below is a copy of diffutils' analyze.c, structure
 * and constants together, and what it buys is agreement with `diff -u`. It
 * does not buy a shorter edit script: where the two answers differ they cost
 * the same number of edits, and the discarding one can only be longer, never
 * shorter, than the minimal script `--minimal` still produces.
 *
 * One line of GNU's is deliberately absent. Its marking loop begins
 * `if (equivs[i] == 0) continue;`, which exempts the lines that -I or -B
 * told it to ignore: those are given class 0 there and no real line ever has
 * it. We have neither option and our class 0 is an ordinary class, the first
 * line hashed, so copying that test would exempt an arbitrary line from the
 * heuristic. Dropping it is the faithful translation, not a shortcut.
 */

/* Mark each line of one file: 1 for "matches nothing on the other side",
 * 2 for "matches so many that it is probably noise", which is provisional
 * and may be taken back below. COUNTS is the other file's per-class count. */
static void mark_discards(char *discards, const long *equivs, long end,
                          const long *counts)
{
	long i, many = 5, tem = end / 64;

	/* MANY ends up as 5 times roughly the square root of END. */
	while ((tem >>= 2) > 0)
		many *= 2;
	for (i = 0; i < end; i++) {
		long nmatch = counts[equivs[i]];
		if (nmatch == 0)
			discards[i] = 1;
		else if (nmatch > many)
			discards[i] = 2;
	}
}

/* Keep a provisional discard only where it sits inside a run of discardable
 * lines that both begins and ends with a definite one, and then only under
 * the four further conditions GNU applies to the inside of such a run. */
static void cancel_provisional(char *discards, long end)
{
	long i;

	for (i = 0; i < end; i++) {
		long j, length, provisional = 0;

		/* A provisional discard outside any run of discards goes. */
		if (discards[i] == 2) {
			discards[i] = 0;
			continue;
		}
		if (discards[i] == 0)
			continue;

		/* A definite discard: find the end of its run, counting the
		 * provisionals in it. */
		for (j = i; j < end; j++) {
			if (discards[j] == 0)
				break;
			if (discards[j] == 2)
				provisional++;
		}
		/* Cancel provisionals at the end, shrinking the run, so that
		 * it is definite at both ends. */
		while (j > i && discards[j - 1] == 2) {
			discards[--j] = 0;
			provisional--;
		}
		length = j - i;

		if (provisional * 4 > length) {
			/* A quarter of the run is provisional: trust none
			 * of them. */
			while (j > i)
				if (discards[--j] == 2)
					discards[j] = 0;
		} else {
			long consec, minimum = 1, tem = length >> 2;

			/* MINIMUM is roughly the square root of LENGTH/4,
			 * plus one: a subrun of two or more provisionals can
			 * stand once LENGTH reaches 16, of four or more once
			 * it reaches 64. */
			while ((tem >>= 2) > 0)
				minimum <<= 1;
			minimum++;

			/* Cancel any subrun of MINIMUM or more provisionals
			 * inside the larger run. */
			for (j = 0, consec = 0; j < length; j++)
				if (discards[i + j] != 2)
					consec = 0;
				else if (minimum == ++consec)
					j -= consec;	/* back up and redo */
				else if (minimum < consec)
					discards[i + j] = 0;

			/* From the start of the run, cancel provisionals
			 * until three definite discards in a row, or the
			 * first definite one at least 8 lines in. */
			for (j = 0, consec = 0; j < length; j++) {
				if (j >= 8 && discards[i + j] == 1)
					break;
				if (discards[i + j] == 2) {
					consec = 0;
					discards[i + j] = 0;
				} else if (discards[i + j] == 0)
					consec = 0;
				else
					consec++;
				if (consec == 3)
					break;
			}

			i += length - 1;	/* I is the run's last line */

			/* The same, backwards from the end of the run. */
			for (j = 0, consec = 0; j < length; j++) {
				if (j >= 8 && discards[i - j] == 1)
					break;
				if (discards[i - j] == 2) {
					consec = 0;
					discards[i - j] = 0;
				} else if (discards[i - j] == 0)
					consec = 0;
				else
					consec++;
				if (consec == 3)
					break;
			}
		}
	}
}

/* Shrink an array to exactly the part of it that is used.
 *
 * Not a tidiness measure. The undiscarded sequences are allocated for the
 * case where nothing is discarded, so after discarding they are buffers
 * larger than their data, and a read one past the end of one lands inside
 * the same live allocation where no sanitizer can see it. Three repositories
 * in this collection have had a bounds bug hidden exactly that way. One
 * realloc each buys the coverage back, and t/run.sh keeps it honest. */
static long *trim(long *p, long n)
{
	long *q = realloc(p, sizeof *p * (size_t)(n > 0 ? n : 1));
	return q ? q : p;
}

/* Build the sequence the comparison will actually see, and the map from a
 * position in it back to a line of the file. Every discarded line is marked
 * changed here and is never considered again. Returns the length. */
static long apply_discards(const char *discards, const long *equivs, long end,
                           long *undiscarded, long *realindexes, char *changed)
{
	long i, j = 0;

	for (i = 0; i < end; i++) {
		if (discards[i] == 0) {
			undiscarded[j] = equivs[i];
			realindexes[j++] = i;
		} else {
			changed[i] = 1;
		}
	}
	return j;
}

/* --- the D-path search ----------------------------------------------------
 *
 * Myers 1986. Two breadth-first searches are run over the edit graph at once,
 * one from the top-left corner and one from the bottom-right, each advancing
 * one edit at a time. The furthest-reaching forward path on diagonal k after
 * c edits is fd[k]; likewise bd[k] backwards. When the two frontiers touch,
 * the point where they touch lies on a shortest path, and the problem splits
 * there into two independent halves. That is the linear-space variant: cost
 * O((N+M)D), space O(N+M), and no stored trace to walk backwards through.
 *
 * The tie-breaks here are not arbitrary. Where two incoming paths reach the
 * same diagonal, which one is taken decides which of several equally short
 * edit scripts comes out, and the output has to match GNU diff's choice byte
 * for byte -- so `tlo >= thi ? tlo + 1 : thi` forward and `tlo < thi ? tlo :
 * thi - 1` backward are copied from the shape diffutils' diffseq.h uses, not
 * invented.
 */
#define SNAKE_LIMIT 20		/* a "big" snake, for the heuristic below */

struct part {
	long xmid, ymid;
	int lo_minimal, hi_minimal;
};

struct ctx {
	const long *xv, *yv;	/* equivalence classes of each file */
	long *fd, *bd;		/* furthest-reaching paths, indexable < 0 */
	char *ch0, *ch1;	/* output: per-line changed flags, middle only */
	const long *ri0, *ri1;	/* undiscarded index -> line within the middle */
	int heuristic;
	long too_expensive;
};

/* Find a point on some shortest edit path between the two subsequences, and
 * store it in *part. Returns nothing: the midpoint is the whole answer. */
static void diag(long xoff, long xlim, long yoff, long ylim, int find_minimal,
                 struct part *part, struct ctx *c)
{
	long *const fd = c->fd;
	long *const bd = c->bd;
	const long *const xv = c->xv;
	const long *const yv = c->yv;
	const long dmin = xoff - ylim;	/* lowest valid diagonal */
	const long dmax = xlim - yoff;	/* highest valid diagonal */
	const long fmid = xoff - yoff;	/* centre of the forward search */
	const long bmid = xlim - ylim;	/* centre of the backward search */
	long fmin = fmid, fmax = fmid;
	long bmin = bmid, bmax = bmid;
	long cost;
	/* True when the far corner sits on an odd diagonal relative to the
	 * near one; it decides which of the two frontiers can detect the
	 * overlap, and getting it wrong costs one edit of minimality. */
	const int odd = (fmid - bmid) & 1;

	fd[fmid] = xoff;
	bd[bmid] = xlim;

	for (cost = 1;; cost++) {
		long d;
		int big_snake = 0;

		/* Widen the forward frontier by one edit on each diagonal. */
		if (fmin > dmin)
			fd[--fmin - 1] = -1;
		else
			fmin++;
		if (fmax < dmax)
			fd[++fmax + 1] = -1;
		else
			fmax--;
		for (d = fmax; d >= fmin; d -= 2) {
			long x, y, oldx, tlo = fd[d - 1], thi = fd[d + 1];

			x = tlo >= thi ? tlo + 1 : thi;
			oldx = x;
			y = x - d;
			while (x < xlim && y < ylim && xv[x] == yv[y])
				x++, y++;
			if (x - oldx > SNAKE_LIMIT)
				big_snake = 1;
			fd[d] = x;
			if (odd && bmin <= d && d <= bmax && bd[d] <= x) {
				part->xmid = x;
				part->ymid = y;
				part->lo_minimal = part->hi_minimal = 1;
				return;
			}
		}

		/* And the backward frontier. */
		if (bmin > dmin)
			bd[--bmin - 1] = LONG_MAX;
		else
			bmin++;
		if (bmax < dmax)
			bd[++bmax + 1] = LONG_MAX;
		else
			bmax--;
		for (d = bmax; d >= bmin; d -= 2) {
			long x, y, oldx, tlo = bd[d - 1], thi = bd[d + 1];

			x = tlo < thi ? tlo : thi - 1;
			oldx = x;
			y = x - d;
			while (x > xoff && y > yoff && xv[x - 1] == yv[y - 1])
				x--, y--;
			if (oldx - x > SNAKE_LIMIT)
				big_snake = 1;
			bd[d] = x;
			if (!odd && fmin <= d && d <= fmax && x <= fd[d]) {
				part->xmid = x;
				part->ymid = y;
				part->lo_minimal = part->hi_minimal = 1;
				return;
			}
		}

		if (find_minimal)
			continue;

		/* The two heuristics below are GNU diff's, and they are the
		 * reason `diff` without --minimal can return a non-minimal
		 * script. They are here only because matching GNU's default
		 * output byte for byte is the project's bar. Both are off
		 * under --minimal.
		 *
		 * shortcut: this first one, the big-snake shortcut, is dead
		 * code. GNU gates it on `speed_large_files`, which only
		 * `-H` sets, and -H is not an option here, so c->heuristic is
		 * always 0 and this block never runs. It was reached until
		 * this commit, because the flag was wired to !minimal, which
		 * is not what GNU does: that made us take a non-minimal
		 * shortcut on inputs where GNU would not, and a diff of two
		 * 20000-line files with 2000 edits was measured firing it
		 * eight times. Upgrade path: implement -H, with `diff -H -u`
		 * as its oracle, and set c->heuristic from it. Until then the
		 * block is kept rather than deleted because it is GNU's code
		 * for that flag and nothing else wants the space.
		 *
		 * The too_expensive bailout below it is NOT gated this way,
		 * in GNU either: it runs by default and it does fire here. */
		if (cost > 200 && big_snake && c->heuristic) {
			long best = 0;

			for (d = fmax; d >= fmin; d -= 2) {
				long dd = d - fmid;
				long x = fd[d], y = fd[d] - d;
				long v = (x - xoff) * 2 - dd;

				if (v <= 12 * (cost + (dd < 0 ? -dd : dd)))
					continue;
				if (v > best && xoff + SNAKE_LIMIT <= x
				    && x < xlim && yoff + SNAKE_LIMIT <= y
				    && y < ylim) {
					long k;
					for (k = 1; xv[x - k] == yv[y - k]; k++)
						if (k == SNAKE_LIMIT) {
							best = v;
							part->xmid = x;
							part->ymid = y;
							break;
						}
				}
			}
			if (best > 0) {
				part->lo_minimal = 1;
				part->hi_minimal = 0;
				return;
			}

			best = 0;
			for (d = bmax; d >= bmin; d -= 2) {
				long dd = d - bmid;
				long x = bd[d], y = bd[d] - d;
				long v = (xlim - x) * 2 + dd;

				if (v <= 12 * (cost + (dd < 0 ? -dd : dd)))
					continue;
				if (v > best && xoff < x
				    && x <= xlim - SNAKE_LIMIT && yoff < y
				    && y <= ylim - SNAKE_LIMIT) {
					long k;
					for (k = 0; xv[x + k] == yv[y + k]; k++)
						if (k == SNAKE_LIMIT - 1) {
							best = v;
							part->xmid = x;
							part->ymid = y;
							break;
						}
				}
			}
			if (best > 0) {
				part->lo_minimal = 0;
				part->hi_minimal = 1;
				return;
			}
		}

		/* Gone well past the point of diminishing returns: split at
		 * the best point either frontier reached and accept a
		 * non-minimal answer rather than run for minutes. */
		if (cost >= c->too_expensive) {
			long fxbest = -1, fybest = 0, bxbest = 0, bybest = 0;
			long bxbest_set = 0;

			/* Best forward diagonal, by distance travelled. */
			for (d = fmax; d >= fmin; d -= 2) {
				long x = fd[d] < xlim ? fd[d] : xlim;
				long y = x - d;
				if (ylim < y)
					x = ylim + d, y = ylim;
				if (fxbest < x + y)
					fxbest = x + y, fybest = d;
			}
			/* Best backward diagonal, likewise. */
			for (d = bmax; d >= bmin; d -= 2) {
				long x = xoff > bd[d] ? xoff : bd[d];
				long y = x - d;
				if (y < yoff)
					x = yoff + d, y = yoff;
				if (!bxbest_set || x + y < bxbest)
					bxbest = x + y, bybest = d,
					bxbest_set = 1;
			}

			if ((xlim + ylim) - bxbest < fxbest - (xoff + yoff)) {
				part->xmid = (fxbest + fybest) / 2;
				part->ymid = (fxbest - fybest) / 2;
			} else {
				part->xmid = (bxbest + bybest) / 2;
				part->ymid = (bxbest - bybest) / 2;
			}
			part->lo_minimal = part->hi_minimal = 0;
			return;
		}
	}
}

/* Mark every changed line between the two subsequences. Slides off the shared
 * head and tail first -- which is both the common-prefix optimisation and the
 * recursion's base case -- then splits at a midpoint and recurses. */
static void compareseq(long xoff, long xlim, long yoff, long ylim,
                       int find_minimal, struct ctx *c)
{
	const long *xv = c->xv, *yv = c->yv;

	while (xoff < xlim && yoff < ylim && xv[xoff] == yv[yoff])
		xoff++, yoff++;
	while (xlim > xoff && ylim > yoff && xv[xlim - 1] == yv[ylim - 1])
		xlim--, ylim--;

	if (xoff == xlim) {
		while (yoff < ylim)
			c->ch1[c->ri1[yoff++]] = 1;
	} else if (yoff == ylim) {
		while (xoff < xlim)
			c->ch0[c->ri0[xoff++]] = 1;
	} else {
		struct part part;
		diag(xoff, xlim, yoff, ylim, find_minimal, &part, c);

		/* A split point outside the range it splits is always wrong, and
		 * the failure it causes is the worst kind: both halves of the
		 * recursion get an inverted range, diag() scans one without a
		 * bound, and the process simply never returns.  Observed once,
		 * for real -- compareseq(xlim=51, ylim=-169) from a parent with
		 * ylim=35 -- after an orphaned run had held a core at 99% for
		 * fifteen hours.  The suite had reported 995 passed throughout,
		 * because it never waited on that invocation.
		 *
		 * Not reproducible from the inputs on disk, so this is not a fix
		 * and is not dressed up as one.  It converts an unbounded hang
		 * into an immediate, located abort, which is the difference
		 * between a bug that can be chased and one that cannot. */
		if (part.xmid < xoff || part.xmid > xlim ||
		    part.ymid < yoff || part.ymid > ylim) {
			fprintf(stderr,
			    "mydiff: internal error: split (%ld,%ld) outside "
			    "x[%ld,%ld] y[%ld,%ld]\n",
			    (long)part.xmid, (long)part.ymid,
			    (long)xoff, (long)xlim, (long)yoff, (long)ylim);
			exit(3);
		}

		compareseq(xoff, part.xmid, yoff, part.ymid,
		           part.lo_minimal, c);
		compareseq(part.xmid, xlim, part.ymid, ylim,
		           part.hi_minimal, c);
	}
}

/* --- boundary normalisation -----------------------------------------------
 *
 * A shortest edit script is not unique, and the ambiguity is not academic:
 * given
 *      a        a
 *      b        b
 *      a   ->   a
 *      b        b
 *               a
 *               b
 * "insert a,b at the top" and "insert a,b at the bottom" are both minimal and
 * the search picks whichever its tie-breaks lead to. GNU diff resolves this
 * after the fact, by sliding each run of changes as far forward as the
 * surrounding identical lines allow and merging runs that meet, then pulling
 * back to line up with a changed run in the other file where one exists. The
 * result is a canonical choice, and without it our output disagrees with
 * GNU's on perfectly minimal scripts -- which is what the 2000-line
 * small-alphabet fixture caught.
 *
 * This is diffutils' shift_boundaries, same structure, run once per file with
 * the roles swapped. It reads changed[-1] and other_changed[-1], hence the
 * slack slot at the front of both arrays.
 *
 * It is handed the *middle* of each file, with the common prefix and suffix
 * cut off, because a run must not slide into the common tail. GNU keeps that
 * distinction as buffered_lines (the middle) versus valid_lines (the whole
 * file), and losing it is a real bug with a one-line symptom: see the commit
 * that added this.
 *
 * The middle is not quite "everything that is not common", though: GNU leaves
 * `context` lines of the common prefix and suffix inside it, so with -U3 a
 * run can slide three lines further than with -U0. That is measurable from
 * outside -- feed GNU an insertion into a run of identical lines next to the
 * common tail and the reported line number moves by exactly N for -U N -- and
 * it is the reason output depends on the context width in a way that looks at
 * first like a bug in one of the two programs.
 */
static void shift_one(char *changed, long i_end, const long *equivs,
                      const char *other_changed)
{
	long i = 0, j = 0;

	for (;;) {
		long runlength, start, corresponding;

		/* Find the next run of changes, tracking the matching point
		 * in the other file as we go. */
		while (i < i_end && !changed[i]) {
			while (other_changed[j++])
				continue;
			i++;
		}
		if (i == i_end)
			break;
		start = i;
		while (changed[++i])
			continue;
		while (other_changed[j])
			j++;

		do {
			runlength = i - start;

			/* Slide back while the line before the run equals the
			 * last line of it; this merges with earlier runs. */
			while (start && equivs[start - 1] == equivs[i - 1]) {
				changed[--start] = 1;
				changed[--i] = 0;
				while (changed[start - 1])
					start--;
				while (other_changed[--j])
					continue;
			}

			/* Last position at which this run still lines up with
			 * a changed run in the other file; i_end for none. */
			corresponding = other_changed[j - 1] ? i : i_end;

			/* Then slide forward as far as possible. Forward
			 * second, so that with nothing to merge the run ends
			 * up as late as the identical lines allow. */
			while (i != i_end && equivs[start] == equivs[i]) {
				changed[start++] = 0;
				changed[i++] = 1;
				while (changed[i])
					i++;
				while (other_changed[++j])
					corresponding = i;
			}
		} while (runlength != i - start);

		/* Having merged all it can, pull the run back to a
		 * corresponding run in the other file if there was one. */
		while (corresponding < i) {
			changed[--start] = 1;
			changed[--i] = 0;
			while (other_changed[--j])
				continue;
		}
	}
}

/* --- changed flags to an edit script --------------------------------------
 *
 * The two boolean arrays are a better representation than a list of edits,
 * because they make the ordering ambiguity disappear: "delete A3, insert B3"
 * and "insert B3, delete A3" are the same pair of arrays. Grouping them into
 * runs, with unchanged lines advancing in lockstep, is all "build the edit
 * script" means here.
 */
struct run {
	long line0, deleted, line1, inserted;
};

struct script {
	struct run *r;
	long n, cap;
};

static void addrun(struct script *s, long l0, long d, long l1, long ins)
{
	if (s->n == s->cap) {
		s->cap = s->cap ? s->cap * 2 : 64;
		s->r = realloc(s->r, sizeof *s->r * (size_t)s->cap);
		if (!s->r)
			die("out of memory", NULL);
	}
	s->r[s->n].line0 = l0;
	s->r[s->n].deleted = d;
	s->r[s->n].line1 = l1;
	s->r[s->n].inserted = ins;
	s->n++;
}

static void build_script(struct script *s, const char *ch0, long n0,
                         const char *ch1, long n1)
{
	long i0 = 0, i1 = 0;

	s->r = NULL;
	s->n = s->cap = 0;
	while (i0 < n0 || i1 < n1) {
		if ((i0 < n0 && ch0[i0]) || (i1 < n1 && ch1[i1])) {
			long s0 = i0, s1 = i1;
			while (i0 < n0 && ch0[i0])
				i0++;
			while (i1 < n1 && ch1[i1])
				i1++;
			addrun(s, s0, i0 - s0, s1, i1 - s1);
		} else {
			i0++;
			i1++;
		}
	}
}

/* Write one line exactly as it appeared in its file: bytes, then the newline
 * only if the file had one there. This is what GNU's %L line format does. */
static void putline(const struct line *l)
{
	fwrite(l->p, 1, l->n, stdout);
	if (l->nl)
		putchar('\n');
}

/* Write one line into a diff. A diff is itself line-oriented, so a line that
 * had no newline in its file still gets one here -- followed by the marker
 * that says so. Without this the next line's +/-/space prefix is glued onto
 * the end of this one and the diff is unparseable. */
static void putdiffline(const struct line *l)
{
	fwrite(l->p, 1, l->n, stdout);
	putchar('\n');
	if (!l->nl)
		fputs("\\ No newline at end of file\n", stdout);
}

/* --- unified output -------------------------------------------------------
 *
 * All of the arithmetic here is GNU's, because the bar is byte-identical
 * output and the line numbers in an @@ header are the easiest thing in a diff
 * to get subtly wrong. Three rules that are not obvious:
 *
 *  - two runs of changes land in the same hunk when the gap between them is
 *    less than 2*context+1 lines, so that their context windows would overlap
 *    or touch;
 *  - a range of zero lines prints as "N,0" where N is the line *before* the
 *    insertion point, not as "N" or "N,0" at the insertion point;
 *  - a range of one line prints as just "N", with no count.
 */
static long context = 3;

static void timestr(char *out, size_t n, const struct timespec *ts)
{
	struct tm tm;
	char base[24], zone[8];

	/* GNU's time_format is "%Y-%m-%d %H:%M:%S.%N %z"; %N is not in C11
	 * strftime, so the nanoseconds go in by hand. */
	if (!localtime_r(&ts->tv_sec, &tm)) {
		snprintf(out, n, "?");
		return;
	}
	strftime(base, sizeof base, "%Y-%m-%d %H:%M:%S", &tm);
	strftime(zone, sizeof zone, "%z", &tm);
	snprintf(out, n, "%s.%09ld %s", base, (long)ts->tv_nsec, zone);
}

/* A line range, from 0-based inclusive bounds. b < a means "no lines here". */
static void prange(long a, long b)
{
	long ta = a + 1, tb = b + 1;

	/* "<" and not "<=": with a == b the range is one line and prints as a
	 * bare number. `diff -u` on two one-line files says "@@ -1 +1 @@",
	 * and "<=" makes that "@@ -1,0 +1,0 @@". */
	if (tb < ta)
		printf("%ld,0", tb);
	else if (ta == tb)
		printf("%ld", tb);
	else
		printf("%ld,%ld", ta, tb - ta + 1);
}

static int print_unified(const struct file *a, const struct file *b,
                         const struct script *sc)
{
	long k = 0;

	if (!sc->n)
		return 0;
	{
		char ta[64], tb[64];
		timestr(ta, sizeof ta, &a->mtime);
		timestr(tb, sizeof tb, &b->mtime);
		printf("--- %s\t%s\n", a->name, ta);
		printf("+++ %s\t%s\n", b->name, tb);
	}

	while (k < sc->n) {
		long e = k, first0, last0, first1, last1, i, j;

		while (e + 1 < sc->n
		       && sc->r[e + 1].line0
		          - (sc->r[e].line0 + sc->r[e].deleted)
		          < 2 * context + 1)
			e++;

		first0 = sc->r[k].line0;
		first1 = sc->r[k].line1;
		last0 = sc->r[e].line0 + sc->r[e].deleted - 1;
		last1 = sc->r[e].line1 + sc->r[e].inserted - 1;

		first0 = first0 - context > 0 ? first0 - context : 0;
		first1 = first1 - context > 0 ? first1 - context : 0;
		last0 = last0 < a->n - context ? last0 + context : a->n - 1;
		last1 = last1 < b->n - context ? last1 + context : b->n - 1;

		fputs("@@ -", stdout);
		prange(first0, last0);
		fputs(" +", stdout);
		prange(first1, last1);
		fputs(" @@\n", stdout);

		i = first0;
		j = first1;
		while (i <= last0 || j <= last1) {
			if (k > e || i < sc->r[k].line0) {
				putchar(' ');
				putdiffline(&a->l[i++]);
				j++;
			} else {
				long n;
				for (n = sc->r[k].deleted; n--;) {
					putchar('-');
					putdiffline(&a->l[i++]);
				}
				for (n = sc->r[k].inserted; n--;) {
					putchar('+');
					putdiffline(&b->l[j++]);
				}
				k++;
			}
		}
	}
	return 1;
}

/* --- patch mode -----------------------------------------------------------
 *
 * `mydiff -p PATCH A` writes A with PATCH applied to stdout. This exists so
 * the project closes the loop in both directions: GNU patch has to accept
 * what we emit, and we have to accept what GNU diff emits.
 *
 * It is strict on purpose. No fuzz, no offset search, no reversed-patch
 * detection, no file-renaming or multi-file patches: a hunk's context and
 * deleted lines must match the original exactly, at exactly the line the
 * header names, or we exit 2 and say which line disagreed. GNU patch's
 * tolerance is a feature for humans applying stale patches by hand; here it
 * would hide precisely the bugs the loop is meant to find, because a patch
 * with the line numbers one off would still apply.
 *
 * shortcut: strict-only, single-file patches, and hunks must appear in
 * increasing order. Upgrade path if this ever has to replace patch(1):
 * fuzz and offset are a search over candidate positions around old_start,
 * and multi-file support is a loop over "--- / +++" sections.
 *
 * shortcut: output is streamed to stdout as hunks are applied, so a patch
 * that fails on its third hunk leaves the first two hunks' worth of lines
 * already written. The exit status is 2 and the caller must throw the
 * output away. Upgrade path: buffer the result and write it only on
 * success, which costs one copy of the file in memory -- cheap, but it
 * also loses the ability to pipe a patched file larger than RAM, so it is
 * not obviously the better trade.
 */
static long hunkno(const char *p, const char *end, long *out)
{
	long v = 0;
	const char *q = p;

	if (q == end || *q < '0' || *q > '9')
		return -1;
	while (q < end && *q >= '0' && *q <= '9')
		v = v * 10 + (*q++ - '0');
	*out = v;
	return (long)(q - p);
}

/* Parse "@@ -a[,b] +c[,d] @@" into its four numbers. Returns 0 on success. */
static int parse_hunk_header(const struct line *l, long *a, long *b,
                             long *c, long *d)
{
	const char *p = l->p, *end = l->p + l->n;
	long k;

	if (l->n < 4 || memcmp(p, "@@ -", 4) != 0)
		return -1;
	p += 4;
	if ((k = hunkno(p, end, a)) < 0)
		return -1;
	p += k;
	*b = 1;
	if (p < end && *p == ',') {
		p++;
		if ((k = hunkno(p, end, b)) < 0)
			return -1;
		p += k;
	}
	if (p + 2 > end || p[0] != ' ' || p[1] != '+')
		return -1;
	p += 2;
	if ((k = hunkno(p, end, c)) < 0)
		return -1;
	p += k;
	*d = 1;
	if (p < end && *p == ',') {
		p++;
		if ((k = hunkno(p, end, d)) < 0)
			return -1;
		p += k;
	}
	if (p + 2 > end || memcmp(p, " @", 2) != 0)
		return -1;
	return 0;
}

/* Re-print a parsed header in GNU's exact spelling: the count is omitted when
 * it is 1. Used by --hunks, whose whole job is to be checkable: re-printing
 * every @@ line of a real `diff -u` has to reproduce those lines byte for
 * byte, which catches a parser that is quietly dropping or defaulting a
 * field.
 */
static void print_hunk_header(long a, long b, long c, long d)
{
	fputs("@@ -", stdout);
	printf("%ld", a);
	if (b != 1)
		printf(",%ld", b);
	fputs(" +", stdout);
	printf("%ld", c);
	if (d != 1)
		printf(",%ld", d);
	fputs(" @@\n", stdout);
}

/* Apply PATCH to ORIG, writing the result to stdout. Returns 0 on success,
 * 2 if the patch does not fit. */
static int apply(const struct file *pf, const struct file *orig)
{
	long cur = 0, i = 0;
	long lastend = 0;

	while (i < pf->n) {
		long ha, hb, hc, hd, start, oldleft, newleft;

		if (parse_hunk_header(&pf->l[i], &ha, &hb, &hc, &hd)) {
			i++;	/* headers, junk, trailing text: skipped */
			continue;
		}
		i++;
		/* A zero-length old range names the line *before* the
		 * insertion point, so it is already the 0-based index. */
		start = hb == 0 ? ha : ha - 1;
		if (start < lastend || start > orig->n) {
			fprintf(stderr,
			        "%s: hunk at @@ -%ld,%ld wants line %ld, "
			        "out of order or past end of %s (%ld lines)\n",
			        prog, ha, hb, start + 1, orig->name, orig->n);
			return 2;
		}
		while (cur < start)
			putline(&orig->l[cur++]);

		oldleft = hb;
		newleft = hd;
		while ((oldleft > 0 || newleft > 0) && i < pf->n) {
			struct line body;
			char tag;

			tag = pf->l[i].n ? pf->l[i].p[0] : ' ';
			if (tag == '\\') {	/* "\ No newline..." */
				i++;
				continue;
			}
			if (tag != ' ' && tag != '-' && tag != '+')
				break;
			body.p = pf->l[i].p + (pf->l[i].n ? 1 : 0);
			body.n = pf->l[i].n ? pf->l[i].n - 1 : 0;
			/* A patch line always ends with a newline; whether the
			 * *file* line does is said by a following marker. */
			body.nl = 1;
			if (i + 1 < pf->n && pf->l[i + 1].n
			    && pf->l[i + 1].p[0] == '\\')
				body.nl = 0;
			i++;

			if (tag == '+') {
				if (newleft-- <= 0)
					goto botch;
				putline(&body);
				continue;
			}
			if (oldleft-- <= 0)
				goto botch;
			if (tag == ' ' && newleft-- <= 0)
				goto botch;
			if (cur >= orig->n || !lineeq(&orig->l[cur], &body)) {
				fprintf(stderr,
				        "%s: hunk at @@ -%ld,%ld does not "
				        "apply: %s line %ld is not %.*s\n",
				        prog, ha, hb, orig->name, cur + 1,
				        (int)body.n, body.p);
				return 2;
			}
			if (tag == ' ')
				putline(&orig->l[cur]);
			cur++;
		}
		if (oldleft || newleft) {
botch:
			fprintf(stderr, "%s: hunk at @@ -%ld,%ld is "
			        "truncated or miscounted\n", prog, ha, hb);
			return 2;
		}
		lastend = cur;
	}
	while (cur < orig->n)
		putline(&orig->l[cur++]);
	return 0;
}

int main(int argc, char **argv)
{
	struct file a, b;
	struct eqtab t;
	struct ctx c;
	struct script sc;
	long *ea, *eb, *fbuf, *bbuf, diags, i;
	long *un0, *un1, *ri0, *ri1;	/* the undiscarded lines and their map */
	long nun0, nun1, pre = 0, suf = 0, mid0, mid1;
	char *ch0, *ch1;		/* changed flags, indexed by real line */
	int minimal = 0, mode = 0;	/* 0 unified, 1 --stat, 2 --runs */
	int ai = 1;

	for (; ai < argc && argv[ai][0] == '-' && argv[ai][1]; ai++) {
		/* GNU accepts -U3, -U 3 and --unified=3 interchangeably, and
		 * so must we: the suite drives us with whatever string it
		 * drove diff with, and `-U0` failing as an unknown option
		 * while `-U 0` worked was a real trap. */
		if (!strncmp(argv[ai], "-U", 2) && argv[ai][2]) {
			context = strtol(argv[ai] + 2, NULL, 10);
		} else if (!strncmp(argv[ai], "--unified=", 10)) {
			context = strtol(argv[ai] + 10, NULL, 10);
		} else if (!strcmp(argv[ai], "-U") && ai + 1 < argc) {
			context = strtol(argv[++ai], NULL, 10);
		} else if (!strcmp(argv[ai], "--minimal") || !strcmp(argv[ai], "-d"))
			minimal = 1;
		else if (!strcmp(argv[ai], "--stat"))
			mode = 1;
		else if (!strcmp(argv[ai], "--runs"))
			mode = 2;
		else if (!strcmp(argv[ai], "--hunks"))
			mode = 3;
		else if (!strcmp(argv[ai], "-p"))
			mode = 4;
		else {
			fprintf(stderr, "%s: unknown option %s\n", prog,
			        argv[ai]);
			return 2;
		}
		if (context < 0)
			context = 0;
	}
	if (mode == 4) {
		struct file pf, of;

		if (argc - ai != 2) {
			fprintf(stderr, "usage: %s -p PATCH ORIG\n", prog);
			return 2;
		}
		slurp(&pf, argv[ai]);
		slurp(&of, argv[ai + 1]);
		return apply(&pf, &of);
	}

	if (mode == 3) {
		struct file pf;
		long i;

		if (argc - ai != 1) {
			fprintf(stderr, "usage: %s --hunks PATCH\n", prog);
			return 2;
		}
		slurp(&pf, argv[ai]);
		for (i = 0; i < pf.n; i++) {
			long ha, hb, hc, hd;
			if (parse_hunk_header(&pf.l[i], &ha, &hb, &hc, &hd))
				continue;
			print_hunk_header(ha, hb, hc, hd);
		}
		return 0;
	}

	if (argc - ai != 2) {
		fprintf(stderr,
		        "usage: %s [-U N|-UN|--unified=N] [--minimal] A B\n"
		        "       %s --stat A B      line/class/changed counts\n"
		        "       %s --runs A B      the raw edit script\n"
		        "       %s --hunks PATCH   re-print parsed @@ headers\n"
		        "       %s -p PATCH ORIG   apply PATCH, result to "
		        "stdout\n", prog, prog, prog, prog, prog);
		return 2;
	}
	slurp(&a, argv[ai]);
	slurp(&b, argv[ai + 1]);

	eq_init(&t, a.n + b.n);
	ea = classify(&t, &a);
	eb = classify(&t, &b);

	/* One slack slot at the front of each changed array: the boundary
	 * normaliser reads index -1. */
	ch0 = (char *)xcalloc((size_t)a.n + 2, 1) + 1;
	ch1 = (char *)xcalloc((size_t)b.n + 2, 1) + 1;

	/* The common prefix and suffix, computed the way GNU's
	 * find_identical_ends does: prefix first, then suffix over what is
	 * left, so the two cannot overlap. Then `keep` lines of each are
	 * handed back, because GNU does that too: diff.c raises horizon_lines
	 * to the context width with the comment "so that shift_boundaries has
	 * more freedom to shift the first and last hunks", and io.c spends
	 * that horizon putting lines back into the middle at both ends. It is
	 * why `diff -U0` and `diff -u` can report the same insertion at
	 * different line numbers.
	 *
	 * Everything after this point -- discarding, the comparison, the
	 * normalisation -- happens inside [pre, n - suf) and nowhere else. */
	{
		long keep = mode == 0 ? context : 0;
		while (pre < a.n && pre < b.n && ea[pre] == eb[pre])
			pre++;
		while (suf < a.n - pre && suf < b.n - pre
		       && ea[a.n - 1 - suf] == eb[b.n - 1 - suf])
			suf++;
		pre -= pre < keep ? pre : keep;
		suf -= suf < keep ? suf : keep;
	}
	mid0 = a.n - pre - suf;
	mid1 = b.n - pre - suf;

	/* Throw away the confusing lines, leaving the sequences the
	 * comparison will actually see. With --minimal nothing is discarded,
	 * which is the whole of what that option does here: GNU spells the
	 * same thing `if (minimal || discards[i] == 0)` and runs the marking
	 * anyway. */
	un0 = xmalloc(sizeof *un0 * (size_t)(mid0 > 0 ? mid0 : 1));
	un1 = xmalloc(sizeof *un1 * (size_t)(mid1 > 0 ? mid1 : 1));
	ri0 = xmalloc(sizeof *ri0 * (size_t)(mid0 > 0 ? mid0 : 1));
	ri1 = xmalloc(sizeof *ri1 * (size_t)(mid1 > 0 ? mid1 : 1));
	{
		char *d0 = xcalloc((size_t)(mid0 + mid1), 1);
		char *d1 = d0 + mid0;

		if (!minimal) {
			/* equiv_count: how many lines of each file fall in
			 * each class, counted over the middle only. */
			long *n0 = xcalloc((size_t)t.nent * 2, sizeof *n0);
			long *n1 = n0 + t.nent;

			for (i = 0; i < mid0; i++)
				n0[ea[pre + i]]++;
			for (i = 0; i < mid1; i++)
				n1[eb[pre + i]]++;
			mark_discards(d0, ea + pre, mid0, n1);
			mark_discards(d1, eb + pre, mid1, n0);
			cancel_provisional(d0, mid0);
			cancel_provisional(d1, mid1);
			free(n0);
		}
		nun0 = apply_discards(d0, ea + pre, mid0, un0, ri0, ch0 + pre);
		nun1 = apply_discards(d1, eb + pre, mid1, un1, ri1, ch1 + pre);
		free(d0);
	}
	un0 = trim(un0, nun0);
	un1 = trim(un1, nun1);
	ri0 = trim(ri0, nun0);
	ri1 = trim(ri1, nun1);

	diags = nun0 + nun1 + 3;
	fbuf = xmalloc(sizeof *fbuf * (size_t)diags);
	bbuf = xmalloc(sizeof *bbuf * (size_t)diags);
	c.ch0 = ch0 + pre;
	c.ch1 = ch1 + pre;
	c.ri0 = ri0;
	c.ri1 = ri1;
	c.xv = un0;
	c.yv = un1;
	c.fd = fbuf + nun1 + 1;
	c.bd = bbuf + nun1 + 1;
	/* GNU sets this from speed_large_files, which only -H turns on, so
	 * the default is off and `!minimal` was wrong. See diag(). */
	c.heuristic = 0;
	c.too_expensive = 1;
	for (i = diags; i != 0; i >>= 2)
		c.too_expensive <<= 1;
	if (c.too_expensive < 4096)
		c.too_expensive = 4096;

	compareseq(0, nun0, 0, nun1, minimal, &c);

	shift_one(ch0 + pre, mid0, ea + pre, ch1 + pre);
	shift_one(ch1 + pre, mid1, eb + pre, ch0 + pre);

	if (mode == 1) {
		long n0 = 0, n1 = 0;
		for (i = 0; i < a.n; i++)
			n0 += ch0[i];
		for (i = 0; i < b.n; i++)
			n1 += ch1[i];
		printf("lines %ld %ld classes %ld changed %ld %ld\n",
		       a.n, b.n, t.nent, n0, n1);
		return 0;
	}

	build_script(&sc, ch0, a.n, ch1, b.n);

	if (mode == 2) {
		for (i = 0; i < sc.n; i++) {
			long k;
			fputs("@\n", stdout);
			for (k = 0; k < sc.r[i].deleted; k++) {
				printf("-%ld ", sc.r[i].line0 + k + 1);
				putline(&a.l[sc.r[i].line0 + k]);
			}
			for (k = 0; k < sc.r[i].inserted; k++) {
				printf("+%ld ", sc.r[i].line1 + k + 1);
				putline(&b.l[sc.r[i].line1 + k]);
			}
		}
		return sc.n ? 1 : 0;
	}
	return print_unified(&a, &b, &sc);
}
