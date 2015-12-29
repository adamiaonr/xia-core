/*
 * lsht.h
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

#ifndef _LSHT_H_
#define _LSHT_H_

#include <string.h>

#include "uthash.h"
#include "rid_utils.h"
#include "lookup_stats.h"

struct lsht_fwd {

	// the RID prefix, in its encoded form + the forwarding information (port,
	// next hop, etc.)
	struct click_xia_xid * prefix_rid;

	// pointer to the next node
	struct lsht_fwd * next;

	// XXX: this field is optional as it simply exists to keep a record of
	// lookup statistics
	struct lookup_stats * stats;
};

struct lsht_ht {

	// prefix size: either in (1) number of encoded elements, or (2) Hamming
	// weight (number of '1s' in XID), depending on lsht_ht_mode used in
	// lsht_ht_add()
	int prefix_size;

	// pointer to the fwd entry list
	struct lsht_fwd * list;

	// makes this structure hashable
    UT_hash_handle hh;
};
extern int lsht_ht_print_stats(struct lsht_ht * fib);
extern struct lsht_ht * lsht_ht_search(struct lsht_ht * ht, int prefix_size);
extern struct lsht_fwd * lsht_ht_add(
		struct lsht_ht ** ht,
		struct click_xia_xid * rid,
		char * prefix,
		int prefix_size);

extern struct lookup_stats lsht_ht_lookup(
		struct lsht_ht * lsht_fib,
		char * request,
		int request_size,
		struct click_xia_xid * request_rid);

#endif /* _LSHT_H_ */
