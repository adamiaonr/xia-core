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

#include "Xsocket.h"
#include "dagaddr.hpp"
#include "rid.h"

#define VERSION "v0.1"
#define TITLE "XIA RID Requester Application"

#define RID_REQUEST_DEFAULT_AD 	"1000000000000000000000000000000000000002"
#define RID_REQUEST_DEFAULT_HID	"0000000000000000000000000000000000000005"

#define SID_REQUESTER "SID:00000000dd41b924c1001cfa1e1117a812492434"

int main(int argc, char **argv)
{
	// file descriptors for 'SOCK_DGRAM' and 'XSOCK_CHUNK' Xsockets:
	//	-# 'SOCK_DGRAM': send RID request
	//	-# 'XSOCK_CHUNK': listens to CID responses directed at a particular
	//			SID, explicitly set via XbindPush()
	int xSock = 0, xCIDListenSock = 0;

	char * prefix = (char *) calloc(PREFIX_MAX_LENGTH, sizeof(char));
	char * ridString = (char *) calloc(PREFIX_MAX_LENGTH, sizeof(char));

	// similarly to other XIA example apps, print some initial info on the app
	say("\n%s (%s): started\n", TITLE, VERSION);

	// ************************************************************************
	// 1) gather the application arguments
	// ************************************************************************

	// 1.1) if not enough arguments, stop immediately...
	if (argc < 3) {
		die(-1, "usage: rid_requester -u <URL-like name>\n");
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
            	// RIDs (e.g. uof/cs/r9x/r9001/)
				case 'u':
					// 1.1.1) shift to the argument value, save the url-like
					// name in prefix
					argc--, av++;
					strncpy(prefix, av[0], strlen(av[0]));
					break;

				// 1.3) file with list of URL-like names to generate RIDs for
				// request
				case 'f':
					argc--, av++;
					//npackets = atoi(av[0]);
					break;
            }
        }

		argc--, av++;
	}

	// ************************************************************************
	// 2) create XID and DAG(s) for request (and later, response)
	// ************************************************************************

	// 2.1) generate XID as a bloom filter (BF) out of the given prefix.
	// resulting 20 byte array (unsigned char *) is obtained via xid.bf[].
	struct bloom ridBloom;

	if (prefix[0] != '\0') {

		bloomifyPrefix(&ridBloom, prefix);

	} else {

		// 2.1.1) if no prefix has been specified, no point going on...
		die(-1, "[rid_requester]: prefix string is empty\n");
	}

	// 2.2) transform byte array into string w/ format "RID:[40 char]"
	// FIXME: this RID will later be used as the SID arg in XbindPush()
	ridString = toRIDString(ridBloom.bf);

	// 2.3) generate the DAG Graph, which will be composed by 4 nodes, like
	// this:
	// direct graph:    (SRC) ---------------> (RID)
	// fallback graph:    |--> (AD) --> (HID) -->|
	Node n_src;

	// 2.3.1) create a DAG node of type RID
	// FIXME: we make use of a special constructor in dagaddr.cpp which
	// takes in a pair (string type_str, string id_str) as argument and uses the
	// Node::construct_from_strings() method: if type_str is not a built-in type,
	// it will look for it the the user defined types set in /etc/xids, so
	// this should work...
	Node n_rid(XID_TYPE_RID, ridString);

	// 2.3.2) 'fallback' nodes
	// TODO: make this an input argument AND/OR retrieve it from some sort of
	// RID directory service
	Node n_ad(Node::XID_TYPE_AD, RID_REQUEST_DEFAULT_AD);
	Node n_hid(Node::XID_TYPE_HID, RID_REQUEST_DEFAULT_HID);

	// 2.4) create the final DAG and a sockaddr_x * struct which can be used
	// in an XSocket API call

	// 2.4.1) build direct and final graphs as shown in 2.3)
	Graph directGraph = n_src * n_rid;
	Graph fallbackGraph = n_src * n_ad * n_hid * n_rid;

	// 2.4.2) use Graph '+' operator to build final version of the Graph
	Graph finalGraph = directGraph + fallbackGraph;

	// 2.4.3) finally, get the sockaddr_x * struct
	sockaddr_x * ridSockAddr = (sockaddr_x *) malloc(sizeof(sockaddr_x));
	finalGraph.fill_sockaddr(ridSockAddr);

	// ************************************************************************
	// 3) open a 'SOCK_DGRAM' Xsocket, send RID request over it using XSendto
	// ************************************************************************
	if ((xSock = Xsocket(AF_XIA, SOCK_DGRAM, 0)) < 0) {
		die(-1, "[rid_requester]: Xsocket(SOCK_DGRAM) error = %d", errno);
	}

	// 3.1) bind the requester to a particular SID_REQUESTER, on which it will
	// listen for

	// 3.1.1) generate a local DAG (and Graph, for printing purposes) in the
	// style AD:HID:RID_app

	// FIXME: will use Xgetaddrinfo(name = NULL, ...) to build
	// AD:HID:SIDx (= SID_REQUESTER) with local AD:HID
	struct addrinfo * localAddrInfo;
	if (Xgetaddrinfo(NULL, (const char *) SID_REQUESTER, NULL, &localAddrInfo) != 0) {
		die(-1, "[rid_requester]: Xgetaddrinfo() error");
	}

	sockaddr_x * listenAddr = (sockaddr_x *) localAddrInfo->ai_addr;

	// 3.1.2) Xbind() xSock to the listenDAG
	if (Xbind(
			xSock,
			(struct sockaddr *) listenAddr,
			sizeof(listenAddr)) < 0) {

		Xclose(xSock);

		die(	-1,
				"[rid_requester]: unable to Xbind() to DAG %s error = %d",
				Graph(listenAddr).dag_string().c_str(),
				errno);
	}

	// 3.2) bombs away...
	// TODO: what should be set in the void * buf arg? send gibberish for
	// now...
	char * ridPacketPayload; strncpy(ridPacketPayload, prefix, RID_MAX_PACKET_SIZE);
	int rc = 0, ridPacketLength = strlen(ridPacketPayload);

	say("[rid_requester]: sending RID request:"\
			"\n\t[DST_DAG] = %s"\
			"\n\t[PAYLOAD] = %s\n",
			finalGraph.dag_string().c_str(),
			ridPacketPayload);

	rc = Xsendto(
			xSock,
			ridPacketPayload,
			ridPacketLength,
			0,
			(struct sockaddr *) &ridSockAddr,
			sizeof(sockaddr_x));

	// 3.3) check status of RID request
	if (rc < 0 || rc != ridPacketLength) {

		if (rc < 0) {
			die(-1, "[rid_requester]: Xsendto() error = %d", errno);
		}

		printf("[rid_requester]: sent RID request:"\
				"\n\t[DST_DAG] = %s"\
				"\n\t[PAYLOAD] = %s"\
				"\n\t[RETURN_CODE] = %d\n",
				finalGraph.dag_string().c_str(),
				ridPacketPayload,
				rc);

		fflush(stdout);
	}

	// ************************************************************************
	// 4) listen to CID responses via XbindPush(), using RID as the explicit
	//		SIDx on which to listen...
	// ************************************************************************

	// 4.1) create Xsocket of type 'XSOCK_CHUNK'
	if ((xCIDListenSock = Xsocket(AF_XIA, XSOCK_CHUNK, 0)) < 0) {
		die(-1, "[rid_requester]: Xsocket(XSOCK_CHUNK) error = %d", errno);
	}

//	// 4.2) call XbindPush() to start listening to CID responses
//	// on SID = ridString
//	// NOTE: the SID we're listening on should actually be a 'full-blown' DAG
//	// like AD:HID:SIDx (= RID)
//
//	// FIXME: will use Xgetaddrinfo(name = NULL, ...) to build
//	// AD:HID:SIDx (= SID_REQUESTER) with local AD:HID
//	struct addrinfo * localAddrInfo;
//	if (Xgetaddrinfo(NULL, (const char *) SID_REQUESTER, NULL, &localAddrInfo) != 0) {
//
//		Xclose(xSock);
//		Xclose(xCIDListenSock);
//
//		die(-1, "[rid_requester]: Xgetaddrinfo() error = %d", errno);
//	}
//
//	sockaddr_x * listenCIDDag = (sockaddr_x *) localAddrInfo->ai_addr;
//	Graph listenCIDGraph = Graph(listenCIDDag);

	// 4.3) the actual XbindPush() call
	if (XbindPush(
			xCIDListenSock,
			(struct sockaddr *) listenAddr,
			sizeof(listenAddr)) < 0) {

		Xclose(xSock);
		Xclose(xCIDListenSock);

		die(	-1,
				"[rid_requester]: unable to Xbind() to DAG %s error = %d",
				Graph(listenAddr).dag_string().c_str(),
				errno);
	}

	say("[rid_requester]: will listen to CID packets directed at: %s\n",
			Graph(listenAddr).dag_string().c_str());

	// 4.4) start listening to CID responses
	// FIXME: ... for now, in an endless loop. this is likely to change in the
	// future after i receive some feedback from prs on the RID protocols
	while (1) {

		char chunkBuf[XIA_MAXBUF];
		ChunkInfo * info = (ChunkInfo *) malloc(sizeof(ChunkInfo));
		memset(chunkBuf, 0, sizeof(chunkBuf));

		say("[rid_requester]: listening for new chunks...\n");

		int nBytesRcv = -1;

		// 4.4.1) XrecvChunkfrom() blocks while waiting for something to
		// appear on xCIDListenSock
		if ((nBytesRcv = XrecvChunkfrom(
										xCIDListenSock,
										chunkBuf,
										sizeof(chunkBuf),
										0,
										info)) < 0) {

			die(-1, "[rid_requester]: XrecvChunkfrom() error = %d", errno);

		} else {

			say("[rid_requester]: received chunk "\
					"\n\t[CID]: %s"\
					"\n\t[SIZE]: %d\n"\
					"\n\t[TIMESTAMP]: %ld\n"\
					"\n\t[CONTENT]: %s\n",
					info->cid,
					info->size,
					info->timestamp.tv_sec,
					chunkBuf);
		}
	}

	// ************************************************************************
	// 5) this is rid_requester, signing off...
	// ************************************************************************
	say("[rid_requester]: shutting down");

	Xclose(xSock);
	Xclose(xCIDListenSock);

	exit(rc);
}
