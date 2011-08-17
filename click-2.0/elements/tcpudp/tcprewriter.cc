// -*- c-basic-offset: 4 -*-
/*
 * tcprewriter.{cc,hh} -- rewrites packet source and destination
 * Eddie Kohler
 *
 * Copyright (c) 2000 Massachusetts Institute of Technology
 * Copyright (c) 2000 Mazu Networks, Inc.
 * Copyright (c) 2008-2010 Meraki, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, subject to the conditions
 * listed in the Click LICENSE file. These conditions include: you must
 * preserve this copyright notice, and you cannot mention the copyright
 * holders in advertising related to the Software without their permission.
 * The Software is provided WITHOUT ANY WARRANTY, EXPRESS OR IMPLIED. This
 * notice is a summary of the Click LICENSE file; the license in that file is
 * legally binding.
 */

#include <click/config.h>
#include "tcprewriter.hh"
#include <clicknet/ip.h>
#include <clicknet/tcp.h>
#include <click/args.hh>
#include <click/straccum.hh>
#include <click/error.hh>
CLICK_DECLS

// TCPMapping

int
TCPRewriter::TCPFlow::update_seqno_delta(bool direction,
					 tcp_seq_t trigger, int32_t d)
{
    if (SEQ_LEQ(trigger, _trigger[direction])
	&& (_trigger[direction] || _delta[direction]
	    || _old_delta[direction]))
	return -1;
    else {
	_old_delta[direction] = _delta[direction];
	_trigger[direction] = trigger;
	_delta[direction] += d;
	if (_old_delta[direction] || _delta[direction])
	    _tflags |= tf_seqno_delta << direction;
	return 0;
    }
}

void
TCPRewriter::TCPFlow::apply_sack(bool direction, click_tcp *tcph, int len)
{
    if ((int)(tcph->th_off << 2) < len)
	len = tcph->th_off << 2;
    uint8_t *begin_opt = reinterpret_cast<uint8_t *>(tcph + 1);
    uint8_t *end_opt = reinterpret_cast<uint8_t *>(tcph) + len;
    uint32_t csum_delta = 0;

    uint8_t *opt = begin_opt;
    while (opt < end_opt)
	switch (*opt) {
	  case TCPOPT_EOL:
	    goto done;
	  case TCPOPT_NOP:
	    opt++;
	    break;
	  case TCPOPT_SACK:
	      if (opt + opt[1] > end_opt || (opt[1] % 8) != 2) {
		  goto done;
	      } else {
		  uint8_t *end_sack = opt + opt[1];

		  // develop initial checksum value
		  uint16_t *csum_begin = reinterpret_cast<uint16_t *>(begin_opt + ((opt + 2 - begin_opt) & ~1));
		  for (uint16_t *csum = csum_begin; reinterpret_cast<uint8_t *>(csum) < end_sack; csum++)
		      csum_delta += ~*csum & 0xFFFF;

		  for (opt += 2; opt < end_sack; opt += 8) {
#if HAVE_INDIFFERENT_ALIGNMENT
		      uint32_t *uopt = reinterpret_cast<uint32_t *>(opt);
		      uopt[0] = htonl(new_ack(direction, ntohl(uopt[0])));
		      uopt[1] = htonl(new_ack(direction, ntohl(uopt[1])));
#else
		      uint32_t buf[2];
		      memcpy(&buf[0], opt, 8);
		      buf[0] = htonl(new_ack(direction, ntohl(buf[0])));
		      buf[1] = htonl(new_ack(direction, ntohl(buf[1])));
		      memcpy(opt, &buf[0], 8);
#endif
		  }

		  // finish off csum_delta calculation
		  for (uint16_t *csum = csum_begin; reinterpret_cast<uint8_t *>(csum) < end_sack; csum++)
		      csum_delta += *csum;
		  break;
	      }
	  default:
	    if (opt[1] < 2)
		goto done;
	    opt += opt[1];
	    break;
	}

  done:
    if (csum_delta) {
	uint32_t sum = (~tcph->th_sum & 0xFFFF) + csum_delta;
	sum = (sum & 0xFFFF) + (sum >> 16);
	tcph->th_sum = ~(sum + (sum >> 16));
    }
}

void
TCPRewriter::TCPFlow::apply(WritablePacket *p, bool direction, unsigned annos)
{
    assert(p->has_network_header());
    click_ip *iph = p->ip_header();

    // IP header
    const IPFlowID &revflow = _e[!direction].flowid();
    iph->ip_src = revflow.daddr();
    iph->ip_dst = revflow.saddr();
    if (annos & 1)
	p->set_dst_ip_anno(revflow.saddr());
    if (direction && (annos & 2))
	p->set_anno_u8(annos >> 2, _reply_anno);
    update_csum(&iph->ip_sum, direction, _ip_csum_delta);

    // end if not first fragment
    if (!IP_FIRSTFRAG(iph) || p->transport_length() < 18)
	return;

    // TCP header
    click_tcp *tcph = p->tcp_header();
    tcph->th_sport = revflow.dport();
    tcph->th_dport = revflow.sport();
    update_csum(&tcph->th_sum, direction, _udp_csum_delta);

    // track connection state
    bool have_payload = ((iph->ip_hl + tcph->th_off) << 2) < ntohs(iph->ip_len);
    if (tcph->th_flags & TH_RST)
	_state |= s_both_done;
    else if (tcph->th_flags & TH_FIN)
	_state |= s_forward_done << direction;
    else if ((tcph->th_flags & TH_SYN) || have_payload)
	_state &= ~(s_forward_done << direction);
    if (have_payload)
	_state |= s_forward_data << direction;

    // end if weird transport length
    if (p->transport_length() < (tcph->th_off << 2))
	return;

    // update sequence numbers
    if (_tflags & (tf_seqno_delta << direction)) {
	uint32_t newval = htonl(new_seq(direction, ntohl(tcph->th_seq)));
	click_update_in_cksum(&tcph->th_sum, tcph->th_seq >> 16, newval >> 16);
	click_update_in_cksum(&tcph->th_sum, tcph->th_seq, newval);
	tcph->th_seq = newval;
    }

    if (_tflags & (tf_seqno_delta << !direction)) {
	uint32_t newval = htonl(new_ack(direction, ntohl(tcph->th_ack)));
	click_update_in_cksum(&tcph->th_sum, tcph->th_ack >> 16, newval >> 16);
	click_update_in_cksum(&tcph->th_sum, tcph->th_ack, newval);
	tcph->th_ack = newval;

	// update SACK sequence numbers
	if (tcph->th_off > 8
	    || (tcph->th_off == 8
		&& *(reinterpret_cast<const uint32_t *>(tcph + 1)) != htonl(0x0101080A)))
	    apply_sack(direction, tcph, p->transport_length());
    }
}

void
TCPRewriter::TCPFlow::unparse(StringAccum &sa, bool direction, click_jiffies_t now) const
{
    sa << _e[direction].flowid() << " => " << _e[direction].rewritten_flowid();
    if (_delta[direction] != 0)
	sa << " seq " << (_delta[direction] > 0 ? "+" : "") << _delta[direction];
    unparse_ports(sa, direction, now);
}


// TCPRewriter

TCPRewriter::TCPRewriter()
{
}

TCPRewriter::~TCPRewriter()
{
}

void *
TCPRewriter::cast(const char *n)
{
    if (strcmp(n, "IPRewriterBase") == 0)
	return (IPRewriterBase *)this;
    else if (strcmp(n, "TCPRewriter") == 0)
	return (TCPRewriter *)this;
    else
	return 0;
}

int
TCPRewriter::configure(Vector<String> &conf, ErrorHandler *errh)
{
    // numbers in seconds
    _timeouts[0] = 300;		// nodata: 5 minutes (should be > TCP_DONE)
    _tcp_data_timeout = 86400;	// 24 hours
    _tcp_done_timeout = 240;	// 4 minutes
    bool dst_anno = true, has_reply_anno = false;
    int reply_anno;

    if (Args(this, errh).bind(conf)
	.read("TCP_NODATA_TIMEOUT", SecondsArg(), _timeouts[0])
	.read("TCP_GUARANTEE", SecondsArg(), _timeouts[1])
	.read("TIMEOUT", SecondsArg(), _tcp_data_timeout)
	.read("TCP_TIMEOUT", SecondsArg(), _tcp_data_timeout)
	.read("TCP_DONE_TIMEOUT", SecondsArg(), _tcp_done_timeout)
	.read("DST_ANNO", dst_anno)
	.read("REPLY_ANNO", AnnoArg(1), reply_anno).read_status(has_reply_anno)
	.consume() < 0)
	return -1;

    _annos = (dst_anno ? 1 : 0) + (has_reply_anno ? 2 + (reply_anno << 2) : 0);
    _tcp_data_timeout *= CLICK_HZ; // IPRewriterBase handles the others
    _tcp_done_timeout *= CLICK_HZ;

    return IPRewriterBase::configure(conf, errh);
}

IPRewriterEntry *
TCPRewriter::add_flow(int /*ip_p*/, const IPFlowID &flowid,
		      const IPFlowID &rewritten_flowid, int input)
{
    void *data;
    if (!(data = _allocator.allocate()))
	return 0;

    TCPFlow *flow = new(data) TCPFlow
	(flowid, _input_specs[input].foutput,
	 rewritten_flowid, _input_specs[input].routput,
	 !!_timeouts[1], click_jiffies() + relevant_timeout(_timeouts),
	 this, input);

    return store_flow(flow, input, _map);
}

void
TCPRewriter::push(int port, Packet *p_in)
{
    WritablePacket *p = p_in->uniqueify();
    click_ip *iph = p->ip_header();

    // handle non-first fragments
    if (iph->ip_p != IP_PROTO_TCP
	|| !IP_FIRSTFRAG(iph)
	|| p->transport_length() < 8) {
	const IPRewriterInput &is = _input_specs[port];
	if (is.kind == IPRewriterInput::i_nochange)
	    output(is.foutput).push(p);
	else
	    p->kill();
	return;
    }

    IPFlowID flowid(p);
    IPRewriterEntry *m = _map.get(flowid);

    if (!m) {			// create new mapping
	IPRewriterInput &is = _input_specs.at_u(port);
	IPFlowID rewritten_flowid = IPFlowID::uninitialized_t();
	int result = is.rewrite_flowid(flowid, rewritten_flowid, p);
	if (result == rw_addmap)
	    m = TCPRewriter::add_flow(IP_PROTO_TCP, flowid, rewritten_flowid, port);
	if (!m) {
	    checked_output_push(result, p);
	    return;
	} else if (_annos & 2)
	    m->flow()->set_reply_anno(p->anno_u8(_annos >> 2));
    }

    TCPFlow *mf = static_cast<TCPFlow *>(m->flow());
    mf->apply(p, m->direction(), _annos);

    click_jiffies_t now_j = click_jiffies();
    if (_timeouts[1])
	mf->change_expiry(_heap, true, now_j + _timeouts[1]);
    else
	mf->change_expiry(_heap, false, now_j + tcp_flow_timeout(mf));

    output(m->output()).push(p);
}


String
TCPRewriter::tcp_mappings_handler(Element *e, void *)
{
    TCPRewriter *rw = (TCPRewriter *)e;
    click_jiffies_t now = click_jiffies();
    StringAccum sa;
    for (Map::iterator iter = rw->_map.begin(); iter.live(); ++iter) {
	TCPFlow *f = static_cast<TCPFlow *>(iter->flow());
	f->unparse(sa, iter->direction(), now);
	sa << '\n';
    }
    return sa.take_string();
}

void
TCPRewriter::add_handlers()
{
    add_read_handler("mappings", tcp_mappings_handler, 0);
    add_rewriter_handlers(true);
}

CLICK_ENDDECLS
ELEMENT_REQUIRES(IPRewriterBase)
EXPORT_ELEMENT(TCPRewriter)