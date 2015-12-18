// -*- related-file-name: "../include/click/xiaridheader.hh" -*-
/*
 */

#include <click/config.h>
#include <click/string.hh>
#include <click/glue.hh>
#include <click/xiaextheader.hh>
#include <click/xiaridheader.hh>

#if CLICK_USERLEVEL
# include <unistd.h>
#endif

CLICK_DECLS

RIDHeaderEncap::RIDHeaderEncap(uint8_t type, uint32_t prefix_size) {

	this->map()[RIDHeader::TYPE]= String((const char *) & type, sizeof(type));
	this->map()[RIDHeader::PREFIX_SIZE]= String((const char *) & prefix_size, sizeof(prefix_size));

	this->update();
}

const char * RIDHeader::TypeStr(uint8_t type) {

	const char * t;

	switch (type) {

		case RID_REQUEST:
			t = "RID REQUEST";
			break;

		case RID_RESPONSE:
			t = "RID RESPONSE";
			break;

		case RID_PUSH:
			t = "PUSH";
			break;
	}

	return t;
}

CLICK_ENDDECLS
