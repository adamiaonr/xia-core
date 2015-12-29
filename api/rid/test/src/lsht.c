/*
 * lsht.c
 *
 * statistics gathering for RID FIB tests.
 *
 * structs and functions to keep track of forwarding stats of the RID
 * forwarding process, e.g. false positives (fps), tps, tns.
 *
 * Antonio Rodrigues <antonior@andrew.cmu.edu>
 *
 * Copyright 2015 Carnegie Mellon University
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * 		http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * limitations under the License.
 */

#include "lsht.h"

struct lsht_ht * lsht_ht_search(struct lsht_ht * ht, int prefix_size) {

	struct lsht_ht * s;

	HASH_FIND_INT(ht, &prefix_size, s);

	return s;
}

int lsht_ht_print_stats(struct lsht_ht * fib) {

	long int num_entries = 0, total_entries = 0, total_sizes = 0;

	// cycle through subtries and print their size
	printf(
			"\n--------------------------------------------------------------------------\n"\
			"%-12s\t| %-12s\t\n"\
			"--------------------------------------------------------------------------\n",
			"SIZE (|F|)", "NR. ENTRIES");

		struct lsht_ht * itr;

		for (itr = fib; itr != NULL; itr = (struct lsht_ht *) itr->hh.next) {

			struct lsht_fwd * n = itr->list;

			// XXX: traverse the linked list to get the number of forwarding
			// entries
			num_entries = 0;
			while (n != NULL) { num_entries++; n = n->next; }

			total_entries += num_entries;
			total_sizes++;

			printf(
					"%-12d\t| %-12ld\t\n",
					itr->prefix_size,
					num_entries);
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

int lsht_sort(struct lsht_ht * a, struct lsht_ht * b) {

    return (a->prefix_size - b->prefix_size);
}

struct lsht_fwd * lsht_ht_add(
		struct lsht_ht ** ht,
		struct click_xia_xid * rid,
		char * prefix,
		int prefix_size) {

	struct lsht_ht * s;

	// allocate memory for a fwd entry to be included in a linked list, pointed
	// to by the HT entry for prefix_size
	struct lsht_fwd * f = (struct lsht_fwd *) malloc(sizeof(struct lsht_fwd));

	f->prefix_rid = rid;
	f->next = NULL;

	// for control purposes, we also create and fill a lookup_stats struct
	// and set f->stats to point to it
	f->stats = (struct lookup_stats *) malloc(sizeof(struct lookup_stats));
	lookup_stats_init(&(f->stats), prefix, prefix_size);

	HASH_FIND_INT(*ht, &prefix_size, s);

	if (s == NULL) {

		s = (struct lsht_ht *) malloc(sizeof(struct lsht_ht));

		s->prefix_size = prefix_size;
		s->list = f;

		HASH_ADD_INT(*ht, prefix_size, s);

		// when inserting new elements in the FIB, sort it by prefix size
		HASH_SORT(*ht, lsht_sort);

	} else {

		// FIXME: let's change this a bit to only include entries which don't
		// 'RID match' with already existing entries.
//		f->next = s->list;
//		s->list = f;

		struct lsht_fwd * n = s->list;

		while (n != NULL) {

			if (rid_compare(f->prefix_rid, n->prefix_rid)) break;

			n = n->next;
		}

		if (n == NULL) {

			f->next = s->list;
			s->list = f;
		}
	}

	return f;
}

struct lookup_stats lsht_ht_lookup(
		struct lsht_ht * lsht_fib,
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

	// find the largest prefix size which is less than or equal than the
	// request size
	struct lsht_ht * s = NULL;
	int prefix_size = request_size;

	while ((s == NULL) && prefix_size > 0)
		s = lsht_ht_search(lsht_fib, prefix_size--);

	// iterate the HT back from prefix_size to 1 to get all possible matching
	// prefix sizes
	// XXX: we are guaranteed (?) to follow a decreasing order because we
	// sort the pt_ht (by prefix size) every time we add a new entry
	struct lsht_fwd * f = NULL;

	int tps = 0, fps = 0, tns = 0;
	int count = 0;

	for ( ; s != NULL; s = (struct lsht_ht *) s->hh.prev) {

		// linear search-match on the entry's linked list
		f = s->list;

		while (f != NULL) {

			// TN check: directly maps to a simple pass or fail of a normal
			// RID matching operation
			if (rid_match(request_rid, f->prefix_rid) == 0) {

				tns = 1;

			} else {

				// TP or FP check: this requires the consultation of the backup
				// char * on f->stats for substrings of request
				if (strstr(request, f->stats->prefix) != NULL) {

					tps = 1;

				} else {

					fps = 1;
				}
			}

			// add the lookup results to the fwd entries own stats record
			lookup_stats_update(&(f->stats), tps, fps, tns, 1);

			// add the lookup results to the accumulator of this whole lookup
			// procedure
			// FIXME: why not use a function like lookup_stats_update()?
			res.tps += tps;
			res.fps += fps;
			res.tns += tns;
			res.total_matches++;

			f = f->next;

			tps = 0;
			fps = 0;
			tns = 0;
		}
	}

	return res;
}
