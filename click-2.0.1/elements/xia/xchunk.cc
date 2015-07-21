#include "../../userlevel/xia.pb.h"
#include <click/config.h>
#include <click/glue.hh>
#include <click/error.hh>
#include <click/confparse.hh>
#include <click/packet_anno.hh>
#include <click/packet.hh>
#include <click/vector.hh>


#include "xchunk.hh"
#include "xtransport.hh"
#include <click/xiatransportheader.hh>

#define TCPTIMERS
#define TCPOUTFLAGS
#define TCPSTATES

#define UNUSED(x) ((void)(x))
#define min(a,b) (((a)<(b))?(a):(b))
#define max(a,b) (((a)>(b))?(a):(b))

#define TCP_PAWS_IDLE	(24 * 24 * 60 * 60 * PR_SLOWHZ)

/* for modulo comparisons of timestamps */
#define TSTMP_LT(a,b)	((int)((a)-(b)) < 0)
#define TSTMP_GEQ(a,b)	((int)((a)-(b)) >= 0)

#define _tcp_rcvseqinit(tp) \
    (tp)->rcv_adv = (tp)->rcv_nxt = (tp)->irs + 1

#define _tcp_sendseqinit(tp) \
    (tp)->snd_una = (tp)->snd_nxt = (tp)->snd_max = \
(tp)->snd_up = (tp)->iss

CLICK_DECLS

void
XChunk::push(Packet *_p) {
	WritablePacket *p = _p->uniqueify(); 
	tcp_input(p);
}


inline void 
XChunk::print_tcpstats(WritablePacket *p, const char* label)
{
 //    const click_tcp *tcph= p->tcp_header();
 //    const click_ip 	*iph = p->ip_header();
	// int len = ntohs(iph->ip_len) - sizeof(click_ip) - (tcph->th_off << 2); 

	//debug_output(VERB_TCPSTATS, "[%s] [%s] S/A: [%u/%u] len: [%u] 59: [%u] 60: [%u] 62: [%u] 63: [%u] 64: [%u] 65: [%u] 67: [%u] 68: [%u] 80:[%u] 81:[%u] fifo: [%u] q1st/len: [%u/%u] qlast: [%u] qbtok: [%u] qisord: [%u]", SPKRNAME, label, ntohl(tcph->th_seq), ntohl(tcph->th_ack), len, tp->snd_una, tp->snd_nxt, tp->snd_wl1, tp->snd_wl2, tp->iss, tp->snd_wnd, tp->rcv_wnd, tp->rcv_nxt, tp->snd_cwnd, tp->snd_ssthresh, _q_usr_input.byte_length(), _q_recv.first(), _q_recv.first_len(), _q_recv.last(), _q_recv.bytes_ok(), _q_recv.is_ordered());

}


// Stateful TCP segment input (recvd packet) handling
void 
XChunk::tcp_input(WritablePacket *p)
{
    unsigned 	tiwin, tiflags; 
    u_long		ts_val, ts_ecr;
    int			len; //,tlen; /* seems to be unused */
    unsigned	off, optlen;
    u_char		*optp;
    int		ts_present = 0;
    int 	iss = 0; 
    int 	todrop, acked, ourfinisacked, needoutput = 0;
    struct 	mini_tcpip  ti; 

//  tcp_seq_t	tseq; 
    XIAHeader xiah(p->xia_header());
    TransportHeader thdr(p);

    click_tcp *tcph= (click_tcp *)thdr.header();
    if (tcph == NULL)
    {
    	click_chatter("Invalid header\n");
    }
    printf("Tcp input with state %d\n", tp->t_state);
    get_transport()->_tcpstat.tcps_rcvtotal++; 

    /* we need to copy ti, since we need it later */
    ti.ti_seq = ntohl(tcph->th_seq);
    ti.ti_ack = ntohl(tcph->th_ack);
    ti.ti_off = tcph->th_off;
    ti.ti_flags = tcph->th_flags;
    ti.ti_win = ntohs(tcph->th_win);
    ti.ti_urp = ntohs(tcph->th_urp);
    ti.ti_len = (uint16_t)(xiah.plen() - thdr.hlen());
	   
	printf("\t\t\ttcpinput flag is %d\n", ti.ti_flags);
	printf("\t\t\ttcpinput seq is %d\n", (ti.ti_seq));
	printf("\t\t\ttcpinput ack is %d\n", (ti.ti_ack));
	printf("\t\t\ttcpinput data length is %d\n", (int)ti.ti_len);
    /*205 packet should be sane, skip tests */ 
    off = ti.ti_off << 2; 
    if (0&&off < sizeof(click_tcp)) {
		get_transport()->_tcpstat.tcps_rcvbadoff++; 
	    p->kill();
		return;
    }
    // ti.ti_len -= sizeof(click_tcp) + off; 

    if (tp->so_flags & SO_FIN_AFTER_TCP_IDLE)
		tp->t_timer[TCPT_IDLE] = get_transport()->globals()->so_idletime; 

    /*237*/
    optlen = off - sizeof(click_tcp);
    printf("\t\t\ttcpinput option length is %d\n", (int)optlen);
    optp = thdr.tcpopt();
    // optp   = (u_char *)iph + 40;

	/* TODO Timestamp prediction */

    /*257*/
    tiflags = ti.ti_flags;

    /*293*/
    if ((tiflags & TH_SYN) == 0) 
		tiwin = ti.ti_win << tp->snd_scale; 
    else
		tiwin = ti.ti_win;

    /*334*/
    tp->t_idle = 0; 
    tp->t_timer[TCPT_KEEP] = get_transport()->globals()->tcp_keepidle; 

    /*344*/
	_tcp_dooptions(optp, optlen, ti.ti_flags, &ts_present, &ts_val, &ts_ecr);

    /*347 TCP "Fast Path" packet processing */ 

	/* 
	 * Header prediction: check for the two common cases of a uni-directional
	 * data xfer.  If the packet has no control flags, is in-sequence, the
	 * window didn't change and we're not retransmitting, it's a candidate.  If
	 * the length is zero and the ack moved forward, we're the sender side of
	 * the xfer.  Just free the data acked & wake any higher level process that
	 * was blocked waiting for space.  If the length is non-zero and the ack
	 * didn't move, we're the receiver side.  If we're getting packets in-order
	 * (the reassembly queue is empty), add the data to the socket buffer and
	 * note that we need a delayed ack.
	 */
	if (tp->t_state == TCPS_ESTABLISHED &&
		(tiflags & (TH_SYN|TH_FIN|TH_RST|TH_URG|TH_ACK)) == TH_ACK &&
		(!ts_present || TSTMP_GEQ(ts_val, tp->ts_recent)) &&
		ti.ti_seq == tp->rcv_nxt &&
		tiwin && 
		tiwin == tp->snd_wnd &&
		tp->snd_nxt == tp->snd_max) {

			// We have entered the fast path
			printf("tcpinput: Enter the fast path\n");
			// print_tcpstats(p, "tcp_input (fp)");

			/* If last ACK falls within this segment's sequence numbers,
			 *  record the timestamp. */
			if (ts_present && SEQ_LEQ(ti.ti_seq, tp->last_ack_sent) &&
				SEQ_LT(tp->last_ack_sent, ti.ti_seq + ti.ti_len)) {
				tp->ts_recent_age = get_transport()->tcp_now();
				tp->ts_recent = ts_val;
			}

			if (ti.ti_len == 0) {
				if (SEQ_GT(ti.ti_ack, tp->snd_una) &&
					SEQ_LEQ(ti.ti_ack, tp->snd_max) &&
					tp->snd_cwnd >= tp->snd_wnd) {

					cout << "this is a pure ack for outstanding data" << endl;
					//debug_output(VERB_TCP, "[%s] got pure ack: [%u]", SPKRNAME, ti.ti_ack);
					++(get_transport()->_tcpstat.tcps_predack);
					if (ts_present)
						tcp_xmit_timer((get_transport()->tcp_now()) - ts_ecr+1);
					else if (tp->t_rtt && SEQ_GT(ti.ti_ack, tp->t_rtseq))
						tcp_xmit_timer(tp->t_rtt);

					acked = ti.ti_ack - tp->snd_una;
					(get_transport()->_tcpstat.tcps_rcvackpack)++;
					get_transport()->_tcpstat.tcps_rcvackbyte += acked;
					
					// We can now drop data we know was recieved by the other side
					_q_usr_input.drop_until(acked); 
					tp->snd_una = ti.ti_ack;
					p->kill(); 

					/* If all outstanding data are acked, stop
					 * retransmit timer, otherwise restart timer
					 * using current (possibly backed-off) value.
					 * If process is waiting for space,
					 * wakeup/selwakeup/signal.  If data
					 * are ready to send, let tcp_output
					 * decide between more output or persist.
					 */
					if (tp->snd_una == tp->snd_max)
						tp->t_timer[TCPT_REXMT] = 0;
					else if (tp->t_timer[TCPT_PERSIST] == 0)
						tp->t_timer[TCPT_REXMT] = tp->t_rxtcur;

					if (! _q_usr_input.is_empty()) 
						tcp_output(); 
					return;
				}
			} else if (ti.ti_ack == tp->snd_una &&
				(_q_recv.is_empty() || _q_recv.is_ordered()) &&
				(so_recv_buffer_size > _q_recv.bytes_ok() + ti.ti_len)) {
				/* this is a pure, in-sequence data packet
				 * where the reassembly queue is empty or in order and
				 * we have enough buffer space to take it.
				 */

				//debug_output(VERB_TCP, "[%s] got pure data: [%u]", SPKRNAME, ti.ti_seq);
				//debug_output(VERB_TCPSTATS, "input (fp) updating rcv_nxt [%u] -> [%u]", tp->rcv_nxt, tp->rcv_nxt + ti.ti_len);
				tp->rcv_nxt += ti.ti_len;

				// Update some TCP Stats
				++(get_transport()->_tcpstat.tcps_preddat);
				(get_transport()->_tcpstat.tcps_rcvpack)++;
				get_transport()->_tcpstat.tcps_rcvbyte += ti.ti_len;
				
				/* Drop TCP/IP hdrs and TCP opts, add data to recv queue. */
				WritablePacket *copy = WritablePacket::make(0, (const void*)thdr.payload(), (uint32_t)ti.ti_len, 0);
				p -> kill();

				/* _q_recv.push() corresponds to the tcp_reass function whose purpose is
				 * to put all data into the CTCPQueue for both possible reassembly and
				 * in-order presentation to the "application socket" which in the
				 * TCPget_transport is the stateless pull port. */
				if (_q_recv.push(copy, ti.ti_seq, ti.ti_seq + ti.ti_len) < 0) {
					//debug_output(VERB_ERRORS, "Fast Path segment push into q_recv FAILED");
				}

				// If the reassembly queue has data, a gap should have just been
				// filled - then we set rcv_next to the last seq num in the queue to
				// indicate the next packet we expect to get from the sender.
				if (! _q_recv.is_empty() && has_pullable_data()) {
					tp->rcv_nxt = _q_recv.last_nxt();
					if (polling) {
						// tell API we are readable
						get_transport()->ProcessPollEvent(port, POLLIN);
					}
					check_for_and_handle_pending_recv();
					//debug_output(VERB_TCPSTATS, "input (fp) updating rcv_nxt to [%u]", tp->rcv_nxt);
				}

				tp->t_flags |= TF_DELACK;
				tcp_output();
				return;
			}
		}

	print_tcpstats(p, "tcp_input (sp)");
	printf("Slow path begins\n");
    /* 438 TCP "Slow Path" processing begins here */
    WritablePacket *copy = NULL;
    if (ti.ti_len)
    {
    	copy = WritablePacket::make(0, (const void*)thdr.payload(), (uint32_t)ti.ti_len, 0);
    }
	

    int win = so_recv_buffer_space();
	if (win < 0) { win = 0; }
	tp->rcv_wnd = max(win, (int)(tp->rcv_adv - tp->rcv_nxt)); 

    /* 456 Transitioning FROM tp->t_state TO... */
	switch (tp->t_state) {
		case TCPS_CLOSED:
		case TCPS_LISTEN:
			/* If the RST flag is set */
			if (tiflags & TH_RST) 
				goto drop;
			/* If ACK is set */
			if (tiflags & TH_ACK) 
				goto dropwithreset; 
			/* If the SYN flag is not set exclusively */
			if (!(tiflags & TH_SYN)) 
				goto drop; 

			/*479 no need to do socket stuff */

			/* 506 we don't use a template */

			/* 512 we have handled to options already */

			/* 515 */
			if (iss) {
				tp->iss = iss; 
			} else {
				printf("tcpinput TCPS_LISTEN: You should pick a correct tcpiss\n");
				tp->iss = 0x1; /* TODO: sensible iss function */
				//tp->iss = _tcp_iss(); /* suggested sensible iss function */
			}
			tp->irs = ti.ti_seq; 
			_tcp_sendseqinit(tp);
			_tcp_rcvseqinit(tp);
			tp->t_flags |= TF_ACKNOW;
			tcp_set_state(TCPS_SYN_RECEIVED); 
			tp->t_timer[TCPT_KEEP] = TCPTV_KEEP_INIT; 
			get_transport()->_tcpstat.tcps_accepts++;


			// // If the app is ready for a new connection, alert it
			// XSocketMsg *acceptXSM = sk->pendingAccepts.front();
			// get_transport()->ReturnResult(_dport, acceptXSM);
			// sk->pendingAccepts.pop();
			// delete acceptXSM;

			goto trimthenstep6;

			/* 530 */
		case TCPS_SYN_SENT:
			if ((tiflags & TH_ACK) && 
				(SEQ_LEQ(ti.ti_ack, tp->iss) ||
				 SEQ_GT(ti.ti_ack, tp->snd_max))) 
			goto dropwithreset;

			if (tiflags & TH_RST) {
				if (tiflags & TH_ACK) {
					tcp_drop(ECONNREFUSED); 
				}
				goto drop; 
			}
			if ((tiflags & TH_SYN) == 0)
				goto drop; 
			/* 554 */
			if (tiflags & TH_ACK) {
				tp->snd_una = ti.ti_ack; 
				if (SEQ_LT(tp->snd_nxt, tp->snd_una))
					tp->snd_nxt = tp->snd_una; 
			}
			tp->t_timer[TCPT_REXMT] = 0; 
			tp->irs = ti.ti_seq; 
			_tcp_rcvseqinit(tp); 
			tp->t_flags |= TF_ACKNOW; 
			if (tiflags & TH_ACK && SEQ_GT(tp->snd_una, tp->iss)) {
	    		get_transport() -> ChangeState(this, CONNECTED);
				tcp_set_state(TCPS_ESTABLISHED);
				printf("\t\t\t\tClient side 3way handshake is done.\n");
				if (polling) {
					// tell API we are writble now
					get_transport()->ProcessPollEvent(port, POLLOUT);
				}

				//sk->expiry = Timestamp::now() + Timestamp::make_msec(_ackdelay_ms);

				// Notify API that the connection is established
				XSocketMsg xsm;
				xsm.set_type(XCONNECT);
				xsm.set_sequence(0); // TODO: what should this be?
				xia::X_Connect_Msg *connect_msg = xsm.mutable_x_connect();
				connect_msg->set_ddag(src_path.unparse().c_str());
				connect_msg->set_status(X_Connect_Msg::XCONNECTED);
				get_transport()->ReturnResult(port, &xsm);

				/* Apply Window Scaling Options if set in incoming header */
				if ((tp->t_flags & (TF_RCVD_SCALE | TF_REQ_SCALE)) == 
					(TF_RCVD_SCALE | TF_REQ_SCALE)) { 
					tp->snd_scale = tp->requested_s_scale; 
					tp->rcv_scale = tp->request_r_scale; 
				}

				/* Record the RTT if set in incoming header */
				if (tp->t_rtt) { 
					tcp_xmit_timer(tp->t_rtt);
				} 
			} else {
				tcp_set_state(TCPS_SYN_RECEIVED); 
			}
			/* 583 */
	trimthenstep6:
		ti.ti_seq++;
		/* we don't accept half of a packet */
		if (ti.ti_len > tp->rcv_wnd) {
			goto dropafterack;
		}
		tp->snd_wl1 = ti.ti_seq;    // @Harald: I noticed that these values were +1 greater than in the book
		tp->rcv_up = ti.ti_seq + 1;  
		goto step6;
	}

    /* 602 */ 
    /* timestamp processing */
    /*
     * States other than LISTEN or SYN_SENT.
     * First check timestamp, if present.
     * Then check that at least some bytes of segment are within 
     * receive window.  If segment begins before rcv_nxt,
     * drop leading data (and SYN); if nothing left, just ack.
     * 
     * RFC 1323 PAWS: If we have a timestamp reply on this segment
     * and it's less than ts_recent, drop it.
     */
    if (ts_present && (tiflags & TH_RST) == 0 && tp->ts_recent &&
	    TSTMP_LT(ts_val, tp->ts_recent)) {

		// print_tcpstats(p, "tcp_input:ts - dan doesn't expect code execution to reach here unless connection is VERY old");
		/* Check to see if ts_recent is over 24 days old.  */
		if ((int)(get_transport()->tcp_now() - tp->ts_recent_age) > TCP_PAWS_IDLE) {
			/*
			 * Invalidate ts_recent.  If this segment updates ts_recent, the age
			 * will be reset later and ts_recent will get a valid value.  If it
			 * does not, setting ts_recent to zero will at least satisfy the
			 * requirement that zero be placed in the timestamp echo reply when
			 * ts_recent isn't valid.  The age isn't reset until we get a valid
			 * ts_recent because we don't want out-of-order segments to be
			 * dropped when ts_recent is old.
			 */
			tp->ts_recent = 0;
		} else {
			get_transport()->_tcpstat.tcps_rcvduppack++;
			get_transport()->_tcpstat.tcps_rcvdupbyte += ti.ti_len;
			get_transport()->_tcpstat.tcps_pawsdrop++;
			goto dropafterack;
		}
    }

    /* 635 646 */ 
    /* Determine if we need to trim the head off of an incoming segment */ 
    todrop = tp->rcv_nxt - ti.ti_seq; 
    printf("tp -> rcv_nxt %d, tiseq %d\n", (int)tp->rcv_nxt, (int)ti.ti_seq);

	// todrop is > 0 IF the incoming segment begins prior to the end of the last
	// recieved segment (a.k.a. tp->rcv_nxt)
	if (todrop > 0) {
		if (tiflags & TH_SYN) { 
				tiflags &= ~TH_SYN; 
			ti.ti_seq++; 
			if (ti.ti_urp > 1) 
				ti.ti_urp--; 
			else
				tiflags &= ~TH_URG; 
			todrop --; 
		}
		if (todrop >= ti.ti_len) { 
			get_transport()->_tcpstat.tcps_rcvduppack++; 
			get_transport()->_tcpstat.tcps_rcvdupbyte += ti.ti_len; 

			if ((tiflags & TH_FIN && todrop == ti.ti_len + 1)) {
				todrop = ti.ti_len; 
				tiflags &= ~TH_FIN; 
				tp->t_flags |= TF_ACKNOW; 
			} else {
				if (todrop != 0 || (tiflags & TH_ACK) == 0)
					goto dropafterack; 
			}
		} else {
			get_transport()->_tcpstat.tcps_rcvpartduppack++;
			get_transport()->_tcpstat.tcps_rcvpartdupbyte += todrop;
		}

		/* alternatively */
		// if (todrop > ti.ti_len ||
		// 	topdrop == ti.ti_len && (tiflags & TH_FIN) == 0) {
		// 	tiflags &= ~TH_FIN;
		// 	tp->t_flags |= TF_ACKNOW;
		// 	todrop = ti.ti_len;
		// 	get_transport()->_tcpstat.tcps_rcvdupbyte += todrop; 
		// 	get_transport()->_tcpstat.tcps_rcvduppack++; 
		// } else {
		// 	get_transport()->_tcpstat.tcps_rcvpartduppack++;
		// 	get_transport()->_tcpstat.tcps_rcvpartdupbyte += todrop;
		// }
		printf("becareful 465\n");
		if (copy != NULL) copy->pull(todrop);
		printf("bad\n");
		ti.ti_seq += todrop;
		ti.ti_len -= todrop;
		if (ti.ti_urp > todrop) { 
			ti.ti_urp -= todrop; 
		} else { 
			tiflags &= ~TH_URG; 
			ti.ti_urp = 0; 
		}
	}	


    /* 687 */
    /* drop after socket close */
    if (tp->t_state > TCPS_CLOSE_WAIT && ti.ti_len) { 
		tcp_set_state(TCPS_CLOSED); 
		get_transport()->_tcpstat.tcps_rcvafterclose++; 
		goto dropwithreset; 
    }	

	/* 697 More segment trimming: If segment ends after window, drop trailing
	 * data (and PUSH and FIN); if nothing left, just ACK. */
	todrop = (ti.ti_seq+ti.ti_len) - (tp->rcv_nxt+tp->rcv_wnd);
	if (todrop > 0) {
		get_transport()->_tcpstat.tcps_rcvpackafterwin++;
		if (todrop >= ti.ti_len) {
			get_transport()->_tcpstat.tcps_rcvbyteafterwin += ti.ti_len;
			/*
			 * If a new connection request is received
			 * while in TIME_WAIT, drop the old connection
			 * and start over if the sequence numbers
			 * are above the previous ones.
			 */
			/*FIXME: */ 
			/* 
			if (tiflags & TH_SYN &&
			    tp->t_state == TCPS_TIME_WAIT &&
			    SEQ_GT(ti->ti_seq, tp->rcv_nxt)) {
				iss = tp->rcv_nxt + TCP_ISSINCR;

				tp = tcp_close(tp);
				goto findpcb;
			}
			*/ 
			/*
			 * If window is closed can only take segments at
			 * window edge, and have to drop data and PUSH from
			 * incoming segments.  Continue processing, but
			 * remember to ack.  Otherwise, drop segment
			 * and ack.
			 */
			if (tp->rcv_wnd == 0 && ti.ti_seq == tp->rcv_nxt) {
				tp->t_flags |= TF_ACKNOW;
				get_transport()->_tcpstat.tcps_rcvwinprobe++;
			} else
				goto dropafterack;
		} else {
			get_transport()->_tcpstat.tcps_rcvbyteafterwin += todrop;
		}
		printf("becareful 535\n");
		copy->pull(todrop); 
		ti.ti_len -= todrop;
		tiflags &= ~(TH_PUSH|TH_FIN);
	}


    /*737*/
    /* record timestamp*/
	if (ts_present && SEQ_LEQ(ti.ti_seq, tp->last_ack_sent) &&
		SEQ_LT(tp->last_ack_sent, ti.ti_seq + ti.ti_len + 
		((tiflags & (TH_SYN|TH_FIN)) != 0))) {
		tp->ts_recent_age = get_transport()->tcp_now();
		tp->ts_recent = ts_val;
	}


    /* 747 process RST */
	/*
	 * If the RST bit is set examine the state:
	 *    SYN_RECEIVED STATE:
	 *	If passive open, return to LISTEN state.
	 *	If active open, inform user that connection was refused.
	 *    ESTABLISHED, FIN_WAIT_1, FIN_WAIT2, CLOSE_WAIT STATES:
	 *	Inform user that connection was reset, and close tcb.
	 *    CLOSING, LAST_ACK, TIME_WAIT STATES
	 *	Close the tcb.
	 */
	if (tiflags & TH_RST) {
		printf("551\n");
		switch (tp->t_state) {
		case TCPS_SYN_RECEIVED:
			tp->so_error = ECONNREFUSED;
			goto close;
		case TCPS_ESTABLISHED:
		case TCPS_FIN_WAIT_1:
		case TCPS_FIN_WAIT_2:
		case TCPS_CLOSE_WAIT:
			tp->so_error = ECONNRESET;
		close:
			tp->t_state = TCPS_CLOSED;
			get_transport()->_tcpstat.tcps_drops++;
			tcp_set_state(TCPS_CLOSED);
			goto drop;
		case TCPS_CLOSING:
		case TCPS_LAST_ACK:
		case TCPS_TIME_WAIT:
			tcp_set_state(TCPS_CLOSED); 
			goto drop;
		}
	}

    /* 778 */
    /* drop SYN or !ACK during connection */	
    if (tiflags & TH_SYN) {
		tcp_drop(ECONNRESET);
		goto dropwithreset;
	}

	assert(tiflags & TH_ACK);

    /* 791 ack processing */
    switch (tp->t_state) { 
	case TCPS_SYN_RECEIVED: 
		if (SEQ_GT(tp->snd_una, ti.ti_ack) || SEQ_GT(ti.ti_ack, tp->snd_max)) {
			goto dropwithreset;
		}
		printf("\t\t\t\tServer side 3way handshake is done.\n");
		listening_sock->pending_connection_buf.push(this);

		// push this socket into pending_connection_buf and let Xaccept handle that

		// If the app is ready for a new connection, alert it
		if (!listening_sock->pendingAccepts.empty()) {
			xia::XSocketMsg *acceptXSM = listening_sock->pendingAccepts.front();
			get_transport() -> ReturnResult(listening_sock->port, acceptXSM);
			listening_sock->pendingAccepts.pop();
			delete acceptXSM;
		}
		if (listening_sock->polling) {
			// tell API we are writeable
			get_transport() -> ProcessPollEvent(listening_sock->port, POLLIN|POLLOUT);
		}
		// finish the connection handshake
		get_transport() -> XIDpairToConnectPending.erase(key);
	    get_transport() -> ChangeState(this, CONNECTED);

	    tcp_set_state(TCPS_ESTABLISHED);
	    if ((tp->t_flags & (TF_RCVD_SCALE | TF_REQ_SCALE)) == 
		    (TF_RCVD_SCALE | TF_REQ_SCALE)) { 
			tp->snd_scale = tp->requested_s_scale; 
			tp->rcv_scale = tp->request_r_scale; 
	    }
	    tp->snd_wl1 = ti.ti_seq -1 ; 

	/*
	 * In ESTABLISHED state: drop duplicate ACKs; ACK out of range
	 * ACKs.  If the ack is in the range
	 *	tp->snd_una < ti->ti_ack <= tp->snd_max
	 * then advance tp->snd_una to ti->ti_ack and drop
	 * data from the retransmission queue.  If this ACK reflects
	 * more up to date window information we update our window information.
	 */
    /* 815 */
	case TCPS_ESTABLISHED:
	case TCPS_FIN_WAIT_1:
	case TCPS_FIN_WAIT_2:
	case TCPS_CLOSE_WAIT:
	case TCPS_CLOSING:
	case TCPS_LAST_ACK:
	case TCPS_TIME_WAIT:

	    if (SEQ_LEQ(ti.ti_ack, tp->snd_una)) { 
			if (ti.ti_len == 0 && tiwin == tp->snd_wnd) {
				get_transport()->_tcpstat.tcps_rcvdupack++; 
				/*
				 * If we have outstanding data (other than
				 * a window probe), this is a completely
				 * duplicate ack (ie, window info didn't
				 * change), the ack is the biggest we've
				 * seen and we've seen exactly our rexmt
				 * threshhold of them, assume a packet
				 * has been dropped and retransmit it.
				 * Kludge snd_nxt & the congestion
				 * window so we send only this one
				 * packet.
				 *
				 * We know we're losing at the current
				 * window size so do congestion avoidance
				 * (set ssthresh to half the current window
				 * and pull our congestion window back to
				 * the new ssthresh).
				 *
				 * Dup acks mean that packets have left the
				 * network (they're now cached at the receiver) 
				 * so bump cwnd by the amount in the receiver
				 * to keep a constant cwnd packets in the
				 * network.
				 */
				if ( tp->t_timer[TCPT_REXMT] == 0 ||
					ti.ti_ack != tp->snd_una) 
					tp->t_dupacks = 0 ;
				else if (++tp->t_dupacks == TCP_REXMT_THRESH ) {
					tcp_seq_t onxt = tp->snd_nxt;
					u_int win = min(tp->snd_wnd, tp->snd_cwnd) / 2 / tp->t_maxseg; 
					if (win < 2) 
						win = 2;
					tp->snd_ssthresh = win * tp->t_maxseg;
					tp->t_timer[TCPT_REXMT] = 0;
					tp->t_rtt = 0;
					tp->snd_nxt = ti.ti_ack;
					tp->snd_cwnd = tp->t_maxseg;
					//debug_output(VERB_TCP, "[%s] now: [%u] cwnd: %u, 3 dups, slowstart", SPKRNAME, get_transport()->tcp_now(), tp->snd_cwnd);
					tcp_output();
					tp->snd_cwnd = tp->snd_ssthresh + tp->t_maxseg *
						tp->t_dupacks;
					//debug_output(VERB_TCP, "[%s] now: [%u] cwnd: %u, 3 dups, slowstart", SPKRNAME, get_transport()->tcp_now(), tp->snd_cwnd );
					if (SEQ_GT(onxt, tp->snd_nxt)) 
						tp->snd_nxt = onxt;
					goto drop;
				} else if (tp->t_dupacks > TCP_REXMT_THRESH) {
					tp->snd_cwnd += tp->t_maxseg;
					//debug_output(VERB_TCP, "[%s] now: [%u] cwnd: %u, dups", SPKRNAME, get_transport()->tcp_now(), tp->snd_cwnd );
					tcp_output();
					goto drop;
				}
			} else {
				tp->t_dupacks = 0 ;
			}
			break;
	    }
	    /* 888 */ 
	    if (tp->t_dupacks > TCP_REXMT_THRESH && 
		    tp->snd_cwnd > tp->snd_ssthresh) {  
			tp->snd_cwnd = tp->snd_ssthresh;
			//debug_output(VERB_TCP, "%u: cwnd: %u, reduced to ssthresh", get_transport()->tcp_now(), tp->snd_cwnd );  
	    }
	    tp->t_dupacks = 0; 

	    if (SEQ_GT(ti.ti_ack, tp->snd_max)) { 
			get_transport()->_tcpstat.tcps_rcvacktoomuch++; 
			goto dropafterack; 
	    }
	    acked = ti.ti_ack - tp->snd_una; 
	    get_transport()->_tcpstat.tcps_rcvackpack++; 
	    get_transport()->_tcpstat.tcps_rcvackbyte += acked; 

	    /* 903 */

	   //debug_output(VERB_TCP, "[%s] now: [%u]  RTT measurement: ts_present: %u, now: %u, ecr: %u", SPKRNAME, get_transport()->tcp_now(), ts_present, get_transport()->tcp_now(), ts_ecr); 

	    if (ts_present) 
			tcp_xmit_timer(get_transport()->tcp_now() - ts_ecr + 1); 
	    else if (tp->t_rtt && SEQ_GT(ti.ti_ack, tp->t_rtseq))
			tcp_xmit_timer( tp->t_rtt );

	    /*
	     * If all outstanding data is acked, stop retransmit
	     * timer and remember to restart (more output or persist).
	     * If there is more data to be acked, restart retransmit
	     * timer, using current (possibly backed-off) value.
	     */
	    if (ti.ti_ack == tp->snd_max) {
			tp->t_timer[TCPT_REXMT] = 0;
			needoutput = 1;
	    } else if (tp->t_timer[TCPT_PERSIST] == 0)
			tp->t_timer[TCPT_REXMT] = tp->t_rxtcur;

	    /* 927 */
	    { 
		u_int cw = tp->snd_cwnd;
		u_int incr = tp->t_maxseg;
		if (cw > tp->snd_ssthresh ) 
		    incr = incr * incr / cw + incr / 8; 
		tp->snd_cwnd = min(cw + incr, (u_int) TCP_MAXWIN << tp->snd_scale); 
		//debug_output(VERB_TCP, "[%s] now: [%u] cwnd: %u, increase: %u", SPKRNAME, get_transport()->tcp_now(), tp->snd_cwnd, incr); 		
	    }

	    /* 943 */
		//NOTICE: unsigned/signed comparison acked is an int, byte_length() returns an unsigned 32 int
		//this is taken verbatim from TCP Illustrated vol2
	    if (acked > _q_usr_input.byte_length()) { 
			tp->snd_wnd -= _q_usr_input.byte_length(); 
			_q_usr_input.drop_until(_q_usr_input.byte_length()); 
			ourfinisacked = 1; 
	    } else { 
			_q_usr_input.drop_until(acked); 
			tp->snd_wnd -= acked; 
			ourfinisacked = 0; 
	    }
	    tp->snd_una = ti.ti_ack; 
	    if (SEQ_LT(tp->snd_nxt, tp->snd_una))
			tp->snd_nxt = tp->snd_una; 

	    /* 957 */ 
	    switch (tp->t_state) { 
		case TCPS_FIN_WAIT_1:
		    if (ourfinisacked) { 
				tp->t_timer[TCPT_2MSL] = get_transport()->globals()->tcp_maxidle;
				tcp_set_state(TCPS_FIN_WAIT_2); 
		    }
		    break; 
		    /* 985 */
		case TCPS_CLOSING: 
			if (ourfinisacked) {
				tcp_set_state(TCPS_TIME_WAIT);  
				tp->t_timer[TCPT_2MSL] = 2 * TCPTV_MSL;
				if (_q_recv.push(copy, ti.ti_seq, ti.ti_seq + ti.ti_len < 0)) {
					//debug_output(VERB_ERRORS, "TCPClosing segment push into reassembly Queue FAILED");
				}
				
				if (! _q_recv.is_empty() && has_pullable_data()) {
					tp->rcv_nxt = _q_recv.last_nxt();
					if (polling) {
						// tell API we are readable
						get_transport()->ProcessPollEvent(port, POLLIN);
					}
					check_for_and_handle_pending_recv();
					//debug_output(VERB_TCPSTATS, "input (closing) updating rcv_nxt to [%u]", tp->rcv_nxt);
				}
				
			}
		    break;
		    /* 993 */
		case TCPS_LAST_ACK:
		    if (ourfinisacked) { 
				tcp_set_state(TCPS_CLOSED);
				goto drop; 
		    } 
		    break; 
		case TCPS_TIME_WAIT: 
		    tp->t_timer[TCPT_2MSL] = 2 * TCPTV_MSL;
		    goto dropafterack; 
	    }
    }

    /* 1015 Update the Send Window */
step6: 
	/* Check for ACK flag AND any one of the following 3 conditions */
	if ((tiflags & TH_ACK) && 
		(SEQ_LT(tp->snd_wl1, ti.ti_seq) || tp->snd_wl1 == ti.ti_seq && 
	     (SEQ_LT(tp->snd_wl2, ti.ti_ack) || 
		  tp->snd_wl2 == ti.ti_ack && tiwin > tp->snd_wnd))) { 

		/* Keep track of pure window updates */
		if (ti.ti_len == 0 && tp->snd_wl2 == ti.ti_ack && tiwin > tp->snd_wnd)
			get_transport()->_tcpstat.tcps_rcvwinupd++;

		tp->snd_wnd = tiwin; 
		tp->snd_wl1 = ti.ti_seq; 
		tp->snd_wl2 = ti.ti_ack; 
		if (tp->snd_wnd > tp->max_sndwnd) 
			tp->max_sndwnd = tp->snd_wnd; 
		//tp->t_flags |= TF_ACKNOW; //REMOVE THIS it should not be here - added by dan as a test to make tcp_output send ack when we get out-of-order segment
		needoutput = 1; 
    }

    /*1038 TODO: Urgent data processing */
	// if ((tiflags & TH_URG) && ti.ti_urp &&
	// 	TCPS_HAVERCVDFIN(tp->t_state) == 0) {
	// 		if (false) {
	// 			//if (ti.ti_urp + so_recv_buffer_size > so_recv_buffer_size)
	// 			ti.ti_urp = 0;
	// 			tiflags &= ~TH_URG;
	// 			goto dodata;
	// 	}

	// 	if (SEQ_GT(ti.ti_seq + ti.ti_urp, tp->rcv_up)) {
	// 		// do some stuf pg 984
	// 	}
	// 	// more stuff goes here from pg 984
	// }

    /*1094*/
dodata: 
    if ((ti.ti_len || (tiflags & TH_FIN)) &&  
	    TCPS_HAVERCVDFIN(tp->t_state) == 0) { 
    	/* If the receive buffer is empty and this is an out-order packet,
    	   drop it until we receive the first packet */ 
    	if (ti.ti_seq > tp->rcv_nxt && tp->t_state == TCPS_ESTABLISHED &&
    		_q_recv.is_empty()) 
    		goto drop;
    	printf("becareful 843\n");
		/* begin TCP_REASS */ 
		if (ti.ti_seq == tp->rcv_nxt && 
			tp->t_state == TCPS_ESTABLISHED) {
				tp->t_flags |= TF_DELACK; 
				tp->rcv_nxt += ti.ti_len; 
				tiflags = ti.ti_flags & TH_FIN; 
		} 

		//Dan's experimental ACK_NOW: if ti.ti_seq > tp->rcv_nxt, acknow
		if (ti.ti_seq > tp->rcv_nxt && tp->t_state == TCPS_ESTABLISHED) {
			tp->t_flags |= TF_ACKNOW;
		}

		/* _q_recv.push() corresponds to the tcp_reass function whose purpose is
		 * to put all data into the CTCPQueue for both possible reassembly and
		 * in-order presentation to the "application socket" which in the
		 * TCPget_transport is the stateless pull port.
		 */
		if (_q_recv.push(copy, ti.ti_seq, ti.ti_seq + ti.ti_len) < 0) {
			//debug_output(VERB_ERRORS, "Slow Path segment push into reassembly Queue FAILED");
		}	
		
		if (! _q_recv.is_empty() && has_pullable_data()) {
			tp->rcv_nxt = _q_recv.last_nxt();
			if (polling) {
				// tell API we are readable
				get_transport()->ProcessPollEvent(port, POLLIN);
			}
			check_for_and_handle_pending_recv();
			//debug_output(VERB_TCPSTATS, "input (sp) updating rcv_nxt to [%u]", tp->rcv_nxt);
		}

		/* end TCP_REASS */ 


		// TODO len calculation @Harald: What exactly needs to be done?
		len = ti.ti_len; 
    } else {
    	if (copy != NULL)
    	{
    		copy -> kill();
    	}
		// p->kill();		// don't know why comment this in order to prevent deadlock
		tiflags &= ~TH_FIN;
    }

    /*1116*/
    /* FIN processing */
	if ( tiflags & TH_FIN ) {
		// tell API peer requested close
		if (isBlocking) {
			if (recv_pending) {
				// The api is blocking on a recv, return 0 bytes available
				get_transport() -> ReturnResult(port, pending_recv_msg, 0, 0);

				recv_pending = false;
				delete pending_recv_msg;
				pending_recv_msg = NULL;
			}
		}
		if (polling) {
			get_transport() -> ProcessPollEvent(port, POLLIN|POLLHUP);
		}

		if (TCPS_HAVERCVDFIN(tp->t_state) == 0) { 
			tp->t_flags |= TF_ACKNOW; 
			tp->rcv_nxt++;
		}
		switch (tp->t_state) {
		case TCPS_SYN_RECEIVED:
		case TCPS_ESTABLISHED:
			if ( tp->so_flags & SO_FIN_AFTER_TCP_FIN ) { 
				tcp_set_state(TCPS_LAST_ACK); 
			} else { 
				tcp_set_state(TCPS_CLOSE_WAIT); 
			}
			break;
		case TCPS_FIN_WAIT_1:
			tcp_set_state(TCPS_CLOSING); 
			break;
		case TCPS_FIN_WAIT_2:
			tcp_set_state(TCPS_TIME_WAIT); 
			tcp_canceltimers(); 
			tp->t_timer[TCPT_2MSL] = 2 * TCPTV_MSL;
			// the socket is really disconnected
			break;
		case TCPS_TIME_WAIT:
			//debug_output(VERB_TCP, "%u: TIME_WAIT", get_transport()->tcp_now());
			tp->t_timer[TCPT_2MSL] = 2 * TCPTV_MSL;
			break;
		}
	}

    /*1163*/
    if (needoutput || (tp->t_flags & TF_ACKNOW)) {
		//debug_output(VERB_TCPSTATS, "[%s] we need output! true?: [%x] needoutput: [%x]", SPKRNAME, (tp->t_flags & TF_ACKNOW), needoutput);
		tcp_output();
    }

    return;

dropafterack:
	/* Drop incoming segment and send an ACK. */
	if (tiflags & TH_RST)
		goto drop;
	printf("becareful 913\n");
	if (copy != NULL) copy -> kill();
    if (p != NULL) p->kill();
	tp->t_flags |= TF_ACKNOW;
	tcp_output(); 
	return;


dropwithreset:
	/*
	 * Generate a RST and drop incoming segment.
	 * Make ACK acceptable to originator of segment.
	 * Don't bother to respond if destination was broadcast/multicast.
	 */
	if (tiflags & TH_ACK)
		tcp_respond((tcp_seq_t)0, ti.ti_ack, TH_RST);
	else {
		if (tiflags & TH_SYN)
			ti.ti_len++;
		tcp_respond(ti.ti_seq+ti.ti_len, (tcp_seq_t)0, TH_RST|TH_ACK);
	}
	return;

drop:
    //debug_output(VERB_TCP, "[%s] tcpcon::input drop", SPKRNAME); 
	if (copy != NULL) copy -> kill();
    if (p != NULL) p->kill();
	printf("drop: 938\n");
    return ;
}


/* Send data from the TCP FIFO in the stateful way out to the tcp-speaking
 * client.
 */
void 
XChunk::tcp_output() 
{

    int 		idle, sendalot, off, flags;
    unsigned 	optlen, hdrlen;
    u_char		opt[MAX_TCPOPTLEN];
    long		len, win;
    click_tcp 	ti;
    WritablePacket *p = NULL;
    WritablePacket *tcp_payload = NULL;


	for (int i=0; i < MAX_TCPOPTLEN; i++) {
		opt[i] = 0;
	}

    /*61*/
    idle = (tp->snd_max == tp->snd_una);
    if (idle && tp->t_idle >= tp->t_rxtcur) { 
       tp->snd_cwnd = tp->t_maxseg;
       //debug_output(VERB_TCP, "[%s] now: [%u] cnwd: %u, been idle", SPKRNAME, get_transport()->tcp_now()); 
    }

	// TCP FIFO (Addresses (and seq num) decrease in this dir ->)
	//		                       snd_nxt  (off)  snd_una
	//		   ----------------------------------------
	// push -> | empty  | unsent data | sent, unacked | -> pop
	//		   ----------------------------------------
	//		                          {      off      }
	// 		   			{  _q_usr_input.byte_length() }	
	
again:
    sendalot = 0;
    /*71*/
	/* off is the offset in bytes from the beginning of the send buf of the
	 * first data byte to send - a.k.a. bytes already sent, but unacked*/
    off = tp->snd_nxt - tp->snd_una; 
    win = min(tp->snd_wnd, tp->snd_cwnd); 
    flags = tcp_outflags[tp->t_state]; 
    // printf("flags: %d\n", flags);
    // printf("t_state %d\n", tp->t_state);
    /*80*/
    if (tp->t_force) { 
		if (win == 0) { 
			if (! _q_usr_input.is_empty()) 
				flags &= ~TH_FIN; 
			win = 1; 
		} else { 
			tp->t_timer[TCPT_PERSIST] = 0; 
			tp->t_rxtshift = 0; 
		}
    }
	/* we subtract off, because off bytes have been sent and are awaiting
	 * acknowledgement */
    len = min(_q_usr_input.byte_length(),  win) - off; 

    /*106*/
    if (len < 0) { 
		len = 0; 
		if (win == 0) { 
			tp->t_timer[TCPT_REXMT]=0; 
			tp->snd_nxt = tp->snd_una; 
		}
    } 

    if (_q_usr_input.pkts_to_send(off,win) > 1) { sendalot = 1; }

    if (len > tp->t_maxseg) { len = tp->t_maxseg; }

    win = so_recv_buffer_space(); 

    /*131 Silly window avoidance */ 
	if (len) {
		if (len == tp->t_maxseg)
			goto send;
		if ((idle || tp->t_flags & TF_NODELAY) &&
		    len + off >= _q_usr_input.byte_length())
			goto send;
		if (tp->t_force)
			goto send;
		if (len >= tp->max_sndwnd / 2)
			goto send;
		if (SEQ_LT(tp->snd_nxt, tp->snd_max))
			goto send;
	}
	
    /*154*/
    if (win > 0) { 
		long adv = min(win, (long)TCP_MAXWIN << tp->rcv_scale) -
		(tp->rcv_adv - tp->rcv_nxt);
		//debug_output(VERB_TCPSTATS, "[%s] adv: [%d] = min([%u],[%u]):  - (radv: [%u] rnxt: [%u]) [%u]", SPKRNAME, adv, win, (long)TCP_MAXWIN << tp->rcv_scale, tp->rcv_adv, tp->rcv_nxt, tp->rcv_adv-tp->rcv_nxt);
		/* Slight Hack Below - we are using (t_maxseg + 1) here to ensure that
		 * once we have recvd at least 1 byte more than a full MSS we goto send
		 * to dispatch an ACK to the sender. This is necessary because the
		 * incoming tcp payload bytes are less than maxseg due to header options. */
		if (adv >= (long) (tp->t_maxseg + 1)) 
			goto send;
		if (2 * adv >= so_recv_buffer_size )
			goto send;
    } else {
    	printf("\t\t\t\twin <= 0!!!\n");
		//debug_output(VERB_TCPSTATS, "[%s] win: [%u]  (radv: [%u] rnxt: [%u]) [%u]", SPKRNAME, win, tp->rcv_adv, tp->rcv_nxt, tp->rcv_adv-tp->rcv_nxt);
	}

    /*174*/
    if (tp->t_flags & TF_ACKNOW)
		goto send;
    if (flags & ( TH_SYN | TH_RST ))
		goto send;
    if (SEQ_GT(tp->snd_up, tp->snd_una))
		goto send;

	if (flags & TH_FIN && 
		((tp->t_flags & TF_SENTFIN) == 0 || tp->snd_nxt == tp->snd_una)) {
		goto send;
	}

    /*213*/
	if ((! _q_usr_input.is_empty()) && tp->t_timer[TCPT_REXMT] == 0 &&
		tp->t_timer[TCPT_PERSIST] == 0 ) { 
		tp->t_rxtshift = 0; 
		tcp_setpersist(); 
    }
    return; 

    /*222*/
send:
	// cout << "So send"<<endl;
    optlen = 0;
    hdrlen = sizeof(click_tcp);

	// a SYN or SYN/ACK flagged segment is to be created 
	if (flags & TH_SYN) {
		tp->snd_nxt = tp->iss; 
		if ((tp->t_flags & TF_NOOPT) == 0) {
			u_short mss;
			opt[0] = TCPOPT_MAXSEG;
			opt[1] = 4; 
			mss = htons((u_short)tcp_mss(0));  
			memcpy( &(opt[2]), &mss, sizeof(mss)) ; 
			optlen = 4; 

			// Here we set the Window Scale option if it was requested of us to
			// do so
			//debug_output(VERB_DEBUG, "[%s] t_flags: [%x]", SPKRNAME, tp->t_flags);
			if ((tp->t_flags & TF_REQ_SCALE) && 
				((flags & TH_ACK) == 0 ||
				 (tp->t_flags & TF_RCVD_SCALE))) { 
			*((u_long *) (opt + optlen) ) = htonl( //FIXME 4-byte ALIGNMENT problem occurs here
				TCPOPT_NOP << 24 | 
				TCPOPT_WSCALE << 16 | 
				TCPOLEN_WSCALE << 8 |
				tp->request_r_scale); 
			optlen += 4;
			}
		}
	}

    /* 253 timestamp generation */
//	//debug_output(VERB_DEBUG, "[%s] timestamp: [%X] [%x] [%x] [%x]", SPKRNAME, (tp->t_flags),((flags & TH_RST) == 0), ((flags & (TH_SYN | TH_ACK)) == TH_SYN),(tp->t_flags & TF_RCVD_TSTMP));

	//HACK FIX TO MAKE TIMESTAMPS GET Printed as the first stmt evalutates false when window scaling is > 0
    if (((tp->t_flags & (TF_REQ_TSTMP | TF_NOOPT)) == TF_REQ_TSTMP || 1 ) && 
	(flags & TH_RST) == 0 && 
	((flags & (TH_SYN | TH_ACK)) == TH_SYN || 
	(tp->t_flags & TF_RCVD_TSTMP))) { 
		//debug_output(VERB_DEBUG, "[%s] timestamp: SETTING TIMESTAMP", SPKRNAME);
		u_long *lp = (u_long*) (opt + optlen); 
		*lp++ = htonl(TCPOPT_TSTAMP_HDR); 
		*lp++ = htonl(get_transport()->tcp_now()); 
		*lp = htonl(tp->ts_recent); 
		optlen += TCPOLEN_TSTAMP_APPA; 
    } else { 
		// Remove this clause after it's been debugged and timestamps are working properly
		//debug_output(VERB_DEBUG, "[%s] timestamp: NOT setting timestamp", SPKRNAME);
	}
		// printf("1076\n");
		
    hdrlen += optlen; 

    if (len > tp->t_maxseg - optlen) { 
		len = tp->t_maxseg - optlen; 
		sendalot = 1; 
    } 

    /*278*/
    if (len) {
		p = _q_usr_input.get(off); 
		if (!p) { 
			//debug_output(VERB_ERRORS, "[%s] offset [%u] not in fifo!", SPKRNAME, off); 
			return; 
		}
		if (p->length() > len) {
			p->take(p->length() - len);
		}
		if (p->length() < len) { 
			len = p->length(); 
			sendalot = 1; 
		}
		// p = p->push( sizeof(click_ip) + sizeof(click_tcp) + optlen); 

	/*317*/
    } else { 
    	// empty packet
		// p = Packet::make(sizeof(click_ip) + sizeof(click_tcp) + optlen);
		/* TODO: errorhandling */
    }
		// printf("1107\n");

    /*339*/
    if (flags & TH_FIN && tp->t_flags & TF_SENTFIN && 
	    tp->snd_nxt == tp->snd_max) 
	tp->snd_nxt -- ; 
// printf("1113\n");
	// @Harald: Is there a reason that the persist timer was not being checked?
	if (len || (flags & (TH_SYN | TH_FIN)) || tp->t_timer[TCPT_PERSIST]) 
		ti.th_seq = htonl(tp->snd_nxt); 
    else 
		ti.th_seq = htonl(tp->snd_max);
// printf("1119\n");
    ti.th_ack = htonl(tp->rcv_nxt);
printf("1121+++++++%d\n",optlen);
	    if (optlen) {
	    	// printf("1123\n");
			// memcpy((&ti + 1), opt, optlen); 
			// printf("1125\n");
			ti.th_off = (sizeof(click_tcp) + optlen) >> 2;
	    } 
    
    ti.th_flags = flags; 

    /*370*/
    /* receiver window calculations */ 

    /*TODO: silly window */
		// printf("1132\n");

	// Correct window if it is too large or too small
    if (win > (long) TCP_MAXWIN << tp->rcv_scale)
		win = (long) TCP_MAXWIN << tp->rcv_scale; 
    if (win < (long) (tp->rcv_adv - tp->rcv_nxt))
		win = (long) (tp->rcv_adv - tp->rcv_nxt); 

	// Set the tcp header window size we will advertisement
    ti.th_win = htons((u_short) (win >> tp->rcv_scale)) ;

    if (SEQ_GT(tp->snd_up, tp->snd_nxt)) { 
		ti.th_urp = htons((u_short) (tp->snd_up - tp->snd_nxt) ); 
		ti.th_flags |= TH_URG; 
    } else { 
		tp->snd_up = tp->snd_una; 
    }
    /* TODO: do we need to set p->length here ??? */
		// printf("1150\n");

    /*400*/
	if (tp->t_force == 0  || tp->t_timer[TCPT_PERSIST] == 0) {
		tcp_seq_t startseq = tp->snd_nxt; 

		if (flags & (TH_SYN | TH_FIN)) {
			if (flags & TH_SYN) 
				tp->snd_nxt++; 
			if (flags & TH_FIN) {
				tp->snd_nxt++; 
				tp->t_flags |= TF_SENTFIN;
			}
		}

		tp->snd_nxt += len;
		if (SEQ_GT(tp->snd_nxt, tp->snd_max)) {
			tp->snd_max = tp->snd_nxt ; 
			if (tp->t_rtt == 0) {
				tp->t_rtt = 1;
				tp->t_rtseq = startseq;
			}
		}

		if (tp->t_timer[TCPT_REXMT] == 0 && tp->snd_nxt != tp->snd_una) {
			tp->t_timer[TCPT_REXMT] = tp->t_rxtcur;
			//debug_output(VERB_TCP, "[%s] now: [%u] REXMT set to %u == %f", SPKRNAME, get_transport()->tcp_now(), tp->t_timer[TCPT_REXMT], tp->t_timer[TCPT_REXMT]*0.5 );
			if (tp->t_timer[TCPT_PERSIST]) {
				tp->t_timer[TCPT_PERSIST] = 0;
				tp->t_rxtshift = 0;
			}
		}

	} else if (SEQ_GT(tp->snd_nxt + len, tp->snd_max)) {
		tp->snd_max = tp->snd_nxt + len; 
	}
		// assert(0);
	// printf("1189+%p\n",p);
	// THE MAGIC MOMENT! Our beloved tcp data segment goes to be wrapped in IP and
	// sent to its tcp-speaking destination :-)
	//Add XIA headers
	XIAHeaderEncap xiah;
	xiah.set_nxt(CLICK_XIA_NXT_TRN);
	xiah.set_last(LAST_NODE_DEFAULT);
	xiah.set_hlim(hlim);
	xiah.set_dst_path(dst_path);
	xiah.set_src_path(src_path);

	ti.th_sport = 0xABCD;
	ti.th_dport = 0XEF01;
	TransportHeaderEncap *send_hdr = TransportHeaderEncap::MakeTCPHeader(&ti, opt, optlen);
	int payload_length = 0;
	if (p==NULL)
	{
		printf("a control packet is sent\n");
		p = WritablePacket::make(0, '\0', 0, 0);
	} else {
		payload_length = p -> length();
	}
	printf("\t\t\ttcpoutput with payload_length %d\n", payload_length);
	tcp_payload = send_hdr->encap(p);
	send_hdr -> update();
	xiah.set_plen(payload_length + send_hdr->hlen()); // XIA payload = transport header + transport-layer data
	tcp_payload = xiah.encap(tcp_payload, false);
	delete send_hdr;
	get_transport()->output(NETWORK_PORT).push(tcp_payload);
	// printf("1207\n");
	printf("\t\t\ttcpoutput flag is %d\n", ti.th_flags);
	printf("\t\t\ttcpoutput seq is %d\n", ntohl(ti.th_seq));
	printf("\t\t\ttcpoutput ack is %d\n", ntohl(ti.th_ack));
	printf("\t\t\ttcpoutput off is %d\n", ti.th_off);
	printf("\t\t\ttcpoutput optlen is %d\n", optlen);
	/* Data has been sent out at this point. If we advertised a positive window
	 * and if this new window advertisement will result in us recieving a higher
	 * sequence numbered segment than before this window announcement, we record
	 * the new highest sequence number which the sener is allowed to send to us.
	 * (tp->rcv_adv). Any pending ACK has now been sent. */
    if (win > 0 && SEQ_GT(tp->rcv_nxt + win, tp->rcv_adv)) {
		tp->rcv_adv = tp->rcv_nxt + win; 
	}
    tp->last_ack_sent = tp->rcv_nxt;
    tp->t_flags &= ~(TF_ACKNOW | TF_DELACK); 

    if (sendalot) {
		goto again; 
	}
	// printf("1223\n");
    return; 
}

void
XChunk::tcp_respond(tcp_seq_t ack, tcp_seq_t seq, int flags)
{
	printf("tcp_respond is called, there is bug sending a packet\n");
	click_tcp th;

	int win = min(so_recv_buffer_space(),  TCP_MAXWIN << tp->rcv_scale); 

	if (! (flags & TH_RST)) {
	    flags = TH_ACK; 
	    th.th_win = htons((u_short)(win >> tp->rcv_scale)); 
		
	} else { 
	    th.th_win = htons((u_short)win); 
	}
	
/*	setports(th->th_sport, _con_id._ports); */
	th.th_flags2 = 0; 
	th.th_seq =   htonl(seq+1); 
	th.th_ack =   htonl(ack); 
	th.th_flags = flags; 
	th.th_urp = 0; 
	th.th_sum = 0; 
	th.th_off = (sizeof(click_tcp)) >> 2;

	//Add XIA headers
	XIAHeaderEncap xiah;
	xiah.set_nxt(CLICK_XIA_NXT_TRN);
	xiah.set_last(LAST_NODE_DEFAULT);
	xiah.set_hlim(hlim);
	xiah.set_dst_path(dst_path);
	xiah.set_src_path(src_path);

	TransportHeaderEncap *send_hdr = TransportHeaderEncap::MakeTCPHeader(&th);
	printf("a control packet is sent\n");
	WritablePacket *p =  WritablePacket::make(0, '\0', 0, 0);
	WritablePacket *tcp_payload = send_hdr->encap(p);
	send_hdr -> update();
	xiah.set_plen(send_hdr->hlen()); // XIA payload = transport header + transport-layer data
	tcp_payload = xiah.encap(tcp_payload, false);
	delete send_hdr;
	get_transport()->output(NETWORK_PORT).push(tcp_payload);

}

tcp_seq_t
XChunk::so_recv_buffer_space() { 
	return so_recv_buffer_size - _q_recv.bytes_ok(); 
} 


void 
XChunk::fasttimo() {
    	// printf("_fast_ticks");

	if ( tp->t_flags & TF_DELACK) { 
		tp->t_flags &= ~TF_DELACK; 
		tp->t_flags |= TF_ACKNOW; 
		tcp_output(); 
	}
}

void 
XChunk::slowtimo() { 
    	// printf("_slow_ticks");

	int i; 
	//debug_output(VERB_TIMERS, "[%s] now: [%u] Timers: %s %d %s %d %s %d %s %d %s %d", 
	// SPKRNAME,
	// get_transport()->tcp_now(), 
	// tcptimers[0], tp->t_timer[0], 
	// tcptimers[1], tp->t_timer[1], 
	// tcptimers[2], tp->t_timer[2], 
	// tcptimers[3], tp->t_timer[3], 
	// tcptimers[4], tp->t_timer[4] );  

	for ( i = 0; i < TCPT_NTIMERS; i++ ) { 
	  // //debug_output(VERB_TCP, "%u: XChunk::slowtimo: %s %d\n", get_transport()->tcp_now(), tcptimers[i], tp->t_timer[i]); 
		if ( tp->t_timer[i] && --(tp->t_timer[i]) == 0) { 
		  // StringAccum sa;
		  // sa << *(flowid()); 

		  //   //debug_output(VERB_TIMERS, "[%s] now: [%u] TIMEOUT %s: %s, now: %u", SPKRNAME, get_transport()->tcp_now(), sa.c_str(), tcptimers[i], get_transport()->tcp_now()); 
		    tcp_timers(i); 
		} 
	}
	tp->t_idle++; 
	if (tp->t_rtt) 
	    tp->t_rtt++;
}

int	tcp_backoff[TCP_MAXRXTSHIFT + 1] =
    { 1, 2, 4, 8, 16, 32, 64, 64, 64, 64, 64, 64, 64 };

void
XChunk::tcp_timers (int timer) { 
	int rexmt;

	switch (timer) {
		/*127*/
		case TCPT_2MSL:
		  if (tp->t_state != TCPS_TIME_WAIT && 
		      tp->t_idle <= get_transport()->globals()->tcp_maxidle) 
		    tp->t_timer[TCPT_2MSL] = get_transport()->globals()->tcp_keepintvl; 
		  else
		    tcp_set_state(TCPS_CLOSED); 
			// the socket is really closed
		  break; 
		case TCPT_PERSIST:
		  tcp_setpersist(); 
		  tp->t_force = 1; 
		  tcp_output(); 
		  tp->t_force = 0; 
		  break; 
		case TCPT_KEEP: 
		  if ( tp->t_state < TCPS_ESTABLISHED) {
			// Notify API that the connection failed
			XSocketMsg xsm;
			xsm.set_type(xia::XCONNECT);
			xsm.set_sequence(0); // TODO: what should This be?
			xia::X_Connect_Msg *connect_msg = xsm.mutable_x_connect();
			connect_msg->set_status(xia::X_Connect_Msg::XFAILED);
			connect_msg->set_ddag(dst_path.unparse().c_str());
			get_transport()->ReturnResult(port, &xsm);

			if (polling) {
				printf("checking poll event for %d from timer\n", port);
				get_transport()->ProcessPollEvent(port, POLLHUP);
			}

		    goto dropit; 
		  }
		  if ( tp->so_flags & SO_KEEPALIVE && 
		       tp->t_state <= TCPS_CLOSE_WAIT) { 
		  	  	printf("1449\n");
		    if (tp->t_idle >= get_transport()->globals()->tcp_keepidle + 
			get_transport()->globals()->tcp_maxidle) 
			goto dropit;
			get_transport()->_tcpstat.tcps_keepprobe++; 
			tcp_respond(tp->rcv_nxt, tp->snd_una, 0); 
			tp->t_timer[TCPT_KEEP] = get_transport()->globals()->tcp_keepintvl; 
		  } else
		    tp->t_timer[TCPT_KEEP] = get_transport()->globals()->tcp_keepidle; 
		  break; 
dropit:
		  get_transport()->_tcpstat.tcps_keepdrops++; 
		  tcp_drop(ETIMEDOUT); 
		  //todo this should go to close and not to drop

		  break; 
		case TCPT_REXMT: 
		  if (++tp->t_rxtshift > TCP_MAXRXTSHIFT) { 
		    tp->t_rxtshift = TCP_MAXRXTSHIFT;
		    tcp_drop(ETIMEDOUT); 
		    break; 
		  }
		  rexmt = TCP_REXMTVAL(tp) * tcp_backoff[tp->t_rxtshift];
		  TCPT_RANGESET(tp->t_rxtcur, rexmt, 
		  		tp->t_rttmin, TCPTV_REXMTMAX); 
		  tp->t_timer[TCPT_REXMT] = tp->t_rxtcur; 

		  if (tp->t_rxtshift > TCP_MAXRXTSHIFT / 4) { 
		    /* in_losing(tp->t_inpcb); 
		    notification of lower layers after 4 failed
		    retransmissions is not implemented 
		    */
		    tp->t_rttvar += (tp->t_srtt >> TCP_RTT_SHIFT) ; 
		    tp->t_srtt = 0; 
		  }
		  tp->snd_nxt = tp->snd_una; 
		  tp->t_rtt = 0; 
		  { 
		    u_int win = min(tp->snd_wnd, tp->snd_cwnd)
		    		/ 2 / tp->t_maxseg; 
		    if ( win < 2 )
			win = 2; 
		    tp->snd_cwnd = tp->t_maxseg; 
		    //debug_output(VERB_TCP, "%u: cwnd: %u, TCPT_REXMT", get_transport()->tcp_now(), tp->snd_cwnd); 
		    tp->snd_ssthresh = win * tp->t_maxseg;
		    tp->t_dupacks = 0 ; 
		    }
		  tcp_output();
		  break; 
		  case TCPT_IDLE:
		  	usrclosed(); 
		  break; 
	}
}

void
XChunk::tcp_canceltimers() { 
	int i; 
	for (i=0; i<TCPT_NTIMERS; i++) 
	    tp->t_timer[i] = 0;
}

void
XChunk::tcp_setpersist() { 
	int t; 

	t = ((tp->t_srtt >> 2) + tp->t_rttvar) >> 1; 

	if (tp->t_timer[TCPT_REXMT]) 
	    _errh->error("tcp_output REXMT"); 
	
	TCPT_RANGESET(tp->t_timer[TCPT_PERSIST], 
		t * tcp_backoff[tp->t_rxtshift], 
		TCPTV_PERSMIN, TCPTV_PERSMAX); 
	if(tp->t_rxtshift < TCP_MAXRXTSHIFT) 
	    	tp->t_rxtshift++; 
	
}
void 
XChunk::tcp_xmit_timer(short rtt) { 
	short delta; 
	get_transport()->_tcpstat.tcps_rttupdated++; 
	
	//debug_output(VERB_TIMERS, "[%s] now: [%u]: tcp_xmit_timer: srtt [%d] cur rtt [%d]\n", SPKRNAME, get_transport()->tcp_now(), tp->t_srtt, rtt); 
	if (tp->t_srtt != 0) {
		/*
		 * srtt is stored as fixed point with 3 bits after the
		 * binary point (i.e., scaled by 8).  The following magic
		 * is equivalent to the smoothing algorithm in rfc793 with
		 * an alpha of .875 (srtt = rtt/8 + srtt*7/8 in fixed
		 * point).  Adjust rtt to origin 0.
		 */
		delta = rtt - 1 - (tp->t_srtt >> TCP_RTT_SHIFT);
		if ((tp->t_srtt += delta) <= 0)
			tp->t_srtt = 1;
		/*
		 * We accumulate a smoothed rtt variance (actually, a
		 * smoothed mean difference), then set the retransmit
		 * timer to smoothed rtt + 4 times the smoothed variance.
		 * rttvar is stored as fixed point with 2 bits after the
		 * binary point (scaled by 4).  The following is
		 * equivalent to rfc793 smoothing with an alpha of .75
		 * (rttvar = rttvar*3/4 + |delta| / 4).  This replaces
		 * rfc793's wired-in beta.
		 */
		if (delta < 0)
			delta = -delta;
		delta -= (tp->t_rttvar >> TCP_RTTVAR_SHIFT);
		if ((tp->t_rttvar += delta) <= 0)
			tp->t_rttvar = 1;
	} else {
		/* 
		 * No rtt measurement yet - use the unsmoothed rtt.
		 * Set the variance to half the rtt (so our first
		 * retransmit happens at 3*rtt).
		 */
		tp->t_srtt = rtt << TCP_RTT_SHIFT;
		tp->t_rttvar = rtt << (TCP_RTTVAR_SHIFT - 1);
	}
	tp->t_rtt = 0;
	tp->t_rxtshift = 0;

	/*
	 * the retransmit should happen at rtt + 4 * rttvar.
	 * Because of the way we do the smoothing, srtt and rttvar
	 * will each average +1/2 tick of bias.  When we compute
	 * the retransmit timer, we want 1/2 tick of rounding and
	 * 1 extra tick because of +-1/2 tick uncertainty in the
	 * firing of the timer.  The bias will give us exactly the
	 * 1.5 tick we need.  But, because the bias is
	 * statistical, we have to test that we don't drop below
	 * the minimum feasible timer (which is 2 ticks).
	 */
	TCPT_RANGESET(tp->t_rxtcur, TCP_REXMTVAL(tp),
	    tp->t_rttmin, TCPTV_REXMTMAX);
	//debug_output(VERB_TCP, "[%s] now: [%u]: rxt_cur: %u, RXMTVAL: %u, rttmin: %u, RXMTMAX: %u \n", SPKRNAME, get_transport()->tcp_now(), tp->t_rxtcur, TCP_REXMTVAL(tp), tp->t_rttmin,TCPTV_REXMTMAX ); 
	
	/*
	 * We received an ack for a packet that wasn't retransmitted;
	 * it is probably safe to discard any error indications we've
	 * received recently.  This isn't quite right, but close enough
	 * for now (a route might have failed after we sent a segment,
	 * and the return path might not be symmetrical).
	 */
	// tp->t_softerror = 0;
}

void
XChunk::tcp_drop(int err)
{
	tp->so_error = err; 
	tcp_set_state(TCPS_CLOSED); 
	if (TCPS_HAVERCVDSYN(tp->t_state)) {
		tcp_output(); 
	}
}

u_int
XChunk::tcp_mss(u_int offer) { 
	unsigned glbmaxseg = get_transport()->_tcp_globals.tcp_mssdflt;
	/* FIXME sensible mss function */ 
	u_int mss ;
	if (offer) { 
		mss = min(glbmaxseg, offer);  
	} else { 
		mss = glbmaxseg;
	}
	tp->t_maxseg = mss;
	tp->snd_cwnd = mss; 
	//debug_output(VERB_TCP, "[%s] now: [%u] cnwd: [%u] rcvd_offer: [%u] tcp_mss: [%u]", SPKRNAME, get_transport()->tcp_now(), tp->snd_cwnd, offer, tp->t_maxseg); 

	return mss; 
}

/* Recieves a stateless mesh packet, passess it to have its headers removed, and
 * then if the packet has a payload, pushes it into the FIFO to be pushed to the
 * stateful reciever, OR if any stateless flags were set, performs the
 * appropriate action.
 */
int 
XChunk::usrsend(WritablePacket *p)
{ 
	printf("usrsend: I wanna send\n");
	// Sanity Check: We should never recieve a packet after our tcp state is
	// beyond CLOSE_WAIT.
    if (tp->t_state > TCPS_CLOSE_WAIT) { 
		p->kill(); 
		return -3; 
    }

    if (tp->so_flags & SO_FIN_AFTER_UDP_IDLE) {
		//debug_output(VERB_TIMERS, "[%s] tcpcon::usrsend setting timer TCPT_IDLE to [%d]", 
			// SPKRNAME, get_transport()->globals()->so_idletime); 
		tp->t_timer[TCPT_IDLE] = get_transport()->globals()->so_idletime;  
	}

	// the stateless tcp flags field from the recieved stateless packet
	int retval = 0 ; 
 
	// If we were closed or listening, we will have to send a SYN 
    if ((tp->t_state == TCPS_CLOSED) || (tp->t_state == TCPS_LISTEN)) {
		tcp_set_state(TCPS_SYN_SENT);
	}

	// if (tp->t_sl_flags == TH_SYN) {
	// 	usropen();
	// }

	// if (tp->t_sl_flags == TH_FIN) {
	// 	usrclosed();
	// }

	if (p) {
		printf("usrsend: Push into _q_usr_input\n");
		WritablePacket *wp = NULL;
		int remaining = (int)p -> length();
		char buf[512];
		memset(buf, 0, 512);
		printf("the remaining is %d\n", remaining);
		// while (remaining > 0) {
		// 	int size = remaining > 512 ? 512 : remaining;
		// 	memcpy((void*)buf, (const void*)p->data(), size);
		// 	wp = WritablePacket::make((const void*)buf, size);
		// 	if (size == 512)
		// 		p -> pull(512);
			retval = _q_usr_input.push(p);
		// 	remaining -= 512; 
		// 	printf("the remaining is %d\n", remaining);
		// } 
	}

	//  These are the states where we expect to recieve packets
	//	if ( (tp->t_state == TCPS_ESTABLISHED) || ( tp->t_state == TCPS_CLOSE_WAIT ))
	tcp_output(); 
    return retval;
}

/* user request 424*/
void 
XChunk::usrclosed() 
{ 
	printf("usrclosed is called \n");
    switch (tp->t_state) { 
		case TCPS_CLOSED:
		case TCPS_LISTEN:
		case TCPS_SYN_SENT:
			tcp_set_state(TCPS_CLOSED);
			break;
		case TCPS_SYN_RECEIVED:
		case TCPS_ESTABLISHED:
			tcp_set_state(TCPS_FIN_WAIT_1);
			break;
		case TCPS_CLOSE_WAIT:
			tcp_set_state(TCPS_LAST_ACK); 
			break;
    }
    tcp_output();
}


void 
XChunk::usropen() 
{ 
	tp = tcp_newtcpcb();
    tp->t_state = TCPS_CLOSED;
	if (tp->iss == 0) {
		tp->iss = 0x11111; 
		//debug_output(VERB_ERRORS, "Setting initial sequence to [%d], because it was 0", tp->iss);
		// Setting a non-zero initial sequence number because I see some weird
		// problems in wireshark when initial seq is 0
	}
	_tcp_sendseqinit(tp);
    //debug_output(VERB_STATES,"[%s] usropen with state <%s>, initial seq num <%d> \n", 
    	// dispatcher()->name().c_str(), tcpstates[tp->t_state], tp->iss); 
    if (tp->t_state == TCPS_CLOSED || tp->t_state == TCPS_LISTEN) {
    	cout << "we are good\n";
		tcp_set_state(TCPS_SYN_SENT);
		tp->t_timer[TCPT_KEEP] = TCPTV_KEEP_INIT;
    }
	cout << "we are good+1\n";
    tcp_output(); 
}


void
XChunk::set_state(const HandlerState new_state) { 
	
	HandlerState old_state = get_state(); 

	sock::set_state(new_state); 
	

	if ((old_state == CREATE) && new_state == (INITIALIZE))
	    usropen(); 

	if ((new_state == SHUTDOWN) && tcp_state() <= TCPS_ESTABLISHED) 
	    usrclosed(); 

} 

void 
XChunk::_tcp_dooptions(u_char *cp, int cnt, uint8_t th_flags, 
	int * ts_present, u_long *ts_val, u_long *ts_ecr) 
{ 
	uint16_t mss;
	int opt, optlen; 
	optlen = 0; 

	//debug_output(VERB_DEBUG, "[%s] tcp_dooption cnt [%u]\n", SPKRNAME, cnt);
	for (; cnt > 0; cnt -= optlen, cp += optlen) { 
		printf("1771\n");
		opt = cp[0]; 
		if (opt == TCPOPT_EOL) {
			//debug_output(VERB_DEBUG, "b1");
			break; 
		}
		if (opt == TCPOPT_NOP) 
			optlen = 1; 
		else {
			if (cnt < 2){
				//debug_output(VERB_DEBUG, "b2");
				break; 
			}
			optlen = cp[1]; 
			if (optlen < 1 || optlen > cnt ) {
				//debug_output(VERB_DEBUG, "b3, optlen: [%x] cnt: [%x]", optlen, cnt);
				break;
			}
		}
		//debug_output(VERB_DEBUG, "[%s] doopts: Entering options switch stmt, optlen [%x]", SPKRNAME, optlen);
		switch (opt) { 
			case TCPOPT_MAXSEG:
					//debug_output(VERB_DEBUG, "[%s] doopts: case MAXSEG", SPKRNAME);
				if (optlen != TCPOLEN_MAXSEG) {
					//debug_output(VERB_DEBUG, "[%s] doopts: optlen: [%x] maxseg: [%x]", SPKRNAME, optlen, TCPOLEN_MAXSEG);
					continue;
				}
				if (!(th_flags & TH_SYN)) {
					//debug_output(VERB_DEBUG, "[%s] tcp_dooption SYN flag not set", SPKRNAME);
					continue;
				}
				memcpy((char*) &mss, (char*) cp + 2, sizeof(mss)); 
				////debug_output(VERB_DEBUG, "[%s] doopts: mss p0: [%x]", SPKRNAME, mss);
				mss = ntohs(mss); 
				////debug_output(VERB_DEBUG, "[%s] doopts: mss p1: [%x]", SPKRNAME, mss);
				tcp_mss(mss); 
//				tp->t_maxseg = ntohs((u_short)*((char*)cp + 2)); //BUG WHY are we
//				setting this twice? once here and once in the line above?!
//				//debug_output(VERB_DEBUG, "[%s] doopts: mss p2: [%x]", SPKRNAME, mss);
				//avila this is 5... something here is broken. statically
				//setting MSS whenever it dips below 800
//				if (tp->t_maxseg < 800 || tp->t_maxseg > 1460) {
//					tp->t_maxseg = 1400;
//					//debug_output(VERB_ERRORS, "doopts: Recieved MAXSEG value [%d], setting to 1400", tp->t_maxseg);
//				}
				break;

			case TCPOPT_TIMESTAMP:
				//debug_output(VERB_DEBUG, "[%s] doopts: case TIMESTAMP", SPKRNAME);
				if (optlen != TCPOLEN_TIMESTAMP)
					continue;
				*ts_present = 1; 
				bcopy((char *)cp + 2, (char *)ts_val, sizeof(*ts_val)); //FIXME: Misaligned
				*ts_val = ntohl(*ts_val); 
				bcopy((char *)cp + 6, (char *)ts_ecr, sizeof(*ts_ecr)); //FIXME: Misaligned
				*ts_ecr = ntohl(*ts_ecr); 

				//debug_output(VERB_DEBUG, "[%s] doopts: ts_val [%u] ts_ecr [%u]", SPKRNAME, *ts_val, *ts_ecr);
				if (th_flags & TH_SYN) { 
					//debug_output(VERB_DEBUG, "[%s] doopts: recvd a SYN timetamp, ENABLING TIMESTAMPS", SPKRNAME);
					tp->t_flags |= TF_RCVD_TSTMP; 
					tp->ts_recent = *ts_val; 
					tp->ts_recent_age = get_transport()->tcp_now(); 
				}
				break;
#ifdef UNDEF 
			case TCPOPT_SACK_PERMITTED:
				//debug_output(VERB_DEBUG, "[%s] doopts: case SACK", SPKRNAME);
				if (optlen != TCPOLEN_SACK_PERMITTED)
					continue;
				if (!(flags & TO_SYN))
					continue;
				if (!tcp_do_sack)
					continue;
				to->to_flags |= TOF_SACKPERM;
				break;
				case TCPOPT_SACK:
				if (optlen <= 2 || (optlen - 2) % TCPOLEN_SACK != 0)
					continue;
				if (flags & TO_SYN)
					continue;
				to->to_flags |= TOF_SACK;
				to->to_nsacks = (optlen - 2) / TCPOLEN_SACK;
				to->to_sacks = cp + 2;
				tcpstat.tcps_sack_rcv_blocks++;
				break;
#endif 
			case TCPOPT_WSCALE:
				//debug_output(VERB_DEBUG, "[%s] doopts: case WSCALE", SPKRNAME);
				if (optlen != TCPOLEN_WSCALE) 
					continue;
				if (!(th_flags & TH_SYN)) 
					continue;
				tp->t_flags |=  TF_RCVD_SCALE;
				
				tp->requested_s_scale = min(cp[2], TCP_MAX_WINSHIFT); 
				//debug_output(VERB_DEBUG, "[%s] WSCALE set, flags [%x], req_s_sc [%x]\n", SPKRNAME,
				break;
			default: 
			continue; 
		}
    }
	//debug_output(VERB_DEBUG, "[%s] doopts: finished", SPKRNAME);
}

/**
* @brief check to see if the app is waiting for this data; if so, return it now
*
* @param tcp_conn
*/
void XChunk::check_for_and_handle_pending_recv() {
	if (recv_pending) {
		int bytes_returned = read_from_recv_buf(pending_recv_msg);
		get_transport()->ReturnResult(port, pending_recv_msg, bytes_returned);
		recv_pending = false;
		delete pending_recv_msg;
		pending_recv_msg = NULL;
	}
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
* @param tcp_conn The XChunk struct for this connection
*
* @return  The number of bytes read from the buffer.
*/
int XChunk::read_from_recv_buf(XSocketMsg *xia_socket_msg) {
printf("read_from_recv_buf\n");
		// printf("<<< read_from_recv_buf: port=%u, recv_base=%d, next_recv_seqnum=%d, recv_buf_size=%d\n", tcp_conn->port, tcp_conn->recv_base, tcp_conn->next_recv_seqnum, tcp_conn->recv_buffer_size);
	xia::X_Recv_Msg *x_recv_msg = xia_socket_msg->mutable_x_recv();
	int bytes_requested = x_recv_msg->bytes_requested();
	int bytes_returned = 0;
	char buf[1024*1024]; // TODO: pick a buf size
	memset(buf, 0, 1024*1024);
	while (has_pullable_data()) {

		if (bytes_returned >= bytes_requested) break;

		WritablePacket *p = _q_recv.pull_front();

		size_t data_size = p -> length();

		memcpy((void*)(&buf[bytes_returned]), (const void*)p -> data(), data_size);
		bytes_returned += data_size;

		p->kill();

//			printf("    port %u grabbing index %d, seqnum %d\n", tcp_conn->port, i%tcp_conn->recv_buffer_size, i);
	}
	x_recv_msg->set_payload(buf, bytes_returned); // TODO: check this: need to turn buf into String first?
	x_recv_msg->set_bytes_returned(bytes_returned);

//		printf(">>> read_from_recv_buf: port=%u, recv_base=%d, next_recv_seqnum=%d, recv_buf_size=%d\n", tcp_conn->port, tcp_conn->recv_base, tcp_conn->next_recv_seqnum, tcp_conn->recv_buffer_size);
	return bytes_returned;
}

tcpcb *
XChunk::tcp_newtcpcb() 
{ 
	tcpcb *tp = new tcpcb();
	if (tp == NULL)
	    return NULL; 
	
	bzero((char*)tp, sizeof(tcpcb)); 
	tp->t_maxseg = get_transport()->globals()->tcp_mssdflt; 
	tp->t_flags  = TF_REQ_SCALE | TF_REQ_TSTMP; 
	tp->t_srtt   = TCPTV_SRTTBASE; 
	tp->t_rttvar = get_transport()->globals()->tcp_rttdflt * PR_SLOWHZ << 2;
	tp->t_rttmin = TCPTV_MIN; 
	TCPT_RANGESET(tp->t_rxtcur, 
		((TCPTV_SRTTBASE >> 2) + ( TCPTV_SRTTDFLT << 2)) >> 1, 
		TCPTV_MIN, TCPTV_REXMTMAX); 
	tp->snd_cwnd = TCP_MAXWIN << TCP_MAX_WINSHIFT; 
	tp->snd_ssthresh = TCP_MAXWIN << TCP_MAX_WINSHIFT; 

	tp->rcv_wnd = so_recv_buffer_space();

	tp->so_flags = get_transport()->globals()->so_flags; 
	if (get_transport()->globals()->window_scale) { 
		tp->t_flags &= TF_REQ_SCALE; 
		tp->request_r_scale = get_transport()->globals()->window_scale; 
	} 
	if (get_transport()->globals()->use_timestamp) { 
		tp->t_flags &= TF_REQ_TSTMP; 
	}
	return tp; 
}

/* Code for the (reassembly) queues 
 * 
 *  TODO: (OPTIMIZATION) this is currently allocating and freeing one
 *  CTCPQueueElt object per packet.  I have no idea whether an array and a
 *  freemap would be better.  The pure static ringbuffer code from the other
 *  queues doesn't help, since we need to be able to queue packets in random
 *  order.
 */ 

CTCPQueue::~CTCPQueue() {}


int 
CTCPQueue::push(WritablePacket * p, tcp_seq_t seq, tcp_seq_t seq_nxt)
{
	if (p == NULL)
	{
		return -1;
	}
    CTCPQueueElt *qe = NULL ; 
    CTCPQueueElt *wrk = NULL ; 
    // StringAccum sa;

	////debug_output(VERB_CTCPQueue, "CTCPQueue:push pkt:%ubytes, seq:%ubytes", p->length(), seq_nxt-seq); 
	
	/* TCP Queue (Addresses (and seq num) decrease in this dir ->)
	 *
	 *		   ---------------------------------------------
	 * push -> |   empty   | seg_c | seg_b |  gap  | seg_a | -> pull_front
	 *		   ---------------------------------------------
	 *		         	  {_q_tail}^   {_q_last = _q_first}^ 
	 *
	 * _q_first points to the pkt with the lowest seq num in the queue 
	 * _q_last points to the pkt with the highest seq num where no gaps before it
     * _q_tail points to the pkt with the highest seq num ever received
	 * 
	 * In this example:
	 * 		segment_a->nxt == segment_b
	 * 		segment_b->nxt == segment_c
	 * 		segment_c->nxt == NULL
	 * 		segment_a->seq_next < segment_b->seq
	 * 		segment_b->seq_next == segment_c->seq
	 */
	
    /* CASE 1: Queue is empty */
    if (!_q_first) {  
		qe = new CTCPQueueElt(p, seq, seq_nxt); 
		if (!qe) { return -2; }
		_q_first = _q_last = _q_tail = qe; 
		qe->nxt = NULL; 
		//debug_output(VERB_CTCPQueue, "[%s] CTCPQueue::push (empty)", _con->SPKRNAME); 
		//debug_output(VERB_CTCPQueue, "%s", pretty_print(sa, 60)->c_str()); 
		return 0; 
    }

    /* CASE 2a: TAIL INSERT (we got a segment with seq number >= q_tail->seq_nxt) */
	if (SEQ_GEQ(seq, expected())) {
		assert (! _q_tail->nxt); 
		bool perfect = false;

		/* enqueue after _q_tail */ 
		qe = new CTCPQueueElt(p, seq, seq_nxt); 
		_q_tail->nxt = qe;

		/* CASE 2b: PERFECT TAIL INSERT (we got a segment with the next expected seq number) */
		if (seq == expected() && _q_last == _q_tail) {
			_q_last = qe; /* if q_last is q_tail, then we drag q_last along */
			perfect = true;
		}

		_q_tail = qe; /* qe becomes the new _q_tail */

		if (_q_last == NULL)
			loop_last();

		//debug_output(VERB_CTCPQueue, "[%s] CTCPQueue::push (%s)", _con->SPKRNAME,
			// perfect?"perfect tail":"tail"); 
		//debug_output(VERB_CTCPQueue, "%s", pretty_print(sa, 60)->c_str()); 
		return 0; 
	}


	/* CASE 3: HEAD INSERT (we got a segment to be pushed at the front of the queue) */
	if (SEQ_LT(seq, first())) { 

	/* TCP Queue (Addresses (and seq num) decrease in this dir ->)
	 *
	 *                      -----------------
	 *                      |  new segment  |
	 *                      -----------------
	 *                      |overlap|  {seq}^
	 *		-------------------------
	 * .... |  segment  |  segment  | -> pull_front
	 *		-------------------------
	 *		    {_q_last = _q_first}^ 
	 */
     
		/* If the packet overlaps with _q_first trim qe at end of packet */
		int overlap = (int)(seq_nxt - first());
		if (overlap > 0) {
			if (overlap > p->length()) { return -2; }
			p->take(overlap);
			//debug_output(VERB_CTCPQueue, "[%s] Tail overlap [%d] bytes", _con->SPKRNAME, overlap);
			// Should we update qe->seq_nxt? I don't think we need to.
		}

		qe = new CTCPQueueElt(p, seq, seq_nxt); 
		qe->nxt = _q_first; 
		_q_first = qe; 

		// We are pushing in front of head which does not affect _q_last
		// UNLESS _q_last was set to NULL by pull_front. In this case, call loop
		// and _q_last will iteratively move from first toward q_tail until a gap
		// is found or we hit q_tail.
		if (_q_last == NULL)
			loop_last();

		// If we have just made a gap by pushing at the head, set _q_last=_q_first
		if (_q_first->seq_nxt < _q_first->nxt->seq)
			_q_last = _q_first;

		//debug_output(VERB_CTCPQueue, "[%s] CTCPQueue::push (head)", _con->SPKRNAME); 
		//debug_output(VERB_CTCPQueue, "%s", pretty_print(sa, 60)->c_str()); 
		return 0; 
	} 
	

	/* CASE 4: FILL A GAP (Default) 
	 * KEEP IN MIND, this could also be a tail-enqueue where the packet head
	 * overlaps part of _q_tail */
	wrk = _q_first;
	// Try our luck - the gap might be right after _q_last
	if (_q_last && (seq == _q_last->seq_nxt)) {
		wrk = _q_last;
	} else {
		// No luck, now we have to search from _q_first... 
		// But first try to jump to q_last over any ordered part of the queue
		if (_q_last && SEQ_GT(seq, _q_last->seq_nxt)) { wrk = _q_last; }
		while (wrk->nxt && SEQ_GT(seq, wrk->nxt->seq)) {
			// Move along the queue until we find where seq fits
			wrk = wrk->nxt;
		}
	}

	/* TCP Queue (Addresses (and seq num) decrease in this dir ->)
	 * Now wrk points to the segment before the gap where p should be enqueued
	 *
	 *             -----------------
	 *             |  new segment  |
	 *             -----------------
	 *      (gap between wrk and wrk->nxt)
	 *      ------------------------------
	 * .... |   wrk->nxt   |     wrk     | ....
	 *	    ------------------------------
	 */

	// Test for overlap of front of packet with wrk
	int overlap = (int) (wrk->seq_nxt - seq);
	if (overlap > 0) {
		if (overlap > p->length()) { return -2; }
		//debug_output(VERB_CTCPQueue, "[%s] head overlap [%d] bytes", _con->SPKRNAME, overlap);
		p->pull(overlap);
		seq += overlap;
	}

	// If wrk->nxt exists test for overlap of back of packet with wrk->nxt 
	if (wrk->nxt) {
		overlap = (int) (seq_nxt - wrk->nxt->seq);
		if (overlap > 0) {
			if (overlap > p->length()) { return -2; }
			//debug_output(VERB_CTCPQueue, "[%s] Tail overlap [%d] bytes", _con->SPKRNAME, overlap);
			p->take(overlap);
			seq_nxt -= overlap;
		}
	}

	/* enqueue qe right after wrk */
	qe = new CTCPQueueElt(p, seq, seq_nxt);
	if (wrk->nxt) {
		qe->nxt = wrk->nxt;
	}
	wrk->nxt = qe;

	loop_last();
    
	//debug_output(VERB_CTCPQueue, "[%s] CTCPQueue::push (default)", _con->SPKRNAME); 
	//debug_output(VERB_CTCPQueue, "%s", pretty_print(sa, 60)->c_str()); 
    return 0; 
}

/* In the case that we closed a gap, we can move _q_last toward _q_tail */
void
CTCPQueue::loop_last()
{
	// If q_last is null, begin at q_first (important in CASE3)
	CTCPQueueElt *wrk = (_q_last ? _q_last : _q_first);
	while (wrk->nxt && (wrk->seq_nxt == wrk->nxt->seq)) {
		wrk = wrk->nxt;
		_q_last = wrk; 
		//debug_output(VERB_CTCPQueue, "Looping _q_last to [%u]", last());
	}
	_q_last = wrk; 
	//debug_output(VERB_CTCPQueue, "Looped _q_last to [%u]", last());
}

WritablePacket * 
CTCPQueue::pull_front()
{
	WritablePacket	*p = NULL; 
	CTCPQueueElt 	*e = NULL; 

	// CASE 1: The queue is empty, nothing to pull
	if (_q_first == NULL) { 
		//debug_output(VERB_CTCPQueue, "[%s] QPULL FIRST==NULL", _con->SPKRNAME); 
		_q_tail = _q_last = NULL; 
		return NULL; 
	}

	// CASE 2: _q_last is NULL because we previously encountered CASE 3
	if (_q_last == NULL) { 
		//debug_output(VERB_CTCPQueue, "[%s] QPULL LAST==NULL", _con->SPKRNAME); 
		return NULL; 
	}

	/* CASE 3: There is only one in-order packet to pull, return it and set
	 * _q_last = NULL to indicate that there is no more in-order data to pull
	 * after this pull */
	if (_q_first == _q_last) {
		//debug_output(VERB_CTCPQueue, "[%s] QPULL [%u] FIRST==LAST", _con->SPKRNAME, first()); 
		_q_last = NULL;
	} else {
		//debug_output(VERB_CTCPQueue, "[%s] QPULL [%u]", _con->SPKRNAME, first()); 
	}

	e = _q_first; 
	p = e->_p;

	// _q_first becomes either the next QElt or NULL because of how push()
	// assigns _q_first->nxt
	_q_first = _q_first->nxt; 

	delete e; 
	return p; 
}


StringAccum *
CTCPQueue::pretty_print(StringAccum &sa, int signed_width)
{ 
	tcp_seq_t head = 0;
	tcp_seq_t exp = 0;
	tcp_seq_t tail = 0;
	uint32_t i = 0; 
	uint32_t width = (unsigned int) signed_width; 
	CTCPQueueElt * wp; 
	// StringAccum stars; 
	uint32_t thrd = width/3;

	if (width < 46) { 
		// sa << "Too narrow for prettyprinting"; 
		// return &sa; 
	}
	if (_q_first) { 
		wp = _q_first;  
		for (i = 0; i < width; i++) { 
			if (!wp) {
				// stars << "."; 
				continue;
			}
			if (wp == _q_first)
				head = i; 
			if (wp == _q_tail) 
				exp = i; 
			if (wp == _q_last)
				tail = i; 
			if (wp->nxt && (wp->seq_nxt != wp->nxt->seq) ) { 
				// stars << "*_"; 
				i++; 
			} else { 
				// stars << "*"; 
			}
			wp = wp->nxt; 
		}
    } else { 
		head = exp = tail = 0; 
		// for(i = 0; i < width; i++)
			// stars << "."; 
    }
 //    sa << "     FIRST        LAST        TAIL\n";
	// sa.snprintf(36, "%10u  %10u  %10u\n", first(), last(), tailseq()); 
	// sa.snprintf(36, "%10u  %10u  %10u\n", _q_first->seq_nxt, last_nxt(), expected()); 

	for (i = 0; i < width; i++) { 
		if (i == thrd || i == 2*thrd || i== 3*thrd) { 
			// sa << "|"; 
			continue;
		}
		if (((i < thrd && i >= head) || (i > thrd && i <= head)) ||
			((i < 2 * thrd && i >= exp) || (i > 2 * thrd && i <= exp)) ||
			((i < 3 * thrd  && i >= tail) || (i > 3 * thrd && i <= tail))) {
			// sa << "_"; 
			continue;
		}
		// sa << " "; 
    }
    // sa << "\n"; 
	// for (i = 0; i < width; i++ ) {
	// 	if (i == tail || i == exp || i == tail) 
	// 		// sa << "|"; 
	// 	else
	// 		// sa << " "; 
 //    }
    // sa << "\n" << stars; 

	// return &sa;
	return NULL;
}


CTCPFifo::~CTCPFifo()
{ 
	for (int i=_tail; i!= _head; i = (i + 1) % FIFO_SIZE)
	    _q[i]->kill(); 
	CLICK_LFREE(_q,sizeof(WritablePacket *) * FIFO_SIZE ); 
}


int
CTCPFifo::push(WritablePacket *p)
{ 
	//click_chatter("CTCPFifo::push pushing [%x]", p);
	if ((_head + 1) % FIFO_SIZE == _tail) {
	    p->kill(); 
		//click_chatter("CTCPFifo::push had to kill packet");
	    return -1 ; 
	}
	_q[_head] = p; 
	_bytes += p->length(); 
	_head = (_head + 1) % FIFO_SIZE; 
	return 0; 
}


/* the function name lies: retval of 2 actually means "2 or more" */
int
CTCPFifo::pkts_to_send(int offset, int win)
{ 
	if (is_empty()) return 0; 
	if (offset >= win) return 0; 
	if (pkt_length() == 1) return 1;  

	int wp = _tail; 
	int wo = 0; 
	
	//TEST try casting offset as unsigned - POSSIBLY INTRODUCES WRAPAROUND ERROR
	while (wo + _q[wp]->length() <=  offset) {
	    wo += _q[wp]->length(); 
	    wp = (wp + 1) % FIFO_SIZE; 
	    if (wp == _head ) return 0; 
	} 

	if (((wp + 1) % FIFO_SIZE) == _head) 
	    return 1; 

	//TEST try casting win as unsigned - POSSIBLY INTRODUCES WRAPAROUND ERROR
	if (wo + _q[wp]->length() >=  win)
	    return 1;

	return 2;
}


/* get a piece of payload starting at <offset> bytes from the tail */ 
WritablePacket * 
CTCPFifo::get(tcp_seq_t offset)
{ 
	WritablePacket * retval; 
	int wp = _tail; 
	tcp_seq_t wo = 0; 

	if (is_empty()) return NULL; 

	while (wo + _q[wp]->length() <= offset) {
	    wo += _q[wp]->length(); 
	    wp = (wp + 1) % FIFO_SIZE; 
	    if (wp == _head) return NULL; 
	} 

	/* FIXME: this is an expensive packet copy. Maybe there
	is a better solution.  The problem is: We must keep a copy for later
	retransmissions and one copy to send out now */ 

	retval = _q[wp]->clone()->uniqueify(); 

	if (wo < offset) { 
		retval->pull(offset - wo); 
	}
	return retval; 
}


WritablePacket *
CTCPFifo::pull()
{ 
	WritablePacket *p; 
	if (_head == _tail) return NULL; 
	p = _q[_tail]; 
	_tail = (_tail + 1) % FIFO_SIZE; 
	_bytes -= p->length(); 
	return p; 
}


/* Drop <offset> bytes from tail of the fifo by killing the packet and possibly
 * taking excess bytes from the last packet */ 
void 
CTCPFifo::drop_until(tcp_seq_t offset) 
{ 
	tcp_seq_t wo = 0; 
	
	if (is_empty()) { 
		return; 
	}

	while ( (! is_empty()) && wo + _q[_tail]->length() <= offset ) {
		wo += _q[_tail]->length(); 
		_bytes -= _q[_tail]->length(); 
		_q[_tail]->kill(); 
		_tail = (_tail + 1) % FIFO_SIZE; 
	} 
	if (( ! is_empty()) && wo < offset) { 
		_q[_tail]->pull(offset - wo); 
		_bytes -= (offset - wo); 
	}
}


CLICK_ENDDECLS

EXPORT_ELEMENT(XChunk)
ELEMENT_REQUIRES(userlevel)
ELEMENT_REQUIRES(XIAContentModule)
