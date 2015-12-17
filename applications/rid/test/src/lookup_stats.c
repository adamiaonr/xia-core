/*
 * lookup_stats.c
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

#include "lookup_stats.h"

void lookup_stats_print(struct lookup_stats * stats) {

	printf(
			"\n--------------------------------------------------------------------------\n"\
			"%-12s\t| %-12s\t| %-12s\t| %-12s\t| %-12s\n"\
			"--------------------------------------------------------------------------\n"\
			"%-12ld\t| %-12ld\t| %-12ld\t| %-12ld\t| %-.6f\n",
			"TPs", "FPs", "TNs", "TOTAL", "FP RATE",
			stats->tps,
			stats->fps,
			stats->tns,
			stats->total_matches,
			((double) stats->fps / ((double) stats->tps + (double) stats->fps)));

	printf("\n");
}

void lookup_stats_init(
		struct lookup_stats ** stats,
		char * prefix,
		int prefix_size) {

	// FIXME: to make things more efficient in terms of space, we assume the
	// existence of a char array, previously allocated somewhere in the heap.
	// why? this avoids the double allocation of a char * for entries of type
	// lsht_fwd and pt_fwd (both will point to the same heap location).
	(*stats)->prefix = prefix;
	(*stats)->prefix_size = prefix_size;

	// everything else is initialized to 0
	(*stats)->tps = 0;
	(*stats)->fps = 0;
	(*stats)->tns = 0;
	(*stats)->total_matches = 0;
}

void lookup_stats_update(
		struct lookup_stats ** stats,
		unsigned long tps,
		unsigned long fps,
		unsigned long tns,
		unsigned long total_matches) {

	(*stats)->tps += tps;
	(*stats)->fps += fps;
	(*stats)->tns += tns;
	(*stats)->total_matches += total_matches;
}

struct lookup_stats * lookup_stats_add(
		struct lookup_stats ** ht,
		struct lookup_stats * node) {

	struct lookup_stats * s;

	HASH_FIND_STR(*ht, node->prefix, s);

	if (s == NULL) {
		HASH_ADD_KEYPTR(hh, *ht, node->prefix, strlen(node->prefix), node);
	}

	return (*ht);
}
