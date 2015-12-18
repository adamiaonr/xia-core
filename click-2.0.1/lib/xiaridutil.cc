// -*- c-basic-offset: 4; related-file-name: "../include/click/xiaridutil.hh" -*-
/*
 * xiaridutil.{cc,hh} -- RID class
 */

#include <click/xiaridutil.hh>

CLICK_DECLS

/**
 * \brief basic RID matching between 2 RIDs: fwd against req
 *
 * FIXME: this should be made 'better' (i.e. without a cycle), probably using
 * an inline method similar to the equality test of XIDs in xid.cc
 *
 * \arg req		the RID fwd should be matched against
 * \arg fwd		the RID to be matched against req
 *
 * \return	TRUE if fwd `RID-matches' req, FALSE otherwise.
 */
bool rid_match(const XID & req, const XID & fwd) {

	// XXX: here we assume that .type will be the same and equal to the RID
	// type
	for (int j = 0; j < CLICK_XIA_XID_ID_LEN; j++) {

		if ((req.xid().id[j] & fwd.xid().id[j]) != fwd.xid().id[j]) {

			return false;
		}
	}

	return true;
}

/**
 * \brief 	basic RID matching between 2 RIDs - fwd against req - on the
 * 			leftmost mask_bit_size bits only
 *
 * FIXME: this can be made MUCH better...
 *
 * \arg req				the RID fwd should be matched against
 * \arg fwd				the RID to be matched against req
 * \arg	mask_bit_size	the nr. of leftmost bits to use in the matching
 * 						operation
 *
 * \return	TRUE if fwd `RID-matches' req, FALSE otherwise
 */
bool rid_match_mask(const XID & req, const XID & fwd, uint8_t mask_bit_size) {

	// no mask sizes of size 0 or larger than (CLICK_XIA_XID_ID_LEN * 8)
	if (!(mask_bit_size) || mask_bit_size > (CLICK_XIA_XID_ID_LEN * 8))
		return false;

	// find on which 8 bit block does the bit mask end
	uint8_t endmask_block_pos = (CLICK_XIA_XID_ID_LEN - ((mask_bit_size - 1) / 8) - 1);

	// find the size of the bit mask within the last 8 bit block, keep
	// those bits untouched, clear the other rightmost bits
	uint8_t endmask_block = (~(~0 << ((mask_bit_size) % 8)) << (8 - ((mask_bit_size) % 8)));

	// if req and fwd don't match in the endmask_block, don't bother going on
	uint8_t r_endmask_block = (req.xid().id[endmask_block_pos] & endmask_block);
	uint8_t f_endmask_block = (fwd.xid().id[endmask_block_pos] & endmask_block);

	if ((r_endmask_block & f_endmask_block) != f_endmask_block) {

		return false;
	}

	for (int j = endmask_block_pos; j < CLICK_XIA_XID_ID_LEN; j++) {

		if ((req.xid().id[j] & fwd.xid().id[j]) != fwd.xid().id[j]) {

			return false;
		}
	}

	return true;
}

/**
 * \brief	builds an hex string representation of the leftmost
 * 			mask_bit_size bits of an RID
 *
 * \arg		to				char array to which the RID prefix should be
 * 							extracted
 * \arg		rid				the RID to be processed
 * \arg		mask_bit_size	nr. of leftmost bits to extratct from the given
 * 							RID
 *
 * \return	a pointer to a char array, with an hex string representation of
 * 			the extracted RID prefix
 */
char * rid_extract_prefix_bytes(
		char ** to,
		const XID & rid,
		uint8_t mask_bit_size) {

	// no mask sizes of size 0 or larger than (CLICK_XIA_XID_ID_LEN * 8)
	if (!(mask_bit_size) || mask_bit_size > (CLICK_XIA_XID_ID_LEN * 8))
		return NULL;

	// get the index of the byte which contains the key_bit
	int byte_index = (CLICK_XIA_XID_ID_LEN - ((mask_bit_size - 1) / 8) - 1), i;

	// aux char * which keeps a string representation of a byte
	char byte_str[3] = {0};

	for (i = (CLICK_XIA_XID_ID_LEN - 1); i >= byte_index; i--) {

		sprintf(byte_str, "%02X", rid.xid().id[i]);
		strncat(*to, byte_str, 2);

		memset(byte_str, 0, strlen(byte_str));
	}

	return (*to);
}

/**
 * \brief	calculates the Hamming Weight (HW), i.e. nr. of '1' bits of an RID
 *
 * FIXME: the use of __builtin_popcount() is gonna cause trouble...
 *
 * \arg		rid				the RID to be processed
 *
 * \return	the nr. of bits set to '1' in an RID
 */
int rid_calc_weight(const XID & rid) {

	int hw = 0, i = 0;

	for (i = 0; i < CLICK_XIA_XID_ID_LEN; i++)
		hw += __builtin_popcount(rid.xid().id[i]);

	return hw;
}

CLICK_ENDDECLS
