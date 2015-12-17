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

#define VERSION "v0.1"
#define TITLE "XIA RID Producer Application"

// XXX: 0 = 'permanent', i.e. cached objects live forever (just like diamonds)
#define DEFAULT_CACHE_SLICE_TTL		(unsigned) 0
#define DEFAULT_CACHE_SLICE_SIZE	(unsigned) 200000000

char PRODUCER_AID[4 + (XID_SIZE * 2) + 1];
char PRODUCER_HID[4 + (XID_SIZE * 2) + 1];
char PRODUCER_4ID[4 + (XID_SIZE * 2) + 1];

int sendRIDResponse(
		ChunkContext * ctx,
		struct sockaddr * ridReqSrc,
		socklen_t ridReqSrcLen) {

	// what does sendRIDResponse() do?
	// 	1) generate a sensor value
	//	2) send it to ridReqSrc using XpushChunk (which also saves the
	// 		resulting chunk in the local CID cache)

	int rc = 0;

	// generate a sensor value, save it in valueStr according to a some
	// format
	char valueStr[128] = {'\0'};
	snprintf(
			valueStr,
			128,
			"timestamp:%u:temperature:%d:unit:degC",
			(unsigned) time(NULL),
			(rand() % 111 + (-50)));

	// send it over to ridReqSrc using XpushChunkto
	ChunkInfo * toCache = (ChunkInfo *) calloc(1, DEFAULT_CHUNK_SIZE);

	if ((rc = XpushChunkto(
			ctx,
			valueStr,
			strlen(valueStr),
			0,	// XXX: not currently used by XpushChunkto
			(struct sockaddr *) &ridReqSrc,
			ridReqSrcLen,
			toCache)) < 0) {

		warn("[rid_producer]: XpushChunkto() error = %d", errno);

	} else {

		say("[rid_producer]: 'pushed' RID response:"\
				"\n\t[DST_DAG] = %s"\
				"\n\t[PAYLOAD] = %s"\
				"\n\t[CID] = %s"\
				"\n\t[TTL] = %ld"\
				"\n\t[TIMESTAMP] = %ld\n",
				Graph((sockaddr_x *) ridReqSrc).dag_string().c_str(),
				valueStr,
				toCache->cid,
				toCache->ttl,
				(long int) (toCache->timestamp.tv_sec));
	}

	// don't need the ChunkInfo anymore...
	// FIXME: ... i think.
	free(toCache);

	return rc;
}

int main(int argc, char **argv)
{
	// file descriptors for 'SOCK_DGRAM' and 'XSOCK_CHUNK' Xsockets:
	//
	//	-# ('SOCK_DGRAM':xRIDListenSock): Xbind()s to a single RID associated
	//		with the values produced by this app. E.g. say
	//		this app produces temperature values under the general
	//		URL-like name = '/cmu/ece/rmcic/2221c/sensor/temperature/'. the app
	//		will create an 'SOCK_DGRAM' Xsocket and Xbind() it to a DAG
	//		of the style AD:HID:RID_app, in which
	//		RID_app = toRIDString(bloomifyPrefix(name)).
	//		FIXME: for now we use a single RID, multiple RIDs should be
	//		used in the future...
	//
	//	-# ('XSOCK_CHUNK':xCIDPushSock): used to 'push' CID packets to the
	//		network, as a response to RID packets arriving at xRIDListenSock,
	//		listening at AD:HID:RID_app. the responses should be directed
	//		to the source address of RID requests, and sent via XpushChunkTo().
	//		XXX: to use XpushSock() one must initialize a ChunkContext * via
	//		XallocCacheSlice(), which takes care of the creation of a
	// 		XSOCK_CHUNK socket. so no need to explicitly create xCIDPushSock.
	int xRIDListenSock = 0;
	//int xCIDPushSock = 0;

	char * name = (char *) calloc(PREFIX_MAX_LENGTH, sizeof(char));
	char * ridString = (char *) calloc(PREFIX_MAX_LENGTH, sizeof(char));

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
	// 2) create RID for listening
	// ************************************************************************

	// 2.1) generate the RID as a bloom filter (BF) out of the given name.
	// resulting 20 byte array (unsigned char *) is obtained via ridBloom.bf[].
	struct bloom ridBloom;

	if (name[0] != '\0') {

		bloomifyPrefix(&ridBloom, name);

	} else {
		// 2.1.1) if no prefix has been specified, no point going on...
		die(-1, "[rid_producer]: name string is empty");
	}

	// 2.2) transform byte array into string w/ format "RID:[40 char]"
	// FIXME: this RID will later be used as the SID arg in Xbind()
	ridString = toRIDString(ridBloom.bf);

	// ************************************************************************
	// 3) create xRIDListenSock
	// ************************************************************************

	// 3.1) xRIDListenSock
	if ((xRIDListenSock = Xsocket(AF_XIA, SOCK_DGRAM, 0)) < 0) {
		die(-1, "[rid_producer]: Xsocket(SOCK_DGRAM) error = %d", errno);
	}

	// 3.2) get the localhost address of the producer (AD:HID components)
    if (XreadLocalHostAddr(
    		xRIDListenSock,
			PRODUCER_AID, sizeof(PRODUCER_AID),
			PRODUCER_HID, sizeof(PRODUCER_HID),
			PRODUCER_4ID, sizeof(PRODUCER_4ID)) < 0 ) {

		die(	-1,
				"[rid_producer]: reading localhost address\n");
    }

	// 3.2) create a local RID DAG, in the style AD:HID:RID (= ridString).
	Node n_src;
	Node n_ad(Node::XID_TYPE_AD, PRODUCER_AID);
	Node n_hid(Node::XID_TYPE_HID, PRODUCER_HID);
	Node n_rid(XID_TYPE_RID, ridString);

	Graph finalGraph = n_src * n_ad * n_hid * n_rid;

	// 3.3) create a sockaddr_x out of the RID DAG
	sockaddr_x * listenRIDAddr = (sockaddr_x *) malloc(sizeof(sockaddr_x));
	finalGraph.fill_sockaddr(listenRIDAddr);

	// 3.4) bind xRIDListenSock to listenRIDAddr
	if (Xbind(
			xRIDListenSock,
			(struct sockaddr *) listenRIDAddr,
			sizeof(listenRIDAddr)) < 0) {

		Xclose(xRIDListenSock);

		die(	-1,
				"[rid_producer]: unable to Xbind() to DAG %s error = %d",
				Graph(listenRIDAddr).dag_string().c_str(),
				errno);
	}

	say("[rid_producer]: will listen to RID packets directed at: %s",
			Graph(listenRIDAddr).dag_string().c_str());

	//*************************************************************************
	// 4) launch RID request listener
	//*************************************************************************

	// 4.1) initialize a 'ChunkContext', whatever
	// that is... which in its turn initializes an XSOCK_CHUNK socket by
	// itself
	// FIXME: values picked up from applications/multicast/multicast_source.c:
	//	-# policy: POLICY_FIFO | POLICY_REMOVE_ON_EXIT (exactly what it sounds)
	//	-# ttl: 0, means permanent caching
	//	-# size: max. size for the cache slice (size in what units? bananas?)
	ChunkContext * xCIDPushContext = XallocCacheSlice(
										POLICY_FIFO | POLICY_REMOVE_ON_EXIT,
										DEFAULT_CACHE_SLICE_TTL,
										DEFAULT_CACHE_SLICE_SIZE);

	if (xCIDPushContext == NULL) {
		die(-1, "[rid_producer]: unable to initialize the 'cache slice'");
	}

	// 4.2) start listening to RID requests...
	// FIXME: ... for now, in an endless loop (with select())

	// 4.2.1) create variables to hold src and dst addresses of the RID request.
	// src will be directly set by Xrevfrom(), while dst is implicit (the
	// xRIDListenSock socket should only get requests for the prefix bound
	// to the socket)
	sockaddr_x ridReqSrc;
	socklen_t ridReqSrcLen = sizeof (ridReqSrc);

	// 4.2.2) a buffer for the whole RID request (have no idea what it should
	// contain for now)
	unsigned char ridReq[RID_MAX_PACKET_SIZE];
	int ridReqLen = sizeof (ridReq);

	int rc = 0;

	// XXX: select() parameters, initialized as in xping.c
	int fdmask = 1 << xRIDListenSock;

	struct timeval timeout;
	timeout.tv_sec = 0;
	timeout.tv_usec = 100000;

	while(1) {

		// 4.3) check what's going on with xRIDListenSock...
		rc = select(32, (fd_set *) &fdmask, 0, 0, &timeout);

		if (rc == 0) {
			// 4.3.1) 'last days of summer: nothing happens'
			continue;

		} else if (rc > 1) {

			// 4.3.2) select() says there's something for us.
			if ((rc = Xrecvfrom(
								xRIDListenSock,
								ridReq,
								ridReqLen,
								0,
								(struct sockaddr *) &ridReqSrc,
								&ridReqSrcLen)) < 0) {

				if (errno == EINTR)
					continue;

				warn("[rid_producer]: Xrecvfrom() error = %d", errno);
					continue;
			}

			// 4.3.3) we assume that the packet has been verified as an RID
			// packet, and that the RID carried by the dst DAG can be served
			// by this endpoint.
			say("[rid_producer]: got request from:"\
					"\n\t[SRC_DAG] = %s",
					Graph((sockaddr_x *) &ridReqSrc).dag_string().c_str());

			// TODO: the application should also be able to verify other issues
			// e.g. if the requested name in its `original' form, etc. to
			// distinguish BF false positives (FPs). this information should
			// somehow be included in an RID extension header (e.g. like
			// a ContentHeader).
			sendRIDResponse(
					xCIDPushContext,
					(struct sockaddr *) &ridReqSrc,
					ridReqSrcLen);

		} else {
			die(-1, "[rid_producer]: select() error = %d", errno);
		}
	}

	// ************************************************************************
	// 5) this is rid_requester, signing off...
	// ************************************************************************
	say("[rid_producer]: shutting down");

	XfreeCacheSlice(xCIDPushContext);
	Xclose(xRIDListenSock);

	free(name);
	free(ridString);

	exit(rc);
}
