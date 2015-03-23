/*
** Copyright 2013 Carnegie Mellon University
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**    http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/
/*!
** @file Xselect.c
** @brief implements Xselect()
*/
#include <sys/select.h>
#include <sys/poll.h>
#include <errno.h>
#include "Xsocket.h"
#include "Xinit.h"
#include "Xutil.h"

typedef struct {
	int fd;
	unsigned port;
} Sock2Port;


/*!
** @brief waits for one of a set of Xsockets to become ready to perform I/O.
**
** Xsocket specific version of poll. See the poll man page for more detailed information.
**
** #include <sys/poll.h>
**
** @param ufds array of pollfds indicating sockets and states to check for
** @param nfds number of entries in ufds
** \n socket ids specified as 0 or negative will be ignored
** \n valid flags for events are POLLIN | POLLOUT | POLLERR
** \n revents contains the returned flags and can be POLLIN | POLLOUT | POLLERR | POLLINVAL | POLLHUP
** @param timeout number of milliseconds to wait for an event to happen
**
** @returns 0 if timeout occured
** @returns a positive integer indicating the number of sockets with return events
** @retuns -1 with errno set if an error occured
**
** @warning this function is only valid for stream and datagram sockets. 
*/
int Xpoll(struct pollfd *ufds, unsigned nfds, int timeout)
{
	int sock = 0;
	int nxfds = 0;
	int rc, xrc;

	if (ufds == NULL && nfds > 0) {
		errno = EFAULT;
		return -1;
	}

	struct pollfd *rfds = (struct pollfd*)calloc(nfds + 1, sizeof(struct pollfd));
	Sock2Port *s2p = (Sock2Port*)calloc(nfds, sizeof(Sock2Port));

	memcpy(rfds, ufds, nfds * sizeof(struct pollfd));

	xia::XSocketMsg xsm;
	xia::X_Poll_Msg *pollMsg = xsm.mutable_x_poll();

	pollMsg->set_type(xia::X_Poll_Msg::DOPOLL);

	for (unsigned i = 0; i < nfds; i++) {

		ufds[i].revents = 0;

		if (ufds[i].fd > 0 && (ufds[i].events & (POLLIN | POLLOUT | POLLPRI))) {
			if (getSocketType(ufds[i].fd) != XSOCK_INVALID) {
				// add the Xsocket to the xpoll struct
				// TODO: should this work for Content sockets?

				xia::X_Poll_Msg::PollFD *pfd = pollMsg->add_pfds();
				
				// find the port number associated with this Xsocket
				struct sockaddr_in sin;
				socklen_t slen = sizeof(sin);
				(_f_getsockname)(ufds[i].fd, (struct sockaddr*)&sin, &slen);
				//printf("sock %d, port %d, flags %x\n", ufds[i].fd, ntohs(sin.sin_port), ufds[i].events);

				pfd->set_port(sin.sin_port);
				s2p[i].fd = ufds[i].fd;
				s2p[i].port = sin.sin_port;

				pfd->set_flags(ufds[i].events);

				nxfds++;

				// disable the socket in the real poll list
				rfds[i].fd = -rfds[i].fd;

			} else {
				s2p[i].fd = s2p[i].port = 0;
			}
		}
	}

	if (nxfds == 0) {
		// there are no Xsocket to poll for, just do a straight poll with the original data
		return 	(_f_poll)(ufds, nfds, timeout);
	}

	xsm.set_type(xia::XPOLL);
	xsm.set_sequence(0);
	pollMsg->set_nfds(nxfds);


	// Real sockets in the Poll message are set to 0. They are left in the list to make processing easier

	// the rfds (Real fd) list has the fds flipped negative for the xsockets so they will be ignored 
	//  for the same reason

	if ((sock = (_f_socket)(AF_INET, SOCK_DGRAM, 0)) == -1) {
		LOGF("error creating Xpoll socket: %s", strerror(errno));
		rc = -1;
		goto done;
	}
	allocSocketState(sock, SOCK_DGRAM);

	click_send(sock, &xsm);

	// now we need to do a real poll
	// it will trigger once click generates an xpoll event or pone of the external fds has an event

	// add the poll control socket
	rfds[nfds].fd = sock;
	rfds[nfds].events = POLLIN;
	rfds[nfds].revents = 0;

	rc = (_f_poll)(rfds, nfds + 1, timeout);

	if (rc > 0 && rfds[nfds].revents != 0) {
		
		// there's data from click!

		if (click_reply(sock, 0, &xsm) < 0) {
			LOG("Error getting data from Click\n");
			rc = -1;
			goto done;
		}

		xia::X_Poll_Msg *pout = xsm.mutable_x_poll();
		xrc = pout->nfds();

		// loop thru returned xsockets
		for (int i = 0; i < xrc; i++) {
			const xia::X_Poll_Msg::PollFD& pfd_out = pout->pfds(i);
			unsigned port = pfd_out.port();
			unsigned flags = pfd_out.flags();
				
			//LOGF("poll returned x%0x for %d\n", flags, port);

			// find the associated socket
			int fd = 0;
			for (unsigned j = 0; j < nfds; j++) {
				if (port == s2p[j].port) {
					fd = s2p[j].fd;
					break;
				}
			}

			// find the socket in the original poll & update the revents field
			for (unsigned j = 0; j < nfds; j++) {
				if (ufds[j].fd == fd) {
					ufds[j].revents = flags;
					break;
				}
			}
		}

		// rc is the number of fds returned by poll + plus number of sockets found by click
		//  minus the event for the control socket 
		rc += xrc - 1;

	} else {
		// we need to tell click to cancel the Xpoll event
		xsm.Clear();
		xsm.set_type(xia::XPOLL);
		pollMsg = xsm.mutable_x_poll();
		pollMsg->set_type(xia::X_Poll_Msg::CANCEL);

		click_send(sock, &xsm);
	}


done:
	int eno = errno;
	if (sock > 0) {
		freeSocketState(sock);
		(_f_close)(sock);
	}
	free(rfds);
	free(s2p);
	errno = eno;
	return rc;
}



/*!
** @brief waits for one of a set of Xsockets to become ready to perform I/O.
**
** Xsocket specific version of select. See the select man page for more detailed information.
** This implementation uses Xpoll internally, and is provided to make porting easier. New code
** should call Xpoll instead.
**
** @param ndfs The highest socket number contained in the fd_sets plus 1
** @param readfds fd_set containing sockets to check for readability
** @param writefds fd_set containing sockets to check for writability
** @param errorfds fd_set containing sockets to check for errors
** @param timeout amount of time to wait for a socket to change state
** @returns greater than 0, number of sockets ready
** @returns 0 if the timeout expired
** @returns less than 0 if an error occurs 
**
** @warning this function is only valid for stream and datagram sockets. 
*/
int Xselect(int nfds, fd_set *readfds, fd_set *writefds, fd_set *errorfds, struct timeval *timeout)
{
	fd_set rfds;
	fd_set wfds;
	fd_set efds;
	unsigned nx = 0;
	int xrc = 0;
	int sock;
	int largest = 0;
	int count = 0;
	int rc = 0;

	FD_ZERO(&rfds);
	FD_ZERO(&wfds);
	FD_ZERO(&efds);

	// if the fd sets are sparse, this will waste space especially if nfds is large
	Sock2Port *s2p = (Sock2Port*)calloc(nfds, sizeof(Sock2Port));

	// create protobuf message
	xia::XSocketMsg xsm;
	xia::X_Poll_Msg *pollMsg = xsm.mutable_x_poll();
	pollMsg->set_type(xia::X_Poll_Msg::DOPOLL);

	for (int i = 0; i < nfds; i++) {

		int flags = 0;
		int r = 0;
		int w = 0;
		int e = 0;
		
		if (readfds && FD_ISSET(i, readfds)) {
			flags |= POLLIN;
			r = i;
		}
		if (writefds && FD_ISSET(i, writefds)) {
			flags |= POLLOUT;
			w = i;
		}
		if (errorfds && FD_ISSET(i, errorfds)) {
			flags |= POLLERR;
			e = i;
		}

		// is it an xsocket
		if (flags && getSocketType(i) != XSOCK_INVALID) {
			// we found an Xsocket, do the Xpoll magic
			nx++;

			xia::X_Poll_Msg::PollFD *pfd = pollMsg->add_pfds();

			// find the port number associated with this Xsocket
			struct sockaddr_in sin;
			socklen_t slen = sizeof(sin);
			(_f_getsockname)(i, (struct sockaddr*)&sin, &slen);
			//printf("sock %d, port %d, flags %x\n", ufds[i].fd, ntohs(sin.sin_port), ufds[i].events);

			pfd->set_port(sin.sin_port);
			s2p[i].fd = i;
			s2p[i].port = sin.sin_port;

			pfd->set_flags(flags);

		} else {
			if (i > largest)
				largest = i;

			// it's a regular fd, put it into the select fdsets
			if (r != 0)
				FD_SET(i, &rfds);
			if (w != 0)
				FD_SET(i, &wfds);
			if (e != 0)
				FD_SET(i, &efds);
		
			s2p[i].fd = s2p[i].port = 0;
		}
	}


	if (nx == 0) {
		// there were no xsockets in the FD_SETS, just do a normal select
		rc = (_f_select)(nfds, readfds, writefds, errorfds, timeout);
		goto done;
	}



	// create an control socket
	if ((sock = (_f_socket)(AF_INET, SOCK_DGRAM, 0)) == -1) {
		LOGF("error creating Xpoll socket: %s", strerror(errno));
		goto done;
	}
	allocSocketState(sock, SOCK_DGRAM);

	click_send(sock, &xsm);

	// add the control socket to the select read fdset
	if (sock > largest)
		largest = sock;
	FD_SET(sock, &rfds);

	rc = (_f_select)(largest, &rfds, (writefds != NULL ? &wfds : NULL), (errorfds != NULL ? &efds : NULL), timeout);

	// reset the bit arrays for the return to caller
	if (readfds)
		FD_ZERO(readfds);
	if (writefds)
		FD_ZERO(writefds);
	if (errorfds)
		FD_ZERO(errorfds);

	// fill the fdsets in with the triggered sockets/fds
	count = 0;
	if (rc > 0) {

		// get the regular fds
		for (int i = 0; i < largest; i++) {
			if (i != sock) {
				if (readfds && FD_ISSET(i, &rfds)) {
					FD_SET(i, readfds);
					count++;
				}
				if (writefds && FD_ISSET(i, &wfds)) {
					FD_SET(i, writefds);
					count++;
				}
				if (errorfds && FD_ISSET(i, &efds)) {
					FD_SET(i, errorfds);
					count++;
				}
			}
		}

		if (FD_ISSET(sock, &rfds)) {
			// we have Xsockets data

			xsm.Clear();

			if (click_reply(sock, 0, &xsm) < 0) {
				LOG("Error getting data from Click\n");
				rc = -1;
				goto done;
			}

			xia::X_Poll_Msg *pout = xsm.mutable_x_poll();
			xrc = pout->nfds();

			for (int i = 0; i < xrc; i++) {
				const xia::X_Poll_Msg::PollFD& pfd_out = pout->pfds(i);
				int flags = pfd_out.flags();
				unsigned port = pfd_out.port();

				int fd = 0;
				for (int j = 0; j < nfds; j++) {
					if (port == s2p[j].port) {
						fd = s2p[j].fd;
						break;
					}
				}

				// printf("socket %d out flags:%08x\n", pfds[i].fd, pfds[i].revents);
				if (readfds && (flags & POLLIN)) {
					FD_SET(fd, readfds);
					count++;
				}
				if (writefds && (flags & POLLOUT)) {
					FD_SET(fd, writefds);
					count++;
				}
				if (errorfds && (flags & POLLERR)) {
					FD_SET(fd, errorfds);
					count++;
				}
			}

		} else {
			// we need to tell click to cancel the Xpoll event
			xsm.Clear();
			xsm.set_type(xia::XPOLL);
			pollMsg = xsm.mutable_x_poll();
			pollMsg->set_type(xia::X_Poll_Msg::CANCEL);

			click_send(sock, &xsm);
		}
	}		

done:
	int eno = errno;
	if (sock > 0) {
		freeSocketState(sock);
		(_f_close)(sock);
	}
	free(s2p);
	errno = eno;
	return (rc <= 0 ? rc : count);
}
