// -*- mode: c++; c-basic-offset: 4 -*-
/*
 * mqpolldevice.{cc,hh} -- element steals packets from Linux devices by polling.
 * Benjie Chen, Eddie Kohler
 *
 * Copyright (c) 1999-2000 Massachusetts Institute of Technology
 * Copyright (c) 2000 Mazu Networks, Inc.
 * Copyright (c) 2001 International Computer Science Institute
 * Copyright (c) 2004 Regents of the University of California
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
#include <click/glue.hh>
#include "mqpolldevice.hh"
#include "fromdevice.hh"
#include "mqtodevice.hh"
#include <click/error.hh>
#include <click/confparse.hh>
#include <click/router.hh>
#include <click/skbmgr.hh>
#include <click/standard/scheduleinfo.hh>

#include <click/cxxprotect.h>
CLICK_CXX_PROTECT
#include <linux/delay.h>
#include <linux/netdevice.h>
#if __i386__
#include <asm/msr.h>
#endif
CLICK_CXX_UNPROTECT
#include <click/cxxunprotect.h>

/* for hot-swapping */
static AnyDeviceMap poll_device_map;
static struct notifier_block device_notifier;
extern "C" {
static int device_notifier_hook(struct notifier_block *nb, unsigned long val, void *v);
}

void
MQPollDevice::static_initialize()
{
    poll_device_map.initialize();
    device_notifier.notifier_call = device_notifier_hook;
    device_notifier.priority = 1;
    device_notifier.next = 0;
    register_netdevice_notifier(&device_notifier);
}

void
MQPollDevice::static_cleanup()
{
    unregister_netdevice_notifier(&device_notifier);
}

MQPollDevice::MQPollDevice()
{
}

MQPollDevice::~MQPollDevice()
{
}



int
MQPollDevice::configure(Vector<String> &conf, ErrorHandler *errh)
{
    _queue = 0;
    _burst = 8;
    _headroom = 64;
    if (AnyDevice::configure_keywords(conf, errh, true) < 0
	|| cp_va_kparse(conf, this, errh,
			"DEVNAME", cpkP+cpkM, cpString, &_devname,
			"QUEUE", cpkP, cpUnsigned, &_queue,
			"BURST", cpkP, cpUnsigned, &_burst,
			"HEADROOM", 0, cpUnsigned, &_headroom,
			cpEnd) < 0)
	return -1;

#if HAVE_LINUX_POLLING
    //if (find_device(&poll_device_map, errh) < 0)
    //	return -1;
    net_device* dev = lookup_device(errh);
    if (!dev) return -1;
    set_device(dev, &poll_device_map, anydev_from_device);
    if (_dev && (!_dev->poll_on || _dev->polling < 0))
	return errh->error("device '%s' not pollable, use FromDevice instead", _devname.c_str());
    for (int i=0; i<_burst; i++)
	_ppolledin.push_back(0);
#endif

    return 0;
}


/*
 * Use Linux interface for polling, added by us, in include/linux/netdevice.h,
 * to poll devices.
 */
int
MQPollDevice::initialize(ErrorHandler *errh)
{
    if (AnyDevice::initialize_keywords(errh) < 0)
	return -1;

#if HAVE_LINUX_POLLING
    // check for duplicate readers
    if (ifindex() >= 0) {
	void *&used = router()->force_attachment("device_reader_" + String(ifindex()) + "_queue_" + String(_queue));
	if (used)
	    return errh->error("duplicate reader for device '%s'", _devname.c_str());
	used = this;
       	if (!router()->attachment("device_writer_" + String(ifindex())));
	/*
	    errh->warning("no ToDevice(%s) in configuration\n(	\
Generally, you will get bad performance from MQPollDevice unless\n\
you include a ToDevice for the same device. Try adding\n\
'Idle -> ToDevice(%s)' to your configuration.)", _devname.c_str(), _devname.c_str());
	*/
    }

    if (_queue == 0) {
	if (_dev && !_dev->polling) {
	    /* turn off interrupt if interrupts weren't already off */
	    _dev->poll_on(_dev);
	    if (_dev->polling != 2)
		return errh->error("MQPollDevice detected wrong version of polling patch");
	}
	/* sleep for sec */
	ssleep(5);
    }

    ScheduleInfo::initialize_task(this, &_task, _dev != 0, errh);
#if HAVE_STRIDE_SCHED
    // user specifies max number of tickets; we start with default
    _max_tickets = _task.tickets();
    _task.set_tickets(Task::DEFAULT_TICKETS);
#endif

    reset_counts();

#else
    errh->warning("can't get packets: not compiled with polling extensions");
#endif

    return 0;
}

void
MQPollDevice::reset_counts()
{
  _npackets = 0;
  _empty_polls = 0;

  for(int i=0; i<_burst; i++)
    _ppolledin[i] = 0;

#if CLICK_DEVICE_STATS
  _activations = 0;
  _time_poll = 0;
  _time_refill = 0;
  _time_allocskb = 0;
  _perfcnt1_poll = 0;
  _perfcnt1_refill = 0;
  _perfcnt1_allocskb = 0;
  _perfcnt1_pushing = 0;
  _perfcnt2_poll = 0;
  _perfcnt2_refill = 0;
  _perfcnt2_allocskb = 0;
  _perfcnt2_pushing = 0;
#endif
#if CLICK_DEVICE_THESIS_STATS || CLICK_DEVICE_STATS
  _push_cycles = 0;
#endif
  _buffers_reused = 0;
}

void
MQPollDevice::cleanup(CleanupStage)
{
#if HAVE_LINUX_POLLING
    net_device *had_dev = _dev;

    // call clear_device first so we can check poll_device_map for
    // other users
    clear_device(&poll_device_map, anydev_from_device);

    unsigned long lock_flags;
    poll_device_map.lock(false, lock_flags);
    if (had_dev && had_dev->polling > 0 && !poll_device_map.lookup(had_dev, 0))
	if (_queue == 0)
	    had_dev->poll_off(had_dev);
    poll_device_map.unlock(false, lock_flags);
#endif
}

bool
MQPollDevice::run_task(Task *)
{
#if HAVE_LINUX_POLLING
  struct sk_buff *skb_list, *skb;
  int got=0;
# if CLICK_DEVICE_STATS
  uint64_t time_now;
  unsigned low00, low10;
# endif

  SET_STATS(low00, low10, time_now);

  got = _burst;
  skb_list = _dev->mq_rx_poll(_dev, _queue, &got);

  if (got == 0)
    _empty_polls++;
  else
    _ppolledin[got-1]++;

# if CLICK_DEVICE_STATS
  if (got > 0 || _activations > 0) {
    GET_STATS_RESET(low00, low10, time_now,
		    _perfcnt1_poll, _perfcnt2_poll, _time_poll);

    if (got == 0)
      _empty_polls++;
    else
      _activations++;
  }
# endif

  int nskbs = got;
  if (got == 0)
      nskbs = _dev->mq_rx_refill(_dev, _queue, 0);

  if (nskbs > 0) {
    /*
     * Need to allocate 1536+16 == 1552 bytes per packet.
     * "Extra 16 bytes in the SKB for eepro100 RxFD -- perhaps there
     * should be some callback to the device driver to query for the
     * desired packet size."
     * Skbmgr adds 64 bytes of headroom and tailroom, so back request off to
     * 1536.
     */
    struct sk_buff *new_skbs = skbmgr_allocate_skbs(_headroom, 1536, &nskbs);
//    struct sk_buff *new_skbs = skbmgr_allocate_skbs(_headroom, 270, &nskbs);

# if CLICK_DEVICE_STATS
    if (_activations > 0)
      GET_STATS_RESET(low00, low10, time_now, 
	              _perfcnt1_allocskb, _perfcnt2_allocskb, _time_allocskb);
# endif

    nskbs = _dev->mq_rx_refill(_dev, _queue, &new_skbs);

# if CLICK_DEVICE_STATS
    if (_activations > 0) 
      GET_STATS_RESET(low00, low10, time_now, 
	              _perfcnt1_refill, _perfcnt2_refill, _time_refill);
# endif

    if (new_skbs) {
	for (struct sk_buff *skb = new_skbs; skb; skb = skb->next)
	    _buffers_reused++;
	skbmgr_recycle_skbs(new_skbs);
    }
  }

  for (int i = 0; i < got; i++) {
    skb = skb_list;
    skb_list = skb_list->next;
    skb->next = NULL;
 
    if (skb_list) {
      // prefetch annotation area, and first 2 cache
      // lines that contain ethernet and ip headers.
# if __i386__ && HAVE_INTEL_CPU
      asm volatile("prefetcht0 %0" : : "m" (skb_list->cb[0]));
      // asm volatile("prefetcht0 %0" : : "m" (*(skb_list->data)));
      asm volatile("prefetcht0 %0" : : "m" (*(skb_list->data+32)));
# endif
    }

    /* Retrieve the ether header. */
    skb_push(skb, 14);
    if (skb->pkt_type == PACKET_HOST)
      skb->pkt_type |= PACKET_CLEAN;

    Packet *p = Packet::make(skb); 
   
# ifndef CLICK_WARP9
    if (timestamp())
	p->timestamp_anno().set_now();
# endif

    _npackets++;
# if CLICK_DEVICE_THESIS_STATS && !CLICK_DEVICE_STATS
    click_cycles_t before_push_cycles = click_get_cycles();
# endif
    output(0).push(p);
# if CLICK_DEVICE_THESIS_STATS && !CLICK_DEVICE_STATS
    _push_cycles += click_get_cycles() - before_push_cycles - CLICK_CYCLE_COMPENSATION;
# endif
  }

# if CLICK_DEVICE_STATS
  if (_activations > 0) {
    GET_STATS_RESET(low00, low10, time_now, 
	            _perfcnt1_pushing, _perfcnt2_pushing, _push_cycles);
#  if _DEV_OVRN_STATS_
    if ((_activations % 1024) == 0)
	_dev->get_stats(_dev);
#  endif
  }
# endif

  adjust_tickets(got);
  _task.fast_reschedule();
  return got > 0;
#else
  return false;
#endif /* HAVE_LINUX_POLLING */
}

void
MQPollDevice::change_device(net_device *dev)
{
#if HAVE_LINUX_POLLING
    if (_dev == dev)		// no op
	return;

    //if (_queue == 0) {
    _task.strong_unschedule();

    if (dev && (!dev->poll_on || dev->polling < 0)) {
	click_chatter("%s: device '%s' does not support polling", declaration().c_str(), _devname.c_str());
	dev = 0;
    }

    if (_dev)
	_dev->poll_off(_dev);

    set_device(dev, &poll_device_map, true);

    if (_dev && !_dev->polling)
	_dev->poll_on(_dev);

    if (_dev)
	_task.strong_reschedule();
    //}
#else
    (void) dev;
#endif /* HAVE_LINUX_POLLING */
}

extern "C" {
static int
device_notifier_hook(struct notifier_block *nb, unsigned long flags, void *v)
{
#ifdef NETDEV_GOING_DOWN
    if (flags == NETDEV_GOING_DOWN)
	flags = NETDEV_DOWN;
#endif
    if (flags == NETDEV_DOWN || flags == NETDEV_UP || flags == NETDEV_CHANGE) {
	bool down = (flags == NETDEV_DOWN);
	net_device *dev = (net_device *)v;
	unsigned long lock_flags;
	poll_device_map.lock(true, lock_flags);
	AnyDevice *es[8];
	int nes = poll_device_map.lookup_all(dev, down, es, 8);
	for (int i = 0; i < nes; i++)
	    ((MQPollDevice *)(es[i]))->change_device(down ? 0 : dev);
	poll_device_map.unlock(true, lock_flags);
    }
    return 0;
}
}

static String
MQPollDevice_read_calls(Element *f, void *)
{
  MQPollDevice *kw = (MQPollDevice *)f;
  String ret;
  ret = String(kw->_npackets) + " packets received\n" +
    String(kw->_buffers_reused) + " buffers reused\n" +
    String(kw->_empty_polls) + " empty polls\n" +
#if CLICK_DEVICE_STATS
    String(kw->_time_poll) + " cycles poll\n" +
    String(kw->_time_refill) + " cycles refill\n" +
    String(kw->_time_allocskb) + " cycles allocskb\n" +
    String(kw->_push_cycles) + " cycles pushing\n" +
    String(kw->_perfcnt1_poll) + " perfctr1 poll\n" +
    String(kw->_perfcnt1_refill) + " perfctr1 refill\n" +
    String(kw->_perfcnt1_allocskb) + " perfctr1 allocskb\n" +
    String(kw->_perfcnt1_pushing) + " perfctr1 pushing\n" +
    String(kw->_perfcnt2_poll) + " perfctr2 poll\n" +
    String(kw->_perfcnt2_refill) + " perfctr2 refill\n" +
    String(kw->_perfcnt2_allocskb) + " perfctr2 allocskb\n" +
    String(kw->_perfcnt2_pushing) + " perfctr2 pushing\n" +
    String(kw->_activations) + " activations\n";
#else
    String();
#endif
  ret += "Polled in-->\n";
  for (int i=0; i<kw->_burst; i++)
    ret += String(i+1)+":\t"+String(kw->_ppolledin[i])+"\n";
  return ret;
}

static String
MQPollDevice_read_stats(Element *e, void *thunk)
{
  MQPollDevice *pd = (MQPollDevice *)e;
  switch (reinterpret_cast<intptr_t>(thunk)) {
   case 0:
    return String(pd->_npackets);
#if CLICK_DEVICE_THESIS_STATS || CLICK_DEVICE_STATS
   case 1:
    return String(pd->_push_cycles);
#endif
#if CLICK_DEVICE_STATS
   case 2:
    return String(pd->_time_poll);
   case 3:
    return String(pd->_time_refill);
#endif
   case 4:
    return String(pd->_buffers_reused);
   default:
    return String();
  }
}

static int
MQPollDevice_write_stats(const String &, Element *e, void *, ErrorHandler *)
{
  MQPollDevice *pd = (MQPollDevice *)e;
  pd->reset_counts();
  return 0;
}

void
MQPollDevice::add_handlers()
{
  add_read_handler("calls", MQPollDevice_read_calls, 0);
  add_read_handler("count", MQPollDevice_read_stats, 0);
  // XXX deprecated
  add_read_handler("packets", MQPollDevice_read_stats, 0);
#if CLICK_DEVICE_THESIS_STATS || CLICK_DEVICE_STATS
  add_read_handler("push_cycles", MQPollDevice_read_stats, (void *)1);
#endif
#if CLICK_DEVICE_STATS
  add_read_handler("poll_cycles", MQPollDevice_read_stats, (void *)2);
  add_read_handler("refill_dma_cycles", MQPollDevice_read_stats, (void *)3);
#endif
  add_write_handler("reset_counts", MQPollDevice_write_stats, 0, Handler::BUTTON);
  add_read_handler("buffers_reused", MQPollDevice_read_stats, (void *)4);
  add_task_handlers(&_task);
}

ELEMENT_REQUIRES(AnyDevice linuxmodule)
EXPORT_ELEMENT(MQPollDevice)