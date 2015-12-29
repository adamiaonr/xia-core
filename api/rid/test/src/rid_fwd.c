/*
 * rid_fwd.c
 *
 * tests 2 different RID FIB schemes: Linear Search of Hash Tables (LSHT),
 * organized by prefix size and Patricia Tries (PTs), organized by either (a)
 * prefix size or (2) Hamming weight (i.e. number of '1s' in XIDs):
 *
 * -# it reads URLs from a file (passed as an argument to the program e.g.
 * 		'rid_fwd -f url.tx'), builds LSHT and PT RID FIBs out of all URLs in the
 * 		file.
 * -# it generates random request names out of the same URLs by
 * 		adding them a random number of suffixes.
 * -# it then passes the requests through the FIBs, collecting statistics on
 * 		True Positives (TPs), False Positives (FPs), True Negatives (TNs) and
 * 		total number of matching tests.
 *
 * Antonio Rodrigues <adamiaonr@cmu.edu>
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

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>

// XXX: as a substitute for <click/hashtable.hh>
#include "uthash.h"

#include "pt.h"
#include "lsht.h"
#include "lookup_stats.h"
#include "rid_utils.h"

#ifdef __linux
#include <sys/time.h>
#include <time.h>
#endif

const char * SUFFIXES[] = {"/ph", "/ghc", "/cylab", "/9001", "/yt1300", "/r2d2", "/c3po"};
const int SUFFIXES_SIZE = 7;

const char * DEFAULT_URL_FILE = "url.txt";

// constants for different ways of grouping forwarding entries in FIBs: (1)
// by prefix size (PREFIX_SIZE_MODE = 0x01) or (2) Hamming weight
// (HAMMING_WEIGHT_MODE = 0x02), i.e. number of '1s' in the XID of the
// entry
typedef enum {PREFIX_SIZE, HAMMING_WEIGHT} ht_mode;
static const char * ht_mode_strs[] = {"p", "h"};

static __inline int rand_int(int min, int max) {
	return min + rand() / (RAND_MAX / (max - min + 1) + 1);
}

void generate_request_name(char ** request_name, char * request_prefix) {

//	int suffixes_num = rand_int(1, SUFFIXES_SIZE);

	strncpy(*request_name, request_prefix, PREFIX_MAX_LENGTH);

//	int request_name_lenght = strlen(*request_name);
//
//	for ( ; suffixes_num > 0; suffixes_num--) {
//
//		strncat(*request_name, SUFFIXES[rand_int(0, SUFFIXES_SIZE - 1)], (PREFIX_MAX_LENGTH - request_name_lenght));
//	}
}

void print_namespace_stats(int * url_sizes, int max_prefix_size) {

	printf(
			"\n--------------------------------------------------------------------------\n"\
			"%-12s\t| %-12s\t\n"\
			"--------------------------------------------------------------------------\n",
			"SIZE (|F|)", "NR. ENTRIES");

	int u_size = 0, nr_u_size = 0, nr_entries = 0;

	for (u_size = 0; u_size < max_prefix_size; u_size++) {

//		if (url_sizes[u_size] > 0) {

			printf(
					"%-12d\t| %-12d\t\n", u_size + 1, url_sizes[u_size]);

			nr_u_size += (url_sizes[u_size] > 0 ? 1 : 0);
			nr_entries += url_sizes[u_size];
//		}
	}

	printf(
			"--------------------------------------------------------------------------\n"\
			"%-12s\t| %-12s\t\n"\
			"--------------------------------------------------------------------------\n"\
			"%-12d\t| %-12d\t\n",
			"TOTAL |F|", "TOTAL NR. ENTRIES",
			nr_u_size, nr_entries);

	printf("\n");
}

int main(int argc, char **argv) {

	// XXX: default mode for FIB organization is by prefix size (in nr. of URL
	// elements)
	ht_mode mode = PREFIX_SIZE;

	printf("RID FIB organization: LSHT &  Patricia Tries (PTs) as in Papalini et al. 2014\n");

	// ************************************************************************
	// A) build forwarding tables
	// ************************************************************************

	// A.1) the base data structures which will hold the FIBs. we try to use
	// the data types which will be used by the Click modular router

	// XXX: Linear Search of Hash Tables (LSHT) FIB: an HT indexed
	// by prefix size (for quick access to each size) + linked list of
	// LSHTNode * (since the search for RID matches will have to be linear
	// anyway).
	struct lsht_ht * lsht_fib = NULL;

	// XXX: Patricia Trie (PT) FIB: also an HT indexed by prefix size +
    // ptree pointer (we do NOT index by Hamming weight (HW) here as in
    // Papalini et al. 2014: note that regardless of the pair {|F|, k},
    // the most probable HW to get is always ~70, when m = 160 bit).
	struct pt_ht * pt_fib = NULL;

	// A.2) open an URL file
	printf("[fwd table build]: reading URL file\n");

	char * url_file_name = (char *) calloc(128, sizeof(char));

	if (argc > 2 && !(strncmp(argv[1], "-f", 2))) {

		strncpy(url_file_name, argv[2], strlen(argv[2]));

	} else {

		// A.2.1) use a default one if argument not given...
		printf("[fwd table build]: using default URL file: %s\n", DEFAULT_URL_FILE);
		strncpy(url_file_name, DEFAULT_URL_FILE, strlen(DEFAULT_URL_FILE));
	}

	if (argc > 3 && !(strncmp(argv[3], "-m", 2))) {

		if (strcmp(argv[4], ht_mode_strs[1]) == 0) {

			mode = HAMMING_WEIGHT;
			printf("[fwd table build]: choosing HAMMING_WEIGHT mode\n");

		} else if (strcmp(argv[4], ht_mode_strs[0]) != 0) {

			printf("[fwd table build]: unknown mode %s choosing PREFIX_SIZE as default\n", argv[4]);
		}

	} else {

		// A.2.1) use a default one if argument not given...
		printf("[fwd table build]: using default URL file: %s\n", DEFAULT_URL_FILE);
		strncpy(url_file_name, DEFAULT_URL_FILE, strlen(DEFAULT_URL_FILE));
	}

	FILE * fr = fopen(url_file_name, "rt");

	// A.3) start reading the URLs and add forwarding entries
	// to each FIB
	char prefix[PREFIX_MAX_LENGTH];
	char * newline_pos;

	// A.3.1) hts with direct references to `per prefix' lookup statistics
	struct lookup_stats * lsht_stats_ht = NULL;
	struct lookup_stats * pt_stats_ht = NULL;

	// A.3.1.1) auxiliary variables for stats gathering...
	// FIXME: this is ugly and you should feel bad...
	struct lsht_fwd * l = NULL;
	struct pt_fwd * p = NULL;

	// A.3.1.2) just an aux array to keep stats on URL sizes
	int url_sizes[PREFIX_MAX_COUNT] = {0}, max_prefix_size = 0;

	while (fgets(prefix, PREFIX_MAX_LENGTH, fr) != NULL) {

		// A.3.2) remove any trailing newline ('\n') character
		if ((newline_pos = strchr(prefix, '\n')) != NULL)
		    *(newline_pos) = '\0';

		// A.3.2) create an RID out of the prefix
		struct click_xia_xid * rid = (struct click_xia_xid *) malloc(sizeof(struct click_xia_xid));
		int prefix_size = name_to_rid(&rid, prefix);

		// FIXME: based on the mode argument, we may need to change the value
		// of prefix_size to the Hamming weight (nr. of '1s' in RID)
		if (mode == HAMMING_WEIGHT)
			prefix_size = rid_hamming_weight(rid);

		// XXX: update the URL size stats
		url_sizes[prefix_size - 1]++;

		if (prefix_size > max_prefix_size)
			max_prefix_size = prefix_size;

		// A.3.3) allocate memory in the heap for this prefix, which will be
		// pointed to by lookup_stats struct *'s in fwd entries, used for
		// control and data gathering
		char * _prefix = (char *) calloc(PREFIX_MAX_LENGTH, sizeof(char));
		strncpy(_prefix, prefix, PREFIX_MAX_LENGTH);

//		printf("[fwd table build]: adding URL %s (size %d)\n", _prefix, prefix_size);

		// A.3.4) add entry to LSHT FIB
		l = lsht_ht_add(&lsht_fib, rid, _prefix, prefix_size);

		// A.3.4.1) add an entry to a lookup_stats ht, which allows for
		// O(1) fetching of `per prefix' stats (avoiding FIB transversals)
		lookup_stats_add(&lsht_stats_ht, l->stats);

		// A.3.5) PT FIB
		p = pt_ht_add(&pt_fib, rid, _prefix, prefix_size);

		lookup_stats_add(&pt_stats_ht, p->stats);
	}

	printf("[fwd table build]: done. added %d prefixes to both LSHT and PT FIBs.\n", HASH_COUNT(pt_stats_ht));

//	printf("[fwd table build]: *** patricia trie *** :\n");
//
//	struct pt_ht * itr;
//
//	for (itr = pt_fib; itr != NULL; itr = (struct pt_ht *) itr->hh.next) {
//		printf("\n[PREFIX SIZE: %d]:\n", itr->prefix_size);
//		pt_fwd_print(itr->trie, PRE_ORDER);
//	}

	fclose(fr);

	// ************************************************************************
	// B) start testing requests against the forwarding tables just built
	// ************************************************************************

	printf("[rid fwd simulation]: reading URL file\n");

	// B.1) re-open the test data file
	fr = fopen(url_file_name, "rt");

	// B.2) create useful vars for the matching tests:

	// B.2.1) request prefix and name:
	//	-# 	request_prefix: directly retrieved from the URL file `as is'
	//	-# 	request_name: the actual name used to generate an
	// 		RID, created by adding a few more URL elements to the prefix
	char * request_prefix = (char *) calloc(PREFIX_MAX_LENGTH, sizeof(char));
	char * request_name = (char *) calloc(PREFIX_MAX_LENGTH, sizeof(char));

	// B.2.2) the RID `holder'
	struct click_xia_xid * request_rid = (struct click_xia_xid *) malloc(sizeof(struct click_xia_xid));

	// B.2.3) request size (guides the lookup procedures)
	int request_size = 0;

	// B.2.4) for stats gathering
	struct lookup_stats lsht_stats = {
		.prefix = NULL,
		.prefix_size = 0,
		.tps = 0,
		.fps = 0,
		.tns = 0,
		.total_matches = 0
	};

	struct lookup_stats pt_stats = lsht_stats;
	struct lookup_stats s;

	printf("[rid fwd simulation]: generating random requests out of prefixes in URL file\n");

	// B.3) start reading the URLs in the test data file
	while (fgets(request_prefix, PREFIX_MAX_LENGTH, fr) != NULL) {

		// B.3.1) remove any trailing newline ('\n') character
		if ((newline_pos = strchr(request_prefix, '\n')) != NULL)
		    *(newline_pos) = '\0';

		// B.3.1) generate some random name out of the request prefix by adding
		// a random number of elements to it
		generate_request_name(&request_name, request_prefix);

		// B.3.2) generate RIDs out of the request names
		request_size = name_to_rid(&request_rid, request_name);

		// FIXME: based on the mode argument, we may need to change the value
		// of prefix_size to the Hamming weight (nr. of '1s' in RID)
		if (mode == HAMMING_WEIGHT)
			request_size = rid_hamming_weight(request_rid);

		// B.3.3) pass the request RID through the FIBs, gather the
		// lookup stats
//		printf("[rid fwd simulation]: lookup for %s started\n", request_name);

		s = lsht_ht_lookup(lsht_fib, request_name, request_size, request_rid);
//		printf("[rid fwd simulation]: lsht lookup end for %s\n", request_name);

		lsht_stats.tps += s.tps;
		lsht_stats.fps += s.fps;
		lsht_stats.tns += s.tns;
		lsht_stats.total_matches += s.total_matches;

		s = pt_ht_lookup(pt_fib, request_name, request_size, request_rid);
//		printf("[rid fwd simulation]: pt lookup end for %s\n", request_name);

		pt_stats.tps += s.tps;
		pt_stats.fps += s.fps;
		pt_stats.tns += s.tns;
		pt_stats.total_matches += s.total_matches;
	}

	// A.3.6) print some stats about the namespace
	printf("[fwd table build]: URL size distribution:\n");
	print_namespace_stats(url_sizes, max_prefix_size);

	printf("[fwd table build]: stats for LSHT FIB:\n");
	lsht_ht_print_stats(lsht_fib);

	printf("[fwd table build]: stats for PT FIB:\n");
	pt_ht_print_stats(pt_fib);

	printf("[rid fwd simulation]: results for LSHT:\n");
	lookup_stats_print(&lsht_stats);

	printf("[rid fwd simulation]: results for PT:\n");
	lookup_stats_print(&pt_stats);

	fclose(fr);

	return 0;
}
