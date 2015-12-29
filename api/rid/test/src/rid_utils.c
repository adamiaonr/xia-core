/*
 * rid_utils.c
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

#include "rid_utils.h"

char * extract_prefix_bytes(
		char ** to,
		struct click_xia_xid * rid,
		int trailing_bit) {

	// get the index of the byte which contains the key_bit
	int byte_index = (CLICK_XIA_XID_ID_LEN - (trailing_bit / 8) - 1), i;

	// aux char * which keeps a string representation of a byte
	char byte_str[3] = {0};

	for (i = (CLICK_XIA_XID_ID_LEN - 1); i >= byte_index; i--) {

		sprintf(byte_str, "%02X", rid->id[i]);
		strncat(*to, byte_str, 2);

		memset(byte_str, 0, strlen(byte_str));
	}

	return *to;
}

int name_to_rid(struct click_xia_xid ** rid, char * _prefix) {

	// transform an URL-like prefix into a Bloom Filter (BF) of size
	// m = 160 bit
	struct bloom bloom_filter;
	bloom_init(&bloom_filter);

	// by default, we consider all possible prefix lengths
	// TODO: implement a non-default behavior (e.g. argument specifying the
	// prefix lengths to encode)

	// do this in order not to destroy _prefix in strtok()
	char * prefix = (char *) calloc(PREFIX_MAX_LENGTH, sizeof(char));
	strncpy(prefix, _prefix, strlen(_prefix));

	// to sequentially build the subprefixes
	char * sub_prefix = (char *) calloc(PREFIX_MAX_LENGTH, sizeof(char));
	int sub_prefix_count = 0;

	size_t sub_prefix_len = 0;
	size_t prefix_len = strlen(prefix);

	// extract the first subprefix (i.e. from start of prefix till the first
	// occurrence of '/')
	char * token = strtok(prefix, PREFIX_DELIM);

	while (token != NULL && (sub_prefix_count <= PREFIX_MAX_COUNT)) {

		//printf("name_to_rid(): strncatin' token %s\n", token);

		// concatenate the next token between '/' to the current subprefix
		sub_prefix = strncat(
				sub_prefix,
				(const char *) token,
				(PREFIX_MAX_LENGTH - strlen(sub_prefix) - 1));

		sub_prefix_len = strlen(sub_prefix);

		// add the current subprefix to the BF
		//printf("name_to_rid(): adding prefix %s, size %d\n", sub_prefix, (int) sub_prefix_len);
		bloom_add(&bloom_filter, sub_prefix, sub_prefix_len);

		// TODO: shall we include '/' for the BF encoding part?
		// check if this is the final token
		if (sub_prefix_len < prefix_len) {

			sub_prefix = strncat(
					sub_prefix,
					PREFIX_DELIM,
					(PREFIX_MAX_LENGTH - sub_prefix_len - 1));

			sub_prefix_len++;
		}

		sub_prefix_count++;

		// capture the new token
		token = strtok(NULL, PREFIX_DELIM);
	}

	(*rid)->type = CLICK_XIA_XID_TYPE_RID;

	// XXX: no risk of 'loosing' stuff here, note that we're copying char from
	// bloom_filter (which will later be deleted via free() anyway)
	int i = 0;

	for (i = 0; i < CLICK_XIA_XID_ID_LEN; i++) {

		(*rid)->id[i] = (uint8_t) bloom_filter.bf[i];
	}

	// free string memory
	free(prefix);
	free(sub_prefix);

	// print and free BF memory
	//bloom_print(bloom_filter);
	bloom_free(&bloom_filter);

	return sub_prefix_count;
}

int rid_compare(struct click_xia_xid * a, struct click_xia_xid * b) {

	int res = 1, j;

	for (j = 0; j < CLICK_XIA_XID_ID_LEN; j++) {

		if (a->id[j] != b->id[j]) {

			res = 0;

			// off (of the cycle) you go...
			break;
		}
	}

	return res;
}

int rid_match(struct click_xia_xid * req, struct click_xia_xid * fwd) {

	int res = 1, j;

	for (j = 0; j < CLICK_XIA_XID_ID_LEN; j++) {

		if ((req->id[j] & fwd->id[j]) != fwd->id[j]) {

			res = 0;

			// off (of the cycle) you go...
			break;
		}
	}

	return res;
}

int rid_match_mask(
		struct click_xia_xid * req,
		struct click_xia_xid * fwd,
		int trailing_bit) {

	// FIXME: it seems inefficient (in terms of memory and speed) to be
	// allocating memory for a mask every time we run this function...
	uint8_t mask[CLICK_XIA_XID_ID_LEN] = {0};

	// find on which byte of the RID is the `key bit': let's call it `key byte'
	int key_byte = (CLICK_XIA_XID_ID_LEN - (trailing_bit / 8) - 1), i;

	// in the `key byte', set the bits to the left of the `key bit' to '1',
	// leaving others at '0'
	mask[key_byte] = (~(~0 << ((trailing_bit + 1) % 8)) << (8 - ((trailing_bit + 1) % 8)));

	// set all bits of the bytes of mask with index larger than byte_index to
	// '1'
	for (i = (key_byte + 1); i < CLICK_XIA_XID_ID_LEN; i++)
		mask[i] = 0xFF;

	int res = 1, j;

	for (j = 0; j < CLICK_XIA_XID_ID_LEN; j++) {

		if (((req->id[j] & mask[j]) & (fwd->id[j] & mask[j])) != (fwd->id[j] & mask[j])) {

			res = 0;

			// off (of the cycle) you go...
			break;
		}
	}

	return res;
}

int rid_hamming_weight(struct click_xia_xid * rid) {

	int rid_hamming_weight = 0, i = 0;

	// FIXME: to make things easy, here we use GNU compiler's built-in
	// population count function. we could also use some 'cool' algorithms
	// such as that found on p. 66 of 'Hacker's Delight' (http://bit.ly/1gfDYpe)
	for (i = 0; i < CLICK_XIA_XID_ID_LEN; i++)
		rid_hamming_weight += __builtin_popcount(rid->id[i]);

	return rid_hamming_weight;
}
