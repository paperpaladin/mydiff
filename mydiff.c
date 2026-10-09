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
	char *ch0, *ch1;	/* output: per-line changed flags */
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

		/* shortcut: the two heuristics below are GNU diff's, and they
		 * are the reason `diff` without --minimal can return a
		 * non-minimal script. They are here only because matching
		 * GNU's default output byte for byte is the project's bar.
		 * Both are off under --minimal. Upgrade path, if the exactness
		 * ever stops mattering: delete them and keep --minimal
		 * behaviour always, which costs time on large files with many
		 * changes and nothing else. */
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
			c->ch1[yoff++] = 1;
	} else if (yoff == ylim) {
		while (xoff < xlim)
			c->ch0[xoff++] = 1;
	} else {
		struct part part;
		diag(xoff, xlim, yoff, ylim, find_minimal, &part, c);
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
 * only if the file had one there. */
static void putline(const struct line *l)
{
	fwrite(l->p, 1, l->n, stdout);
	if (l->nl)
		putchar('\n');
}

int main(int argc, char **argv)
{
	struct file a, b;
	struct eqtab t;
	struct ctx c;
	struct script sc;
	long *ea, *eb, *fbuf, *bbuf, diags, i;
	int minimal = 0, mode = 0;	/* 0 unified, 1 --stat, 2 --runs */
	int ai = 1;

	for (; ai < argc && argv[ai][0] == '-' && argv[ai][1]; ai++) {
		if (!strcmp(argv[ai], "--minimal") || !strcmp(argv[ai], "-d"))
			minimal = 1;
		else if (!strcmp(argv[ai], "--stat"))
			mode = 1;
		else if (!strcmp(argv[ai], "--runs"))
			mode = 2;
		else {
			fprintf(stderr, "%s: unknown option %s\n", prog,
			        argv[ai]);
			return 2;
		}
	}
	if (argc - ai != 2) {
		fprintf(stderr,
		        "usage: %s [--minimal] A B\n"
		        "       %s --stat A B      line/class/changed counts\n"
		        "       %s --runs A B      the raw edit script\n",
		        prog, prog, prog);
		return 2;
	}
	slurp(&a, argv[ai]);
	slurp(&b, argv[ai + 1]);

	eq_init(&t, a.n + b.n);
	ea = classify(&t, &a);
	eb = classify(&t, &b);

	diags = a.n + b.n + 3;
	fbuf = xmalloc(sizeof *fbuf * (size_t)diags);
	bbuf = xmalloc(sizeof *bbuf * (size_t)diags);
	/* One slack slot at the front of each changed array: the boundary
	 * normaliser reads index -1. */
	c.ch0 = (char *)xcalloc((size_t)a.n + 2, 1) + 1;
	c.ch1 = (char *)xcalloc((size_t)b.n + 2, 1) + 1;
	c.xv = ea;
	c.yv = eb;
	c.fd = fbuf + b.n + 1;
	c.bd = bbuf + b.n + 1;
	c.heuristic = !minimal;
	c.too_expensive = 1;
	for (i = a.n + b.n; i != 0; i >>= 2)
		c.too_expensive <<= 1;
	if (c.too_expensive < 4096)
		c.too_expensive = 4096;

	compareseq(0, a.n, 0, b.n, minimal, &c);

	/* The common prefix and suffix, computed the way GNU's
	 * find_identical_ends does: prefix first, then suffix over what is
	 * left, so the two cannot overlap. Normalisation is confined to the
	 * middle. */
	{
		long pre = 0, suf = 0, lim;
		while (pre < a.n && pre < b.n && ea[pre] == eb[pre])
			pre++;
		while (suf < a.n - pre && suf < b.n - pre
		       && ea[a.n - 1 - suf] == eb[b.n - 1 - suf])
			suf++;
		lim = a.n - pre - suf;
		shift_one(c.ch0 + pre, lim, ea + pre, c.ch1 + pre);
		lim = b.n - pre - suf;
		shift_one(c.ch1 + pre, lim, eb + pre, c.ch0 + pre);
	}

	if (mode == 1) {
		long n0 = 0, n1 = 0;
		for (i = 0; i < a.n; i++)
			n0 += c.ch0[i];
		for (i = 0; i < b.n; i++)
			n1 += c.ch1[i];
		printf("lines %ld %ld classes %ld changed %ld %ld\n",
		       a.n, b.n, t.nent, n0, n1);
		return 0;
	}

	build_script(&sc, c.ch0, a.n, c.ch1, b.n);

	if (mode == 2) {
		for (i = 0; i < sc.n; i++) {
			long k;
			fputs("@\n", stdout);
			for (k = 0; k < sc.r[i].deleted; k++) {
				putchar('-');
				putline(&a.l[sc.r[i].line0 + k]);
			}
			for (k = 0; k < sc.r[i].inserted; k++) {
				putchar('+');
				putline(&b.l[sc.r[i].line1 + k]);
			}
		}
		return sc.n ? 1 : 0;
	}
	return sc.n ? 1 : 0;
}
