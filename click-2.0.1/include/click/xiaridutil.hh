// -*- c-basic-offset: 4; related-file-name: "../../lib/xiaridutil.cc" -*-
#ifndef CLICK_XIARIDUTIL_HH
#define CLICK_XIARIDUTIL_HH

#include <click/config.h>
#include <click/glue.hh>
#include <click/xid.hh>
#include <click/confparse.hh>
#include <click/straccum.hh>
#include <click/integers.hh>
#include <click/standard/xiaxidinfo.hh>
#include <click/args.hh>
#include <click/xiautil.hh>
#include <click/string.hh>
#include <click/glue.hh>

#include <clicknet/xia.h>

CLICK_DECLS

extern bool rid_match(const XID & req, const XID & fwd);
extern bool rid_match_mask(
		const XID & req,
		const XID & fwd,
		uint8_t mask_bit_size);

extern char * rid_extract_prefix_bytes(
		char ** to,
		const XID & rid,
		uint8_t mask_bit_size);

extern int rid_calc_weight(const XID & rid);

CLICK_ENDDECLS
#endif
