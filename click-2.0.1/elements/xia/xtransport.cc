#include "../../userlevel/xia.pb.h"
#include <click/config.h>
#include <click/glue.hh>
#include <click/error.hh>
#include <click/error-syslog.hh>
#include <click/confparse.hh>
#include <click/packet_anno.hh>
#include <click/packet.hh>
#include <click/vector.hh>

#include <click/xiacontentheader.hh>
#include "xiatransport.hh"
#include "xtransport.hh"
#include <click/xiatransportheader.hh>

/*
** FIXME:
** - implement a backoff delay on retransmits so we don't flood the connection
** - fix cid header size issue so we work correctly with the linux version
** - migrate from uisng printf and click_chatter to using the click ErrorHandler class
** - there are still some small memory leaks happening when stream sockets are created/used/closed
**   (problem does not happen if sockets are just opened and closed)
** - fix issue in SYN code with XIDPairToConnectPending (see comment in code for details)
*/

CLICK_DECLS

XTRANSPORT::XTRANSPORT()
	: _timer(this)
{
	GOOGLE_PROTOBUF_VERIFY_VERSION;
	_id = 0;
	isConnected = false;

	_ackdelay_ms = ACK_DELAY;
	_teardown_wait_ms = TEARDOWN_DELAY;

//	pthread_mutexattr_init(&_lock_attr);
//	pthread_mutexattr_settype(&_lock_attr, PTHREAD_MUTEX_RECURSIVE);
//	pthread_mutex_init(&_lock, &_lock_attr);

	cp_xid_type("SID", &_sid_type);
}


int
XTRANSPORT::configure(Vector<String> &conf, ErrorHandler *errh)
{
	XIAPath local_addr;
	XID local_4id;
	Element* routing_table_elem;
	bool is_dual_stack_router;
	_is_dual_stack_router = false;

	if (cp_va_kparse(conf, this, errh,
					 "LOCAL_ADDR", cpkP + cpkM, cpXIAPath, &local_addr,
					 "LOCAL_4ID", cpkP + cpkM, cpXID, &local_4id,
					 "ROUTETABLENAME", cpkP + cpkM, cpElement, &routing_table_elem,
					 "IS_DUAL_STACK_ROUTER", 0, cpBool, &is_dual_stack_router,
					 cpEnd) < 0)
		return -1;

	_local_addr = local_addr;
	_local_hid = local_addr.xid(local_addr.destination_node());
	_local_4id = local_4id;
	// IP:0.0.0.0 indicates NULL 4ID
	_null_4id.parse("IP:0.0.0.0");

	_is_dual_stack_router = is_dual_stack_router;

	/*
	// If a valid 4ID is given, it is included (as a fallback) in the local_addr
	if(_local_4id != _null_4id) {
		String str_local_addr = _local_addr.unparse();
		size_t AD_found_start = str_local_addr.find_left("AD:");
		size_t AD_found_end = str_local_addr.find_left(" ", AD_found_start);
		String AD_str = str_local_addr.substring(AD_found_start, AD_found_end - AD_found_start);
		String HID_str = _local_hid.unparse();
		String IP4ID_str = _local_4id.unparse();
		String new_local_addr = "RE ( " + IP4ID_str + " ) " + AD_str + " " + HID_str;
		//click_chatter("new address is - %s", new_local_addr.c_str());
		_local_addr.parse(new_local_addr);
	}
	*/

#if USERLEVEL
	_routeTable = dynamic_cast<XIAXIDRouteTable*>(routing_table_elem);
#else
	_routeTable = reinterpret_cast<XIAXIDRouteTable*>(routing_table_elem);
#endif

	return 0;
}

XTRANSPORT::~XTRANSPORT()
{
	//Clear all hashtable entries
	XIDtoPort.clear();
	portToSock.clear();
	XIDtoPushPort.clear();
	XIDpairToPort.clear();
	XIDpairToConnectPending.clear();

	hlim.clear();
	xcmp_listeners.clear();
	nxt_xport.clear();

//	pthread_mutex_destroy(&_lock);
//	pthread_mutexattr_destroy(&_lock_attr);
}


int
XTRANSPORT::initialize(ErrorHandler *)
{
	_errh = (SyslogErrorHandler*)ErrorHandler::default_handler();
	_timer.initialize(this);
	//_timer.schedule_after_msec(1000);
	//_timer.unschedule();
	return 0;
}

char *XTRANSPORT::random_xid(const char *type, char *buf)
{
	// This is a stand-in function until we get certificate based names
	//
	// note: buf must be at least 45 characters long
	// (longer if the XID type gets longer than 3 characters)
	sprintf(buf, RANDOM_XID_FMT, type, click_random(0, 0xffffffff));

	return buf;
}

void
XTRANSPORT::run_timer(Timer *timer)
{
//	pthread_mutex_lock(&_lock);

	assert(timer == &_timer);

	Timestamp now = Timestamp::now();
	Timestamp earlist_pending_expiry = now;

	WritablePacket *copy;

	bool tear_down;

	for (HashTable<unsigned short, sock*>::iterator iter = portToSock.begin(); iter != portToSock.end(); ++iter ) {
		unsigned short _sport = iter->first;
		sock *sk = portToSock.get(_sport);
		tear_down = false;

		// reset the concurrent poll flag so we know we can return a result to the next poll request
		sk->did_poll = false;

		// check if pending
		if (sk->timer_on == true) {
			// check if synack waiting
			if (sk->synack_waiting == true && sk->expiry <= now ) {
				//click_chatter("Timer: synack waiting\n");

				if (sk->num_connect_tries <= MAX_CONNECT_TRIES) {

					//click_chatter("Timer: SYN RETRANSMIT! \n");
					copy = copy_packet(sk->syn_pkt, sk);
					// retransmit syn
					XIAHeader xiah(copy);
					// click_chatter("Timer: (%s) send=%s  len=%d \n\n", (_local_addr.unparse()).c_str(), (char *)xiah.payload(), xiah.plen());
					output(NETWORK_PORT).push(copy);

					sk->timer_on = true;
					sk->synack_waiting = true;
					sk->expiry = now + Timestamp::make_msec(_ackdelay_ms);
					sk->num_connect_tries++;

				} else {
					// Stop sending the connection request & Report the failure to the application

					sk->timer_on = false;
					sk->synack_waiting = false;

					// Notify API that the connection failed
					xia::XSocketMsg xsm;

					//_errh->debug("Timer: Sent packet to socket with port %d", _sport);
					xsm.set_type(xia::XCONNECT);
					xsm.set_sequence(0); // TODO: what should This be?
					xia::X_Connect_Msg *connect_msg = xsm.mutable_x_connect();
					connect_msg->set_status(xia::X_Connect_Msg::XFAILED);
					ReturnResult(_sport, &xsm);

					if (sk->polling) {
						printf("checking poll event for %d from timer\n", _sport);
						ProcessPollEvent(_sport, POLLHUP);
					}

				}

			} else if (sk->dataack_waiting == true && sk->expiry <= now ) {

				// adding check to see if anything was retransmitted. We can get in here with
				// no packets in the sk->send_bufer array waiting to go and will stay here forever
				bool retransmit_sent = false;

				if (sk->num_retransmit_tries < MAX_RETRANSMIT_TRIES) {

				//click_chatter("Timer: DATA RETRANSMIT at from (%s) from_port=%d send_base=%d next_seq=%d \n\n", (_local_addr.unparse()).c_str(), _sport, sk->send_base, sk->next_send_seqnum );

					// retransmit data
					for (unsigned int i = sk->send_base; i < sk->next_send_seqnum; i++) {
						if (sk->send_buffer[i % sk->send_buffer_size] != NULL) {
							copy = copy_packet(sk->send_buffer[i % sk->send_buffer_size], sk);
							XIAHeader xiah(copy);
							//click_chatter("Timer: (%s) send=%s  len=%d \n\n", (_local_addr.unparse()).c_str(), (char *)xiah.payload(), xiah.plen());
							//click_chatter("pusing the retransmit pkt\n");
							output(NETWORK_PORT).push(copy);
							retransmit_sent = true;
						}
					}
				} else {
					//click_chatter("retransmit counter exceeded\n");
					// FIXME what cleanup should happen here?
					// should we do a NAK?
				}

				if (retransmit_sent) {
					//click_chatter("resetting retransmit timer for %d\n", _sport);
					sk->timer_on = true;
					sk->dataack_waiting = true;
					sk-> num_retransmit_tries++;
					sk->expiry = now + Timestamp::make_msec(_ackdelay_ms);
				} else {
					//click_chatter("terminating retransmit timer for %d\n", _sport);
					sk->timer_on = false;
					sk->dataack_waiting = false;
					sk->num_retransmit_tries = 0;
				}

			} else if (sk->teardown_waiting == true && sk->teardown_expiry <= now) {
				tear_down = true;
				sk->timer_on = false;
				portToActive.set(_sport, false);

				//XID source_xid = portToSock.get(_sport).xid;

				// this check for -1 prevents a segfault cause by bad XIDs
				// it may happen in other cases, but opening a XSOCK_STREAM socket, calling
				// XreadLocalHostAddr and then closing the socket without doing anything else will
				// cause the problem
				// TODO: make sure that -1 is the only condition that will cause us to get a bad XID
				if (sk->src_path.destination_node() != -1) {
					XID source_xid = sk->src_path.xid(sk->src_path.destination_node());
					if (!sk->isAcceptSocket) {

						//click_chatter("deleting route %s from port %d\n", source_xid.unparse().c_str(), _sport);
						delRoute(source_xid);
						XIDtoPort.erase(source_xid);
					}
				}

				delete sk;
				portToSock.erase(_sport);
				portToActive.erase(_sport);
				hlim.erase(_sport);

				nxt_xport.erase(_sport);
				xcmp_listeners.remove(_sport);
				for (int i = 0; i < sk->send_buffer_size; i++) {
					if (sk->send_buffer[i] != NULL) {
						sk->send_buffer[i]->kill();
						sk->send_buffer[i] = NULL;
					}
				}
			}
		}

		if (tear_down == false) {

			// find the (next) earlist expiry
			if (sk->timer_on == true && sk->expiry > now && ( sk->expiry < earlist_pending_expiry || earlist_pending_expiry == now ) ) {
				earlist_pending_expiry = sk->expiry;
			}
			if (sk->timer_on == true && sk->teardown_expiry > now && ( sk->teardown_expiry < earlist_pending_expiry || earlist_pending_expiry == now ) ) {
				earlist_pending_expiry = sk->teardown_expiry;
			}


			// check for CID request cases
			for (HashTable<XID, bool>::iterator it = sk->XIDtoTimerOn.begin(); it != sk->XIDtoTimerOn.end(); ++it ) {
				XID requested_cid = it->first;
				bool timer_on = it->second;

				HashTable<XID, Timestamp>::iterator it2;
				it2 = sk->XIDtoExpiryTime.find(requested_cid);
				Timestamp cid_req_expiry = it2->second;

				if (timer_on == true && cid_req_expiry <= now) {
					//click_chatter("CID-REQ RETRANSMIT! \n");
					//retransmit cid-request
					HashTable<XID, WritablePacket*>::iterator it3;
					it3 = sk->XIDtoCIDreqPkt.find(requested_cid);
					copy = copy_cid_req_packet(it3->second, sk);
					XIAHeader xiah(copy);
					//click_chatter("\n\n (%s) send=%s  len=%d \n\n", (_local_addr.unparse()).c_str(), (char *)xiah.payload(), xiah.plen());
					output(NETWORK_PORT).push(copy);

					cid_req_expiry  = Timestamp::now() + Timestamp::make_msec(_ackdelay_ms);
					sk->XIDtoExpiryTime.set(requested_cid, cid_req_expiry);
					sk->XIDtoTimerOn.set(requested_cid, true);
				}

				if (timer_on == true && cid_req_expiry > now && ( cid_req_expiry < earlist_pending_expiry || earlist_pending_expiry == now ) ) {
					earlist_pending_expiry = cid_req_expiry;
				}
			}

			portToSock.set(_sport, sk);
		}
	}

//	pthread_mutex_unlock(&_lock);
}

void XTRANSPORT::copy_common(sock *sk, XIAHeader &xiahdr, XIAHeaderEncap &xiah) {

	//Recalculate source path
	XID	source_xid = sk->src_path.xid(sk->src_path.destination_node());
	String str_local_addr = _local_addr.unparse_re() + " " + source_xid.unparse();
	//Make source DAG _local_addr:SID
	String dagstr = sk->src_path.unparse_re();

	//Client Mobility...
	if (dagstr.length() != 0 && dagstr != str_local_addr) {
		//Moved!
		// 1. Update 'sk->src_path'
		sk->src_path.parse_re(str_local_addr);
	}

	xiah.set_nxt(xiahdr.nxt());
	xiah.set_last(xiahdr.last());
	xiah.set_hlim(xiahdr.hlim());
	xiah.set_dst_path(sk->dst_path);
	xiah.set_src_path(sk->src_path);
	xiah.set_plen(xiahdr.plen());
}

WritablePacket *
XTRANSPORT::copy_packet(Packet *p, sock *sk) {

	XIAHeader xiahdr(p);
	XIAHeaderEncap xiah;
	copy_common(sk, xiahdr, xiah);

	TransportHeader thdr(p);
	TransportHeaderEncap *new_thdr = new TransportHeaderEncap(thdr.type(), thdr.pkt_info(), thdr.seq_num(), thdr.ack_num(), thdr.length(), thdr.recv_window());

	WritablePacket *copy = WritablePacket::make(256, thdr.payload(), xiahdr.plen() - thdr.hlen(), 20);

	copy = new_thdr->encap(copy);
	copy = xiah.encap(copy, false);
	delete new_thdr;

	return copy;
}


WritablePacket *
XTRANSPORT::copy_cid_req_packet(Packet *p, sock *sk) {

	XIAHeader xiahdr(p);
	XIAHeaderEncap xiah;
	copy_common(sk, xiahdr, xiah);

	WritablePacket *copy = WritablePacket::make(256, xiahdr.payload(), xiahdr.plen(), 20);

	ContentHeaderEncap *chdr = ContentHeaderEncap::MakeRequestHeader();

	copy = chdr->encap(copy);
	copy = xiah.encap(copy, false);
	delete chdr;
	xiah.set_plen(xiahdr.plen());

	return copy;
}


WritablePacket *
XTRANSPORT::copy_cid_response_packet(Packet *p, sock *sk) {

	XIAHeader xiahdr(p);
	XIAHeaderEncap xiah;
	copy_common(sk, xiahdr, xiah);

	WritablePacket *copy = WritablePacket::make(256, xiahdr.payload(), xiahdr.plen(), 20);

	ContentHeader chdr(p);
	ContentHeaderEncap *new_chdr = new ContentHeaderEncap(chdr.opcode(), chdr.chunk_offset(), chdr.length());

	copy = new_chdr->encap(copy);
	copy = xiah.encap(copy, false);
	delete new_chdr;
	xiah.set_plen(xiahdr.plen());

	return copy;
}

/**
* @brief Calculates a connection's loacal receive window.
*
* recv_window = recv_buffer_size - (next_seqnum - base)
*
* @param sk
*
* @return The receive window.
*/
uint32_t XTRANSPORT::calc_recv_window(sock *sk) {
	return sk->recv_buffer_size - (sk->next_recv_seqnum - sk->recv_base);
}

/**
* @brief Checks whether or not a received packet can be buffered.
*
* Checks if we have room to buffer the received packet; that is, is the packet's
* sequence number within our recieve window? (Or, in the case of a DGRAM socket,
* simply checks if there is an unused slot at the end of the recv buffer.)
*
* @param p
* @param sk
*
* @return true if packet can be buffered, false otherwise
*/
bool XTRANSPORT::should_buffer_received_packet(WritablePacket *p, sock *sk) {

//printf("<<<< should_buffer_received_packet\n");

	if (sk->sock_type == XSOCKET_STREAM) {
		// check if received_seqnum is within our current recv window
		// TODO: if we switch to a byte-based, buf size, this needs to change
		TransportHeader thdr(p);
		int received_seqnum = thdr.seq_num();
		if (received_seqnum >= sk->next_recv_seqnum &&
			received_seqnum < sk->next_recv_seqnum + sk->recv_buffer_size) {
			return true;
		}
	} else if (sk->sock_type == XSOCKET_DGRAM) {

//printf("    sk->recv_buffer_size: %u\n    sk->dgram_buffer_start: %u\n    sk->dgram_buffer_end: %u\n\n", sk->recv_buffer_size, sk->dgram_buffer_start, sk->dgram_buffer_end);

		//if ( (sk->dgram_buffer_end + 1) % sk->recv_buffer_size != sk->dgram_buffer_start) {
		if (sk->recv_buffer_count < sk->recv_buffer_size) {
//printf("    return: TRUE\n");
			return true;
		}
	}
//printf("    return: FALSE\n");
	return false;
}

/**
* @brief Adds a packet to the connection's receive buffer.
*
* Stores the supplied packet pointer, p, in a slot depending on sock type:
*
*   STREAM: index = seqnum % bufsize.
*   DGRAM:  index = (end + 1) % bufsize
*
* @param p
* @param sk
*/
void XTRANSPORT::add_packet_to_recv_buf(WritablePacket *p, sock *sk) {

	int index = -1;
	if (sk->sock_type == XSOCKET_STREAM) {
		TransportHeader thdr(p);
		int received_seqnum = thdr.seq_num();
		index = received_seqnum % sk->recv_buffer_size;
//printf("    port=%u adding packet to index %d\n", sk->port, index);
	} else if (sk->sock_type == XSOCKET_DGRAM) {
		index = (sk->dgram_buffer_end + 1) % sk->recv_buffer_size;
		sk->dgram_buffer_end = index;
		sk->recv_buffer_count++;
	}

	WritablePacket *p_cpy = p->clone()->uniqueify();
	sk->recv_buffer[index] = p_cpy;
}

/**
* @brief check to see if the app is waiting for this data; if so, return it now
*
* @param sk
*/
void XTRANSPORT::check_for_and_handle_pending_recv(sock *sk) {
	if (sk->recv_pending) {
		int bytes_returned = read_from_recv_buf(sk->pending_recv_msg, sk);
		ReturnResult(sk->port, sk->pending_recv_msg, bytes_returned);

		sk->recv_pending = false;
		delete sk->pending_recv_msg;
		sk->pending_recv_msg = NULL;
	}
}

/**
* @brief Returns the next expected sequence number.
*
* Beginning with sk->recv_base, this function checks consecutive slots
* in the receive buffer and returns the first missing sequence number.
* (This function only applies to STREAM sockets.)
*
* @param sk
*/
uint32_t XTRANSPORT::next_missing_seqnum(sock *sk) {

	uint32_t next_missing = sk->recv_base;
	for (uint32_t i = 0; i < sk->recv_buffer_size; i++) {

		// checking if we have the next consecutive packet
		uint32_t seqnum_to_check = sk->recv_base + i;
		uint32_t index_to_check = seqnum_to_check % sk->recv_buffer_size;

		next_missing = seqnum_to_check;

		if (sk->recv_buffer[index_to_check]) {
			TransportHeader thdr(sk->recv_buffer[index_to_check]);
			if (thdr.seq_num() != seqnum_to_check) {
				break; // found packet, but its seqnum isn't right, so break and return next_missing
			}
		} else {
			break; // no packet here, so break and return next_missing
		}
	}

	return next_missing;
}


void XTRANSPORT::resize_buffer(WritablePacket* buf[], int max, int type, uint32_t old_size, uint32_t new_size, int *dgram_start, int *dgram_end) {

	if (new_size < old_size) {
		click_chatter("WARNING: new buffer size is smaller than old size. Some data may be discarded.\n");
		old_size = new_size; // so we stop after moving as many packets as will fit in the new buffer
	}

	// General procedure: make a temporary buffer and copy pointers to their
	// new indices in the temp buffer. Then, rewrite the original buffer.
	WritablePacket *temp[max];
	memset(temp, 0, max);

	// Figure out the new index for each packet in buffer
	int new_index = -1;
	for (int i = 0; i < old_size; i++) {
		if (type == XSOCKET_STREAM) {
			TransportHeader thdr(buf[i]);
			new_index = thdr.seq_num() % new_size;
		} else if (type == XSOCKET_DGRAM) {
			new_index = (i + *dgram_start) % old_size;
		}
		temp[new_index] = buf[i];
	}

	// For DGRAM socket, reset start and end vars
	if (type == XSOCKET_DGRAM) {
		*dgram_start = 0;
		*dgram_end = (*dgram_start + *dgram_end) % old_size;
	}

	// Copy new locations from temp back to original buf
	memset(buf, 0, max);
	for (int i = 0; i < max; i++) {
		buf[i] = temp[i];
	}
}

void XTRANSPORT::resize_send_buffer(sock *sk, uint32_t new_size) {
	resize_buffer(sk->send_buffer, MAX_SEND_WIN_SIZE, sk->sock_type, sk->send_buffer_size, new_size, &(sk->dgram_buffer_start), &(sk->dgram_buffer_end));
	sk->send_buffer_size = new_size;
}

void XTRANSPORT::resize_recv_buffer(sock *sk, uint32_t new_size) {
	resize_buffer(sk->recv_buffer, MAX_RECV_WIN_SIZE, sk->sock_type, sk->recv_buffer_size, new_size, &(sk->dgram_buffer_start), &(sk->dgram_buffer_end));
	sk->recv_buffer_size = new_size;
}

/**
* @brief Read received data from buffer.
*
* We'll use this same xia_socket_msg as the response to the API:
* 1) We fill in the data (from *only one* packet for DGRAM)
* 2) We fill in how many bytes we're returning
* 3) We fill in the sender's DAG (DGRAM only)
* 4) We clear out any buffered packets whose data we return to the app
*
* @param xia_socket_msg The Xrecv or Xrecvfrom message from the API
* @param sk The sock struct for this connection
*
* @return  The number of bytes read from the buffer.
*/
int XTRANSPORT::read_from_recv_buf(xia::XSocketMsg *xia_socket_msg, sock *sk) {

	if (sk->sock_type == XSOCKET_STREAM) {
//		printf("<<< read_from_recv_buf: port=%u, recv_base=%d, next_recv_seqnum=%d, recv_buf_size=%d\n", sk->port, sk->recv_base, sk->next_recv_seqnum, sk->recv_buffer_size);
		xia::X_Recv_Msg *x_recv_msg = xia_socket_msg->mutable_x_recv();
		int bytes_requested = x_recv_msg->bytes_requested();
		int bytes_returned = 0;
		char buf[1024*1024]; // TODO: pick a buf size
		memset(buf, 0, 1024*1024);
		for (int i = sk->recv_base; i < sk->next_recv_seqnum; i++) {

			if (bytes_returned >= bytes_requested) break;

			WritablePacket *p = sk->recv_buffer[i % sk->recv_buffer_size];
			XIAHeader xiah(p->xia_header());
			TransportHeader thdr(p);
			size_t data_size = xiah.plen() - thdr.hlen();

			memcpy((void*)(&buf[bytes_returned]), (const void*)thdr.payload(), data_size);
			bytes_returned += data_size;

			p->kill();
			sk->recv_buffer[i % sk->recv_buffer_size] = NULL;
			sk->recv_base++;
//			printf("    port %u grabbing index %d, seqnum %d\n", sk->port, i%sk->recv_buffer_size, i);
		}
		x_recv_msg->set_payload(buf, bytes_returned); // TODO: check this: need to turn buf into String first?
		x_recv_msg->set_bytes_returned(bytes_returned);

//		printf(">>> read_from_recv_buf: port=%u, recv_base=%d, next_recv_seqnum=%d, recv_buf_size=%d\n", sk->port, sk->recv_base, sk->next_recv_seqnum, sk->recv_buffer_size);
		return bytes_returned;

	} else if (sk->sock_type == XSOCKET_DGRAM) {
		xia::X_Recvfrom_Msg *x_recvfrom_msg = xia_socket_msg->mutable_x_recvfrom();
	
		// Get just the next packet in the recv buffer (we don't return data from more
		// than one packet in case the packets came from different senders). If no
		// packet is available, we indicate to the app that we returned 0 bytes.
		WritablePacket *p = sk->recv_buffer[sk->dgram_buffer_start];

		if (sk->recv_buffer_count > 0 && p) {
			XIAHeader xiah(p->xia_header());
			TransportHeader thdr(p);
			int data_size = xiah.plen() - thdr.hlen();

			String src_path = xiah.src_path().unparse();
			String payload((const char*)thdr.payload(), data_size);
			x_recvfrom_msg->set_payload(payload.c_str(), payload.length());
			x_recvfrom_msg->set_sender_dag(src_path.c_str());
			x_recvfrom_msg->set_bytes_returned(data_size);

			p->kill();
			sk->recv_buffer[sk->dgram_buffer_start] = NULL;
			sk->recv_buffer_count--;
			sk->dgram_buffer_start = (sk->dgram_buffer_start + 1) % sk->recv_buffer_size;
			return data_size;
		} else {
			x_recvfrom_msg->set_bytes_returned(0);
			return 0;
		}
	}

	return -1;
}

void XTRANSPORT::ProcessAPIPacket(WritablePacket *p_in)
{
		
	//Extract the destination port
	unsigned short _sport = SRC_PORT_ANNO(p_in);


//	_errh->debug("\nPush: Got packet from API sport:%d",ntohs(_sport));


	std::string p_buf;
	p_buf.assign((const char*)p_in->data(), (const char*)p_in->end_data());

	//protobuf message parsing
	xia::XSocketMsg xia_socket_msg;
	xia_socket_msg.ParseFromString(p_buf);

	switch(xia_socket_msg.type()) {
	case xia::XSOCKET:
		Xsocket(_sport, &xia_socket_msg);
		break;
	case xia::XSETSOCKOPT:
		Xsetsockopt(_sport, &xia_socket_msg);
		break;
	case xia::XGETSOCKOPT:
		Xgetsockopt(_sport, &xia_socket_msg);
		break;
	case xia::XBIND:
		Xbind(_sport, &xia_socket_msg);
		break;
	case xia::XCLOSE:
		Xclose(_sport, &xia_socket_msg);
		break;
	case xia::XCONNECT:
		Xconnect(_sport, &xia_socket_msg);
		break;
	case xia::XREADYTOACCEPT:
		XreadyToAccept(_sport, &xia_socket_msg);
		break;
	case xia::XACCEPT:
		Xaccept(_sport, &xia_socket_msg);
		break;
	case xia::XCHANGEAD:
		Xchangead(_sport, &xia_socket_msg);
		break;
	case xia::XREADLOCALHOSTADDR:
		Xreadlocalhostaddr(_sport, &xia_socket_msg);
		break;
	case xia::XUPDATENAMESERVERDAG:
		Xupdatenameserverdag(_sport, &xia_socket_msg);
		break;
	case xia::XREADNAMESERVERDAG:
		Xreadnameserverdag(_sport, &xia_socket_msg);
		break;
	case xia::XISDUALSTACKROUTER:
		Xisdualstackrouter(_sport, &xia_socket_msg);
		break;
	case xia::XSEND:
		Xsend(_sport, &xia_socket_msg, p_in);
		break;
	case xia::XSENDTO:
		Xsendto(_sport, &xia_socket_msg, p_in);
		break;
	case xia::XRECV:
		Xrecv(_sport, &xia_socket_msg);
		break;
	case xia::XRECVFROM:
		Xrecvfrom(_sport, &xia_socket_msg);
		break;
	case xia::XREQUESTCHUNK:
		XrequestChunk(_sport, &xia_socket_msg, p_in);
		break;
	case xia::XGETCHUNKSTATUS:
		XgetChunkStatus(_sport, &xia_socket_msg);
		break;
	case xia::XREADCHUNK:
		XreadChunk(_sport, &xia_socket_msg);
		break;
	case xia::XREMOVECHUNK:
		XremoveChunk(_sport, &xia_socket_msg);
		break;
	case xia::XPUTCHUNK:
		XputChunk(_sport, &xia_socket_msg);
		break;
	case xia::XGETPEERNAME:
		Xgetpeername(_sport, &xia_socket_msg);
		break;
	case xia::XGETSOCKNAME:
		Xgetsockname(_sport, &xia_socket_msg);
		break;
	case xia::XPOLL:
		Xpoll(_sport, &xia_socket_msg);
		break;
	case xia::XPUSHCHUNKTO:
		XpushChunkto(_sport, &xia_socket_msg, p_in);
		break;
	case xia::XBINDPUSH:
		XbindPush(_sport, &xia_socket_msg);
		break;
	default:
		click_chatter("\n\nERROR: API TRAFFIC !!!\n\n");
		break;
	}

	p_in->kill();
}

void XTRANSPORT::ProcessNetworkPacket(WritablePacket *p_in)
{

//	_errh->debug("Got packet from network");


	//Extract the SID/CID
	XIAHeader xiah(p_in->xia_header());
	XIAPath dst_path = xiah.dst_path();
	XID _destination_xid(xiah.hdr()->node[xiah.last()].xid);
	//TODO:In case of stream use source AND destination XID to find port, if not found use source. No TCP like protocol exists though
	//TODO:pass dag back to recvfrom. But what format?

	XIAPath src_path = xiah.src_path();
	XID	_source_xid = src_path.xid(src_path.destination_node());
	
// 	click_chatter("NetworkPacket, Src: %s, Dest: %s", xiah.dst_path().unparse().c_str(), xiah.src_path().unparse().c_str());

	unsigned short _dport = XIDtoPort.get(_destination_xid);  // This is to be updated for the XSOCK_STREAM type connections below

	//String pld((char *)xiah.payload(), xiah.plen());
	//click_chatter("\n\n 1. (%s) Received=%s  len=%d \n\n", (_local_addr.unparse()).c_str(), pld.c_str(), xiah.plen());

	TransportHeader thdr(p_in);

	if (xiah.nxt() == CLICK_XIA_NXT_XCMP) { // TODO:  Should these be put in recv buffer???

		String src_path = xiah.src_path().unparse();
		String header((const char*)xiah.hdr(), xiah.hdr_size());
		String payload((const char*)xiah.payload(), xiah.plen());
		String str = header + payload;

		xia::XSocketMsg xsm;
		xsm.set_type(xia::XRECV);
		xia::X_Recvfrom_Msg *x_recvfrom_msg = xsm.mutable_x_recvfrom();
		x_recvfrom_msg->set_sender_dag(src_path.c_str());
		x_recvfrom_msg->set_payload(str.c_str(), str.length());

		std::string p_buf;
		xsm.SerializeToString(&p_buf);

		WritablePacket *xcmp_pkt = WritablePacket::make(256, p_buf.c_str(), p_buf.size(), 0);

		list<int>::iterator i;

		for (i = xcmp_listeners.begin(); i != xcmp_listeners.end(); i++) {
			int port = *i;
			output(API_PORT).push(UDPIPPrep(xcmp_pkt, port));
		}

		return;

	} else if (thdr.type() == TransportHeader::XSOCK_STREAM) {

		// some common actions for all STREAM packets
		XIDpair xid_pair;
		xid_pair.set_src(_destination_xid);
		xid_pair.set_dst(_source_xid);

		// update _dport if there's already a connection, then get sock
		if (thdr.pkt_info() != TransportHeader::SYN) {
			_dport = XIDpairToPort.get(xid_pair);
		}
		sock *sk = portToSock.get(_dport); // TODO: check that mapping exists

		// update remote recv window
//		if (thdr.recv_window() == 0)
//			click_chatter("received STREAM packet on port %u;   recv window = %u\n", _dport, thdr.recv_window());
		sk->remote_recv_window = thdr.recv_window();
		

		if (thdr.pkt_info() == TransportHeader::SYN) {
			//click_chatter("syn dport = %d\n", _dport);
			// Connection request from client...
		
			//sock *sk = portToSock.get(_dport); // TODO: check that mapping exists

			// First, check if this request is already in the pending queue
//			HashTable<XIDpair , bool>::iterator it;
//			it = XIDpairToConnectPending.find(xid_pair);

			// FIXME:
			// XIDpairToConnectPending never gets cleared, and will cause problems if matching XIDs
			// were used previously. Commenting out the check for now. Need to look into whether
			// or not we can just get rid of this logic? probably neede for retransmit cases
			// if needed, where should it be cleared???
//			if (it == XIDpairToConnectPending.end()) {
				// if this is new request, put it in the queue

				// Todo: 1. prepare new Daginfo and store it
				//	 2. send SYNACK to client

				//1. Prepare new sock for this connection
				sock *new_sk = new sock();
				new_sk->port = -1; // just for now. This will be updated via Xaccept call

				new_sk->sock_type = XSOCKET_STREAM;

				new_sk->dst_path = src_path;
				new_sk->src_path = dst_path;
				new_sk->isConnected = true;
				new_sk->initialized = true;
				new_sk->nxt = LAST_NODE_DEFAULT;
				new_sk->last = LAST_NODE_DEFAULT;
				new_sk->hlim = HLIM_DEFAULT;
				new_sk->seq_num = 0;
				new_sk->ack_num = 0;
				memset(new_sk->send_buffer, 0, new_sk->send_buffer_size * sizeof(WritablePacket*));
				memset(new_sk->recv_buffer, 0, new_sk->recv_buffer_size * sizeof(WritablePacket*));
				//new_sk->pending_connection_buf = new queue<sock>();
				//new_sk->pendingAccepts = new queue<xia::XSocketMsg*>();

				sk->pending_connection_buf.push(new_sk);

				// Mark these src & dst XID pair
				XIDpairToConnectPending.set(xid_pair, true);

				// If the app is ready for a new connection, alert it
				if (!sk->pendingAccepts.empty()) {
					xia::XSocketMsg *acceptXSM = sk->pendingAccepts.front();
					ReturnResult(_dport, acceptXSM);
					sk->pendingAccepts.pop();
					delete acceptXSM;
				}
//			}

		} else if (thdr.pkt_info() == TransportHeader::SYNACK) {

			// Clear timer
			sk->timer_on = false;
			sk->synack_waiting = false;

			if (sk->polling) {
				// tell API we are writble now
				ProcessPollEvent(_dport, POLLOUT);
			}

			//sk->expiry = Timestamp::now() + Timestamp::make_msec(_ackdelay_ms);

			// Notify API that the connection is established
			xia::XSocketMsg xsm;
			xsm.set_type(xia::XCONNECT);
			xsm.set_sequence(0); // TODO: what should this be?
			xia::X_Connect_Msg *connect_msg = xsm.mutable_x_connect();
			connect_msg->set_ddag(src_path.unparse().c_str());
			connect_msg->set_status(xia::X_Connect_Msg::XCONNECTED);
			ReturnResult(_dport, &xsm);

		} else if (thdr.pkt_info() == TransportHeader::DATA) {
			//printf("(%s) my_sport=%u  my_sid=%s  his_sid=%s\n", (_local_addr.unparse()).c_str(),  _dport,  _destination_xid.unparse().c_str(), _source_xid.unparse().c_str());
			HashTable<unsigned short, bool>::iterator it1;
			it1 = portToActive.find(_dport);

			if(it1 != portToActive.end() ) {

				// buffer data, if we have room
				if (should_buffer_received_packet(p_in, sk)) {
//					printf("<<< add_packet_to_recv_buf: port=%u, recv_base=%d, next_recv_seqnum=%d, recv_buf_size=%d\n", sk->port, sk->recv_base, sk->next_recv_seqnum, sk->recv_buffer_size);
					add_packet_to_recv_buf(p_in, sk);
					sk->next_recv_seqnum = next_missing_seqnum(sk);
					// TODO: update recv window
//					printf(">>> add_packet_to_recv_buf: port=%u, recv_base=%d, next_recv_seqnum=%d, recv_buf_size=%d\n", sk->port, sk->recv_base, sk->next_recv_seqnum, sk->recv_buffer_size);

					if (sk->polling) {
						// tell API we are readable
						ProcessPollEvent(_dport, POLLIN);
					}
					check_for_and_handle_pending_recv(sk);
				}

				portToSock.set(_dport, sk); // TODO: why do we need this?

				//In case of Client Mobility...	 Update 'sk->dst_path'
				sk->dst_path = src_path;

				// send the cumulative ACK to the sender
				//Add XIA headers
				XIAHeaderEncap xiah_new;
				xiah_new.set_nxt(CLICK_XIA_NXT_TRN);
				xiah_new.set_last(LAST_NODE_DEFAULT);
				xiah_new.set_hlim(HLIM_DEFAULT);
				xiah_new.set_dst_path(src_path);
				xiah_new.set_src_path(dst_path);

				const char* dummy = "cumulative_ACK";
				WritablePacket *just_payload_part = WritablePacket::make(256, dummy, strlen(dummy), 0);

				WritablePacket *p = NULL;

				xiah_new.set_plen(strlen(dummy));

				TransportHeaderEncap *thdr_new = TransportHeaderEncap::MakeACKHeader( 0, sk->next_recv_seqnum, 0, calc_recv_window(sk)); // #seq, #ack, length, recv_wind
				p = thdr_new->encap(just_payload_part);

				thdr_new->update();
				xiah_new.set_plen(strlen(dummy) + thdr_new->hlen()); // XIA payload = transport header + transport-layer data

				p = xiah_new.encap(p, false);
				delete thdr_new;

				XIAHeader xiah1(p);
				String pld((char *)xiah1.payload(), xiah1.plen());
				//click_chatter("\n\n (%s) send=%s  len=%d \n\n", (_local_addr.unparse()).c_str(), pld.c_str(), xiah1.plen());

				output(NETWORK_PORT).push(p);

			} else {
				click_chatter("destination port not found: %d\n", _dport);
			}

		} else if (thdr.pkt_info() == TransportHeader::ACK) {

			HashTable<unsigned short, bool>::iterator it1;
			it1 = portToActive.find(_dport);

			if(it1 != portToActive.end() ) {
				//In case of Client Mobility...	 Update 'sk->dst_path'
				sk->dst_path = src_path;

				int remote_next_seqnum_expected = thdr.ack_num();

				bool resetTimer = false;

				// Clear all Acked packets
				for (int i = sk->send_base; i < remote_next_seqnum_expected; i++) {
					int idx = i % sk->send_buffer_size;
					if (sk->send_buffer[idx]) {
						sk->send_buffer[idx]->kill();
						sk->send_buffer[idx] = NULL;
					}

					resetTimer = true;
				}

				// Update the variables
				sk->send_base = remote_next_seqnum_expected;

				// Reset timer
				if (resetTimer) {
					sk->timer_on = true;
					sk->dataack_waiting = true;
					// FIXME: should we reset retransmit_tries here?
					sk->expiry = Timestamp::now() + Timestamp::make_msec(_ackdelay_ms);

					if (! _timer.scheduled() || _timer.expiry() >= sk->expiry )
						_timer.reschedule_at(sk->expiry);

					if (sk->send_base == sk->next_send_seqnum) {

						// Clear timer
						sk->timer_on = false;
						sk->dataack_waiting = false;
						sk->num_retransmit_tries = 0;
						//sk->expiry = Timestamp::now() + Timestamp::make_msec(_ackdelay_ms);
					}
				}
				portToSock.set(_dport, sk);

			} else {
				//click_chatter("port not found\n");
			}

		} else if (thdr.pkt_info() == TransportHeader::FIN) {
			//click_chatter("FIN received, doing nothing\n");
		}
		else {
			click_chatter("UNKNOWN dport = %d hdr=%d\n", _dport, thdr.pkt_info());
		}

	} else if (thdr.type() == TransportHeader::XSOCK_DGRAM) {

		sock *sk = portToSock.get(_dport);

		// buffer packet if this is a DGRAM socket and we have room
		if (sk->sock_type == XSOCKET_DGRAM &&
			should_buffer_received_packet(p_in, sk)) {
			add_packet_to_recv_buf(p_in, sk);

			if (sk->polling) {
				// tell API we are readable
				ProcessPollEvent(_dport, POLLIN);
			}
			check_for_and_handle_pending_recv(sk);
		}

	} else {
		click_chatter("UNKNOWN!!!!! dport = %d\n", _dport);
	}
}

void XTRANSPORT::ProcessCachePacket(WritablePacket *p_in)
{
 	_errh->debug("Got packet from cache");		

	//Extract the SID/CID
	XIAHeader xiah(p_in->xia_header());
	XIAPath dst_path = xiah.dst_path();
	XIAPath src_path = xiah.src_path();
	XID	destination_sid = dst_path.xid(dst_path.destination_node());
	XID	source_cid = src_path.xid(src_path.destination_node());	
	
	ContentHeader ch(p_in);
	
//	click_chatter("dest %s, src_cid %s, dst_path: %s, src_path: %s\n", 
//		destination_sid.unparse().c_str(), source_cid.unparse().c_str(), dst_path.unparse().c_str(), src_path.unparse().c_str());
//	click_chatter("dst_path: %s, src_path: %s, OPCode: %d\n", dst_path.unparse().c_str(), src_path.unparse().c_str(), ch.opcode());
	
	
	if (ch.opcode()==ContentHeader::OP_PUSH) {
		// compute the hash and verify it matches the CID
		String hash = "CID:";
		char hexBuf[3];
		int i = 0;
		SHA1_ctx sha_ctx;
		unsigned char digest[HASH_KEYSIZE];
		SHA1_init(&sha_ctx);
		SHA1_update(&sha_ctx, (unsigned char *)xiah.payload(), xiah.plen());
		SHA1_final(digest, &sha_ctx);
		for(i = 0; i < HASH_KEYSIZE; i++) {
			sprintf(hexBuf, "%02x", digest[i]);
			hash.append(const_cast<char *>(hexBuf), 2);
		}

// 		int status = READY_TO_READ;
		if (hash != source_cid.unparse()) {
			click_chatter("CID with invalid hash received: %s\n", source_cid.unparse().c_str());
// 			status = INVALID_HASH;
		}
		
		unsigned short _dport = XIDtoPushPort.get(destination_sid);
		if (!_dport) {
			click_chatter("Couldn't find SID to send to: %s\n", destination_sid.unparse().c_str());
			return;
		}
		
		sock *sk = portToSock.get(_dport);
		// check if _destination_sid is of XSOCK_DGRAM
		if (sk->sock_type != XSOCKET_CHUNK) {
			click_chatter("This is not a chunk socket. dport: %i, Socktype: %i", _dport, sk->sock_type);
		}
		
		// Send pkt up

		//Unparse dag info
		String src_path = xiah.src_path().unparse();

// FIXMEFIXMEFIXME
		xia::XSocketMsg xia_socket_msg;
		xia_socket_msg.set_type(xia::XRECVCHUNKFROM);
		xia::X_Recvchunkfrom_Msg *x_recvchunkfrom_msg = xia_socket_msg.mutable_x_recvchunkfrom();
		x_recvchunkfrom_msg->set_cid(source_cid.unparse().c_str());
		x_recvchunkfrom_msg->set_payload((const char*)xiah.payload(), xiah.plen());
		x_recvchunkfrom_msg->set_cachepolicy(ch.cachePolicy());
		x_recvchunkfrom_msg->set_ttl(ch.ttl());
		x_recvchunkfrom_msg->set_cachesize(ch.cacheSize());
		x_recvchunkfrom_msg->set_contextid(ch.contextID());
		x_recvchunkfrom_msg->set_length(ch.length());
 		x_recvchunkfrom_msg->set_ddag(dst_path.unparse().c_str());

		std::string p_buf;
		xia_socket_msg.SerializeToString(&p_buf);

		WritablePacket *p2 = WritablePacket::make(256, p_buf.c_str(), p_buf.size(), 0);

		//click_chatter("FROM CACHE. data length = %d  \n", str.length());
		_errh->debug("Sent packet to socket: sport %d dport %d", _dport, _dport);

		output(API_PORT).push(UDPIPPrep(p2, _dport));
		return;
	}

	XIDpair xid_pair;
	xid_pair.set_src(destination_sid);
	xid_pair.set_dst(source_cid);

	unsigned short _dport = XIDpairToPort.get(xid_pair);
	
// 	click_chatter(">>packet from processCACHEpackets %d\n", _dport);
// 	click_chatter("CachePacket, Src: %s, Dest: %s, Local: %s", xiah.dst_path().unparse().c_str(),
// 			      xiah.src_path().unparse().c_str(), _local_addr.unparse_re().c_str());
	click_chatter("CachePacket, dest: %s, src_cid %s OPCode: %d \n", destination_sid.unparse().c_str(), source_cid.unparse().c_str(), ch.opcode());
// 	click_chatter("dst_path: %s, src_path: %s, OPCode: %d\n", dst_path.unparse().c_str(), src_path.unparse().c_str(), ch.opcode());

	if(_dport)
	{
		//TODO: Refine the way we change DAG in case of migration. Use some control bits. Add verification
		//sock sk=portToSock.get(_dport);
		//sk.dst_path=xiah.src_path();
		//portToSock.set(_dport,sk);
		//ENDTODO

		sock *sk = portToSock.get(_dport);

		// Reset timer or just Remove the corresponding entry in the hash tables (Done below)
		HashTable<XID, WritablePacket*>::iterator it1;
		it1 = sk->XIDtoCIDreqPkt.find(source_cid);

		if(it1 != sk->XIDtoCIDreqPkt.end() ) {
			// Remove the entry
			sk->XIDtoCIDreqPkt.erase(it1);
		}

		HashTable<XID, Timestamp>::iterator it2;
		it2 = sk->XIDtoExpiryTime.find(source_cid);

		if(it2 != sk->XIDtoExpiryTime.end()) {
			// Remove the entry
			sk->XIDtoExpiryTime.erase(it2);
		}

		HashTable<XID, bool>::iterator it3;
		it3 = sk->XIDtoTimerOn.find(source_cid);

		if(it3 != sk->XIDtoTimerOn.end()) {
			// Remove the entry
			sk->XIDtoTimerOn.erase(it3);
		}

		// compute the hash and verify it matches the CID
		String hash = "CID:";
		char hexBuf[3];
		int i = 0;
		SHA1_ctx sha_ctx;
		unsigned char digest[HASH_KEYSIZE];
		SHA1_init(&sha_ctx);
		SHA1_update(&sha_ctx, (unsigned char *)xiah.payload(), xiah.plen());
		SHA1_final(digest, &sha_ctx);
		for(i = 0; i < HASH_KEYSIZE; i++) {
			sprintf(hexBuf, "%02x", digest[i]);
			hash.append(const_cast<char *>(hexBuf), 2);
		}

		int status = READY_TO_READ;
		if (hash != source_cid.unparse()) {
			click_chatter("CID with invalid hash received: %s\n", source_cid.unparse().c_str());
			status = INVALID_HASH;
		}

		// Update the status of CID request
		sk->XIDtoStatus.set(source_cid, status);

		// Check if the ReadCID() was called for this CID
		HashTable<XID, bool>::iterator it4;
		it4 = sk->XIDtoReadReq.find(source_cid);

		if(it4 != sk->XIDtoReadReq.end()) {
			// There is an entry
			bool read_cid_req = it4->second;

			if (read_cid_req == true) {
				// Send pkt up
				sk->XIDtoReadReq.erase(it4);

				portToSock.set(_dport, sk);

				//Unparse dag info
				String src_path = xiah.src_path().unparse();

				xia::XSocketMsg xia_socket_msg;
				xia_socket_msg.set_type(xia::XREADCHUNK);
				xia::X_Readchunk_Msg *x_readchunk_msg = xia_socket_msg.mutable_x_readchunk();
				x_readchunk_msg->set_dag(src_path.c_str());
				x_readchunk_msg->set_payload((const char*)xiah.payload(), xiah.plen());

				std::string p_buf;
				xia_socket_msg.SerializeToString(&p_buf);

				WritablePacket *p2 = WritablePacket::make(256, p_buf.c_str(), p_buf.size(), 0);

				//click_chatter("FROM CACHE. data length = %d  \n", str.length());
				_errh->debug("Sent packet to socket: sport %d dport %d \n", _dport, _dport);

				output(API_PORT).push(UDPIPPrep(p2, _dport));

			} else {
				// Store the packet into temp buffer (until ReadCID() is called for this CID)
				WritablePacket *copy_response_pkt = copy_cid_response_packet(p_in, sk);
				sk->XIDtoCIDresponsePkt.set(source_cid, copy_response_pkt);

				portToSock.set(_dport, sk);
			}

		} else {
			WritablePacket *copy_response_pkt = copy_cid_response_packet(p_in, sk);
			sk->XIDtoCIDresponsePkt.set(source_cid, copy_response_pkt);
			portToSock.set(_dport, sk);
		}
	}
	else
	{
		click_chatter("Case 2. Packet to unknown %s, src_cid %s\n", destination_sid.unparse().c_str(), source_cid.unparse().c_str());
// 		click_chatter("Case 2. Packet to unknown dest %s, src_cid %s, dst_path: %s, src_path: %s\n", 
// 		      destination_sid.unparse().c_str(), source_cid.unparse().c_str(), dst_path.unparse().c_str(), src_path.unparse().c_str());
// 		click_chatter("Case 2. Packet to unknown  dst_path: %s, src_path: %s\n", dst_path.unparse().c_str(), src_path.unparse().c_str());

	  
	}
}

void XTRANSPORT::ProcessXhcpPacket(WritablePacket *p_in)
{
	XIAHeader xiah(p_in->xia_header());
	String temp = _local_addr.unparse();
	Vector<String> ids;
	cp_spacevec(temp, ids);;
	if (ids.size() < 3) {
		String new_route((char *)xiah.payload());
		String new_local_addr = new_route + " " + ids[1];
		click_chatter("new address is - %s", new_local_addr.c_str());
		_local_addr.parse(new_local_addr);
	}
}


void XTRANSPORT::push(int port, Packet *p_input)
{
//	pthread_mutex_lock(&_lock);

	WritablePacket *p_in = p_input->uniqueify();
	//Depending on which CLICK-module-port it arrives at it could be control/API traffic/Data traffic

	switch(port) { // This is a "CLICK" port of UDP module.
	case API_PORT:	// control packet from socket API
		ProcessAPIPacket(p_in);
		break;

	case BAD_PORT: //packet from ???
		_errh->debug("\n\nERROR: BAD INPUT PORT TO XTRANSPORT!!!\n\n");
		break;

	case NETWORK_PORT: //Packet from network layer
		ProcessNetworkPacket(p_in);
		p_in->kill();
		break;

	case CACHE_PORT:	//Packet from cache
//  		click_chatter(">>>>> Cache Port packet from socket Cache %d\n", port);
		ProcessCachePacket(p_in);
		p_in->kill();
		break;

	case XHCP_PORT:		//Packet with DHCP information
		ProcessXhcpPacket(p_in);
		p_in->kill();
		break;

	default:
		click_chatter("packet from unknown port: %d\n", port);
		break;
	}

//	pthread_mutex_unlock(&_lock);
}

void XTRANSPORT::ReturnResult(int sport, xia::XSocketMsg *xia_socket_msg, int rc, int err)
{
//	click_chatter("sport=%d type=%d rc=%d err=%d\n", sport, type, rc, err);
	xia::X_Result_Msg *x_result = xia_socket_msg->mutable_x_result();
	x_result->set_return_code(rc);
	x_result->set_err_code(err);

	std::string p_buf;
	xia_socket_msg->SerializeToString(&p_buf);
	WritablePacket *reply = WritablePacket::make(256, p_buf.c_str(), p_buf.size(), 0);
	output(API_PORT).push(UDPIPPrep(reply, sport));
}

Packet *
XTRANSPORT::UDPIPPrep(Packet *p_in, int dport)
{
    p_in->set_dst_ip_anno(IPAddress("127.0.0.1"));
    SET_DST_PORT_ANNO(p_in, dport);

	return p_in;
}


enum {H_MOVE};

int XTRANSPORT::write_param(const String &conf, Element *e, void *vparam,
							ErrorHandler *errh)
{
	XTRANSPORT *f = static_cast<XTRANSPORT *>(e);
	switch(reinterpret_cast<intptr_t>(vparam)) {
	case H_MOVE:
	{
		XIAPath local_addr;
		if (cp_va_kparse(conf, f, errh,
						 "LOCAL_ADDR", cpkP + cpkM, cpXIAPath, &local_addr,
						 cpEnd) < 0)
			return -1;
		f->_local_addr = local_addr;
		click_chatter("Moved to %s", local_addr.unparse().c_str());
		f->_local_hid = local_addr.xid(local_addr.destination_node());

	}
	break;
	default:
		break;
	}
	return 0;
}

void XTRANSPORT::add_handlers() {
	add_write_handler("local_addr", write_param, (void *)H_MOVE);
}

/*
** Handler for the Xsocket API call
**
** FIXME: why is xia_socket_msg part of the xtransport class and not a local variable?????
*/
void XTRANSPORT::Xsocket(unsigned short _sport, xia::XSocketMsg *xia_socket_msg) {
	//Open socket.
//	click_chatter("Xsocket: create socket %d\n", _sport);

	xia::X_Socket_Msg *x_socket_msg = xia_socket_msg->mutable_x_socket();
	int sock_type = x_socket_msg->type();

	//Set the source port in sock
	sock *sk = new sock();
	sk->port = _sport;
	sk->timer_on = false;
	sk->synack_waiting = false;
	sk->dataack_waiting = false;
	sk->num_retransmit_tries = 0;
	sk->teardown_waiting = false;
	sk->isAcceptSocket = false;
	sk->num_connect_tries = 0; // number of xconnect tries (Xconnect will fail after MAX_CONNECT_TRIES trials)
	memset(sk->send_buffer, 0, sk->send_buffer_size * sizeof(WritablePacket*));
	memset(sk->recv_buffer, 0, sk->recv_buffer_size * sizeof(WritablePacket*));
	//sk->pending_connection_buf = new queue<sock>();
	//sk->pendingAccepts = new queue<xia::XSocketMsg*>();

	//Set the socket_type (reliable or not) in sock
	sk->sock_type = sock_type;

	// Map the source port to sock
	portToSock.set(_sport, sk);

	portToActive.set(_sport, true);

	hlim.set(_sport, HLIM_DEFAULT);
	nxt_xport.set(_sport, CLICK_XIA_NXT_TRN);

	// click_chatter("XSOCKET: sport=%hu\n", _sport);

	// Return result to API
	ReturnResult(_sport, xia_socket_msg, 0);
}

/*
** Xsetsockopt API handler
*/
void XTRANSPORT::Xsetsockopt(unsigned short _sport, xia::XSocketMsg *xia_socket_msg) {

	// click_chatter("\nSet Socket Option\n");
	xia::X_Setsockopt_Msg *x_sso_msg = xia_socket_msg->mutable_x_setsockopt();

	switch (x_sso_msg->opt_type())
	{
	case XOPT_HLIM:
	{
		int hl = x_sso_msg->int_opt();

		hlim.set(_sport, hl);
		//click_chatter("sso:hlim:%d\n",hl);
	}
	break;

	case XOPT_NEXT_PROTO:
	{
		int nxt = x_sso_msg->int_opt();
		nxt_xport.set(_sport, nxt);
		if (nxt == CLICK_XIA_NXT_XCMP)
			xcmp_listeners.push_back(_sport);
		else
			xcmp_listeners.remove(_sport);
	}
	break;

	default:
		// unsupported option
		break;
	}

	ReturnResult(_sport, xia_socket_msg); // TODO: return code
}

/*
** Xgetsockopt API handler
*/
void XTRANSPORT::Xgetsockopt(unsigned short _sport, xia::XSocketMsg *xia_socket_msg) {
	// click_chatter("\nGet Socket Option\n");
	xia::X_Getsockopt_Msg *x_sso_msg = xia_socket_msg->mutable_x_getsockopt();

	// click_chatter("opt = %d\n", x_sso_msg->opt_type());
	switch (x_sso_msg->opt_type())
	{
	case XOPT_HLIM:
	{
		x_sso_msg->set_int_opt(hlim.get(_sport));
		//click_chatter("gso:hlim:%d\n", hlim.get(_sport));
	}
	break;

	case XOPT_NEXT_PROTO:
	{
		x_sso_msg->set_int_opt(nxt_xport.get(_sport));
	}
	break;

	default:
		// unsupported option
		break;
	}

	ReturnResult(_sport, xia_socket_msg); // TODO: return code
}

void XTRANSPORT::Xbind(unsigned short _sport, xia::XSocketMsg *xia_socket_msg) {

	int rc = 0, ec = 0;

	//Bind XID
	//click_chatter("\n\nOK: SOCKET BIND !!!\\n");
	//get source DAG from protobuf message

	xia::X_Bind_Msg *x_bind_msg = xia_socket_msg->mutable_x_bind();

	String sdag_string(x_bind_msg->sdag().c_str(), x_bind_msg->sdag().size());

	//String sdag_string((const char*)p_in->data(),(const char*)p_in->end_data());
//	_errh->debug("\nbind requested to %s, length=%d\n", sdag_string.c_str(), (int)p_in->length());

	//String str_local_addr=_local_addr.unparse();
	//str_local_addr=str_local_addr+" "+xid_string;//Make source DAG _local_addr:SID

	//Set the source DAG in sock
	sock *sk = portToSock.get(_sport);
	if (sk->src_path.parse(sdag_string)) {
		sk->nxt = LAST_NODE_DEFAULT;
		sk->last = LAST_NODE_DEFAULT;
		sk->hlim = hlim.get(_sport);
		sk->isConnected = false;
		sk->initialized = true;
		sk->sdag = sdag_string;

		//Check if binding to full DAG or just to SID only
		Vector<XIAPath::handle_t> xids = sk->src_path.next_nodes( sk->src_path.source_node() );
		XID front_xid = sk->src_path.xid( xids[0] );
		struct click_xia_xid head_xid = front_xid.xid();
		uint32_t head_xid_type = head_xid.type;
		if(head_xid_type == _sid_type) {
			sk->full_src_dag = false;
		} else {
			sk->full_src_dag = true;
		}

		XID	source_xid = sk->src_path.xid(sk->src_path.destination_node());
		//XID xid(xid_string);
		//TODO: Add a check to see if XID is already being used

		// Map the source XID to source port (for now, for either type of tranports)
		XIDtoPort.set(source_xid, _sport);
		addRoute(source_xid);
//		printf("Xbind, S2P %d, %p\n", _sport, sk);
		portToSock.set(_sport, sk);

		//click_chatter("Bound");
		//click_chatter("set %d %d",_sport, __LINE__);

	} else {
		rc = -1;
		ec = EADDRNOTAVAIL;
	}

	ReturnResult(_sport, xia_socket_msg, rc, ec);
}

// FIXME: This way of doing things is a bit hacky.
void XTRANSPORT::XbindPush(unsigned short _sport, xia::XSocketMsg *xia_socket_msg) {

	int rc = 0, ec = 0;

	//Bind XID
	//get source DAG from protobuf message

	xia::X_BindPush_Msg *x_bindpush_msg = xia_socket_msg->mutable_x_bindpush();

	String sdag_string(x_bindpush_msg->sdag().c_str(), x_bindpush_msg->sdag().size());

	_errh->debug("\nbind requested to %s\n", sdag_string.c_str());

	//String str_local_addr=_local_addr.unparse();
	//str_local_addr=str_local_addr+" "+xid_string;//Make source DAG _local_addr:SID

	//Set the source DAG in sock
	sock *sk = portToSock.get(_sport);
	if (sk->src_path.parse(sdag_string)) {
		sk->nxt = LAST_NODE_DEFAULT;
		sk->last = LAST_NODE_DEFAULT;
		sk->hlim = hlim.get(_sport);
		sk->isConnected = false;
		sk->initialized = true;
		sk->sdag = sdag_string;

		//Check if binding to full DAG or just to SID only
		Vector<XIAPath::handle_t> xids = sk->src_path.next_nodes( sk->src_path.source_node() );		
		XID front_xid = sk->src_path.xid( xids[0] );
		struct click_xia_xid head_xid = front_xid.xid();
		uint32_t head_xid_type = head_xid.type;
		if(head_xid_type == _sid_type) {
			sk->full_src_dag = false; 
		} else {
			sk->full_src_dag = true;
		}

		XID	source_xid = sk->src_path.xid(sk->src_path.destination_node());
		//XID xid(xid_string);
		//TODO: Add a check to see if XID is already being used

		// Map the source XID to source port (for now, for either type of tranports)
		XIDtoPushPort.set(source_xid, _sport);
		addRoute(source_xid);

		portToSock.set(_sport, sk);

		//click_chatter("Bound");
		//click_chatter("set %d %d",_sport, __LINE__);

	} else {
		rc = -1;
		ec = EADDRNOTAVAIL;
		click_chatter("\n\nERROR: SOCKET PUSH BIND !!!\\n");
	}
	
	// (for Ack purpose) Reply with a packet with the destination port=source port
// 	click_chatter("\n\nPUSHBIND: DONE, SENDING ACK !!!\\n");
	ReturnResult(_sport, xia_socket_msg, rc, ec);
// 	click_chatter("\n\nAFTER PUSHBIND: DONE, SENDING ACK !!!\\n");
}

void XTRANSPORT::Xclose(unsigned short _sport, xia::XSocketMsg *xia_socket_msg)
{
	// Close port
	//click_chatter("Xclose: closing %d\n", _sport);

	sock *sk = portToSock.get(_sport);

	// Set timer
	sk->timer_on = true;
	sk->teardown_waiting = true;
	sk->teardown_expiry = Timestamp::now() + Timestamp::make_msec(_teardown_wait_ms);

	if (! _timer.scheduled() || _timer.expiry() >= sk->teardown_expiry )
		_timer.reschedule_at(sk->teardown_expiry);

	portToSock.set(_sport, sk);

	xcmp_listeners.remove(_sport);

	ReturnResult(_sport, xia_socket_msg);
}

void XTRANSPORT::Xconnect(unsigned short _sport, xia::XSocketMsg *xia_socket_msg)
{
	//click_chatter("Xconect: connecting %d\n", _sport);

	//isConnected=true;
	//String dest((const char*)p_in->data(),(const char*)p_in->end_data());
	//click_chatter("\nconnect to %s, length=%d\n",dest.c_str(),(int)p_in->length());

	xia::X_Connect_Msg *x_connect_msg = xia_socket_msg->mutable_x_connect();

	String dest(x_connect_msg->ddag().c_str());

	//String sdag_string((const char*)p_in->data(),(const char*)p_in->end_data());
	//click_chatter("\nconnect requested to %s, length=%d\n",dest.c_str(),(int)p_in->length());

	XIAPath dst_path;
	dst_path.parse(dest);

	sock *sk = portToSock.get(_sport);
	//click_chatter("connect %d %x",_sport, sk);

	if(!sk) {
		//click_chatter("Create DAGINFO connect %d %x",_sport, sk);
		//No local SID bound yet, so bind ephemeral one
		sk = new sock();
	} else {
		if (sk->synack_waiting) {
			// a connect is already in progress
			x_connect_msg->set_status(xia::X_Connect_Msg::XCONNECTING);
			ReturnResult(_sport, xia_socket_msg, -1, EALREADY);
		}
	}

	sk->dst_path = dst_path;
	sk->port = _sport;
	sk->isConnected = true;
	sk->initialized = true;
	sk->ddag = dest;
	sk->seq_num = 0;
	sk->ack_num = 0;
	sk->send_base = 0;
	sk->next_send_seqnum = 0;
	sk->next_recv_seqnum = 0;
	sk->num_connect_tries++; // number of xconnect tries (Xconnect will fail after MAX_CONNECT_TRIES trials)

	String str_local_addr = _local_addr.unparse_re();
	//String dagstr = sk->src_path.unparse_re();

	/* Use src_path set by Xbind() if exists */
	if(sk->sdag.length() == 0) {
		char xid_string[50];
		random_xid("SID", xid_string);

		str_local_addr = str_local_addr + " " + xid_string; //Make source DAG _local_addr:SID
		sk->src_path.parse_re(str_local_addr);
	}

	sk->nxt = LAST_NODE_DEFAULT;
	sk->last = LAST_NODE_DEFAULT;
	sk->hlim = hlim.get(_sport);

	XID source_xid = sk->src_path.xid(sk->src_path.destination_node());
	XID destination_xid = sk->dst_path.xid(sk->dst_path.destination_node());

	XIDpair xid_pair;
	xid_pair.set_src(source_xid);
	xid_pair.set_dst(destination_xid);

	// Map the src & dst XID pair to source port()
	//click_chatter("setting pair to port1 %d\n", _sport);

	XIDpairToPort.set(xid_pair, _sport);

	// Map the source XID to source port
	XIDtoPort.set(source_xid, _sport);
	addRoute(source_xid);

	// click_chatter("XCONNECT: set %d %x",_sport, sk);

	// Prepare SYN packet

	//Add XIA headers
	XIAHeaderEncap xiah;
	xiah.set_nxt(CLICK_XIA_NXT_TRN);
	xiah.set_last(LAST_NODE_DEFAULT);
	xiah.set_hlim(hlim.get(_sport));
	xiah.set_dst_path(dst_path);
	xiah.set_src_path(sk->src_path);

	//click_chatter("Sent packet to network");
	const char* dummy = "Connection_request";
	WritablePacket *just_payload_part = WritablePacket::make(256, dummy, strlen(dummy), 20);

	WritablePacket *p = NULL;

	TransportHeaderEncap *thdr = TransportHeaderEncap::MakeSYNHeader( 0, -1, 0, calc_recv_window(sk)); // #seq, #ack, length, recv_wind

	p = thdr->encap(just_payload_part);

	thdr->update();
	xiah.set_plen(strlen(dummy) + thdr->hlen()); // XIA payload = transport header + transport-layer data

	p = xiah.encap(p, false);

	delete thdr;

	// Set timer
	sk->timer_on = true;
	sk->synack_waiting = true;
	sk->expiry = Timestamp::now() + Timestamp::make_msec(_ackdelay_ms);

	if (! _timer.scheduled() || _timer.expiry() >= sk->expiry )
		_timer.reschedule_at(sk->expiry);

	// Store the syn packet for potential retransmission
	sk->syn_pkt = copy_packet(p, sk);

	portToSock.set(_sport, sk);
	XIAHeader xiah1(p);
	//String pld((char *)xiah1.payload(), xiah1.plen());
	// click_chatter("XCONNECT: %d: %s\n", _sport, (_local_addr.unparse()).c_str());
	output(NETWORK_PORT).push(p);

	//sk=portToSock.get(_sport);
	//click_chatter("\nbound to %s\n",portToSock.get(_sport)->src_path.unparse().c_str());

	// We return EINPROGRESS no matter what. If we're in non-blocking mode, the
	// API will pass EINPROGRESS on to the app. If we're in blocking mode, the API
	// will wait until it gets another message from xtransport notifying it that
	// the other end responded and the connection has been established.
	x_connect_msg->set_status(xia::X_Connect_Msg::XCONNECTING);
	ReturnResult(_sport, xia_socket_msg, -1, EINPROGRESS);
}

void XTRANSPORT::XreadyToAccept(unsigned short _sport, xia::XSocketMsg *xia_socket_msg)
{
	// If there is already a pending connection, return true now
	// If not, add this request to the pendingAccept queue
	sock *sk = portToSock.get(_sport);

	if (!sk->pending_connection_buf.empty()) {
		ReturnResult(_sport, xia_socket_msg);
	} else {
		// xia_socket_msg is saved on the stack; allocate a copy on the heap
		xia::XSocketMsg *xsm_cpy = new xia::XSocketMsg();
		xsm_cpy->CopyFrom(*xia_socket_msg);
		sk->pendingAccepts.push(xsm_cpy);
	}
}

void XTRANSPORT::Xaccept(unsigned short _sport, xia::XSocketMsg *xia_socket_msg)
{
	int rc = 0, ec = 0;
	
	// _sport is the *existing accept socket*
	unsigned short new_port = xia_socket_msg->x_accept().new_port();
	sock *sk = portToSock.get(_sport);

	hlim.set(new_port, HLIM_DEFAULT);
	nxt_xport.set(new_port, CLICK_XIA_NXT_TRN);

	if (!sk->pending_connection_buf.empty()) {
		sock *new_sk = sk->pending_connection_buf.front();
		new_sk->port = new_port;

		new_sk->seq_num = 0;
		new_sk->ack_num = 0;
		new_sk->send_base = 0;
		new_sk->hlim = hlim.get(new_port);
		new_sk->next_send_seqnum = 0;
		new_sk->next_recv_seqnum = 0;
		new_sk->isAcceptSocket = true; // FIXME backwards? shouldn't sk be the accpet socket?
		memset(new_sk->send_buffer, 0, new_sk->send_buffer_size * sizeof(WritablePacket*));
		memset(new_sk->recv_buffer, 0, new_sk->recv_buffer_size * sizeof(WritablePacket*));
		//new_sk->pending_connection_buf = new queue<sock>();
		//new_sk->pendingAccepts = new queue<xia::XSocketMsg*>();

		portToSock.set(new_port, new_sk);

		XID source_xid = new_sk->src_path.xid(new_sk->src_path.destination_node());
		XID destination_xid = new_sk->dst_path.xid(new_sk->dst_path.destination_node());

		XIDpair xid_pair;
		xid_pair.set_src(source_xid);
		xid_pair.set_dst(destination_xid);

		// Map the src & dst XID pair to source port
		XIDpairToPort.set(xid_pair, new_port);
		//printf("Xaccept pair to port %d %s %s\n", _sport, source_xid.unparse().c_str(), destination_xid.unparse().c_str());

		portToActive.set(new_port, true);

		// click_chatter("XACCEPT: (%s) my_sport=%d  my_sid=%s  his_sid=%s \n\n", (_local_addr.unparse()).c_str(), _sport, source_xid.unparse().c_str(), destination_xid.unparse().c_str());

		sk->pending_connection_buf.pop();

		XIAHeaderEncap xiah_new;
		xiah_new.set_nxt(CLICK_XIA_NXT_TRN);
		xiah_new.set_last(LAST_NODE_DEFAULT);
		xiah_new.set_hlim(HLIM_DEFAULT);
		xiah_new.set_dst_path(new_sk->dst_path);
		xiah_new.set_src_path(new_sk->src_path);

		//printf("Xaccept src: %s\n", new_sk->src_path.unparse().c_str());
		//printf("Xaccept dst: %s\n", new_sk->dst_path.unparse().c_str());

		const char* dummy = "Connection_granted";
		WritablePacket *just_payload_part = WritablePacket::make(256, dummy, strlen(dummy), 0);

		WritablePacket *p = NULL;

		xiah_new.set_plen(strlen(dummy));
		//click_chatter("Sent packet to network");

		TransportHeaderEncap *thdr_new = TransportHeaderEncap::MakeSYNACKHeader( 0, 0, 0, calc_recv_window(new_sk)); // #seq, #ack, length, recv_wind
		p = thdr_new->encap(just_payload_part);

		thdr_new->update();
		xiah_new.set_plen(strlen(dummy) + thdr_new->hlen()); // XIA payload = transport header + transport-layer data

		p = xiah_new.encap(p, false);
		delete thdr_new;
		output(NETWORK_PORT).push(p);

		// Get remote DAG to return to app
		xia::X_Accept_Msg *x_accept_msg = xia_socket_msg->mutable_x_accept();
		x_accept_msg->set_remote_dag(new_sk->dst_path.unparse().c_str()); // remote endpoint is dest from our perspective

	} else {
		rc = -1;
		ec = EWOULDBLOCK;
	}

	ReturnResult(_sport, xia_socket_msg, rc, ec);
}


// note this is only going to return status for a single socket in the poll response
// the only time we will return multiple sockets is when poll returns immediately
void XTRANSPORT::ProcessPollEvent(unsigned short _sport, unsigned int flags_out)
{
	// loop thru all the polls that are registered looking for the socket associated with _sport
	for (HashTable<unsigned short, PollEvent>::iterator it = poll_events.begin(); it != poll_events.end(); it++) {
		unsigned short pollport = it->first;
		PollEvent pe = it->second;

		HashTable<unsigned short, unsigned int>::iterator sevent = pe.events.find(_sport);

		// socket isn't in this poll instance, keep looking
		if (sevent == pe.events.end())
			continue;

		unsigned short port = sevent->first;
		unsigned int mask = sevent->second;

		// if flags_out isn't an error and doesn't match the event mask keep looking
		if (!(flags_out & mask) && !(flags_out & (POLLHUP | POLLERR | POLLNVAL)))
			continue;

		xia::XSocketMsg xsm;
		xsm.set_type(xia::XPOLL);
		xia::X_Poll_Msg *msg = xsm.mutable_x_poll();
		
		xia::X_Poll_Msg::PollFD *pfd = msg->add_pfds();
		pfd->set_flags(flags_out);
		pfd->set_port(port);

		msg->set_nfds(1);

		// do I need to set other flags in the return struct?
		ReturnResult(pollport, &xsm, 1, 0);

		// found the socket, decrement the polling count for all the sockets in the poll instance
		for (HashTable<unsigned short, unsigned int>::iterator pit = pe.events.begin(); pit != pe.events.end(); pit++) {
			port = pit->first;

			sock *sk = portToSock.get(port);
			sk->polling--;
		}

		// get rid of this poll event
		poll_events.erase(it);
	}
}

void XTRANSPORT::CancelPollEvent(unsigned short _sport)
{
	PollEvent pe;
	unsigned short pollport;
	HashTable<unsigned short, PollEvent>::iterator it;

	// loop thru all the polls that are registered looking for the socket associated with _sport
	for (it = poll_events.begin(); it != poll_events.end(); it++) {
		pollport = it->first;
		pe = it->second;

		if (pollport == _sport)
			break;
		pollport = 0;
	}

	if (pollport == 0) {
		// we didn't find any events for this control socket
		// should we report error in this case?
		return;
	}

	// we have the poll event associated with this control socket

	// decrement the polling count for all the sockets in the poll instance
	for (HashTable<unsigned short, unsigned int>::iterator pit = pe.events.begin(); pit != pe.events.end(); pit++) {
		unsigned short port = pit->first;

		sock *sk = portToSock.get(port);
		sk->polling--;
	}

	// get rid of this poll event
	poll_events.erase(it);
}


void XTRANSPORT::CreatePollEvent(unsigned short _sport, xia::X_Poll_Msg *msg)
{
	PollEvent pe;
	uint32_t nfds = msg->nfds();

	// printf("XPOLL Create:\nnfds:%d\n", nfds);

	for (int i = 0; i < nfds; i++) {
		const xia::X_Poll_Msg::PollFD& pfd = msg->pfds(i);

		int port = pfd.port();
		unsigned flags = pfd.flags();

		// ignore ports that are set to 0, or are negative
		if (port <= 0)
			continue;

		// add the socket to this poll event
		pe.events.set(port, flags);
		sock *sk = portToSock.get(port);

		// let the socket know a poll is enabled on it
		sk->polling++;
	}

	// register the poll event 
	poll_events.set(_sport, pe);
}


void XTRANSPORT::Xpoll(unsigned short _sport, xia::XSocketMsg *xia_socket_msg)
{
	xia::X_Poll_Msg *poll_in = xia_socket_msg->mutable_x_poll();

	if (poll_in->type() == xia::X_Poll_Msg::DOPOLL) {

		int actionable = 0;	
		xia::XSocketMsg msg_out;
		msg_out.set_type(xia::XPOLL);
		xia::X_Poll_Msg *poll_out = msg_out.mutable_x_poll();

		unsigned nfds = poll_in->nfds();

		// printf("XPOLL:\nnfds:%d\n", nfds);
		for (int i = 0; i < nfds; i++) {
			const xia::X_Poll_Msg::PollFD& pfd_in = poll_in->pfds(i);

			int port = pfd_in.port();
			unsigned flags = pfd_in.flags();
			// printf("port: %d, flags: %x\n", pfd_in.port(), pfd_in.flags());

			// skip over ignored ports
			if ( port <= 0) {
				// printf("skipping ignored port\n");
				continue;
			}

			sock *sk = portToSock.get(port);
			unsigned flags_out = 0;

			if (!sk) {
				// no socket state, we'll return an error right away
				// printf("No socket state found for %d\n", port);
				flags_out = POLLNVAL;
			
			} else {
				// is there any read data?
				if (flags & POLLIN) {
					if (sk->recv_pending) {
						// printf("read data avaialable on %d\n", port);
						flags_out |= POLLIN;
					}
				}

				if (flags & POLLOUT) {
					// see if the socket is writable
					// FIXME should we be looking for anything else (send window, etc...)
					if (sk->sock_type == SOCK_STREAM) {
						if (sk->isConnected) {
							// printf("stream socket is connected, so setting POLLOUT: %d\n", port);
							flags_out |= POLLOUT;
						}

					} else {
						// printf("assume POLLOUT is always set for datagram sockets: %d\n", port);
						flags_out |= POLLOUT;
					}
				}
			}

			if (flags_out) {
				// the socket can respond to the poll immediately
				xia::X_Poll_Msg::PollFD *pfd_out = poll_out->add_pfds();
				pfd_out->set_flags(flags_out);
				pfd_out->set_port(port);

				actionable++;
			}
		}

		// we can return a result right away
		if (actionable) {
			// printf("returning immediately number of actionable sockets is %d\n", actionable);
			poll_out->set_nfds(actionable);
			ReturnResult(_sport, &msg_out, actionable, 0);
		
		} else {
			// we can't return a result yet
			CreatePollEvent(_sport, poll_in);
		}
	} else { // type == CANCEL
		// cancel the poll(s) on this control socket
		CancelPollEvent(_sport);
	}
}


void XTRANSPORT::Xchangead(unsigned short _sport, xia::XSocketMsg *xia_socket_msg)
{
	UNUSED(_sport);

	xia::X_Changead_Msg *x_changead_msg = xia_socket_msg->mutable_x_changead();
	//String tmp = _local_addr.unparse();
	//Vector<String> ids;
	//cp_spacevec(tmp, ids);
	String AD_str(x_changead_msg->ad().c_str());
	String HID_str = _local_hid.unparse();
	String IP4ID_str(x_changead_msg->ip4id().c_str());
	_local_4id.parse(IP4ID_str);
	String new_local_addr;
	// If a valid 4ID is given, it is included (as a fallback) in the local_addr
	if(_local_4id != _null_4id) {
		new_local_addr = "RE ( " + IP4ID_str + " ) " + AD_str + " " + HID_str;
	} else {
		new_local_addr = "RE " + AD_str + " " + HID_str;
	}
	click_chatter("new address is - %s", new_local_addr.c_str());
	_local_addr.parse(new_local_addr);

	ReturnResult(_sport, xia_socket_msg);
}

void XTRANSPORT::Xreadlocalhostaddr(unsigned short _sport, xia::XSocketMsg *xia_socket_msg)
{
	// read the localhost AD and HID
	String local_addr = _local_addr.unparse();
	size_t AD_found_start = local_addr.find_left("AD:");
	size_t AD_found_end = local_addr.find_left(" ", AD_found_start);
	String AD_str = local_addr.substring(AD_found_start, AD_found_end - AD_found_start);
	String HID_str = _local_hid.unparse();
	String IP4ID_str = _local_4id.unparse();
	// return a packet containing localhost AD and HID
	xia::X_ReadLocalHostAddr_Msg *_msg = xia_socket_msg->mutable_x_readlocalhostaddr();
	_msg->set_ad(AD_str.c_str());
	_msg->set_hid(HID_str.c_str());
	_msg->set_ip4id(IP4ID_str.c_str());

	ReturnResult(_sport, xia_socket_msg);
}

void XTRANSPORT::Xupdatenameserverdag(unsigned short _sport, xia::XSocketMsg *xia_socket_msg)
{
	UNUSED(_sport);

	xia::X_Updatenameserverdag_Msg *x_updatenameserverdag_msg = xia_socket_msg->mutable_x_updatenameserverdag();
	String ns_dag(x_updatenameserverdag_msg->dag().c_str());
	//click_chatter("new nameserver address is - %s", ns_dag.c_str());
	_nameserver_addr.parse(ns_dag);

	ReturnResult(_sport, xia_socket_msg);
}

void XTRANSPORT::Xreadnameserverdag(unsigned short _sport, xia::XSocketMsg *xia_socket_msg)
{
	// read the nameserver DAG
	String ns_addr = _nameserver_addr.unparse();

	// return a packet containing the nameserver DAG
	xia::X_ReadNameServerDag_Msg *_msg = xia_socket_msg->mutable_x_readnameserverdag();
	_msg->set_dag(ns_addr.c_str());

	ReturnResult(_sport, xia_socket_msg);
}

void XTRANSPORT::Xisdualstackrouter(unsigned short _sport, xia::XSocketMsg *xia_socket_msg)
{
	// return a packet indicating whether this node is an XIA-IPv4 dual-stack router
	xia::X_IsDualStackRouter_Msg *_msg = xia_socket_msg->mutable_x_isdualstackrouter();
	_msg->set_flag(_is_dual_stack_router);

	ReturnResult(_sport, xia_socket_msg);
}

void XTRANSPORT::Xgetpeername(unsigned short _sport, xia::XSocketMsg *xia_socket_msg)
{
	sock *sk = portToSock.get(_sport);

	xia::X_GetPeername_Msg *_msg = xia_socket_msg->mutable_x_getpeername();
	_msg->set_dag(sk->dst_path.unparse().c_str());

	ReturnResult(_sport, xia_socket_msg);
}


void XTRANSPORT::Xgetsockname(unsigned short _sport, xia::XSocketMsg *xia_socket_msg)
{
	sock *sk = portToSock.get(_sport);

	xia::X_GetSockname_Msg *_msg = xia_socket_msg->mutable_x_getsockname();
	_msg->set_dag(sk->src_path.unparse().c_str());

	ReturnResult(_sport, xia_socket_msg);
}


void XTRANSPORT::Xsend(unsigned short _sport, xia::XSocketMsg *xia_socket_msg, WritablePacket *p_in)
{
	int rc = 0, ec = 0;
	//click_chatter("Xsend on %d\n", _sport);

	xia::X_Send_Msg *x_send_msg = xia_socket_msg->mutable_x_send();
	int pktPayloadSize = x_send_msg->payload().size();

	//click_chatter("pkt %s port %d", pktPayload.c_str(), _sport);
	//click_chatter("XSEND: %d bytes from (%d)\n", pktPayloadSize, _sport);

	//Find socket state
	sock *sk = portToSock.get(_sport);

	// Make sure the socket state isn't null
	if (rc == 0 && !sk) {
		rc = -1;
		ec = EBADF; // FIXME: is this the right error?
	}

	// Make sure socket is connected
	if (rc == 0 && !sk->isConnected) {
		rc = -1;
		ec = ENOTCONN;
	}

	// FIXME: in blocking mode, send should block until buffer space is available.
	int numUnACKedSentPackets = sk->next_send_seqnum - sk->send_base;
	if (rc == 0 && 
		numUnACKedSentPackets >= sk->send_buffer_size &&  // make sure we have space in send buf
		numUnACKedSentPackets >= sk->remote_recv_window) { // and receiver has space in recv buf

//		if (numUnACKedSentPackets >= sk->send_buffer_size)
//			printf("Not sending -- out of send buf space\n");
//		else if (numUnACKedSentPackets >= sk->remote_recv_window)
//			printf("Not sending -- out of recv buf space\n");

		rc = 0; // -1;  // set to 0 for now until blocking behavior is fixed
		ec = EAGAIN;
	}

	// If everything is OK so far, try sending
	if (rc == 0) {
		rc = pktPayloadSize;

		//Recalculate source path
		XID	source_xid = sk->src_path.xid(sk->src_path.destination_node());
		String str_local_addr = _local_addr.unparse_re() + " " + source_xid.unparse();
		//Make source DAG _local_addr:SID
		String dagstr = sk->src_path.unparse_re();

		//Client Mobility...
		if (dagstr.length() != 0 && dagstr != str_local_addr) {
			//Moved!
			// 1. Update 'sk->src_path'
			sk->src_path.parse_re(str_local_addr);
		}

		// Case of initial binding to only SID
		if(sk->full_src_dag == false) {
			sk->full_src_dag = true;
			String str_local_addr = _local_addr.unparse_re();
			XID front_xid = sk->src_path.xid(sk->src_path.destination_node());
			String xid_string = front_xid.unparse();
			str_local_addr = str_local_addr + " " + xid_string; //Make source DAG _local_addr:SID
			sk->src_path.parse_re(str_local_addr);
		}

		//Add XIA headers
		XIAHeaderEncap xiah;
		xiah.set_nxt(CLICK_XIA_NXT_TRN);
		xiah.set_last(LAST_NODE_DEFAULT);
		xiah.set_hlim(hlim.get(_sport));
		xiah.set_dst_path(sk->dst_path);
		xiah.set_src_path(sk->src_path);
		xiah.set_plen(pktPayloadSize);


		_errh->debug("XSEND: (%d) sent packet to %s, from %s\n", _sport, sk->dst_path.unparse_re().c_str(), sk->src_path.unparse_re().c_str());

		WritablePacket *just_payload_part = WritablePacket::make(p_in->headroom() + 1, (const void*)x_send_msg->payload().c_str(), pktPayloadSize, p_in->tailroom());

		WritablePacket *p = NULL;

		//Add XIA Transport headers
		TransportHeaderEncap *thdr = TransportHeaderEncap::MakeDATAHeader(sk->next_send_seqnum, sk->ack_num, 0, calc_recv_window(sk) ); // #seq, #ack, length, recv_wind
		p = thdr->encap(just_payload_part);

		thdr->update();
		xiah.set_plen(pktPayloadSize + thdr->hlen()); // XIA payload = transport header + transport-layer data

		p = xiah.encap(p, false);

		delete thdr;

		// Store the packet into buffer
		WritablePacket *tmp = sk->send_buffer[sk->seq_num % sk->send_buffer_size];
		sk->send_buffer[sk->seq_num % sk->send_buffer_size] = copy_packet(p, sk);
		if (tmp)
			tmp->kill();

		// click_chatter("XSEND: SENT DATA at (%s) seq=%d \n\n", dagstr.c_str(), daginfo->seq_num%MAX_WIN_SIZE);

		sk->seq_num++;
		sk->next_send_seqnum++;

		// Set timer
		sk->timer_on = true;
		sk->dataack_waiting = true;
		sk->num_retransmit_tries = 0;
		sk->expiry = Timestamp::now() + Timestamp::make_msec(_ackdelay_ms);

		if (! _timer.scheduled() || _timer.expiry() >= sk->expiry )
			_timer.reschedule_at(sk->expiry);

		portToSock.set(_sport, sk);

		//click_chatter("Sent packet to network");
		XIAHeader xiah1(p);
		String pld((char *)xiah1.payload(), xiah1.plen());
		//click_chatter("\n\n (%s) send (timer set at %f) =%s  len=%d \n\n", (_local_addr.unparse()).c_str(), (daginfo->expiry).doubleval(), pld.c_str(), xiah1.plen());
		output(NETWORK_PORT).push(p);
	}

	x_send_msg->clear_payload(); // clear payload before returning result
	ReturnResult(_sport, xia_socket_msg, rc, ec);
}

void XTRANSPORT::Xsendto(unsigned short _sport, xia::XSocketMsg *xia_socket_msg, WritablePacket *p_in)
{
	int rc = 0, ec = 0;

	xia::X_Sendto_Msg *x_sendto_msg = xia_socket_msg->mutable_x_sendto();

	String dest(x_sendto_msg->ddag().c_str());
	int pktPayloadSize = x_sendto_msg->payload().size();
	//click_chatter("\n SENDTO ddag:%s, payload:%s, length=%d\n",xia_socket_msg.ddag().c_str(), xia_socket_msg.payload().c_str(), pktPayloadSize);

	XIAPath dst_path;
	dst_path.parse(dest);

	//Find DAG info for this DGRAM
	sock *sk = portToSock.get(_sport);

	if(!sk) {
		//No local SID bound yet, so bind one
		sk = new sock();
	}

	if (sk->initialized == false) {
		sk->initialized = true;
		sk->full_src_dag = true;
		sk->port = _sport;
		String str_local_addr = _local_addr.unparse_re();

		char xid_string[50];
		random_xid("SID", xid_string);
		str_local_addr = str_local_addr + " " + xid_string; //Make source DAG _local_addr:SID

		sk->src_path.parse_re(str_local_addr);

		sk->last = LAST_NODE_DEFAULT;
		sk->hlim = hlim.get(_sport);

		XID	source_xid = sk->src_path.xid(sk->src_path.destination_node());

		XIDtoPort.set(source_xid, _sport); //Maybe change the mapping to XID->sock?
		addRoute(source_xid);
	}

	// Case of initial binding to only SID
	if(sk->full_src_dag == false) {
		sk->full_src_dag = true;
		String str_local_addr = _local_addr.unparse_re();
		XID front_xid = sk->src_path.xid(sk->src_path.destination_node());
		String xid_string = front_xid.unparse();
		str_local_addr = str_local_addr + " " + xid_string; //Make source DAG _local_addr:SID
		sk->src_path.parse_re(str_local_addr);
	}


	if(sk->src_path.unparse_re().length() != 0) {
		//Recalculate source path
		XID	source_xid = sk->src_path.xid(sk->src_path.destination_node());
		String str_local_addr = _local_addr.unparse_re() + " " + source_xid.unparse(); //Make source DAG _local_addr:SID
		sk->src_path.parse(str_local_addr);
	}

	portToSock.set(_sport, sk);

	sk = portToSock.get(_sport);

//	_errh->debug("sent packet from %s, to %s\n", daginfo->src_path.unparse_re().c_str(), dest.c_str());

	//Add XIA headers
	XIAHeaderEncap xiah;

	xiah.set_last(LAST_NODE_DEFAULT);
	xiah.set_hlim(hlim.get(_sport));
	xiah.set_dst_path(dst_path);
	xiah.set_src_path(sk->src_path);

	WritablePacket *just_payload_part = WritablePacket::make(p_in->headroom() + 1, (const void*)x_sendto_msg->payload().c_str(), pktPayloadSize, p_in->tailroom());

	WritablePacket *p = NULL;

	if (sk->sock_type == XSOCKET_RAW) {
		xiah.set_nxt(nxt_xport.get(_sport));

		xiah.set_plen(pktPayloadSize);
		p = xiah.encap(just_payload_part, false);

	} else {
		xiah.set_nxt(CLICK_XIA_NXT_TRN);
		xiah.set_plen(pktPayloadSize);

		//p = xiah.encap(just_payload_part, true);
		//click_chatter("\n\nSEND: %s ---> %s\n\n", daginfo->src_path.unparse_re().c_str(), dest.c_str());
		//click_chatter("payload=%s len=%d \n\n", x_sendto_msg->payload().c_str(), pktPayloadSize);

		//Add XIA Transport headers
		TransportHeaderEncap *thdr = TransportHeaderEncap::MakeDGRAMHeader(0); // length
		p = thdr->encap(just_payload_part);

		thdr->update();
		xiah.set_plen(pktPayloadSize + thdr->hlen()); // XIA payload = transport header + transport-layer data

		p = xiah.encap(p, false);
		delete thdr;
	}

	output(NETWORK_PORT).push(p);

	rc = pktPayloadSize;
	x_sendto_msg->clear_payload();
	ReturnResult(_sport, xia_socket_msg, rc, ec);
}

void XTRANSPORT::Xrecv(unsigned short _sport, xia::XSocketMsg *xia_socket_msg)
{
	sock *sk = portToSock.get(_sport);
	read_from_recv_buf(xia_socket_msg, sk);

	if (xia_socket_msg->x_recv().bytes_returned() > 0) {
		// Return response to API
		ReturnResult(_sport, xia_socket_msg, xia_socket_msg->x_recv().bytes_returned());
	} else if (!xia_socket_msg->blocking()) {
		// we're not blocking and there's no data, so let API know immediately
		sk->recv_pending = false;
		ReturnResult(_sport, xia_socket_msg, -1, EWOULDBLOCK);

	} else {
		// rather than returning a response, wait until we get data
		sk->recv_pending = true; // when we get data next, send straight to app

		// xia_socket_msg is saved on the stack; allocate a copy on the heap
		xia::XSocketMsg *xsm_cpy = new xia::XSocketMsg();
		xsm_cpy->CopyFrom(*xia_socket_msg);
		sk->pending_recv_msg = xsm_cpy;
	}
}

void XTRANSPORT::Xrecvfrom(unsigned short _sport, xia::XSocketMsg *xia_socket_msg)
{
	sock *sk = portToSock.get(_sport);
	read_from_recv_buf(xia_socket_msg, sk);

	if (xia_socket_msg->x_recvfrom().bytes_returned() > 0) {
		// Return response to API
		ReturnResult(_sport, xia_socket_msg, xia_socket_msg->x_recvfrom().bytes_returned());

	} else if (!xia_socket_msg->blocking()) {

		// we're not blocking and there's no data, so let API know immediately
		ReturnResult(_sport, xia_socket_msg, -1, EWOULDBLOCK);

	} else {
		// rather than returning a response, wait until we get data
		sk->recv_pending = true; // when we get data next, send straight to app

		// xia_socket_msg is saved on the stack; allocate a copy on the heap
		xia::XSocketMsg *xsm_cpy = new xia::XSocketMsg();
		xsm_cpy->CopyFrom(*xia_socket_msg);
		sk->pending_recv_msg = xsm_cpy;
	}
}

void XTRANSPORT::XrequestChunk(unsigned short _sport, xia::XSocketMsg *xia_socket_msg, WritablePacket *p_in)
{
	xia::X_Requestchunk_Msg *x_requestchunk_msg = xia_socket_msg->mutable_x_requestchunk();

	String pktPayload(x_requestchunk_msg->payload().c_str(), x_requestchunk_msg->payload().size());
	int pktPayloadSize = pktPayload.length();

	// send CID-Requests

	for (int i = 0; i < x_requestchunk_msg->dag_size(); i++) {
		String dest = x_requestchunk_msg->dag(i).c_str();
		//click_chatter("CID-Request for %s  (size=%d) \n", dest.c_str(), dag_size);
		//click_chatter("\n\n (%s) hi 3 \n\n", (_local_addr.unparse()).c_str());
		XIAPath dst_path;
		dst_path.parse(dest);

		//Find DAG info for this DGRAM
		sock *sk = portToSock.get(_sport);

		if(!sk) {
			//No local SID bound yet, so bind one
			sk = new sock();
		}

		if (sk->initialized == false) {
			sk->initialized = true;
			sk->full_src_dag = true;
			sk->port = _sport;
			String str_local_addr = _local_addr.unparse_re();

			char xid_string[50];
			random_xid("SID", xid_string);
			str_local_addr = str_local_addr + " " + xid_string; //Make source DAG _local_addr:SID

			sk->src_path.parse_re(str_local_addr);

			sk->last = LAST_NODE_DEFAULT;
			sk->hlim = hlim.get(_sport);

			XID	source_xid = sk->src_path.xid(sk->src_path.destination_node());

			XIDtoPort.set(source_xid, _sport); //Maybe change the mapping to XID->sock?
			addRoute(source_xid);

		}

		// Case of initial binding to only SID
		if(sk->full_src_dag == false) {
			sk->full_src_dag = true;
			String str_local_addr = _local_addr.unparse_re();
			XID front_xid = sk->src_path.xid(sk->src_path.destination_node());
			String xid_string = front_xid.unparse();
			str_local_addr = str_local_addr + " " + xid_string; //Make source DAG _local_addr:SID
			sk->src_path.parse_re(str_local_addr);
		}

		if(sk->src_path.unparse_re().length() != 0) {
			//Recalculate source path
			XID	source_xid = sk->src_path.xid(sk->src_path.destination_node());
			String str_local_addr = _local_addr.unparse_re() + " " + source_xid.unparse(); //Make source DAG _local_addr:SID
			sk->src_path.parse(str_local_addr);
		}

		portToSock.set(_sport, sk);

		sk = portToSock.get(_sport);

		_errh->debug("sent packet to %s, from %s\n", dest.c_str(), sk->src_path.unparse_re().c_str());

		//Add XIA headers
		XIAHeaderEncap xiah;
		xiah.set_nxt(CLICK_XIA_NXT_CID);
		xiah.set_last(LAST_NODE_DEFAULT);
		xiah.set_hlim(hlim.get(_sport));
		xiah.set_dst_path(dst_path);
		xiah.set_src_path(sk->src_path);
		xiah.set_plen(pktPayloadSize);

		WritablePacket *just_payload_part = WritablePacket::make(p_in->headroom() + 1, (const void*)x_requestchunk_msg->payload().c_str(), pktPayloadSize, p_in->tailroom());

		WritablePacket *p = NULL;

		//Add Content header
		ContentHeaderEncap *chdr = ContentHeaderEncap::MakeRequestHeader();
		p = chdr->encap(just_payload_part);
		p = xiah.encap(p, true);
		delete chdr;

		XID	source_sid = sk->src_path.xid(sk->src_path.destination_node());
		XID	destination_cid = dst_path.xid(dst_path.destination_node());

		XIDpair xid_pair;
		xid_pair.set_src(source_sid);
		xid_pair.set_dst(destination_cid);

		// Map the src & dst XID pair to source port
		XIDpairToPort.set(xid_pair, _sport);

		// Store the packet into buffer
		WritablePacket *copy_req_pkt = copy_cid_req_packet(p, sk);
		sk->XIDtoCIDreqPkt.set(destination_cid, copy_req_pkt);

		// Set the status of CID request
		sk->XIDtoStatus.set(destination_cid, WAITING_FOR_CHUNK);

		// Set the status of ReadCID reqeust
		sk->XIDtoReadReq.set(destination_cid, false);

		// Set timer
		Timestamp cid_req_expiry  = Timestamp::now() + Timestamp::make_msec(_ackdelay_ms);
		sk->XIDtoExpiryTime.set(destination_cid, cid_req_expiry);
		sk->XIDtoTimerOn.set(destination_cid, true);

		if (! _timer.scheduled() || _timer.expiry() >= cid_req_expiry )
			_timer.reschedule_at(cid_req_expiry);

		portToSock.set(_sport, sk);

		output(NETWORK_PORT).push(p);
	}

	ReturnResult(_sport, xia_socket_msg); // TODO: Error codes?
}

void XTRANSPORT::XgetChunkStatus(unsigned short _sport, xia::XSocketMsg *xia_socket_msg)
{
	xia::X_Getchunkstatus_Msg *x_getchunkstatus_msg = xia_socket_msg->mutable_x_getchunkstatus();

	int numCids = x_getchunkstatus_msg->dag_size();
	String pktPayload(x_getchunkstatus_msg->payload().c_str(), x_getchunkstatus_msg->payload().size());

	// send CID-Requests
	for (int i = 0; i < numCids; i++) {
		String dest = x_getchunkstatus_msg->dag(i).c_str();
		//click_chatter("CID-Request for %s  (size=%d) \n", dest.c_str(), dag_size);
		//click_chatter("\n\n (%s) hi 3 \n\n", (_local_addr.unparse()).c_str());
		XIAPath dst_path;
		dst_path.parse(dest);

		//Find DAG info for this DGRAM
		sock *sk = portToSock.get(_sport);

		XID	destination_cid = dst_path.xid(dst_path.destination_node());

		// Check the status of CID request
		HashTable<XID, int>::iterator it;
		it = sk->XIDtoStatus.find(destination_cid);

		if(it != sk->XIDtoStatus.end()) {
			// There is an entry
			int status = it->second;

			if(status == WAITING_FOR_CHUNK) {
				x_getchunkstatus_msg->add_status("WAITING");

			} else if(status == READY_TO_READ) {
				x_getchunkstatus_msg->add_status("READY");

			} else if(status == INVALID_HASH) {
				x_getchunkstatus_msg->add_status("INVALID_HASH");

			} else if(status == REQUEST_FAILED) {
				x_getchunkstatus_msg->add_status("FAILED");
			}

		} else {
			// Status query for the CID that was not requested...
			x_getchunkstatus_msg->add_status("FAILED");
		}
	}

	// Send back the report

	const char *buf = "CID request status response";
	x_getchunkstatus_msg->set_payload((const char*)buf, strlen(buf) + 1);

	ReturnResult(_sport, xia_socket_msg); // TODO: Error codes?
}

void XTRANSPORT::XreadChunk(unsigned short _sport, xia::XSocketMsg *xia_socket_msg)
{
	xia::X_Readchunk_Msg *x_readchunk_msg = xia_socket_msg->mutable_x_readchunk();
	click_chatter(">>READ chunk message from API %d\n", _sport);
	
	

	String dest = x_readchunk_msg->dag().c_str();
	WritablePacket *copy;
	//click_chatter("CID-Request for %s  (size=%d) \n", dest.c_str(), dag_size);
	//click_chatter("\n\n (%s) hi 3 \n\n", (_local_addr.unparse()).c_str());
	XIAPath dst_path;
	dst_path.parse(dest);

	//Find DAG info for this DGRAM
	sock *sk = portToSock.get(_sport);

	XID	destination_cid = dst_path.xid(dst_path.destination_node());

	// Update the status of ReadCID reqeust
	sk->XIDtoReadReq.set(destination_cid, true);
	portToSock.set(_sport, sk);

	// Check the status of CID request
	HashTable<XID, int>::iterator it;
	it = sk->XIDtoStatus.find(destination_cid);

	if(it != sk->XIDtoStatus.end()) {
		// There is an entry
		int status = it->second;

		if (status != READY_TO_READ  &&
			status != INVALID_HASH) {
			// Do nothing

		} else {
			// Send the buffered pkt to upper layer

			sk->XIDtoReadReq.set(destination_cid, false);
			portToSock.set(_sport, sk);

			HashTable<XID, WritablePacket*>::iterator it2;
			it2 = sk->XIDtoCIDresponsePkt.find(destination_cid);
			copy = copy_cid_response_packet(it2->second, sk);

			XIAHeader xiah(copy->xia_header());

			//Unparse dag info
			String src_path = xiah.src_path().unparse();

			xia::X_Readchunk_Msg *x_readchunk_msg = xia_socket_msg->mutable_x_readchunk();
			x_readchunk_msg->set_dag(src_path.c_str());
			x_readchunk_msg->set_payload((const char *)xiah.payload(), xiah.plen());

			//click_chatter("FROM CACHE. data length = %d  \n", str.length());
			_errh->debug("Sent packet to socket: sport %d dport %d", _sport, _sport);
			
			//TODO: remove
			click_chatter(">>send chunk to API after read %d\n", _sport);
			it2->second->kill();
			sk->XIDtoCIDresponsePkt.erase(it2);

			portToSock.set(_sport, sk);
		}
	}

	ReturnResult(_sport, xia_socket_msg); // TODO: Error codes?
}

void XTRANSPORT::XremoveChunk(unsigned short _sport, xia::XSocketMsg *xia_socket_msg)
{
	xia::X_Removechunk_Msg *x_rmchunk_msg = xia_socket_msg->mutable_x_removechunk();

	int32_t contextID = x_rmchunk_msg->contextid();
	String src(x_rmchunk_msg->cid().c_str(), x_rmchunk_msg->cid().size());
	//append local address before CID
	String str_local_addr = _local_addr.unparse_re();
	str_local_addr = "RE " + str_local_addr + " CID:" + src;
	XIAPath src_path;
	src_path.parse(str_local_addr);

	//Add XIA headers
	XIAHeaderEncap xiah;
	xiah.set_last(LAST_NODE_DEFAULT);
	xiah.set_hlim(HLIM_DEFAULT);
	xiah.set_dst_path(_local_addr);
	xiah.set_src_path(src_path);
	xiah.set_nxt(CLICK_XIA_NXT_CID);

	WritablePacket *just_payload_part = WritablePacket::make(256, (const void*)NULL, 0, 0);

	WritablePacket *p = NULL;
	ContentHeaderEncap  contenth(0, 0, 0, 0, ContentHeader::OP_LOCAL_REMOVECID, contextID);
	p = contenth.encap(just_payload_part);
	p = xiah.encap(p, true);

	_errh->debug("sent remove cid packet to cache");
	output(CACHE_PORT).push(p);

	xia::X_Removechunk_Msg *_msg = xia_socket_msg->mutable_x_removechunk();
	_msg->set_contextid(contextID);
	_msg->set_cid(src.c_str());
	_msg->set_status(0);

	ReturnResult(_sport, xia_socket_msg); // TODO: Error codes?
}

void XTRANSPORT::XputChunk(unsigned short _sport, xia::XSocketMsg *xia_socket_msg)
{
	xia::X_Putchunk_Msg *x_putchunk_msg = xia_socket_msg->mutable_x_putchunk();
	
	click_chatter(">>putchunk message from API %d\n", _sport);
	
	
	
//			int hasCID = x_putchunk_msg->hascid();
	int32_t contextID = x_putchunk_msg->contextid();
	int32_t ttl = x_putchunk_msg->ttl();
	int32_t cacheSize = x_putchunk_msg->cachesize();
	int32_t cachePolicy = x_putchunk_msg->cachepolicy();

	String pktPayload(x_putchunk_msg->payload().c_str(), x_putchunk_msg->payload().size());
	String src;

	/* Computes SHA1 Hash if user does not supply it */
	char hexBuf[3];
	int i = 0;
	SHA1_ctx sha_ctx;
	unsigned char digest[HASH_KEYSIZE];
	SHA1_init(&sha_ctx);
	SHA1_update(&sha_ctx, (unsigned char *)pktPayload.c_str() , pktPayload.length() );
	SHA1_final(digest, &sha_ctx);
	for(i = 0; i < HASH_KEYSIZE; i++) {
		sprintf(hexBuf, "%02x", digest[i]);
		src.append(const_cast<char *>(hexBuf), 2);
	}

	_errh->debug("ctxID=%d, length=%d, ttl=%d cid=%s\n", contextID, x_putchunk_msg->payload().size(), ttl, src.c_str());

	//append local address before CID
	String str_local_addr = _local_addr.unparse_re();
	str_local_addr = "RE " + str_local_addr + " CID:" + src;
	XIAPath src_path;
	src_path.parse(str_local_addr);

	_errh->debug("DAG: %s\n", str_local_addr.c_str());

	/*TODO: The destination dag of the incoming packet is local_addr:XID
	 * Thus the cache thinks it is destined for local_addr and delivers to socket
	 * This must be ignored. Options
	 * 1. Use an invalid SID
	 * 2. The cache should only store the CID responses and not forward them to
	 *	local_addr when the source and the destination HIDs are the same.
	 * 3. Use the socket SID on which putCID was issued. This will
	 *	result in a reply going to the same socket on which the putCID was issued.
	 *	Use the response to return 1 to the putCID call to indicate success.
	 *	Need to add sk/ephemeral SID generation for this to work.
	 * 4. Special OPCODE in content extension header and treat it specially in content module (done below)
	 */

	//Add XIA headers
	XIAHeaderEncap xiah;
	xiah.set_last(LAST_NODE_DEFAULT);
	xiah.set_hlim(hlim.get(_sport));
	xiah.set_dst_path(_local_addr);
	xiah.set_src_path(src_path);
	xiah.set_nxt(CLICK_XIA_NXT_CID);

	//Might need to remove more if another header is required (eg some control/DAG info)

	WritablePacket *just_payload_part = WritablePacket::make(256, (const void*)pktPayload.c_str(), pktPayload.length(), 0);

	WritablePacket *p = NULL;
	int chunkSize = pktPayload.length();
	ContentHeaderEncap  contenth(0, 0, pktPayload.length(), chunkSize, ContentHeader::OP_LOCAL_PUTCID,
								 contextID, ttl, cacheSize, cachePolicy);
	p = contenth.encap(just_payload_part);
	p = xiah.encap(p, true);

	_errh->debug("sent packet to cache");
	
	output(CACHE_PORT).push(p);

	// (for Ack purpose) Reply with a packet with the destination port=source port
	x_putchunk_msg->set_cid(src.c_str());
	ReturnResult(_sport, xia_socket_msg, 0, 0);
}




void XTRANSPORT::XpushChunkto(unsigned short _sport, xia::XSocketMsg *xia_socket_msg, WritablePacket *p_in)
{
	xia::X_Pushchunkto_Msg *x_pushchunkto_msg= xia_socket_msg->mutable_x_pushchunkto();

	int32_t contextID = x_pushchunkto_msg->contextid();
	int32_t ttl = x_pushchunkto_msg->ttl();
	int32_t cacheSize = x_pushchunkto_msg->cachesize();
	int32_t cachePolicy = x_pushchunkto_msg->cachepolicy();
	
	String pktPayload(x_pushchunkto_msg->payload().c_str(), x_pushchunkto_msg->payload().size());
	int pktPayloadSize = pktPayload.length();
	

	// send CID-Requests

	String dest = x_pushchunkto_msg->ddag().c_str();
	//click_chatter("CID-Request for %s  (size=%d) \n", dest.c_str(), dag_size);
	//click_chatter("\n\n (%s) hi 3 \n\n", (_local_addr.unparse()).c_str());
	XIAPath dst_path;
	dst_path.parse(dest);

	//Find DAG info for this DGRAM
	sock *sk = portToSock.get(_sport);

	if(!sk) {
		//No local SID bound yet, so bind one
		sk = new sock();
	}

	if (sk->initialized == false) {
		sk->initialized = true;
		sk->full_src_dag = true;
		sk->port = _sport;
		String str_local_addr = _local_addr.unparse_re();
		

		
		//TODO: AD->HID->SID->CID We can add SID here (AD->HID->SID->CID) if SID is passed or generate randomly
		// Contentmodule forwarding needs to be fixed to get this to work. (wrong comparison there)
		// Since there is no way to dictate policy to cache which content to accept right now this wasn't added.

		sk->src_path.parse_re(str_local_addr);

		sk->last = LAST_NODE_DEFAULT;
		sk->hlim = hlim.get(_sport);

		XID source_xid = sk->src_path.xid(sk->src_path.destination_node());

		XIDtoPort.set(source_xid, _sport); //Maybe change the mapping to XID->DAGinfo?
		addRoute(source_xid);

	}

	// Case of initial binding to only SID
	if(sk->full_src_dag == false) {
		sk->full_src_dag = true;
		String str_local_addr = _local_addr.unparse_re();
		XID front_xid = sk->src_path.xid(sk->src_path.destination_node());
		String xid_string = front_xid.unparse();
		str_local_addr = str_local_addr + " " + xid_string; //Make source DAG _local_addr:SID
		click_chatter("str_local_addr: %s", str_local_addr.c_str() );
		sk->src_path.parse_re(str_local_addr);
	}

	if(sk->src_path.unparse_re().length() != 0) {
		//Recalculate source path
		XID	source_xid = sk->src_path.xid(sk->src_path.destination_node());
		String str_local_addr = _local_addr.unparse_re() + " " + source_xid.unparse(); //Make source DAG _local_addr:SID
		click_chatter("str_local_addr: %s", str_local_addr.c_str() );
		sk->src_path.parse(str_local_addr);
	}

	portToSock.set(_sport, sk);

	sk = portToSock.get(_sport); // why are we refetching???

	_errh->debug("sent packet to %s, from %s\n", dest.c_str(), sk->src_path.unparse_re().c_str());

	click_chatter("PUSHCID: %s",x_pushchunkto_msg->cid().c_str());
	String src(x_pushchunkto_msg->cid().c_str(), x_pushchunkto_msg->cid().size());
	//append local address before CID
	String cid_str_local_addr = sk->src_path.unparse_re();
	cid_str_local_addr = "RE " + cid_str_local_addr + " CID:" + src;
	XIAPath cid_src_path;
	cid_src_path.parse(cid_str_local_addr);
	click_chatter("cid_local_addr: %s", cid_str_local_addr.c_str() );
	
	
	//Add XIA headers	
	XIAHeaderEncap xiah;
	xiah.set_nxt(CLICK_XIA_NXT_CID);
	xiah.set_last(LAST_NODE_DEFAULT);
	xiah.set_hlim(hlim.get(_sport));
	xiah.set_dst_path(dst_path);
	xiah.set_src_path(cid_src_path); //FIXME: is this the correct way to do it? Do we need SID? AD->HID->SID->CID 
	xiah.set_plen(pktPayloadSize);

// 	WritablePacket *just_payload_part = WritablePacket::make(p_in->headroom() + 1, (const void*)x_pushchunkto_msg->payload().c_str(), pktPayloadSize, p_in->tailroom());
	WritablePacket *just_payload_part = WritablePacket::make(256, (const void*)x_pushchunkto_msg->payload().c_str(), pktPayloadSize, 0);

	WritablePacket *p = NULL;

	//Add Content header
// 	ContentHeaderEncap *chdr = ContentHeaderEncap::MakePushHeader();
	int chunkSize = pktPayload.length();
	ContentHeaderEncap  contenth(0, 0, pktPayload.length(), chunkSize, ContentHeader::OP_PUSH,
								 contextID, ttl, cacheSize, cachePolicy);
	
	
	
	p = contenth.encap(just_payload_part);
	p = xiah.encap(p, true);
	

// 	XID	source_sid = daginfo->src_path.xid(daginfo->src_path.destination_node());
// 	XID	destination_cid = dst_path.xid(dst_path.destination_node());
	//FIXME this is wrong
	XID	source_cid = cid_src_path.xid(cid_src_path.destination_node());
// 	XID	source_cid = daginfo->src_path.xid(cid_src_path.destination_node());
	XID	destination_sid = dst_path.xid(dst_path.destination_node());

	XIDpair xid_pair;
	xid_pair.set_src(source_cid);
	xid_pair.set_dst(destination_sid);

	// Map the src & dst XID pair to source port
	XIDpairToPort.set(xid_pair, _sport);


	portToSock.set(_sport, sk);

	output(NETWORK_PORT).push(p);
	ReturnResult(_sport, xia_socket_msg, 0, 0);
}


CLICK_ENDDECLS

EXPORT_ELEMENT(XTRANSPORT)
ELEMENT_REQUIRES(userlevel)
ELEMENT_REQUIRES(XIAContentModule)
ELEMENT_MT_SAFE(XTRANSPORT)
