/*
 * xiaridroutetable.{cc,hh} -- simple XID routing table for new RID type.
 * The code is almost the same as XIARIDRouteTable.cc
 */
#include <click/config.h>
#include <click/glue.hh>
#include <click/error.hh>
#include <click/confparse.hh>
#include <click/packet_anno.hh>
#include <click/xiaheader.hh>

#include "xiaridroutetable.hh"

#if CLICK_USERLEVEL
#include <fstream>
#include <stdlib.h>
#endif

CLICK_DECLS

XIARIDRouteTable::XIARIDRouteTable(): _drops(0)
{
}

XIARIDRouteTable::~XIARIDRouteTable()
{
	_rid_fwrdng_tbl.clear();
}

int XIARIDRouteTable::configure(Vector<String> &conf, ErrorHandler *errh)
{
	click_chatter("XIARIDRouteTable: configuring %s\n", this->name().c_str());

	_principal_type_enabled = 1;
	// define a default route
	_rid_default_route.set_params(-1, 0, NULL);

	return 0;
}

int
XIARIDRouteTable::set_enabled(int e)
{
	_principal_type_enabled = e;
	return 0;
}

int XIARIDRouteTable::get_enabled()
{
	return _principal_type_enabled;
}

void
XIARIDRouteTable::add_handlers()
{
	add_write_handler("add", set_handler, 0);
	add_write_handler("set", set_handler, (void*)1);
	add_write_handler("add4", set_handler4, 0);
	add_write_handler("set4", set_handler4, (void*)1);
	add_write_handler("remove", remove_handler, 0);
	add_write_handler("load", load_routes_handler, 0);
	add_write_handler("generate", generate_routes_handler, 0);
	add_data_handlers("drops", Handler::OP_READ, &_drops);
	add_read_handler("list", list_routes_handler, 0);
	add_write_handler("enabled", write_handler, (void *)PRINCIPAL_TYPE_ENABLED);
	add_read_handler("enabled", read_handler, (void *)PRINCIPAL_TYPE_ENABLED);
}

String
XIARIDRouteTable::read_handler(Element *e, void *thunk)
{
	XIARIDRouteTable *t = (XIARIDRouteTable *) e;
    switch ((intptr_t)thunk) {
		case PRINCIPAL_TYPE_ENABLED:
			return String(t->get_enabled());

		default:
			return "<error>";
    }
}

int
XIARIDRouteTable::write_handler(const String &str, Element *e, void *thunk, ErrorHandler * /*errh*/)
{
	XIARIDRouteTable *t = (XIARIDRouteTable *) e;
    switch ((intptr_t)thunk) {
		case PRINCIPAL_TYPE_ENABLED:
			return t->set_enabled(atoi(str.c_str()));

		default:
			return -1;
    }
}

/**
 * \brief	prints routing data (XIARIDRouteTable::XIARouteData) of a
 * 			forwarding entry.
 *
 * \arg		route_data	a pointer to a XIARIDRouteTable::XIARouteData object,
 * 						the forwarding entry's route data to print.
 *
 * \return	a String with the forwarding entry's routing data, formatted as
 * 			specified in the print function.
 */
String XIARIDRouteTable::print_data(XIARIDRouteTable::XIARouteData * route_data) {

	click_chatter("XIARIDRouteTable::print_data() : [ENTER]\n");

	String entry;

	if (route_data == NULL) {

		click_chatter("XIARIDRouteTable::print_data() : NULL route\n");
		entry = String("N/A");

	} else {

		click_chatter("XIARIDRouteTable::print_data() : RID route params :"\
			"\n\t[PORT] : %d"\
			"\n\t[NEXT HOP] : %s"\
			"\n\t[FLAGS] : %d\n", 
			route_data->get_port(), 
			(route_data->get_next_hop() != NULL ? route_data->get_next_hop()->unparse().c_str() : "N/A"),
			route_data->get_flags());

		entry = String(route_data->get_port())
			+ "," + (route_data->get_next_hop() != NULL ? route_data->get_next_hop()->unparse() : "N/A")
			+ "," + String(route_data->get_flags());
	}

	return entry;
}

String XIARIDRouteTable::list_routes_handler(
		Element * e,
		void * /*thunk */)
{
	XIARIDRouteTable * table = static_cast<XIARIDRouteTable*>(e);
	// XIARouteData * xrd = new XIARouteData(table->_rid_default_route);

	click_chatter("XIARIDRouteTable::list_routes_handler() : [ENTER] listing RID routes\n");

	// get the default route
	// String tbl = "-," + print_data(xrd) + "\n";
	// click_chatter("XIARIDRouteTable::list_routes_handler() : default route : %s\n", tbl.c_str());
	String tbl = "";

	// get the rest
	HashTable<unsigned, XIARIDPatricia<XIARouteData> *>::iterator it =
			table->_rid_fwrdng_tbl.begin();

	char * header = (char *) calloc(1024, sizeof(char));
	while (it != table->_rid_fwrdng_tbl.end()) {

		if (it.value()) {
		
			click_chatter("XIARIDRouteTable::list_routes_handler() : PATRICIA trie w/ HW = %d\n", it.key());

			sprintf(header,
					"--------------------------------------------------------------------------\n"\
					"%-12s\t| %-12s\t\n"\
					"--------------------------------------------------------------------------\n"\
					"%-12d\t| %-12d\t\n",
					"HW", "NR. ENTRIES",
					it.key(), it.value()->count());

			tbl += header;
			tbl += it.value()->print(PRE_ORDER, FWD_ENTRY_STR_MAX_SIZE, XIARIDRouteTable::print_data);
			// clear header
			memset(header, '\0', 1024);
		}

		it++;
	}

	return tbl;
}

int XIARIDRouteTable::set_handler(
		const String & conf,
		Element * e,
		void * thunk,
		ErrorHandler * errh) {

	// handle older style route entries
	String str_copy = conf;
	String xid_str = cp_shift_spacevec(str_copy);

	if (xid_str.length() == 0)
	{
		// ignore empty entry
		return 0;
	}

	int port;
	if (!cp_integer(str_copy, &port))
		return errh->error("invalid port: ", str_copy.c_str());

	String str = xid_str + "," + String(port) + ",,0";

	return set_handler4(str, e, thunk, errh);
}

int XIARIDRouteTable::set_handler4(
		const String & conf,
		Element * e,
		void * thunk,
		ErrorHandler * errh) {

	XIARIDRouteTable * table = static_cast<XIARIDRouteTable*>(e);

	bool add_mode = !thunk;

	Vector<String> args;
	int port = 0;
	unsigned flags = 0;
	String xid_str;
	XID * nexthop = NULL;

	cp_argvec(conf, args);

	if (args.size() < 2 || args.size() > 4) {
		return errh->error("invalid route: ", conf.c_str());
	}

	xid_str = args[0];

	if (!cp_integer(args[1], &port)) {
		return errh->error("invalid port: ", conf.c_str());
	}

	if (args.size() == 4) {
		if (!cp_integer(args[3], &flags)) {
			return errh->error("invalid flags: ", conf.c_str());
		}
	}

	if (args.size() >= 3 && args[2].length() > 0) {
	    String nxthop = args[2];
		nexthop = new XID;
		cp_xid(nxthop, nexthop, e);
		//nexthop = new XID(args[2]);
		if (!nexthop->valid()) {
			delete nexthop;
			return errh->error("invalid next hop xid: ", conf.c_str());
		}
	}

	// XXX: set the default route
	if (xid_str == "-") {

		if (add_mode && table->_rid_default_route.get_port() != -1) {
			return errh->error("duplicate default route: ", xid_str.c_str());
		}

		table->_rid_default_route.set_params(port, flags, nexthop);

	} else {

		// XXX: other routes. the insertion operation is a bit diff here...

		// 1) extract the RID
		XID rid;

		if (!(cp_xid(xid_str, &rid, e))) {
			if (nexthop) delete nexthop;
			return errh->error("invalid XID: ", xid_str.c_str());
		}

		// 2) find the Hamming Weight (HW) of the RID
		int hw = rid_calc_weight(rid);
		click_chatter("XIARIDRouteTable::set_handler4() : adding RID entry (%s) to PT[%d]\n", args[0].c_str(), hw);

		// 3) is _rid_fwrdng_tbl[hw] empty?
		if (!(table->_rid_fwrdng_tbl[hw])) {

			// 3.1) if yes, create an new RID PATRICIA Trie (PT) root entry
			XIARIDPatricia<XIARouteData> * root =
					new XIARIDPatricia<XIARouteData>(XIARIDPatricia<XIARouteData>::ROOT_NODE);

			// 3.2) ... and add it to _rid_fwrdng_tbl[hw]
			table->_rid_fwrdng_tbl[hw] = root;
		}

		// 4) insert the RID in the _rid_fwrdng_tbl[hw] : the RID PT insert()
		// operation already includes a duplicate RID check
		XIARouteData * xrd = new XIARouteData(port, flags, nexthop);

		if (!(table->_rid_fwrdng_tbl[hw]->insert(rid, xrd))) {

			if (nexthop)
				delete nexthop;

			delete xrd;
			return errh->error("duplicate XID: ", xid_str.c_str());
		}
	}

	return 0;
}

int XIARIDRouteTable::remove_handler(
		const String & xid_str,
		Element * e,
		void *,
		ErrorHandler * errh) {

	XIARIDRouteTable * table = static_cast<XIARIDRouteTable *>(e);

	if (xid_str.length() == 0) {

		// ignore empty entry
		return 0;
	}

	if (xid_str == "-") {

		table->_rid_default_route.set_params(-1, 0, NULL);

	} else {

		// XXX: other routes. the removal operation is a bit diff here...

		// 1) extract the RID
		XID rid;

		if (!(cp_xid(xid_str, &rid, e)))
			return errh->error("invalid XID: ", xid_str.c_str());

		// 2) find the Hamming Weight (HW) of the RID
		int hw = rid_calc_weight(rid);

		// 3) is _rid_fwrdng_tbl[hw] empty?
		if (!(table->_rid_fwrdng_tbl[hw])) {

			return errh->error("invalid XID: ", xid_str.c_str());
		}

		// 4) remove the RID from _rid_fwrdng_tbl[hw] : the RID PT remove()
		// operation already includes a 'non-existence's check
		if ((table->_rid_fwrdng_tbl[hw]->remove(rid)) < 1) {

			return errh->error("nonexistent XID: ", xid_str.c_str());
		}
	}

	return 0;
}

int XIARIDRouteTable::load_routes_handler(
		const String & conf,
		Element * e,
		void *,
		ErrorHandler * errh)
{
#if CLICK_USERLEVEL
	std::ifstream in_f(conf.c_str());
	if (!in_f.is_open())
	{
		errh->error("could not open file: %s", conf.c_str());
		return -1;
	}

	int c = 0;
	while (!in_f.eof())
	{
		char buf[1024];
		in_f.getline(buf, sizeof(buf));

		if (strlen(buf) == 0)
			continue;

		if (set_handler(buf, e, 0, errh) != 0)
			return -1;

		c++;
	}
	click_chatter("loaded %d entries", c);

	return 0;
#elif CLICK_LINUXMODLE
	int c = 0;
	char buf[1024];

	mm_segment_t old_fs = get_fs();
	set_fs(KERNEL_DS);
	struct file * filp = file_open(conf.c_str(), O_RDONLY, 0);
	if (filp==NULL)
	{
		errh->error("could not open file: %s", conf.c_str());
		return -1;
	}
	loff_t file_size = vfs_llseek(filp, (loff_t)0, SEEK_END);
	loff_t curpos = 0;
	while (curpos < file_size)	{
	file_read(filp, curpos, buf, 1020);
	char * eol = strchr(buf, '\n');
	if (eol==NULL) {
			click_chatter("Error at %s %d\n", __FUNCTION__, __LINE__);
		break;
	}
	curpos+=(eol+1-buf);
		eol[1] = '\0';
		if (strlen(buf) == 0)
			continue;

		if (set_handler(buf, e, 0, errh) != 0) {
			click_chatter("Error at %s %d\n", __FUNCTION__, __LINE__);
			return -1;
	}
		c++;
	}
	set_fs(old_fs);

	click_chatter("XIA routing table loaded %d entries", c);
	return 0;
#endif
}

int XIARIDRouteTable::generate_routes_handler(
		const String & conf,
		Element * e,
		void *,
		ErrorHandler * errh)
{
#if CLICK_USERLEVEL
	XIARIDRouteTable* table = dynamic_cast<XIARIDRouteTable*>(e);
#else
	XIARIDRouteTable* table = reinterpret_cast<XIARIDRouteTable*>(e);
#endif
	assert(table);

	String conf_copy = conf;

	String xid_type_str = cp_shift_spacevec(conf_copy);
	uint32_t xid_type;
	if (!cp_xid_type(xid_type_str, &xid_type))
		return errh->error("invalid XID type: ", xid_type_str.c_str());

	String count_str = cp_shift_spacevec(conf_copy);
	int count;
	if (!cp_integer(count_str, &count))
		return errh->error("invalid entry count: ", count_str.c_str());

	String port_str = cp_shift_spacevec(conf_copy);
	int port;
	if (!cp_integer(port_str, &port))
		return errh->error("invalid port: ", port_str.c_str());

#if CLICK_USERLEVEL
	unsigned short xsubi[3];
	xsubi[0] = 1;
	xsubi[1] = 2;
	xsubi[2] = 3;
//	unsigned short xsubi_next[3];
#else
	struct rnd_state state;
	prandom32_seed(&state, 1239);
#endif

	struct click_xia_xid xid_d;
	xid_d.type = xid_type;

	if (port<0) click_chatter("Random %d ports", -port);

	for (int i = 0; i < count; i++)
	{
		uint8_t* xid = xid_d.id;
		const uint8_t* xid_end = xid + CLICK_XIA_XID_ID_LEN;
#define PURE_RANDOM
#ifdef PURE_RANDOM
		uint32_t seed = i;
		memcpy(&xsubi[1], &seed, 2);
		memcpy(&xsubi[2], &(reinterpret_cast<char *>(&seed)[2]), 2);
		xsubi[0]= xsubi[2]+ xsubi[1];
#endif

		while (xid != xid_end)
		{
#if CLICK_USERLEVEL
#ifdef PURE_RANDOM
			*reinterpret_cast<uint32_t*>(xid) = static_cast<uint32_t>(nrand48(xsubi));
#else
			*reinterpret_cast<uint32_t*>(xid) = static_cast<uint32_t>(nrand48(xsubi));
#endif
#else
			*reinterpret_cast<uint32_t*>(xid) = static_cast<uint32_t>(prandom32(&state));
			if (i%5000==0)
				click_chatter("random value %x", *reinterpret_cast<uint32_t*>(xid));
#endif
			xid += sizeof(uint32_t);
		}

		/* random generation from 0 to |port|-1 */
		XIARouteData * xrd = new XIARouteData();
//		xrd->flags = 0;
//		xrd->nexthop = NULL;

		if (port<0) {
#if CLICK_LINUXMODULE
			u32 random = random32();
#else
			int random = rand();
#endif
			random = random % (-port);
//			xrd->port = random;

			xrd->set_params(random, 0, NULL);

			if (i%5000 == 0)
				click_chatter("Random port for XID %s #%d: %d ", XID(xid_d).unparse_pretty(e).c_str(), i, random);
		} else {

			//xrd->port = port;
			xrd->set_params(port, 0, NULL);
		}

		// XXX: RID PT node insertion...

		// 1) RID
		XID rid = XID(xid_d);

		// 2) find the Hamming Weight (HW) of the RID
		int hw = rid_calc_weight(rid);

		// 3) is _rid_fwrdng_tbl[hw] empty?
		if (!(table->_rid_fwrdng_tbl[hw])) {

			// 3.1) if it is, create an new RID PATRICIA Trie (PT) root entry
			XIARIDPatricia<XIARouteData> * root =
					new XIARIDPatricia<XIARouteData>();

			// 3.2) ... and add it to _rid_fwrdng_tbl[hw]
			table->_rid_fwrdng_tbl[hw] = root;
		}

		// 3) insert the RID in the _rid_fwrdng_tbl[hw] : the RID PT insert()
		// operation already includes a duplicate RID check

		if (!(table->_rid_fwrdng_tbl[hw]->insert(rid, xrd))) {

			delete xrd;
			click_chatter("duplicate XID (not added): ", rid.unparse().c_str());
		}
	}

	click_chatter("generated %d entries", count);
	return 0;
}

void XIARIDRouteTable::push(int in_ether_port, Packet * p)
{
    // int port;
	in_ether_port = XIA_PAINT_ANNO(p);

	if (!_principal_type_enabled) {

		output(2).push(p);
		return;
	}

	// XXX: lookup_route() may now result in multiple matches due to
	// RID FPs. we now 'jam' the code that would normally follow (e.g.
	// as in xiaxidroutetable.cc) into lookup_route().
	lookup_route(in_ether_port, p);
}

void XIARIDRouteTable::forward(
		int port,
		int in_ether_port,
		Packet * p) {

	click_chatter("XIARIDRouteTable::forward() : [ENTER] forward over port %d", port);

	if (port >= 0) {

		SET_XIA_PAINT_ANNO(p,port);
		output(0).push(p);

	} else if (port == DESTINED_FOR_LOCALHOST) {

		output(1).push(p);

/*	} else if (port == DESTINED_FOR_DHCP) {

		SET_XIA_PAINT_ANNO(p,port);
		output(3).push(p);

	} else if (port == DESTINED_FOR_BROADCAST) {

		for(int i = 0; i <= _num_ports; i++) {

			Packet * q = p->clone();
			SET_XIA_PAINT_ANNO(q,i);
			//q->set_anno_u8(PAINT_ANNO_OFFSET,i);
			output(0).push(q);
		}

		p->kill();*/

	} else {
		//SET_XIA_PAINT_ANNO(p,UNREACHABLE);

		//p->set_anno_u8(PAINT_ANNO_OFFSET,UNREACHABLE);

		// no match -- discard packet
		// Output 9 is for dropping packets.
		// let the routing engine handle the dropping.
		//_drops++;
		//if (_drops == 1)
		//      click_chatter("Dropping a packet with no match (last message)\n");
		//  p->kill();
		output(2).push(p);
	}
}

/**
 *
 */
void XIARIDRouteTable::handle_unicast_packet(
		void * obj_ptr,
		XIARIDRouteTable::XIARouteData * data) {

	click_chatter("XIARIDRouteTable::handle_unicast_packet : [ENTER]\n");

	XIARIDRouteTable * ridtbl_obj_ptr = (XIARIDRouteTable *) obj_ptr;

	if(data->get_port() != DESTINED_FOR_LOCALHOST
			&& data->get_port() != FALLBACK
			&& data->get_next_hop() != NULL) {

		ridtbl_obj_ptr->_curr_p->set_nexthop_neighbor_xid_anno(*(data->get_next_hop()));
	}

	ridtbl_obj_ptr->forward(
			data->get_port(),
			ridtbl_obj_ptr->_curr_in_ether_port,
			ridtbl_obj_ptr->_curr_p);

	click_chatter("XIARIDRouteTable::handle_unicast_packet : [EXIT]\n");
}

// /**
//  *
//  */
// int XIARIDRouteTable::process_xcmp_redirect(Packet * p) {

// 	XIAHeader hdr(p->xia_header());
// 	const uint8_t * pay = hdr.payload();
// 	XID * dest, * newroute;

// 	dest = new XID((const struct click_xia_xid &)(pay[4]));
// 	newroute = new XID((const struct click_xia_xid &)(pay[4 + sizeof(struct click_xia_xid)]));

// 	// route update (dst, out, newroute, )
// 	int hw = rid_calc_weight(*dest);
// 	XIARouteData * _route_data = NULL;

// 	if ((_route_data = _rid_fwrdng_tbl[hw]->search(*dest)->get_data())
// 			!= NULL) {

// 		_route_data->set_next_hop(newroute);

// 	} else {

// 		// make a new entry for this XID
// 		int port = _rid_default_route.get_port();

// 		if(strstr(_local_addr.unparse().c_str(), dest->unparse().c_str())) {

// 			port = DESTINED_FOR_LOCALHOST;
// 		}

// 		XIARouteData * xrd1 = new XIARouteData(port, 0, newroute);
// 		_rid_fwrdng_tbl[hw]->insert(*dest, xrd1);
// 	}

// 	return -1;
// }

/**
 *
 */
void XIARIDRouteTable::lookup_route(int in_ether_port, Packet * p) {

	click_chatter("XIARIDRouteTable::lookup_route() : [ENTER] time for the router to... uh... 'find a way!'\n");

	const struct click_xia * hdr = p->xia_header();
	int last = hdr->last;

	if (last < 0)
		last += hdr->dnode;

	const struct click_xia_xid_edge * edge = hdr->node[last].edge;
	const struct click_xia_xid_edge & current_edge = edge[XIA_NEXT_PATH_ANNO(p)];

	const int & idx = current_edge.idx;

	if (idx == CLICK_XIA_XID_EDGE_UNUSED) {

		// unused edge -- use default route
		this->forward(
				this->_rid_default_route.get_port(),
				in_ether_port,
				p);
	}

	// XXX: UNICAST packet (w/ RID forwarding)
	const struct click_xia_xid_node & node = hdr->node[idx];

	// starting at the hamming weight of the RID request (say HW), we 
	// lookup the RID in the PATRICIA tries w/ hw = {HW, HW - 1, ..., 1}

	// extract the RID in the request
	XID rid = XID(node.xid);
	// find out the RID's hw
	int hw = rid_calc_weight(rid);
	// keep track of the total number of matches
	int num_matches = 0;

	// load the local _curr_p and _curr_in_ether_port 'registers' with
	// the info of the packet being currently handled...
	// FIXME: temporary fix <- is ugly
	_curr_p = p;
	_curr_in_ether_port = in_ether_port;

	// let XIARIDPatricia::lookup() do the work...
	// ... with the help of the handle_unicast_packet() as callback
	for (; hw > 0; hw--) {

		if (!_rid_fwrdng_tbl[hw]) {

			click_chatter("XIARIDRouteTable::lookup_route() : no PATRICIA trie for hw = %d. skipping...\n", hw);

		} else {

			// run the PATRICIA trie lookup for hw
			click_chatter("XIARIDRouteTable::lookup_route() : initializing PATRICIA lookup for hw = %d \n", hw);
			num_matches += _rid_fwrdng_tbl[hw]->lookup(
				rid,
				-1,
				XIARIDRouteTable::handle_unicast_packet,
				(void *) this);
		}
	}

	click_chatter("XIARIDRouteTable::lookup_route() : lookup is finished!\n");

	// if no matches, use default route
	if (num_matches == 0) {

		click_chatter("XIARIDRouteTable::lookup_route() : no matches (%d). using default route...\n", num_matches);

		if(_rid_default_route.get_port() != DESTINED_FOR_LOCALHOST
				&& _rid_default_route.get_port() != FALLBACK
				&& _rid_default_route.get_next_hop() != NULL) {

			p->set_nexthop_neighbor_xid_anno(*(_rid_default_route.get_next_hop()));
		}

		this->forward(
				this->_rid_default_route.get_port(),
				in_ether_port,
				p);
	}
/*	}*/
}

CLICK_ENDDECLS
EXPORT_ELEMENT(XIARIDRouteTable)
ELEMENT_MT_SAFE(XIARIDRouteTable)
