/* ts=4 */
/*
** Copyright 2015 Carnegie Mellon University
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
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/file.h>

#include "Xsocket.h"
#include "dagaddr.hpp"
#include "rid.h"
#include "xcache.h"

#define VERSION "v0.1"
#define TITLE "XIA RID Producer Application"

// XXX: 0 = 'permanent', i.e. cached objects live forever (just like diamonds)
#define DEFAULT_CACHE_SLICE_TTL		(unsigned) 0
#define DEFAULT_CACHE_SLICE_SIZE	(unsigned) 200000000

char PRODUCER_AID[4 + (XID_SIZE * 2) + 1];
char PRODUCER_HID[4 + (XID_SIZE * 2) + 1];
char PRODUCER_4ID[4 + (XID_SIZE * 2) + 1];

int send_rid_response(
		XcacheHandle * xcache_handle,
		sockaddr_x * rid_req_src,
		socklen_t rid_req_src_len) {

	// what does send_rid_response() do?
	// 	1) generate a sensor value
	//	2) send it to rid_req_src using xcache's interfaces

	int rc = 0;

	// 1) generate a sensor value, save it in value_str according to a some
	// format
	char value_str[128] = {'\0'};
	snprintf(
			value_str,
			128,
			"timestamp:%u:temperature:%d:unit:degC",
			(unsigned) time(NULL),
			(rand() % 111 + (-50)));

	// 2) send the value to rid_req_src, using xcache's XpushChunkto
	sockaddr_x cid_value_addr;

	if ((rc = XpushChunkto(
			xcache_handle,
			value_str,
			strlen(value_str),
			&cid_value_addr,
			rid_req_src,
			rid_req_src_len)) < 0) {

		warn("[rid_producer]: XpushChunkto() error = %d", errno);

	} else {

		say("[rid_producer]: 'Xpushed' RID response:"\
				"\n\t[DST_DAG] = %s"\
				"\n\t[RID PAYLOAD] = %s"\
				"\n\t[CID PAYLOAD] = %s\n",
				Graph((sockaddr_x *) rid_req_src).dag_string().c_str(),
				Graph(cid_value_addr).dag_string().c_str(),
				value_str);
	}

	return rc;
}

int main(int argc, char **argv)
{
	int x_sock = 0;

	char * name = (char *) calloc(PREFIX_MAX_LENGTH, sizeof(char));
	char * rid_string;

	// initialize the producer's xcache handle
	XcacheHandle * xcache_handle = NULL; 
	XcacheHandleInit(xcache_handle);

	// similarly to other XIA example apps, print some initial info on the app
	say("\n%s (%s): started\n", TITLE, VERSION);

	// ************************************************************************
	// 1) gather the application arguments
	// ************************************************************************

	// 1.1) if not enough arguments, stop immediately...
	if (argc < 3) {
		die(-1, "[rid_producer]: usage: rid_producer -n <URL-like (producer) name>");
	}

	char **av = argv;
	argc--, av++;

	while (argc > 0 && *av[0] == '-') {

        int c = argc;

        while (*++av[0]) {

            if (c != argc)
                break;

            switch (*av[0]) {
            	// 1.2) single URL-like name given as parameter to generate
            	// RIDs (e.g. '/cmu/ece/rmcic/2221c/sensor/temperature/')
				case 'u':
					// 1.2.1) shift to the argument value, save the URL-like
					// name in prefix
					argc--, av++;
					strncpy(name, av[0], strlen(av[0]));
					break;

//				// 1.3) file with list of URL-like names to generate RIDs
				// FIXME: will be used when binding to multiple RIDs is
				// implemented
//				case 'f':
//					argc--, av++;
//					//npackets = atoi(av[0]);
//					break;
            }
        }

		argc--, av++;
	}

	// ************************************************************************
	// 2) create RID to be served by this producer
	// ************************************************************************
	if (name[0] != '\0') {

		rid_string = name_to_rid(name);

	} else {

		// 2.1) if no name has been specified, no point going on...
		die(-1, "[rid_producer]: name string is empty\n");
	}

	// ************************************************************************
	// 3) create x_sock, bind() it to the created RID, listen for RID requests
	// ************************************************************************

	// 3.1) x_sock
	if ((x_sock = Xsocket(AF_XIA, SOCK_DGRAM, 0)) < 0) {
		die(-1, "[rid_producer]: Xsocket(SOCK_DGRAM) error = %d", errno);
	}

	// 3.2) get the localhost address of the producer (AD:HID components)
    if (XreadLocalHostAddr(
    		x_sock,
			PRODUCER_AID, sizeof(PRODUCER_AID),
			PRODUCER_HID, sizeof(PRODUCER_HID),
			PRODUCER_4ID, sizeof(PRODUCER_4ID)) < 0 ) {

		die(	-1,
				"[rid_producer]: error reading localhost address\n");
    }

	// 3.2) create a local RID DAG, in the style AD:HID:RID (= rid_string).
    sockaddr_x rid_local_addr;
    socklen_t rid_local_addr_len;

	to_rid_addr(
		rid_string, 
		PRODUCER_AID,
		PRODUCER_HID,
		&rid_local_addr,
		&rid_local_addr_len);

	// 3.3) bind x_sock to rid_local_addr
	if (Xbind(
			x_sock,
			(struct sockaddr *) &rid_local_addr,
			rid_local_addr_len) < 0) {

		Xclose(x_sock);

		die(	-1,
				"[rid_producer]: unable to Xbind() to DAG %s error = %d",
				Graph(&rid_local_addr).dag_string().c_str(),
				errno);
	}

	say("[rid_producer]: will listen to RID packets directed at: %s",
			Graph(&rid_local_addr).dag_string().c_str());

	//*************************************************************************
	// 4) launch RID request listener
	//*************************************************************************

	// 4.1) start listening to RID requests...
	// FIXME: ... for now, in an endless loop (with select())

	// 4.2.1) create variables to hold src and dst addresses of the 
	// RID request. src will be directly set by Xrevfrom(), while dst 
	// is implicit (the x_sock socket should only get requests for the 
	// prefix bound to the socket)
	sockaddr_x rid_req_src;
	socklen_t rid_req_src_len;

	// 4.2.2) a buffer for the whole RID request (have no idea what it should
	// contain for now)
	unsigned char rid_req[RID_MAX_PACKET_SIZE];
	int rid_req_len = sizeof(rid_req);

	// XXX: select() parameters, initialized as in xping.c
	int rc = 0;
	int fdmask = 1 << x_sock;
	struct timeval timeout;
	timeout.tv_sec = 0;
	timeout.tv_usec = 100000;

	while(1) {

		// 4.3) check what's going on with x_sock...
		rc = select(32, (fd_set *) &fdmask, 0, 0, &timeout);

		if (rc == 0) {
			// 4.3.1) 'last days of summer: nothing happens'
			continue;

		} else if (rc > 1) {

			// 4.3.2) select() says there's something for us.
			if ((rc = Xrecvfrom(
								x_sock,
								rid_req,
								rid_req_len,
								0,
								(struct sockaddr *) &rid_req_src,
								&rid_req_src_len)) < 0) {

				if (errno == EINTR)
					continue;

				warn("[rid_producer]: Xrecvfrom() error = %d", errno);
					continue;
			}

			say("[rid_producer]: got request from:"\
					"\n\t[SRC_DAG] = %s",
					Graph((sockaddr_x *) &rid_req_src).dag_string().c_str());

			// TODO: the application should also be able to verify other issues
			// e.g. if the requested name in its `original' form, etc. to
			// distinguish BF false positives (FPs). this information should
			// somehow be included in an RID extension header (e.g. like
			// a ContentHeader).
			send_rid_response(
					xcache_handle,
					&rid_req_src,
					rid_req_src_len);

		} else {

			die(-1, "[rid_producer]: select() error = %d", errno);
		}
	}

	// ************************************************************************
	// 5) close everything
	// ************************************************************************
	say("[rid_producer]: this is rid_requester, signing off...");

	Xclose(x_sock);

	free(name);
	free(rid_string);

	exit(rc);
}
