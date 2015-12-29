/*
 *  Copyright (c) 2012, Jyri J. Virkki
 *  All rights reserved.
 *
 *  This file is under BSD license. See LICENSE file.
 */

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <string.h>

#include "bloom.h"

#ifdef __linux
#include <sys/time.h>
#include <time.h>
#endif

const char * PREFIX_DELIM = "/";
const char * DEFAULT_FILE = "/home/adamiaonr/Workbench/xia/libbloom/url.txt";

const int PREFIX_MAX_COUNT = 32;
const int PREFIX_MAX_LENGTH = 128;

void bloomify(struct bloom * bloomFilter, char * _prefix, int * prefixSizes) {

	// 1) transform the URL-like prefix into a Bloom Filter (BF) of size
	// m = 160 bit

	// the BF itself
//	struct bloom bloomFilter;
	assert(bloom_init(bloomFilter) == 0);

	// by default, we consider all possible prefix lengths
	// TODO: implement a non-default behavior (e.g. argument specifying the
	// prefix lengths to encode)

	// do this in order not to destroy _prefix in strtok()
	char * prefix = (char *) calloc(PREFIX_MAX_LENGTH, sizeof(char));
	strncpy(prefix, _prefix, strlen(_prefix));

	// to sequentially build the subprefixes
	char * subPrefix = (char *) calloc(PREFIX_MAX_LENGTH, sizeof(char));

	// holds the end index of each subprefix in prefix, according to subprefix
	// size

	int * subPrefixIndex = (int *) calloc(PREFIX_MAX_COUNT, sizeof(int));
	//int subPrefixIndex[PREFIX_MAX_COUNT] = { 0 };
	int subPrefixCount = 0;

	size_t subPrefixLen = 0;
	size_t prefixLen = strlen(prefix);

	// extract the first subprefix (i.e. from start of prefix till the first
	// occurrence of '/')
	char * token;
	token = strtok(prefix, PREFIX_DELIM);

	int bfCollisions = 0;

	while (token != NULL
			&& !(subPrefixCount > PREFIX_MAX_COUNT)) {

		// concatenate the next token between '/' to the current subprefix
		subPrefix = strncat(
				subPrefix,
				(const char *) token,
				(PREFIX_MAX_LENGTH - strlen(subPrefix) - 1));

		subPrefixLen = strlen(subPrefix);

		// TODO: shall we include '/' for the BF encoding part?
		// check if this is the final token
		if (subPrefixLen < prefixLen) {

			subPrefix = strncat(
					subPrefix,
					PREFIX_DELIM,
					(PREFIX_MAX_LENGTH - subPrefixLen - 1));

			subPrefixLen++;
		}

		// update the index in prefix at which the current subprefix ends
		subPrefixIndex[subPrefixCount++] = subPrefixLen;

		// add the current subprefix to the BF
		//printf("adding prefix %s, size %d\n", subPrefix, subPrefixLen);

		if(bloom_add(bloomFilter, subPrefix, subPrefixLen)) { bfCollisions++; }

		// capture the new token
		token = strtok(NULL, PREFIX_DELIM);
	}

	//bloom_print(bloomFilter);

	// general stats
	prefixSizes[subPrefixCount]++;
}

/** ***************************************************************************
 * main...
 *
 */
int main(int argc, char **argv) {

	unsigned char print = 0x00;

	if (argc > 2 && !(strncmp(argv[1], "-p", 2))) {

		print = 0xFF;

	}

	int * prefixSizes = (int *) calloc(PREFIX_MAX_COUNT, sizeof(int));

	(void) printf("testing libbloom...\n");

	// 1) get the URL file name
	char * fileName = (char *) calloc(128, sizeof(char));

	if (argc > 3 && !(strncmp(argv[2], "-f", 2))) {

		strncpy(fileName, argv[3], strlen(argv[3]));

	} else {

		// 1.1) use a default one if argument not given...
		strncpy(fileName, DEFAULT_FILE, strlen(DEFAULT_FILE));
	}

	int count = 0, i = 0, j = 0, k = 0;
	int fwdCollisions[4] = {};

	struct bloom * fwdEntries = (struct bloom *) calloc(4, sizeof(struct bloom));

	(void) printf("fwdEntries[0]: [uof/]\n");
	assert(bloom_init(&fwdEntries[0]) == 0);
	bloom_add(&fwdEntries[0], "uof/", strlen("uof/"));

	(void) printf("fwdEntries[1]: [uof/cs/]\n");
	assert(bloom_init(&fwdEntries[1]) == 0);
	bloom_add(&fwdEntries[1], "uof/cs/", strlen("uof/cs/"));

	(void) printf("fwdEntries[2]: [uof/cs/r9x/r9001/sensor/temperature/]\n");
	assert(bloom_init(&fwdEntries[2]) == 0);
	bloom_add(&fwdEntries[2], "uof/cs/r9x/r9001/sensor/temperature/", strlen("uof/cs/r9x/r9001/sensor/temperature/"));

	(void) printf("fwdEntries[3]: [uof/ece/r8x/r8001/monitor/]\n");
	assert(bloom_init(&fwdEntries[3]) == 0);
	bloom_add(&fwdEntries[3], "uof/ece/r8x/r8001/monitor/", strlen("uof/ece/r8x/r8001/monitor/"));

	if (print) {

		bloom_print(&fwdEntries[0]);
		bloom_print(&fwdEntries[1]);
		bloom_print(&fwdEntries[2]);
		bloom_print(&fwdEntries[3]);
	}

	// ************************************************************************
	// TEST 1 - REAL DATA FROM UNIVERSITY URLS
	// http://www.cs.cmu.edu/afs/cs/project/theo-20/www/data/
	// ************************************************************************

	FILE * fr = fopen(fileName, "rt");
	char prefix[PREFIX_MAX_LENGTH];

	while (fgets(prefix, PREFIX_MAX_LENGTH, fr) != NULL) {

		count++;

		// create an RID out of the read prefix
		struct bloom xid;
		bloomify(&xid, prefix, prefixSizes);

		for (k = 0; k < 4; k++) {

			for (j = 0; j < 20; j++) {

				if ((xid.bf[j] & fwdEntries[k].bf[j]) != fwdEntries[k].bf[j])
					break;

				if (print) {

					(void) printf(
						"\t[%02d]: %d%d%d%d%d%d%d%d VS. %d%d%d%d%d%d%d%d\n",
						j,
						(int) ((fwdEntries[k].bf[j] & 0x80) >> 7),
						(int) ((fwdEntries[k].bf[j] & 0x40) >> 6),
						(int) ((fwdEntries[k].bf[j] & 0x20) >> 5),
						(int) ((fwdEntries[k].bf[j] & 0x10) >> 4),
						(int) ((fwdEntries[k].bf[j] & 0x08) >> 3),
						(int) ((fwdEntries[k].bf[j] & 0x04) >> 2),
						(int) ((fwdEntries[k].bf[j] & 0x02) >> 1),
						(int) ((fwdEntries[k].bf[j] & 0x01)),
						(int) ((xid.bf[j] & 0x80) >> 7),
						(int) ((xid.bf[j] & 0x40) >> 6),
						(int) ((xid.bf[j] & 0x20) >> 5),
						(int) ((xid.bf[j] & 0x10) >> 4),
						(int) ((xid.bf[j] & 0x08) >> 3),
						(int) ((xid.bf[j] & 0x04) >> 2),
						(int) ((xid.bf[j] & 0x02) >> 1),
						(int) ((xid.bf[j] & 0x01)));
				}
			}

			if (j == 20) {

				(void) printf("fwdEntries[%d]: hit for prefix %s",
						k,
						prefix);

				fwdCollisions[k]++;
			}
		}
	}

	(void) fclose(fr);

	for (k = 0; k < PREFIX_MAX_COUNT; k++) {

		if (prefixSizes[k] > 0)
			(void) printf(
					"prefixSizes[%d]: %d\n",
					k,
					prefixSizes[k]);
	}

	(void) printf(
					"\nTEST 1 RESULTS: %d URL-GENERATED XIDs FROM %s\n"\
					"------------------------------------------\n"\
					"%-3s\t| %-12s\t| %-12s\n"\
					"------------------------------------------\n",
					count,
					fileName,
					"fwdEntries[ ]", "hits", "rate");

	for (k = 0; k < 4; k++) {

		(void) printf(
				"fwdEntries[%d]\t| %-12d\t| %-.6f\n",
				k,
				fwdCollisions[k],
				(double) fwdCollisions[k] / (double) count);

		// reset variables for next test
		fwdCollisions[k] = 0;
	}

	// ************************************************************************
	// TEST 2 - RANDOMLY GENERATED XIDs
	// ************************************************************************

	count = 0, i = 0, j = 0, k = 0;

	unsigned char _xid[20];

	int controlPrefixesIndex = 0;
	int controlPrefixesShouldHit[4] = {};

	char controlPrefixes[][128] = {
		"uof/cs/r9x/r9001/sensors/light/0001",
		"uof/cs/r9x/r9001/sensors/light/0002",
		"uof/cs/r9x/r9001/sensors/temperature/0001",
		"uof/cs/r9x/r9001/sensors/temperature/0002",
		"uof/cs/r8x/r8001/sensors/temperature/0001",
		"uof/cs/r8x/r8001/sensors/temperature/0002",
		"uof/cs/r9x/r9001/sensors/",
		"uof/cs/r9x/r9001/computer/",
		"uof/cs/r8x/r8001/sensors/",
		"uof/cs/r8x/r8001/lights/"
	};

	if (argc > 5 && !(strncmp(argv[4], "-n", 2))) {

		count = atoi(argv[5]);

	} else {

		count = 100000;
	}

	int fd = open("/dev/urandom", O_RDONLY);

	for (i = 0; i < count; i++) {

		if ((i % 5) == 0) {

			controlPrefixesIndex = rand() % 10;

			struct bloom xid;
			bloomify(&xid, controlPrefixes[controlPrefixesIndex], prefixSizes);

			memcpy(_xid, xid.bf, 20);

			if (print)
				printf(
						"[%d] testing control prefix %s, size %d\n",
						i,
						controlPrefixes[controlPrefixesIndex],
						strlen(controlPrefixes[controlPrefixesIndex]));

		} else {

			assert(read(fd, _xid, 20) == 20);
		}

		for (k = 0; k < 4; k++) {

			for (j = 0; j < 20; j++) {

				if (print) {

					(void) printf(
						"\t[%02d][%02d]: [0x%02X VS 0x%02X] [%d%d%d%d%d%d%d%d VS. %d%d%d%d%d%d%d%d]\n",
						k,j,
						fwdEntries[k].bf[j],
						_xid[j],
						(int) ((fwdEntries[k].bf[j] & 0x80) >> 7),
						(int) ((fwdEntries[k].bf[j] & 0x40) >> 6),
						(int) ((fwdEntries[k].bf[j] & 0x20) >> 5),
						(int) ((fwdEntries[k].bf[j] & 0x10) >> 4),
						(int) ((fwdEntries[k].bf[j] & 0x08) >> 3),
						(int) ((fwdEntries[k].bf[j] & 0x04) >> 2),
						(int) ((fwdEntries[k].bf[j] & 0x02) >> 1),
						(int) ((fwdEntries[k].bf[j] & 0x01)),
						(int) ((_xid[j] & 0x80) >> 7),
						(int) ((_xid[j] & 0x40) >> 6),
						(int) ((_xid[j] & 0x20) >> 5),
						(int) ((_xid[j] & 0x10) >> 4),
						(int) ((_xid[j] & 0x08) >> 3),
						(int) ((_xid[j] & 0x04) >> 2),
						(int) ((_xid[j] & 0x02) >> 1),
						(int) ((_xid[j] & 0x01)));
				}

				if ((_xid[j] & fwdEntries[k].bf[j]) != fwdEntries[k].bf[j]) {

					if (print) {

						(void) printf(
								"\tdiff: [%02d][%02d]: [0x%02X VS 0x%02X (0x%02X)]\n",
								k,j,
								fwdEntries[k].bf[j],
								_xid[j],
								(_xid[j] & fwdEntries[k].bf[j]));
					}

					break;
				}
			}

			if (j == 20) {

				if (print)
					printf("[%d] hit for fwdEntries[%d]\n", i, k);

				fwdCollisions[k]++;

				if ((i % 5) == 0)
					controlPrefixesShouldHit[k]++;
			}
		}
	}

	(void) close(fd);

	(void) printf(
					"\nTEST 2 RESULTS: %d RANDOMLY GENERATED XIDs \n"\
					"--------------------------------------------------------------------------\n"\
					"%-3s\t| %-12s\t| %-12s\t| %-12s\t| %-12s\n"\
					"--------------------------------------------------------------------------\n",
					count,
					"fwdEntries[ ]", "expected", "actual", "real", "rate");

	for (k = 0; k < 4; k++) {

		(void) printf(
				"fwdEntries[%d]\t| %-12d\t| %-12d\t| %-12d\t| %-.6f\n",
				k,
				controlPrefixesShouldHit[k],
				fwdCollisions[k],
				(fwdCollisions[k] - controlPrefixesShouldHit[k]),
				((double) (fwdCollisions[k] - controlPrefixesShouldHit[k]) / (double) count));

		fwdCollisions[k] = 0;
		bloom_free(&fwdEntries[k]);
	}

	printf("\n");
}
