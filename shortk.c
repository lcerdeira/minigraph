#include "mgpriv.h"
#include "ksort.h"
#include "kavl.h"
#include "khash.h"
KHASH_DECLARE(mg_idx, uint64_t, uint64_t)

typedef struct sp_node_s {
	uint64_t di; // dist<<32 | unique_id
	uint32_t v;
	int32_t pre;
	uint64_t l:8, kmer:56;
	KAVL_HEAD(struct sp_node_s) head;
} sp_node_t, *sp_node_p;

#define sp_node_cmp(a, b) (((a)->di > (b)->di) - ((a)->di < (b)->di))
KAVL_INIT(sp, sp_node_t, head, sp_node_cmp)

#define sp_node_lt(a, b) ((a)->di < (b)->di)
KSORT_INIT(sp, sp_node_p, sp_node_lt)

typedef struct {
	int32_t k;
	sp_node_t *p[GFA_MAX_SHORT_K]; // this forms a max-heap
} sp_topk_t;

KHASH_MAP_INIT_INT(sp, sp_topk_t)
KHASH_MAP_INIT_INT(sp2, uint64_t)

static inline sp_node_t *gen_sp_node(void *km, const gfa_t *g, uint32_t v, int32_t d, int32_t id)
{
	sp_node_t *p;
	KMALLOC(km, p, 1);
	p->v = v, p->di = (uint64_t)d<<32 | id, p->pre = -1;
	p->l = 0, p->kmer = 0;
	return p;
}

#define MG_SHORT_KK 19
typedef khash_t(mg_idx) idxhash_t;

mg_pathv_t *mg_shortest_k(void *km0, const gfa_t *g, uint32_t src, int32_t n_dst, mg_path_dst_t *dst, int32_t max_dist, int32_t max_k, int32_t ql, const char *qs, int32_t *n_pathv)
{
	sp_node_t *p, *root = 0, **out;
	sp_topk_t *q;
	khash_t(sp) *h;
	khash_t(sp2) *h2;
	void *km;
	khint_t k;
	int absent;
	int32_t i, j, n_finished, n_found, n_seeds;
	uint32_t id, n_out, m_out;
	int8_t *dst_finish;
	mg_pathv_t *ret = 0;
	uint64_t *dst_group, *seeds;
	void *h_seeds = 0;

	if (n_pathv) *n_pathv = 0;
	if (n_dst <= 0) return 0;
	for (i = 0; i < n_dst; ++i)
		dst[i].dist = -1, dst[i].n_path = 0, dst[i].path_end = -1;
	if (max_k > GFA_MAX_SHORT_K) max_k = GFA_MAX_SHORT_K;
	km = km_init2(km0, 0x10000);

	if (1 && ql > 0 && qs) { // build the seed hash table for the query
		mg128_v mini = {0,0,0};
		mg_sketch(km, qs, ql, 10, MG_SHORT_KK, 0, &mini);
		h_seeds = mg_idx_a2h(km, mini.n, mini.a, 0, &seeds, &n_seeds);
		kfree(km, mini.a);
	}

	KCALLOC(km, dst_finish, n_dst);
	KMALLOC(km, dst_group, n_dst);
	for (i = 0; i < n_dst; ++i) // multiple dst[] may have the same dst[].v. We need to group them first.
		dst_group[i] = (uint64_t)dst[i].v<<32 | i;
	radix_sort_64(dst_group, dst_group + n_dst);

	h2 = kh_init2(sp2, km); // this hash table keeps all destinations
	kh_resize(sp2, h2, n_dst * 2);
	for (i = 1, j = 0; i <= n_dst; ++i) {
		if (i == n_dst || dst_group[i]>>32 != dst_group[j]>>32) {
			k = kh_put(sp2, h2, dst_group[j]>>32, &absent);
			kh_val(h2, k) = (uint64_t)j << 32 | (i - j);
			assert(absent);
			j = i;
		}
	}

	h = kh_init2(sp, km); // this hash table keeps visited vertices
	kh_resize(sp, h, 16);
	m_out = 16, n_out = 0;
	KMALLOC(km, out, m_out);

	id = 0;
	p = gen_sp_node(km, g, src, 0, id++);
	kavl_insert(sp, &root, p, 0);
	k = kh_put(sp, h, src, &absent);
	q = &kh_val(h, k);
	q->k = 1, q->p[0] = p;

	n_finished = 0;
	while (kavl_size(head, root) > 0) {
		int32_t i, nv;
		gfa_arc_t *av;
		sp_node_t *r;

		r = kavl_erase_first(sp, &root); // take out the closest vertex in the heap (as a binary tree)
		//fprintf(stderr, "XX\t%d\t%d\t%d\t%c%s[%d]\t%d\n", n_out, kavl_size(head, root), n_finished, "><"[(r->v&1)^1], g->seg[r->v>>1].name, r->v, (int32_t)(r->di>>32));
		if (n_out == m_out) KEXPAND(km, out, m_out);
		r->di = r->di>>32<<32 | n_out; // lower 32 bits now for position in the out[] array
		out[n_out++] = r;

		k = kh_get(sp2, h2, r->v);
		if (k != kh_end(h2)) { // we have reached one dst vertex
			int32_t finished = 0, j;
			int32_t off = kh_val(h2, k) >> 32, cnt = (int32_t)kh_val(h2, k);
			for (j = 0; j < cnt; ++j) {
				mg_path_dst_t *t = &dst[(int32_t)dst_group[off + j]];
				if (t->n_path == 0) { // TODO: when there is only one path, but the distance is smaller than target_dist, the dst won't be finished
					t->path_end = n_out - 1;
				} else if (t->target_dist >= 0) { // we have a target distance; choose the closest
					int32_t d0 = out[t->path_end]->di >> 32, d1 = r->di >> 32;
					d0 = d0 > t->target_dist? d0 - t->target_dist : t->target_dist - d0;
					d1 = d1 > t->target_dist? d1 - t->target_dist : t->target_dist - d1;
					if (d1 < d0) t->path_end = n_out - 1;
				}
				if (t->target_dist >= 0 && r->di>>32 >= t->target_dist) finished = 1;
				++t->n_path;
				if (t->n_path >= max_k) finished = 1;
				if (dst_finish[j] == 0 && finished)
					dst_finish[j] = 1, ++n_finished;
				if (n_finished == n_dst) break;
			}
		}

		nv = gfa_arc_n(g, r->v);
		av = gfa_arc_a(g, r->v);
		for (i = 0; i < nv; ++i) { // visit all neighbors
			gfa_arc_t *ai = &av[i];
			int32_t d = (r->di>>32) + (uint32_t)ai->v_lv;
			if (d > max_dist) continue; // don't probe vertices too far away
			k = kh_put(sp, h, ai->w, &absent);
			q = &kh_val(h, k);
			if (absent) q->k = 0;
			if (q->k < max_k) { // enough room: add to the heap
				p = gen_sp_node(km, g, ai->w, d, id++);
				p->pre = n_out - 1;
				kavl_insert(sp, &root, p, 0);
				q->p[q->k++] = p;
				ks_heapup_sp(q->k, q->p);
			} else if (q->p[0]->di>>32 > d) { // shorter than the longest path so far: replace the longest (TODO: this block is not well tested)
				p = kavl_erase(sp, &root, q->p[0], 0);
				if (p) {
					p->di = (uint64_t)d<<32 | (id++);
					p->pre = n_out - 1;
					kavl_insert(sp, &root, p, 0);
					ks_heapdown_sp(0, q->k, q->p);
				} else {
					fprintf(stderr, "Warning: logical bug in gfa_shortest_k(): q->k=%d,q->p[0]->{d,i}={%d,%d},d=%d,src=%u,max_dist=%d,n_dst=%d\n", q->k, (int32_t)(q->p[0]->di>>32), (int32_t)q->p[0]->di, d, src, max_dist, n_dst);
					km_destroy(km);
					return 0;
				}
			} // else: the path is longer than all the existing paths ended at ai->w
		}
	}

	n_found = 0;
	for (i = 0; i < n_dst; ++i) {
		mg_path_dst_t *t = &dst[i];
		t->dist = t->n_path > 0? out[t->path_end]->di>>32 : -1;
		if (t->n_path) ++n_found;
	}

	if (n_found > 0 && n_pathv) { // then generate the backtrack array
		int32_t n, *trans;

		kh_destroy(sp, h);
		kfree(km, dst_finish);

		KCALLOC(km, trans, n_out); // used to squeeze unused elements in out[]
		for (i = 0; i < n_dst; ++i) { // mark dst vertices with a target distance
			mg_path_dst_t *t = &dst[i];
			if (t->n_path > 0 && t->target_dist >= 0)
				trans[(int32_t)out[t->path_end]->di] = 1;
		}
		for (i = 0; i < n_out; ++i) { // mark dst vertices without a target distance
			k = kh_get(sp2, h2, out[i]->v);
			if (k != kh_end(h2)) { // TODO: check if this is correct!
				int32_t off = kh_val(h2, k)>>32, cnt = (int32_t)kh_val(h2, k);
				for (j = off; j < off + cnt; ++j)
					if (dst[j].target_dist < 0)
						trans[i] = 1;
			}
		}
		for (i = n_out - 1; i >= 0; --i) // mark all predecessors
			if (trans[i] && out[i]->pre >= 0)
				trans[out[i]->pre] = 1;
		for (i = n = 0; i < n_out; ++i) // generate coordinate translations
			if (trans[i]) trans[i] = n++;
			else trans[i] = -1;

		*n_pathv = n;
		KMALLOC(km0, ret, n);
		for (i = 0; i < n_out; ++i) { // generate the backtrack array
			mg_pathv_t *p;
			if (trans[i] < 0) continue;
			p = &ret[trans[i]];
			p->v = out[i]->v, p->d = out[i]->di >> 32;
			p->pre = out[i]->pre < 0? out[i]->pre : trans[out[i]->pre];
		}
		for (i = 0; i < n_dst; ++i) // translate "path_end"
			if (dst[i].path_end >= 0)
				dst[i].path_end = trans[dst[i].path_end];
	}

	km_destroy(km);
	return ret;
}

void mg_sub_print_path(FILE *fp, const gfa_t *g, int32_t n, mg_pathv_t *path)
{
	int32_t i;
	for (i = 0; i < n; ++i) {
		mg_pathv_t *p = &path[i];
		fprintf(fp, "[%d]\t%d\t%s\t%d\t%d\n", i, p->v, g->seg[p->v>>1].name, p->d, p->pre);
	}
}