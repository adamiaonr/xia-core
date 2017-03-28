/*
 *  Copyright (c) 2012, Jyri J. Virkki
 *  All rights reserved.
 *
 *  This file is under BSD license. See LICENSE file.
 */

/*
 * Refer to bloom.h for documentation on the public interfaces.
 */

#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "bloom.h"
#include "murmurhash2.h"

static int bloom_check_add(struct bloom * bloom,
                           const void * buffer, int len, int add)
{
  if (bloom->ready == 0) {
    (void)printf("bloom at %p not initialized!\n", (void *)bloom);
    return -1;
  }

  int hits = 0;
  register unsigned int a = murmurhash2(buffer, len, 0x9747b28c);
  register unsigned int b = murmurhash2(buffer, len, a);
  register unsigned int x;
  register unsigned int i;
  register unsigned int byte;
  register unsigned int mask;
  register unsigned char c;

  for (i = 0; i < bloom->nHash; i++) {
    x = (a + i*b) % bloom->bitSize;
    byte = x >> 3;
    c = bloom->bf[byte];        // expensive memory access
    mask = 1 << (x % 8);

    if (c & mask) {
      hits++;
    } else {
      if (add) {
        bloom->bf[byte] = c | mask;
      }
    }
  }

  if (hits == bloom->nHash) {
    return 1;                   // 1 == element already in (or collision)
  }

  // +1 element in the filter
  bloom->entries++;

  return 0;
}

/**
 * @brief altered version of bloom_init(), originally proposed in
 * https://github.com/jvirkki/libbloom
 *
 * the initialization procedure is changed to fix the size of the bloom filter
 * to 160 bit.
 *
 */
int bloom_init(struct bloom * bloom)
{
  bloom->ready = 0;

//  if (entries < 1 || fPosProb == 0) {
//    return 1;
//  }

  // TODO: some of the following are ignored or changed, since we're fixing
  // the BF bit size to 160 bit. also, we're changing 'entries' to its true
  // meaning - 'maxEntries' - and leaving entries as a counter of elements in
  // the filter
  //bloom->entries = entries;
  bloom->entries = 0;
  bloom->maxEntries = BF_MAX_ELEMENTS;

//  bloom->fPosProb = fPosProb;
//
//  double num = log(bloom->fPosProb);
//  double denom = 0.480453013918201; // ln(2)^2
//  bloom->bpe = -(num / denom);

//  double dentries = (double) entries;

  // TODO: fixed BF bit size set here
  //bloom->bitSize = (int)(dentries * bloom->bpe);
  bloom->bitSize = BF_BIT_SIZE;

  if (bloom->bitSize % 8) {
    bloom->byteSize = (bloom->bitSize / 8) + 1;
  } else {
    bloom->byteSize = bloom->bitSize / 8;
  }

  // TODO: instead of using entries and fPosProb to determine bloom->nHash, we
  // use BF_MAX_ELEMENTS (set to 15 after reading Papalini et al. 2014,
  // http://doi.acm.org/10.1145/2660129.2660155). 
  // this results in nHash = 8.
  
  //bloom->nHash = (int) ceil(0.693147180559945 * bloom->bpe);  // ln(2)
  bloom->nHash =
		  (int) ceil(0.693147180559945 * ((double) bloom->bitSize / (double) BF_MAX_ELEMENTS));

  bloom->bf = (unsigned char *) calloc(bloom->byteSize, sizeof(unsigned char));

  // TODO: false positive probability and other calculations go here
  double dentries = (double) bloom->maxEntries;
  double dhashes = (double) bloom->nHash;
  double dbits = (double) bloom->bitSize;

  bloom->fPosProb = pow(1.0 - exp(((-dhashes) * dentries) / dbits), dhashes);

  double num = log(bloom->fPosProb);
  double denom = 0.480453013918201; // ln(2)^2
  bloom->bpe = -(num / denom);

  if (bloom->bf == NULL) {
    return 1;
  }

  bloom->ready = 1;
  return 0;
}


int bloom_check(struct bloom * bloom, const void * buffer, int len)
{
  return bloom_check_add(bloom, buffer, len, 0);
}


int bloom_add(struct bloom * bloom, const void * buffer, int len)
{
  return bloom_check_add(bloom, buffer, len, 1);
}


void bloom_print(struct bloom * bloom)
{
  (void)printf("bloom at %p\n", (void *)bloom);
  (void)printf(" ->entries = %d\n", bloom->entries);
  (void)printf(" ->fPosProb = %f\n", bloom->fPosProb);
  (void)printf(" ->bitSize = %d\n", bloom->bitSize);
  (void)printf(" ->bitSize per elem = %f\n", bloom->bpe);
  (void)printf(" ->byteSize = %d\n", bloom->byteSize);
  (void)printf(" ->hash functions = %d\n", bloom->nHash);

  int i;

  (void)printf(" ->raw filter:\n");

  for (i = 0; i < bloom->byteSize; i++) {
	  (void)printf(
			  "\t[%02d]: [0x%02X] [%d%d%d%d%d%d%d%d]\n",
			  i,
			  bloom->bf[i],
			  (int) ((bloom->bf[i] & 0x80) >> 7),
			  (int) ((bloom->bf[i] & 0x40) >> 6),
			  (int) ((bloom->bf[i] & 0x20) >> 5),
			  (int) ((bloom->bf[i] & 0x10) >> 4),
			  (int) ((bloom->bf[i] & 0x08) >> 3),
			  (int) ((bloom->bf[i] & 0x04) >> 2),
			  (int) ((bloom->bf[i] & 0x02) >> 1),
			  (int) ((bloom->bf[i] & 0x01)));
  }
}


void bloom_free(struct bloom * bloom)
{
  if (bloom->ready) {
    free(bloom->bf);
  }
  bloom->ready = 0;
}
