#ifndef CLICK_XIARIDROUTETABLE_HH
#define CLICK_XIARIDROUTETABLE_HH

#include <click/element.hh>
#include <click/hashtable.hh>
#include <click/xid.hh>
#include <click/xiapath.hh>
#include <click/xiaridpatricia.hh>
#include <click/xiaridutil.hh>
#include <clicknet/xia.h>

#include "xcmp.hh"

CLICK_DECLS

#define FWD_ENTRY_STR_MAX_SIZE 1024

/*
FIXME: this is somewhat stupid, but i don't know what does this commenting
syntax means....

=c
XIARIDRouteTable(XID1 OUT1, XID2 OUT2, ..., - OUTn)

=s ip
simple XID routing table

=d
Routes XID according to a routing table.

=e

XIARIDRouteTable(AD0 0, HID2 1, - 2)
It outputs AD0 packets to port 0, HID2 packets to port 1, and other packets to port 2.
If the packet has already arrived at the destination node, the packet will be destroyed,
so use the XIACheckDest element before using this element.

=a StaticIPLookup, IPRouteTable
*/

class XIARIDRouteTable : public Element {

	private:

		/**
		 * \brief	(alternative) route info for a forwarding table entry
		 */
		class XIARouteData {

			public:

				XIARouteData() {

					this->port = 0;
					this->flags = 0;
					this->nexthop = NULL;
				}

				XIARouteData(int port, unsigned flags, XID * nexthop) {

					this->port = port;
					this->flags = flags;
					this->nexthop = nexthop;
				}

				XIARouteData(XIARouteData * data) {

					this->port = data->port;
					this->flags = data->flags;
					this->nexthop = data->nexthop;
				}

				~XIARouteData() { delete this->nexthop; }

				void set_port(int port) { this->port = port; }
				void get_flags(unsigned flags) { this->flags = flags; }
				void set_next_hop(XID * nexthop) {

					if(this->nexthop) { delete nexthop; }
					this->nexthop = nexthop;
				}

				void set_params(int port, unsigned flags, XID * nexthop) {

					this->port = port;
					this->flags = flags;

					// XXX: nexthop must be handled differently
					if(this->nexthop) { delete nexthop; }
					this->nexthop = nexthop;
				}

				int get_port() { return this->port; }
				unsigned get_flags() { return this->flags; }
				XID * get_next_hop() { return this->nexthop; }

			private:

				int	port;
				unsigned flags;
				XID * nexthop;
		};

	public:

		XIARIDRouteTable();
		~XIARIDRouteTable();

		const char *class_name() const		{ return "XIARIDRouteTable"; }
		const char *port_count() const		{ return "-/-"; }
		const char *processing() const		{ return PUSH; }

		int configure(Vector<String> &, ErrorHandler *);
		void add_handlers();

		void push(int in_ether_port, Packet *);

		int set_enabled(int e);
		int get_enabled();

	protected:

		void lookup_route(int in_ether_port, Packet *);
		void forward(int port, int in_ether_port, Packet *);
		int process_xcmp_redirect(Packet *);

		static int set_handler(const String & conf, Element * e, void * thunk, ErrorHandler * errh);
		static int set_handler4(const String &conf, Element *e, void *thunk, ErrorHandler *errh);
		static int remove_handler(const String &conf, Element *e, void *, ErrorHandler *errh);
		static int load_routes_handler(const String &conf, Element *e, void *, ErrorHandler *errh);
		static int generate_routes_handler(const String &conf, Element *e, void *, ErrorHandler *errh);
		static int write_handler(const String &str, Element *e, void *thunk, ErrorHandler *errh);

		static String read_handler(Element *e, void *thunk);
		static String list_routes_handler(Element *e, void *thunk);

		// XXX: callback functions to be used by XIARIDPatricia
		static String print_data(XIARIDRouteTable::XIARouteData * route_data);
		static void handle_unicast_packet(
				void * obj_ptr,
				XIARIDRouteTable::XIARouteData * data);

	private:

		// XXX: forwarding table composed by several Patricia Tries
		// organized by Hamming Weight (HW), i.e. nr. of bits in RID, in an Hash
		// Table (as in Papalini et al. 2014).
		HashTable<unsigned, XIARIDPatricia<XIARouteData> *> _rid_fwrdng_tbl;
		XIARouteData _rid_default_route;

		uint32_t _drops;

		int _principal_type_enabled;
		int _num_ports;
		XIAPath _local_addr;
		XID _local_hid;
		XID _bcast_xid;

		// FIXME: temporary fix to make RIDs work <- is ugly
		// we need these because the handle_unicast() method, used as callback
		// by the XIARIDPatricia class, needs access to the packet being
		// currently forwarded and some port information
		Packet * _curr_p;
		int _curr_in_ether_port;
};

CLICK_ENDDECLS
#endif
