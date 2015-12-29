/*
 * lookup_stats.h
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

#ifndef _LOOKUP_STATS_H_
#define _LOOKUP_STATS_H_

#include <stdio.h>

#include "uthash.h"

struct lookup_stats {

	// pointer to a copy of the respective prefix URL, in its non-encoded form
	char * prefix;

	// FIXME: this may be useless, but i'll keep it...
	int prefix_size;

	// the actual statistics: (TPs, FPs, TNs, total number of matches)
	unsigned long tps;
	unsigned long fps;
	unsigned long tns;
	unsigned long total_matches;

	// makes this structure hashable, so that results per prefix are quickly
	// accessed
    UT_hash_handle hh;
};

extern void lookup_stats_init(
		struct lookup_stats ** stats,
		char * prefix,
		int prefix_size);

extern void lookup_stats_update(
		struct lookup_stats ** stats,
		unsigned long tps,
		unsigned long fps,
		unsigned long tns,
		unsigned long total_matches);

extern struct lookup_stats * lookup_stats_add(
		struct lookup_stats ** ht,
		struct lookup_stats * node);

extern void lookup_stats_print(struct lookup_stats * stats);

#endif /* _LOOKUP_STATS_H_ */
