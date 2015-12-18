// -*- c-basic-offset: 4; related-file-name: "../../lib/xiaextheader.cc" -*-
#ifndef CLICK_RIDEXTHEADER_HH
#define CLICK_RIDEXTHEADER_HH

#include <click/string.hh>
#include <click/glue.hh>
#include <clicknet/xia.h>
#include <click/packet.hh>
#include <click/hashtable.hh>
#include <click/xiaheader.hh>
#include <click/xiaextheader.hh>

CLICK_DECLS

class RIDHeaderEncap;

class RIDHeader : public XIAGenericExtHeader {

	public:

	RIDHeader(const struct click_xia_ext * hdr) : XIAGenericExtHeader(hdr) {};
	RIDHeader(const Packet * p) : XIAGenericExtHeader(p) {};

	bool exists(uint8_t key) { return (_map.find(key)!=_map.end()); }

	uint8_t type() { if (!exists(TYPE)) return 0 ; return *(const uint8_t*)_map[TYPE].data();};
	uint32_t prefix_size() { if (!exists(PREFIX_SIZE)) return 0; return *(const uint32_t*)_map[PREFIX_SIZE].data();};

	enum { TYPE, PREFIX_SIZE};
	enum { RID_REQUEST = 1, RID_RESPONSE, RID_PUSH};

    static const char * TypeStr(uint8_t type);
};

class RIDHeaderEncap : public XIAGenericExtHeaderEncap {

	public:

	RIDHeaderEncap(uint8_t type, uint32_t prefix_size);

	static RIDHeaderEncap * MakeRequestHeader() {
		return new RIDHeaderEncap(RIDHeader::RID_REQUEST, 0);
	};

	static RIDHeaderEncap * MakeResponseHeader() {
		return new RIDHeaderEncap(RIDHeader::RID_RESPONSE, 0);
	};

	static RIDHeaderEncap * MakePushHeader() {
		return new RIDHeaderEncap(RIDHeader::RID_PUSH, 0);
	};
};

CLICK_ENDDECLS
#endif
