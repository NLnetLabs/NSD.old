/*
 * test udbradtree -- generic radix tree for binary strings in udb.
 *
 * Copyright (c) 2010, NLnet Labs.  See LICENSE for license.
 */
#include "config.h"
#include "tpkg/cutest/cutest.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include "udbradtree.h"

#include <stdio.h>
#include <ctype.h>

/* from cutest_udb */
void check_udb_structure(CuTest* t, udb_base* udb);
void check_udb(void);

#define RADTREE(ptr) ((struct udb_radtree_d*)UDB_PTR(ptr))
#define RADARRAY(ptr) ((struct udb_radarray_d*)UDB_PTR(ptr))
#define TESTSTR(ptr) ((struct teststr*)UDB_PTR(ptr))
#ifndef RAND_MAX
#define RAND_MAX 2147483647
#endif

/* a chunk type not in use by radtree or the builtin types */
#define TESTSTR_CHUNK_TYPE 253

/** get the lookup array for a node */
static struct udb_radarray_d* lookup(udb_ptr* n)
{
	return (struct udb_radarray_d*)UDB_REL(*n->base,
		RADNODE(n)->lookup.data);
}

/** get a string in the lookup array */
static uint8_t* lookup_string(udb_ptr* n, unsigned i)
{
	return ((uint8_t*)&(lookup(n)->array[lookup(n)->capacity]))+
		i*lookup(n)->str_cap;
}


static void udb_radtree_1(CuTest* tc);

CuSuite* reg_cutest_udb_radtree(void)
{
	CuSuite* suite = CuSuiteNew();
	SUITE_ADD_TEST(suite, udb_radtree_1);
	return suite;
}

static CuTest* tc = NULL;
/* local verbosity */
static int verb = 0;
static udb_base* base_A = NULL;

/*********** test code ************/
/** test data for radix tree tests */
struct teststr {
	udb_rel_ptr mynode;
	udb_radstrlen_type mylen;
	uint8_t mystr[256];
};

/** max strlen in an array */
static udb_radstrlen_type udb_radarray_max_len(udb_ptr* n)
{
	unsigned i;
	udb_radstrlen_type maxlen = 0;
	for(i=0; i<lookup(n)->len; i++) {
		if(lookup(n)->array[i].node.data &&
			lookup(n)->array[i].len > maxlen)
			maxlen = lookup(n)->array[i].len;
	}
	return maxlen;
}

/** check invariants and count number of elems */
static size_t test_check_invariants(udb_base* udb, udb_ptr* n)
{
	size_t num = 0;
	udb_ptr s;
	if(udb_ptr_is_null(n)) return 0;
	if(RADNODE(n)->elem.data) num++;
	CuAssert(tc, "invariant len<=cap",
		lookup(n)->len <= lookup(n)->capacity);
	CuAssert(tc, "invariant cap<=256", lookup(n)->capacity <= 256);
	CuAssert(tc, "invariant offset",
		((int)RADNODE(n)->offset) + ((int)lookup(n)->len) <= 256);
	if(lookup(n)->len == 0) {
		CuAssert(tc, "invariant empty cap", lookup(n)->capacity == 0);
		CuAssert(tc, "invariant empty strcap", lookup(n)->str_cap == 0);
		CuAssert(tc, "invariant empty len", lookup(n)->len == 0);
		CuAssert(tc, "invariant empty offset", RADNODE(n)->offset == 0);
	} else {
		unsigned idx;
		udb_radstrlen_type maxlen;
		CuAssert(tc, "invariant nonempty cap",
			lookup(n)->capacity != 0);
		CuAssert(tc, "invariant len>cap/2",
			lookup(n)->len >= lookup(n)->capacity/2);
		for(idx=0; idx<lookup(n)->len; idx++) {
			struct udb_radsel_d* r = &lookup(n)->array[idx];
			if(r->node.data == 0) {
				CuAssert(tc, "empty node", r->len == 0);
				/* there may be unused space in the
				 * string, it is undefined */
			} else {
				/* r->len == 0 is an empty string */
				CuAssert(tc, "strcap",
					r->len <= lookup(n)->str_cap);
				udb_ptr_new(&s, udb, &r->node);
				CuAssert(tc, "invariant parent",
					n->data == RADNODE(&s)->parent.data);
				CuAssert(tc, "invariant pidx",
					RADNODE(&s)->pidx == idx);
				num += test_check_invariants(udb, &s);
				udb_ptr_unlink(&s, udb);
			}
		}
		maxlen = udb_radarray_max_len(n);
		CuAssert(tc, "maxlen", maxlen <= lookup(n)->str_cap);
		if(maxlen != lookup(n)->str_cap) {
			CuAssert(tc, "maxlen", maxlen >= lookup(n)->str_cap/2);
		}
	}
	return num;
}

/** find all elems in the list and check that the name as indicated by the
 * lookup structure matches the one in the element */
static void
test_check_list_keys(udb_base* udb, udb_ptr* n, struct teststr** all,
	size_t* all_idx, size_t all_num, uint8_t* fullkey,
	udb_radstrlen_type fullkey_len, udb_radstrlen_type fullkey_max)
{
	unsigned idx;
	if(udb_ptr_is_null(n)) return;
	if(RADNODE(n)->elem.data) {
		/* check this elements key */
		udb_ptr t;
		udb_ptr_new(&t, udb, &RADNODE(n)->elem);
		CuAssert(tc, "testkey node", TESTSTR(&t)->mynode.data==n->data);
		CuAssert(tc, "testkey elem", t.data== RADNODE(n)->elem.data);
		CuAssert(tc, "testkey len", TESTSTR(&t)->mylen == fullkey_len);
		CuAssert(tc, "testkey val", memcmp(TESTSTR(&t)->mystr, fullkey, fullkey_len) == 0);
		/* add to all list */
		CuAssert(tc, "testkey list",  (*all_idx) < all_num);
		all[ (*all_idx)++ ] = TESTSTR(&t);
		udb_ptr_unlink(&t, udb);
	}
	for(idx=0; idx<lookup(n)->len; idx++) {
		udb_ptr s;
		struct udb_radsel_d* r = &lookup(n)->array[idx];
		udb_radstrlen_type newlen = fullkey_len;
		if(!r->node.data)
			continue;
		/* lengthen fullkey with the character and r->str */
		CuAssert(tc, "testkey len", newlen+1 < fullkey_max);
		fullkey[newlen++] = idx + RADNODE(n)->offset;
		if(r->len != 0) {
			CuAssert(tc, "testkey len", newlen+r->len < fullkey_max);
			memmove(fullkey+newlen, lookup_string(n, idx), r->len);
			newlen += r->len;
		}
		udb_ptr_new(&s, udb, &r->node);
		test_check_list_keys(udb, &s, all, all_idx, all_num, fullkey,
			newlen, fullkey_max);
		udb_ptr_unlink(&s, udb);
	}
}

/** compare byte strings like the tree does */
static int bstr_cmp(uint8_t* x, udb_radstrlen_type lenx, uint8_t* y,
	udb_radstrlen_type leny)
{
	size_t m = (lenx<leny)?lenx:leny;
	if(m != 0 && memcmp(x, y, m) != 0)
		return memcmp(x, y, m);
	if(lenx < leny)
		return -1;
	else if(lenx > leny)
		return +1;
	return 0;
}

/** compare for qsort */
int udb_test_sort_cmp(const void *a, const void *b)
{
	struct teststr* x = *(struct teststr**)a;
	struct teststr* y = *(struct teststr**)b;
	return bstr_cmp(x->mystr, x->mylen, y->mystr, y->mylen);
}

/** check walk functions */
static void test_check_walk(udb_base* udb, udb_ptr* rt, struct teststr** all,
	size_t num)
{
	udb_ptr n;
	unsigned idx;

	/* check _first */
	udb_radix_first(udb, rt, &n);
	if(num == 0) {
		CuAssert(tc, "walk first", n.data == 0);
	} else {
		CuAssert(tc, "walk first", n.data == all[0]->mynode.data);
	}
	udb_ptr_unlink(&n, udb);

	/* check _last */
	udb_radix_last(udb, rt, &n);
	if(num == 0) {
		CuAssert(tc, "walk last", n.data == 0);
	} else {
		CuAssert(tc, "walk last", n.data == all[num-1]->mynode.data);
	}
	udb_ptr_unlink(&n, udb);

	/* check _next */
	for(idx = 0; idx < num; idx++) {
		udb_ptr_init(&n, udb);
		udb_ptr_set(&n, udb, all[idx]->mynode.data);
		udb_radix_next(udb, &n);
		if(idx == num-1) {
			CuAssert(tc, "radix_next", n.data == 0);
		} else {
			CuAssert(tc, "radix_next",
				n.data == all[idx+1]->mynode.data);
		}
		udb_ptr_unlink(&n, udb);
	}

	/* check _prev */
	for(idx = 0; idx < num; idx++) {
		udb_ptr_init(&n, udb);
		udb_ptr_set(&n, udb, all[idx]->mynode.data);
		udb_radix_prev(udb, &n);
		if(idx == 0) {
			CuAssert(tc, "radix_prev", n.data == 0);
		} else {
			CuAssert(tc, "radix_prev",
				n.data == all[idx-1]->mynode.data);
		}
		udb_ptr_unlink(&n, udb);
	}
}

/** check search function */
static void test_check_search(udb_ptr* rt, struct teststr** all, size_t num)
{
	udb_void n;
	unsigned idx;
	for(idx = 0; idx < num; idx++) {
		n = udb_radix_search(rt, all[idx]->mystr, all[idx]->mylen);
		CuAssert(tc, "radix_search", n == all[idx]->mynode.data);
	}
}

/** check closest match function for exact matches */
static void test_check_closest_match_exact(udb_base* udb, udb_ptr* rt,
	struct teststr** all, size_t num)
{
	udb_ptr n;
	unsigned idx;
	udb_ptr_init(&n, udb);
	for(idx = 0; idx < num; idx++) {
		udb_ptr_zero(&n, udb);
		if(udb_radix_find_less_equal(udb, rt, all[idx]->mystr,
			all[idx]->mylen, &n)) {
			/* check if exact match is correct */
			CuAssert(tc, "find_le exact match",
				n.data == all[idx]->mynode.data);
		} else {
			/* should have returned true: exact match */
			CuAssert(tc, "find_le", 0);
		}
	}
	udb_ptr_unlink(&n, udb);
}

/** get a random value */
static unsigned
get_ran_val(unsigned max)
{
	unsigned r = (random() & RAND_MAX);
	double ret = ((double)r * (double)max) / (1.0 + RAND_MAX);
	return (unsigned)ret;
}

/** generate random string and length */
static void
gen_ran_str_len(uint8_t* buf, udb_radstrlen_type* len, udb_radstrlen_type max)
{
	udb_radstrlen_type i;
	*len = get_ran_val(5);
	CuAssert(tc, "ranstrlen", *len < max);
	buf[*len] = 0; /* zero terminate for easy debug */
	for(i=0; i< *len; i++) {
		/*buf[i] = get_ran_val(256); */
		buf[i] = 'a' + get_ran_val(26);
	}
}

/** check closest match function for inexact matches */
static void test_check_closest_match_inexact(udb_base* udb, udb_ptr* rt)
{
	uint8_t buf[1024];
	udb_radstrlen_type len;
	udb_ptr n, t;
	int i = 0, num=1000;
	udb_ptr_init(&n, udb);
	udb_ptr_init(&t, udb);
	/* what strings to try out? random */
	/* how to check result? use prev and next (they work checked before)*/
	for(i=0; i<num; i++) {
		/* generate random string */
		gen_ran_str_len(buf, &len, sizeof(buf));
		udb_ptr_zero(&t, udb);
		udb_ptr_zero(&n, udb);
		if(udb_radix_find_less_equal(udb, rt, buf, len, &n)) {
			CuAssert(tc, "find_le", n.data != 0);
			CuAssert(tc, "find_le", RADNODE(&n)->elem.data != 0);
			/* check exact match */
			udb_ptr_set_rptr(&t, udb, &RADNODE(&n)->elem);
			CuAssert(tc, "find_le", TESTSTR(&t)->mylen == len);
			CuAssert(tc, "find_le", memcmp(TESTSTR(&t)->mystr,
				buf, len) == 0);
		} else {
			/* check inexact match */
			if(n.data == 0) {
				/* no elements in rt or before first item */
				if(RADTREE(rt)->count != 0) {
					udb_radix_first(udb, rt, &n);
					udb_ptr_set_rptr(&t, udb,
						&RADNODE(&n)->elem);
					CuAssert(tc, "find_le", bstr_cmp(
					  buf, len, TESTSTR(&t)->mystr,
					  TESTSTR(&t)->mylen) < 0);
				}
			} else {
				udb_ptr nx;
				CuAssert(tc, "ptrforsmallerhaselem",
					RADNODE(&n)->elem.data != 0);
				/* n is before the item */
				udb_ptr_set_rptr(&t, udb, &RADNODE(&n)->elem);
				CuAssert(tc, "find_le", bstr_cmp(
					TESTSTR(&t)->mystr, TESTSTR(&t)->mylen,
					buf, len) < 0);
				/* the next item is NULL or after it */
				udb_ptr_init(&nx, udb);
				udb_ptr_set_ptr(&nx, udb, &n);
				udb_radix_next(udb, &nx);
				if(nx.data) {
					udb_ptr_set_rptr(&t, udb,
						&RADNODE(&nx)->elem);
					CuAssert(tc, "find_le", bstr_cmp(
						TESTSTR(&t)->mystr,
						TESTSTR(&t)->mylen,
						buf, len) > 0);
				}
				udb_ptr_unlink(&nx, udb);
			}
		}
	}
	udb_ptr_unlink(&t, udb);
	udb_ptr_unlink(&n, udb);
}

/** calc size of node and below */
static uint64_t udb_radtree_size_node(udb_base* udb, udb_ptr* n)
{
	uint64_t s;
	unsigned i;
	if(n->data == 0)
		return 0;
	s = sizeof(struct udb_radnode_d) + size_of_lookup_ext(n);
	for(i=0; i<lookup(n)->len; i++)  {
		udb_ptr sub;
		udb_ptr_new(&sub, udb, &lookup(n)->array[i].node);
		s += udb_radtree_size_node(udb, &sub);
		udb_ptr_unlink(&sub, udb);
	}
	return s;
}

/** calculate the allocated(data) size of the radtree (base, nodes, arrays) */
static uint64_t udb_radtree_size(udb_base* udb, udb_ptr* rt)
{
	uint64_t s;
	udb_ptr n;
	udb_ptr_new(&n, udb, &RADTREE(rt)->root);
	s = sizeof(struct udb_radtree_d) + udb_radtree_size_node(udb, &n);
	udb_ptr_unlink(&n, udb);
	return s;
}

/** perform lots of checks on the test tree */
static void test_checks(udb_base* udb, udb_ptr* rt)
{
	struct teststr* all[10240];
	size_t i=0;
	uint8_t fullkey_buf[1024];
	udb_ptr root;
	uint64_t mem;

	/* tree structure invariants */
	size_t num;
	udb_ptr_new(&root, udb, &RADTREE(rt)->root);
	num = test_check_invariants(udb, &root);
	CuAssert(tc, "count", (uint64_t)num == RADTREE(rt)->count);

	/* otherwise does not fit in array */
	CuAssert(tc, "counts", num < sizeof(all)/sizeof(struct teststr*));

	/* check that keys appended match test-elem contents, and also
	 * produce a list of all elements */
	test_check_list_keys(udb, &root, all, &i, num, fullkey_buf, 0, 
		sizeof(fullkey_buf));
	CuAssert(tc, "testlistkey count", i == num);

	/* qsort that list */
	qsort(all, num, sizeof(struct teststr*), &udb_test_sort_cmp);

	test_check_walk(udb, rt, all, num);

	/* check searches for every element */
	test_check_search(rt, all, num);

	/* check closest_match_searches for every exact element */
	test_check_closest_match_exact(udb, rt, all, num);
	/* check closest_match_searches for every inexact element */
	test_check_closest_match_inexact(udb, rt);

	udb_ptr_unlink(&root, udb);

	/* check allocated sizes in the udb */
	mem = udb_radtree_size(udb, rt);
	if(verb) printf("radtree takes %llu\n", (unsigned long long)mem);
	mem += RADTREE(rt)->count * sizeof(struct teststr);
	if(verb) printf("radtree+teststrs takes %llu\n",
		(unsigned long long)mem);
	if(verb) printf("statdata %llu\n",
		(unsigned long long)udb->alloc->disk->stat_data);
	CuAssert(tc, "allocated memory accounted for",
		mem == udb->alloc->disk->stat_data);

	check_udb_structure(tc, udb);
}


static void test_print_str(uint8_t* str, udb_radstrlen_type len)
{
	udb_radstrlen_type x;
	for(x=0; x<len; x++) {
		char c = ((char*)str)[x];
		if(c == 0) fprintf(stderr, ".");
		else fprintf(stderr, "%c", c);
	}
}

static void test_node_print(udb_base* udb, udb_ptr* n, int depth)
{
	udb_ptr s;
	int i;
	uint8_t idx;
	uint8_t par_offset;
	size_t rh = udb->ram_num;
	if(udb_ptr_is_null(n)) return;
	udb_ptr_init(&s, udb);
	udb_ptr_set_rptr(&s, udb, &RADNODE(n)->parent);
	par_offset = RADNODE(&s)->offset;
	for(i=0; i<depth; i++) fprintf(stderr, " ");
	if(RADNODE(n)->parent.data)
		fprintf(stderr, "%c node=%llu.", 
			RADNODE(n)->pidx+par_offset?
			RADNODE(n)->pidx+par_offset:'.',
			(long long unsigned)n->data);
	else
		fprintf(stderr, "rootnode=%llu.", (long long unsigned)n->data);
	fprintf(stderr, " pidx=%d off=%d(%c) len=%d cap=%d strcap=%d parent=%llu lookup=%llu\n",
		RADNODE(n)->pidx, RADNODE(n)->offset,
		isprint(RADNODE(n)->offset)?RADNODE(n)->offset:'.',
		lookup(n)->len, lookup(n)->capacity, lookup(n)->str_cap,
		(long long unsigned)RADNODE(n)->parent.data,
		(long long unsigned)RADNODE(n)->lookup.data);
	for(i=0; i<depth; i++) fprintf(stderr, " ");
	udb_ptr_zero(&s, udb);
	if(RADNODE(n)->elem.data) {
		/* for test setup */
		udb_ptr_set_rptr(&s, udb, &RADNODE(n)->elem);
		fprintf(stderr, "  elem '");
		test_print_str(TESTSTR(&s)->mystr, TESTSTR(&s)->mylen);
		fprintf(stderr, "'");
		fprintf(stderr, "    teststr=%llu",
			(unsigned long long)s.data);
		fprintf(stderr, "\n");
		if(TESTSTR(&s)->mynode.data != n->data)
			fprintf(stderr, "elem data ptr fail\n");
		CuAssertTrue(tc, TESTSTR(&s)->mynode.data == n->data);
	} else fprintf(stderr, "  elem NULL\n");
	udb_ptr_zero(&s, udb);
	for(idx=0; idx<lookup(n)->len; idx++) {
		struct udb_radsel_d* d = &lookup(n)->array[idx];
		if(!d->node.data) {
			CuAssertTrue(tc, d->len == 0);
			continue;
		}
		for(i=0; i<depth; i++) fprintf(stderr, " ");
		if(RADNODE(n)->offset+idx == 0) fprintf(stderr, "[.]");
		else fprintf(stderr, "[%c]", RADNODE(n)->offset + idx);
		if(d->len != 0) {
			fprintf(stderr, "+'");
			test_print_str(lookup_string(n, idx), d->len);
			fprintf(stderr, "'");
		}
		if(d->node.data) {
			fprintf(stderr, " node=%llu\n",
				(long long unsigned)d->node.data);
			udb_ptr_set_rptr(&s, udb, &d->node);
			test_node_print(udb, &s, depth+2);
		} else 	fprintf(stderr, " node NULL\n");
		CuAssertTrue(tc, rh+1 == udb->ram_num);
	}
	udb_ptr_unlink(&s, udb);
	CuAssertTrue(tc, rh == udb->ram_num);
}

static void test_tree_print(udb_base* udb, udb_ptr* rt)
{
	udb_ptr n;
	size_t rh = udb->ram_num;
	fprintf(stderr, "udbradtree %llu elements\n",
		(long long unsigned)RADTREE(rt)->count);
	udb_ptr_new(&n, udb, &RADTREE(rt)->root);
	test_node_print(udb, &n, 0);
	udb_ptr_unlink(&n, udb);
	CuAssertTrue(tc, rh == udb->ram_num);
}

int when = 0;
static void
test_insert_string(udb_base* udb, udb_ptr* rt, char* str)
{
	size_t len = strlen(str);
	udb_ptr s, n;
	size_t rh = udb->ram_num, rh2;
	if(strcmp(str, "e") == 0) when = 1;
	if(!udb_ptr_alloc_space(&s, udb, TESTSTR_CHUNK_TYPE,
		sizeof(struct teststr))) {
		CuAssert(tc, "alloc udb", 0);
	}
	/* MUST initialize the rel_ptr in the teststr */
	memset(TESTSTR(&s), 0, sizeof(struct teststr));
	CuAssertTrue(tc, udb->ram_num == rh+1);
	CuAssert(tc, "insert", s.data != 0);
	TESTSTR(&s)->mylen = len;
	CuAssert(tc, "mystr len", len < sizeof(TESTSTR(&s)->mystr));
	memmove(TESTSTR(&s)->mystr, str, len);
	if(verb) fprintf(stderr, "radix insert: '%s'\n", str);
	rh2 = udb->ram_num;
	/* Note, cannot pass TESTSTR(&s)->mystr, because that pointer
	 * becomes invalid if the udb changes */
	if(!udb_radix_insert(udb, rt, (uint8_t*)str, len, &s, &n)) {
		CuAssert(tc, "insertmustwork", 0);
	}
	CuAssertTrue(tc, udb->ram_num == rh2+1);
	udb_rptr_set_ptr(&TESTSTR(&s)->mynode, udb, &n);
	CuAssert(tc, "insert", n.data != 0);
	udb_ptr_unlink(&s, udb);
	udb_ptr_unlink(&n, udb);

	rh2 = udb->ram_num;
	if(verb) test_tree_print(udb, rt);
	CuAssertTrue(tc, udb->ram_num == rh2);
	test_checks(udb, rt);
	CuAssertTrue(tc, udb->ram_num == rh2);

	CuAssertTrue(tc, udb->ram_num == rh);
}

/** browse all elem's from tree with a for loop */
static void test_browse(udb_base* udb, udb_ptr* rt)
{
	udb_ptr n;
	udb_ptr_init(&n, udb);
	for(udb_radix_first(udb, rt, &n); !udb_ptr_is_null(&n);
		udb_radix_next(udb, &n)) {
		udb_ptr elem;
		udb_ptr_new(&elem, udb, &RADNODE(&n)->elem);
		if(verb) fprintf(stderr, "radix %llu \telem ",
			(unsigned long long)n.data);
		if(verb) test_print_str(TESTSTR(&elem)->mystr,
			TESTSTR(&elem)->mylen);
		if(verb) fprintf(stderr, "\n");
		CuAssert(tc, "walk", TESTSTR(&elem)->mynode.data == n.data);
		udb_ptr_unlink(&elem, udb);
	}
	udb_ptr_unlink(&n, udb);
}

/** delete all elem's from tree with a for loop */
static void test_del(udb_base* udb, udb_ptr* rt)
{
	udb_ptr n;
	udb_ptr_init(&n, udb);
	for(udb_radix_first(udb, rt, &n); !udb_ptr_is_null(&n);
		udb_radix_next(udb, &n)) {
		udb_ptr elem;
		udb_ptr_new(&elem, udb, &RADNODE(&n)->elem);
		if(verb) fprintf(stderr, "del %llu \telem ",
			(unsigned long long)n.data);
		if(verb) test_print_str(TESTSTR(&elem)->mystr,
			TESTSTR(&elem)->mylen);
		if(verb) fprintf(stderr, "\n");
		CuAssert(tc, "del", TESTSTR(&elem)->mynode.data == n.data);
		udb_ptr_free_space(&elem, udb, sizeof(struct teststr));
	}
	udb_ptr_unlink(&n, udb);
}

/* delete a random key */
static void
test_del_a_key(udb_base* udb, udb_ptr* rt)
{
	unsigned x = get_ran_val(RADTREE(rt)->count);
	unsigned i = 0;
	udb_ptr n;
	udb_ptr t;
	size_t rh;

	udb_radix_first(udb, rt, &n);
	while(i++ < x) {
		udb_radix_next(udb, &n);
	}
	if(udb_ptr_is_null(&n)) {
		udb_ptr_unlink(&n, udb);
		return;
	}
	CuAssert(tc, "radix_delete", RADNODE(&n)->elem.data != 0);
	udb_ptr_new(&t, udb, &RADNODE(&n)->elem);
	CuAssert(tc, "refintegrity", TESTSTR(&t)->mynode.data == n.data);
	CuAssert(tc, "refintegrity", t.data == RADNODE(&n)->elem.data);
	if(verb) fprintf(stderr, "delkey %llu \telem ",
		(unsigned long long)n.data);
	if(verb) test_print_str(TESTSTR(&t)->mystr, TESTSTR(&t)->mylen);
	if(verb) fprintf(stderr, "\n");
	udb_rptr_zero(&TESTSTR(&t)->mynode, udb);
	rh = udb->ram_num;
	udb_radix_delete(udb, rt, &n);
	CuAssertTrue(tc, udb->ram_num == rh-1);
	/* and delete the test elem */
	udb_ptr_free_space(&t, udb, sizeof(struct teststr));
	udb_ptr_unlink(&n, udb);

	if(verb) test_tree_print(udb, rt);
	test_checks(udb, rt);
}

/* random add and dell test */
static void
test_ran_add_del(udb_base* udb, udb_ptr* rt)
{
	unsigned i, num = 200;
	unsigned target = 40;
	for(i=0; i<num; i++) {
		/* add or del? */
		unsigned ran = random();
		CuAssertTrue(tc, udb->ram_num == 1);
		if(  (RADTREE(rt)->count < target && ran%4 != 0)
			|| (ran%2 == 0)) {
			uint8_t key[1024];
			udb_radstrlen_type len;
			/* new string key */
			gen_ran_str_len(key, &len, sizeof(key));
			if(!udb_radix_search(rt, key, len)) {
				test_insert_string(udb, rt, (char*)key);
			}
		} else {
			test_del_a_key(udb, rt);
		}
		CuAssertTrue(tc, udb->ram_num == 1);

	}
	CuAssertTrue(tc, udb->ram_num == 1);
	if(verb) test_tree_print(udb, rt);
	while(RADTREE(rt)->count != 0) {
		test_del_a_key(udb, rt);
	}
	CuAssertTrue(tc, udb->ram_num == 1);
}


/** test udb radix tree with this udb base */
static void
udb_radix_test_file(udb_base* udb)
{
	udb_ptr rt;
	CuAssertTrue(tc, udb->ram_num == 0);
	if(!udb_radix_tree_create(udb, &rt)) {
		CuAssert(tc, "udb_radix_tree_create", 0);
	}
	CuAssertTrue(tc, udb->ram_num == 1);
	if(verb) test_tree_print(udb, &rt);
	CuAssertTrue(tc, udb->ram_num == 1);

	test_checks(udb, &rt);
	CuAssertTrue(tc, udb->ram_num == 1);

	test_ran_add_del(udb, &rt);
	if(verb) test_tree_print(udb, &rt);

	test_browse(udb, &rt);
	test_checks(udb, &rt);

	test_del(udb, &rt);
	udb_radix_tree_delete(udb, &rt);
}

/** walk through relptrs in struct teststr */
static void test_str_walk_chunk(void* base, void* d, uint64_t s,
	udb_walk_relptr_cb* cb, void* arg)
{
	struct teststr* p = (struct teststr*)d;
	CuAssertTrue(tc, s >= sizeof(struct teststr));
	(*cb)(base, &p->mynode, arg);
}

/** walk through relptrs in types */
static void testRADwalk(void* base, void* warg, uint8_t t, void* d, uint64_t s,
        udb_walk_relptr_cb* cb, void* arg)
{
	(void)warg;
	switch(t) {
	case udb_chunk_type_radtree:
		udb_radix_tree_walk_chunk(base, d, s, cb, arg);
		break;
	case udb_chunk_type_radnode:
		udb_radix_node_walk_chunk(base, d, s, cb, arg);
		break;
	case udb_chunk_type_radarray:
		udb_radix_array_walk_chunk(base, d, s, cb, arg);
		break;
	case TESTSTR_CHUNK_TYPE:
		test_str_walk_chunk(base, d, s, cb, arg);
		break;
	default:
		/* no rel ptrs */
		break;
	}
}

/** return getpid and suffix a temp file (malloced string) */
char* udbtest_get_temp_file(char* suffix);

static void udb_radtree_1(CuTest* t)
{
	char* fname = udbtest_get_temp_file("rt.udb");
	udb_base* udb;
	if(verb) printf("test udb rad tree (%s)\n", fname);
	tc = t;
	udb = udb_base_create_new(fname, testRADwalk, NULL);
	base_A = udb;

	/* perform udb radix tree tests on this udb file */
	udb_radix_test_file(udb);

	/* close and free it */
	udb_base_close(udb);
	udb_base_free(udb);
	if(verb) printf("End test udb rad tree (%s)\n", fname);
	if(unlink(fname) != 0)
		perror(fname);
	free(fname);
}
