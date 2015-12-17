/*
 * pt.c
 *
 * Patricia trie implementation.
 *
 * Functions for inserting nodes, removing nodes, and searching in
 * a Patricia trie designed for IP addresses and netmasks.  A
 * head node must be created with (key,mask) = (0,0).
 *
 * NOTE: The fact that we keep multiple masks per node makes this
 *       more complicated/computationally expensive then a standard
 *       trie.  This is because we need to do longest prefix matching,
 *       which is useful for computer networks, but not as useful
 *       elsewhere.
 *
 * Matthew Smart <mcsmart@eecs.umich.edu>
 *
 * Copyright (c) 2000
 * The Regents of the University of Michigan
 * All rights reserved
 *
 * Altered by Antonio Rodrigues <antonior@andrew.cmu.edu> for test with RIDs.
 *
 * $Id$
 */

#include "pt.h"

/*
 * \brief 	private function used to return whether or not bit 'i' (starting
 * 			from the most significant bit) is set in an RID.
 */
static __inline unsigned long bit(int i, struct click_xia_xid * rid) {

	// XXX: this needs to be changed `a bit(s)' (pun intended): we now have
	// a CLICK_XIA_XID_ID_LEN byte-sized key (the RID) and so i can be in the
	// interval [0, CLICK_XIA_XID_ID_LEN * 8].
	//return key & (1 << (31-i));

	// CLICK_XIA_XID_ID_LEN - ((i / 8) - 1) identifies the byte of the RID
	// where bit i should be
	// XXX: be careful with endianess (particularly big endianess aka
	// network order, which may be used for XIDs)
	//printf("(%d) %02X vs. %02X = %02X\n", i, rid->id[CLICK_XIA_XID_ID_LEN - (i / 8) - 1], (1 << (8 - i - 1)), rid->id[CLICK_XIA_XID_ID_LEN - (i / 8) - 1] & (1 << (8 - i - 1)));

	return rid->id[CLICK_XIA_XID_ID_LEN - (i / 8) - 1] & (1 << (8 - (i % 8) - 1));
}

/*
 * \brief recursively counts the number of entries in a patricia trie.
 */
static int pt_fwd_count(struct pt_fwd * t, int key_bit) {

	int count;

	if (t->key_bit <= key_bit) return 0;

	count = 1;

	count += pt_fwd_count(t->p_left,  t->key_bit);
	count += pt_fwd_count(t->p_right, t->key_bit);

	return count;
}

int pt_ht_print_stats(struct pt_ht * fib) {

	long int num_entries = 0, total_entries = 0, total_sizes = 0;

	// cycle through subtries and print their size

	printf(
			"\n--------------------------------------------------------------------------\n"\
			"%-12s\t| %-12s\t| %-12s\n"\
			"--------------------------------------------------------------------------\n",
			"SIZE (|F|)", "NR. ENTRIES", "AVG. FEA");

		struct pt_ht * itr;

		for (itr = fib; itr != NULL; itr = (struct pt_ht *) itr->hh.next) {

			num_entries = pt_fwd_count(itr->trie, -1);
			total_entries += num_entries;
			total_sizes++;

			printf(
						"%-12d\t| %-12ld\t| %-.6f\n",
						((itr->trie->p_left != NULL) ? itr->trie->p_left->prefix_size : itr->trie->p_right->prefix_size),
						num_entries,
						itr->fea);
		}

	printf(
			"--------------------------------------------------------------------------\n"\
			"%-12s\t| %-12s\t\n"\
			"--------------------------------------------------------------------------\n"\
			"%-12ld\t| %-12ld\t\n",
			"TOTAL |F|", "TOTAL NR. ENTRIES",
			total_sizes, total_entries);

	printf("\n");

	return total_entries;
}

/*
 * \brief private function used for inserting a node recursively.
 *
 * XXX: note how this method will either return a reference to (1) a
 * `candidate starting node' h; or (2) the node to be inserted itself, n.
 *
 * 2 happens when the current `candidate' parent isn't suitable: we can either
 * find that the decision bit of the parent is either (1) larger than the
 * decision bit of n (which means that n should be a parent of h); or (2)
 * less than that of the grandparent's (which means that ...).
 *
 * \arg h	node were insertion starts (guess it's the insert 'h'ere node)
 * \arg n	new node to insert
 * \arg d	decision bit (?)
 * \arg p	parent of the h node
 *
 *
 */
static struct pt_fwd * insertR(
		struct pt_fwd * h,
		struct pt_fwd * n,
		int d,
		struct pt_fwd * p) {

	if ((h->key_bit >= d) || (h->key_bit <= p->key_bit)) {

		n->key_bit = d;
		n->p_left = bit(d, n->prefix_rid) ? h : n;
		n->p_right = bit(d, n->prefix_rid) ? n : h;

		return n;
	}

	if (bit(h->key_bit, n->prefix_rid)) {

		h->p_right = insertR(h->p_right, n, d, h);

	} else {

		h->p_left = insertR(h->p_left, n, d, h);
	}

	return h;
}

/*
 * \brief inserts a new node into a patricia trie
 *
 * \arg n		new node to insert
 * \arg head	head of the patricia (sub-)trie
 *
 * \return		reference to node, correctly inserted in the trie
 *
 */
struct pt_fwd * pt_fwd_insert(struct pt_fwd * n, struct pt_fwd * head) {

	struct pt_fwd * t;
	int i;

	if (!head || !n)
		return NULL;

	/*
	 * Find closest matching leaf node.
	 */
	t = head;

	do {

		i = t->key_bit;

		t = bit(t->key_bit, n->prefix_rid) ? t->p_right : t->p_left;

		// XXX: Q: how will t->key_bit be ever less than or equal to i?
		// XXX: A: this is the typical stopping condition for a search in
		// in Patricia tries. since there are no explicit NULL links, we verify
		// if any of the links 'points up' the trie (if you
		// inspect insertR(), you'll see that sometimes t->p_right or
		// t->p_left point to the head of the tree, parent node or
		// itself). this can be done because (by definition) the `key bit' in
		// the nodes increase as one traverses down the trie.

	} while (i < t->key_bit);

	/*
	 * Find the first bit that differs.
	 */
	for (i = 1; i < ((8 * CLICK_XIA_XID_ID_LEN) - 1) && (bit(i, n->prefix_rid) == bit(i, t->prefix_rid)); i++);

	/*
	 * Recursive step.
	 * XXX: this is where the actual insertion happens...
	 */
	if (bit(head->key_bit, n->prefix_rid)) {

		head->p_right = insertR(head->p_right, n, i, head);

	} else {

		head->p_left = insertR(head->p_left, n, i, head);
	}

	return n;
}


/*
 * remove an entry given a key in a Patricia trie.
 */
int pt_fwd_remove(struct pt_fwd * n, struct pt_fwd * head) {

	// parent, grandparent, ...
	struct pt_fwd *p, *g, *pt, *pp, *t = NULL;
	int i;

	if (!n || !t)
		return 0;

	/*
	 * Search for the target node, while keeping track of the
	 * parent and grandparent nodes.
	 */
	g = p = t = head;

	do {

		i = t->key_bit;
		g = p;
		p = t;
		t = bit(t->key_bit, n->prefix_rid) ? t->p_right : t->p_left;

	} while (i < t->key_bit);

	/*
	 * For removal, we need an exact match.
	 */
	if (!(rid_compare(t->prefix_rid, n->prefix_rid)))
		return 0;

	/*
	 * Don't allow removal of the default entry.
	 */
	if (t->key_bit == 0)
		return 0;

	/*
	 * Search for the node that points to the parent, so
	 * we can make sure it doesn't get lost.
	 */
	pp = pt = p;

	do {

		i = pt->key_bit;
		pp = pt;
		pt = bit(pt->key_bit, p->prefix_rid) ? pt->p_right : pt->p_left;

	} while (i < pt->key_bit);

	if (bit(pp->key_bit, p->prefix_rid))
		pp->p_right = t;
	else
		pp->p_left = t;

	/*
	 * Point the grandparent to the proper node.
	 */
	if (bit(g->key_bit, n->prefix_rid))
		g->p_right = bit(p->key_bit, n->prefix_rid) ?
			p->p_left : p->p_right;
	else
		g->p_left = bit(p->key_bit, n->prefix_rid) ?
			p->p_left : p->p_right;

	/*
	 * Delete the target's data and copy in its parent's
	 * data, but not the bit value.
	 */
	if (t != p) {
		t->prefix_rid = p->prefix_rid;
	}

	free(p);

	return 1;
}

struct pt_fwd * pt_fwd_init() {

	// allocate memory for a new patricia trie (in this case a single node, or
	// fwd entry).
	struct pt_fwd * root = (struct pt_fwd *) malloc(sizeof(struct pt_fwd));

	int i = 0;

	// we must now allocate memory a root RID with all bits set
	// to 0
	struct click_xia_xid * root_rid = (struct click_xia_xid *) malloc(sizeof(struct click_xia_xid));

	for (i = 0; i < CLICK_XIA_XID_ID_LEN; i++)
		root_rid->id[i] = 0x00;

	root_rid->type = CLICK_XIA_XID_TYPE_RID;

	// assign that RID with the root's prefix RID
	root->prefix_rid = root_rid;

	// initialize the struct lookup_stats * attribute w/ an empty
	// prefix string
	struct lookup_stats * stats = (struct lookup_stats *) malloc(sizeof(struct lookup_stats));
	char * prefix = (char *) calloc(PREFIX_MAX_LENGTH, sizeof(char));

	stats->prefix = prefix;
	stats->prefix_size = 0;

	stats->tps = 0;
	stats->fps = 0;
	stats->tns = 0;
	stats->total_matches = 0;

	root->stats = stats;

	// initialize the rest of the root's attributes and set it's pointers to
	// point to the root itself
	root->prefix_size = 0;
	root->key_bit = 0;
	root->p_left = root->p_right = root;

	return root;
}

struct pt_ht * pt_ht_search(struct pt_ht * ht, int prefix_size) {

	struct pt_ht * s;

	HASH_FIND_INT(ht, &prefix_size, s);

	return s;
}

int pt_ht_sort(struct pt_ht * a, struct pt_ht * b) {

    return (a->prefix_size - b->prefix_size);
}

/*
 * find an entry given a key in a patricia trie.
 */
struct pt_fwd * pt_fwd_search(struct click_xia_xid * rid, struct pt_fwd * head) {

	struct pt_fwd * t = head;
	int i;

	if (!t)
		return 0;

	/*
	 * find closest matching leaf node.
	 */
	do {

		i = t->key_bit;
		t = bit(t->key_bit, rid) ? t->p_right : t->p_left;

	} while (i < t->key_bit);

	/*
	 * Compare keys to see if this
	 * is really the node we want.
	 */
	return (rid_compare(rid, t->prefix_rid) ? t : NULL);
}

struct pt_fwd * pt_ht_add(
		struct pt_ht ** ht,
		struct click_xia_xid * rid,
		char * prefix,
		int prefix_size) {

	struct pt_ht * s;
	struct pt_fwd * f = (struct pt_fwd *) malloc(sizeof(struct pt_fwd));

	f->prefix_rid = rid;
	f->prefix_size = prefix_size;
	f->p_right = NULL;
	f->p_left = NULL;

	// XXX: for control purposes, we also create and fill a lookup_stats struct
	// and set f->stats to point to it
	f->stats = (struct lookup_stats *) malloc(sizeof(struct lookup_stats));
	lookup_stats_init(&(f->stats), prefix, prefix_size);

	HASH_FIND_INT(*ht, &prefix_size, s);

	if (s == NULL) {

		s = (struct pt_ht *) malloc(sizeof(struct pt_ht));
		s->prefix_size = prefix_size;

		// initialize the trie with a `all-zero' root
		s->trie = pt_fwd_init();

		// FIXME: temporary hack to keep track of one more stat
		s->fea = 0.0;
		s->fea_n = 0;

		if (!(s->trie)){

			//fprintf(stderr, "[fwd table build]: pt_fwd_init() failed\n");

		} else {

			HASH_ADD_INT(*ht, prefix_size, s);

			// when inserting new elements in the HT, sort by prefix size
			HASH_SORT(*ht, pt_ht_sort);

			//printf("[fwd table build]: pt_fwd_init() successful\n");
		}
	}

	struct pt_fwd * srch = NULL;

	if ((srch = pt_fwd_search(f->prefix_rid, s->trie)) != NULL) {

		//printf("[fwd table build]: node with %s (vs. %s) exists!\n", f->stats->prefix, srch->stats->prefix);

	} else {

		if (!(pt_fwd_insert(f, s->trie))) {

			printf("[fwd table build]: pat_insert() failed\n");
		}
	}

	return f;
}

/*
 * \brief 	longest prefix matching on a patricia trie, accounting for
 * 			false positives
 *
 * the advantage of this scheme is a potential reduction in lookup time: while
 * in the LSHT scheme we have O(|R| * avg(|HT|)) expected time (we have to
 * lookup all HTs for which |F| \le |R|), which includes all cases - TPs, FPs
 * and TNs - with a PT of RIDs we avoid looking up entire sub-tries made up of
 * TNs.
 *
 * XXX: it would be interesting to quantify how much do we gain, but i don't
 * know how to estimate the number of TNs in this way...
 *
 * organizing the FIB in multiple PTs indexed by Hamming weight (HW) as in
 * Papalini et al. 2014 does not translate into longer prefix matches for
 * larger HWs: since bits in BFs can be overwritten during the encoding
 * operation, we can have situations in which HW(R1) > HW(R2) and |R1| < |R2|
 * (theoretical results in (...)). Therefore, an indication of the number of
 * prefixes (similar to that of a `mask') must always be present in the trie
 * nodes.
 *
 * \return
 */
struct lookup_stats pt_fwd_lookup(
		struct pt_fwd * node,
		char * request,
		int request_size,
		struct click_xia_xid * request_rid,
		int prev_key_bit) {

	// initialize lookup_stats structs to return as a result
	struct lookup_stats res = {
		.prefix = request,
		.prefix_size = request_size,
		.tps = 0,
		.fps = 0,
		.tns = 0,
		.total_matches = 0
	};

	if (node->key_bit <= prev_key_bit) {

//		printf("pt_fwd_lookup(): going upwards for (R) %s (%d vs. %d)\n",
//				request,
//				node->key_bit,
//				prev_key_bit);

		return res;
	}

	struct lookup_stats res_left = {
		.prefix = request,
		.prefix_size = request_size,
		.tps = 0,
		.fps = 0,
		.tns = 0,
		.total_matches = 0
	};

	struct lookup_stats res_right = res_left;

	int tps = 0, fps = 0, tns = 0;

	// XXX: when looking up a PT with FPs, we always follow the left branch,
	// and selectively follow the right branch.

	// "why?", you ask...

	// going left means there's
	// a '0' bit at position key_bit: it doesn't matter if it maps to a '0' or '1'
	// in request, the matching test ((R & F) == F) won't be false because of
	// this fact. going right means there's a '1' at position key_bit: if it maps
	// to a '0' in request, then ((R & F) == F) will fail, and so we avoid
	// traversing the right sub-trie.

	// XXX: apply a mask to the key_bit leftmost bits of node and request, check if
	// ((mask(request) & mask(prefix)) == mask(prefix)): if yes, follow the
	// p_right branch, if not, avoid it.
	if (rid_match_mask(request_rid, node->prefix_rid, node->key_bit)) {

		// XXX: check for a match, now without masks on
		// FIXME: note we're avoiding matches with the default route (or root
		// node) by checking if node->prefix_size > 0
		if (rid_match(request_rid, node->prefix_rid) && (node->prefix_size > 0)) {

			// TP or FP check: this requires the consultation of the backup
			// char * on f->stats for substrings of request
			//printf("%s vs. %s\n", node->stats->prefix, request);

			if (strstr(request, node->stats->prefix) != NULL) {

//				printf("pt_fwd_lookup(): TP %s\n", node->stats->prefix);

				tps = 1;

			} else {

//				char * p = (char *) calloc(CLICK_XIA_XID_ID_STR_LEN, sizeof(char));
//				char * r = (char *) calloc(CLICK_XIA_XID_ID_STR_LEN, sizeof(char));
//
//				printf("pt_fwd_lookup(): (R) %s vs. (P) %s\n", request, node->stats->prefix);
//				printf("pt_fwd_lookup(): (R) %s vs. (P) %s\n",
//						extract_prefix_bytes(&r, request_rid, node->key_bit),
//						extract_prefix_bytes(&p, node->prefix_rid, node->key_bit));
//
//				// free string memory
//				free(r);
//				free(p);

//				printf("pt_fwd_lookup(): FP %s\n", node->stats->prefix);

				fps = 1;
			}

		} else {

			// TN check: directly maps to a simple pass or fail of a normal
			// RID matching operation
			tns = 1;
		}

		// add the lookup results to the fwd entries own stats record
		lookup_stats_update(&(node->stats), tps, fps, tns, 1);

		res_right = pt_fwd_lookup(node->p_right, request, request_size, request_rid, node->key_bit);

	} else {

		tns = 1;

		lookup_stats_update(&(node->stats), tps, fps, tns, 1);
	}

	res_left = pt_fwd_lookup(node->p_left, request, request_size, request_rid, node->key_bit);

	// add the lookup results to the accumulator of this whole lookup
	// procedure
	// FIXME: why not use a function like lookup_stats_update()?
	res.tps += tps + res_right.tps + res_left.tps;
	res.fps += fps + res_right.fps + res_left.fps;
	res.tns += tns + res_right.tns + res_left.tns;
	res.total_matches += 1 + res_right.total_matches + res_left.total_matches;

//	printf("pt_fwd_lookup(): --------\n");
//	printf("pt_fwd_lookup(): node %s \n", node->stats->prefix);
//	printf("pt_fwd_lookup(): --------\n");
//	lookup_stats_print(&res_left);
//	lookup_stats_print(&res_right);
//	lookup_stats_print(&res);

	return res;
}

struct lookup_stats pt_ht_lookup(
		struct pt_ht * pt_fib,
		char * request,
		int request_size,
		struct click_xia_xid * request_rid) {

	// initialize a lookup_stats struct to return as a result
	struct lookup_stats res = {
		.prefix = request,
		.prefix_size = request_size,
		.tps = 0,
		.fps = 0,
		.tns = 0,
		.total_matches = 0
	};

	struct lookup_stats aux;

	// find the largest prefix size which is less than or equal than the
	// request size
	struct pt_ht * s = NULL;
	int prefix_size = request_size;

	while ((s == NULL) && prefix_size > 0)
		s = pt_ht_search(pt_fib, prefix_size--);

	// iterate the HT back from prefix_size to 1 to get all possible matching
	// prefix sizes
	// XXX: we are guaranteed (?) to follow a decreasing order because we
	// sort the pt_ht (by prefix size) every time we add a new entry
	struct pt_fwd * f = NULL;

	for ( ; s != NULL; s = (struct pt_ht *) s->hh.prev) {

		// linear search-match on the entry's patricia trie...
		f = s->trie;

		if (!f) {

			fprintf(stderr, "pt_ht_lookup(): no trie for (%s, %d)\n", request, request_size);

			continue;
		}

		// just start the whole recursive lookup on the patricia trie
		// FIXME: the '-1' is an hack and an ugly one
//		printf("pt_ht_lookup(): LOOKUP FOR (R) %s\n", request);
		aux = pt_fwd_lookup(f, request, request_size, request_rid, -1);

		// FIXME: ugly hack just to keep track of one more stat
		s->fea = ((s->fea) * (double) s->fea_n) + (double) (1.0 - ((double) aux.total_matches / (double) pt_fwd_count(f, -1)));
		s->fea_n++;
		s->fea = s->fea / (double) s->fea_n;

		res.tps += aux.tps;
		res.fps += aux.fps;
		res.tns += aux.tns;
		res.total_matches += aux.total_matches;

//		printf("pt_ht_lookup(): FINAL FOR (R) %s\n", request);
//		lookup_stats_print(&res);
	}

	return res;
}

void pt_fwd_print(
		struct pt_fwd * node,
		uint8_t mode) {

	// FIXME: only one mode is supported as of now...
	if (mode == PRE_ORDER) {

		if (node) {

			// string which will hold the representations of the patricia trie nodes
			char * prefix_bytes = (char *) calloc(CLICK_XIA_XID_ID_STR_LEN, sizeof(char));

			// we only print the `prefix bytes', i.e. those which contain the
			// key_bit leftmost bits

			// left child
			printf("[%s (%d)] <-- ",
					extract_prefix_bytes(&prefix_bytes, node->p_left->prefix_rid, node->p_left->key_bit),
					node->p_left->key_bit);

			memset(prefix_bytes, 0, strlen(prefix_bytes));

			// node itself
			printf("[%s (%d)]",
					extract_prefix_bytes(&prefix_bytes, node->prefix_rid, node->key_bit),
					node->key_bit);

			memset(prefix_bytes, 0, strlen(prefix_bytes));

			// right child
			printf(" --> [%s (%d)]\n",
					extract_prefix_bytes(&prefix_bytes, node->p_right->prefix_rid, node->p_right->key_bit),
					node->p_right->key_bit);

			free(prefix_bytes);

			if (node->p_left->key_bit > node->key_bit)
				pt_fwd_print(node->p_left, PRE_ORDER);

			if (node->p_right->key_bit > node->key_bit)
				pt_fwd_print(node->p_right, PRE_ORDER);
	    }

	} else {

		fprintf(stderr, "pt_fwd_print() : UNKNOWN MODE (%d)\n", mode);
	}
}
