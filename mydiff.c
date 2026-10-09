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

int main(int argc, char **argv)
{
	struct file a, b;
	struct eqtab t;
	long *ea, *eb;

	if (argc != 4 || strcmp(argv[1], "--stat") != 0) {
		fprintf(stderr, "usage: %s --stat A B\n", prog);
		return 2;
	}
	slurp(&a, argv[2]);
	slurp(&b, argv[3]);
	eq_init(&t, a.n + b.n);
	ea = classify(&t, &a);
	eb = classify(&t, &b);
	{
		struct ctx c;
		long diags = a.n + b.n + 3, i, n0 = 0, n1 = 0;
		long *fbuf = xmalloc(sizeof *fbuf * (size_t)diags);
		long *bbuf = xmalloc(sizeof *bbuf * (size_t)diags);

		/* One slack slot at the front of each changed array: the
		 * boundary normaliser added later reads index -1. */
		c.ch0 = (char *)xcalloc((size_t)a.n + 2, 1) + 1;
		c.ch1 = (char *)xcalloc((size_t)b.n + 2, 1) + 1;
		c.xv = ea;
		c.yv = eb;
		c.fd = fbuf + b.n + 1;
		c.bd = bbuf + b.n + 1;
		c.heuristic = 1;
		c.too_expensive = 1;
		for (i = a.n + b.n; i != 0; i >>= 2)
			c.too_expensive <<= 1;
		if (c.too_expensive < 4096)
			c.too_expensive = 4096;

		compareseq(0, a.n, 0, b.n, 0, &c);
		for (i = 0; i < a.n; i++)
			n0 += c.ch0[i];
		for (i = 0; i < b.n; i++)
			n1 += c.ch1[i];
		printf("lines %ld %ld classes %ld changed %ld %ld\n",
		       a.n, b.n, t.nent, n0, n1);
	}
	return 0;
}
