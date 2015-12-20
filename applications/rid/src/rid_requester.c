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

#include "rid.h"
#include "xcache.h"

#define VERSION "v0.1"
#define TITLE "XIA RID Requester Application"

#define RID_REQUEST_DEFAULT_AD 	"1000000000000000000000000000000000000002"
#define RID_REQUEST_DEFAULT_HID	"0000000000000000000000000000000000000005"

#define SID_REQUESTER "SID:00000000dd41b924c1001cfa1e1117a812492434"

int main(int argc, char **argv)
{
	// file descriptors for 'SOCK_DGRAM' Xsocket:
	//	-# 'SOCK_DGRAM': send RID request and listen for responses
	int x_sock = 0;

	char * name = (char *) calloc(PREFIX_MAX_LENGTH, sizeof(char));
	char * rid_string;

	// similarly to other XIA example apps, print some initial info on the app
	say("\n%s (%s): started\n", TITLE, VERSION);

	// initialize the requester's xcache handle
	XcacheHandle * xcache_handle = NULL; 
	XcacheHandleInit(xcache_handle);

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
					// name
					argc--, av++;
					strncpy(name, av[0], strlen(av[0]));
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
	// 2) create RID out of the provided name
	// ************************************************************************
	if (name[0] != '\0') {

		rid_string = name_to_rid(name);

	} else {

		// 2.1) if no name has been specified, no point going on...
		die(-1, "[rid_requester]: name string is empty\n");
	}

	// ************************************************************************
	// 3) open a 'SOCK_DGRAM' Xsocket, make it listen on SID_REQUESTER
	// ************************************************************************
	if ((x_sock = Xsocket(AF_XIA, SOCK_DGRAM, 0)) < 0) {
		die(-1, "[rid_requester]: Xsocket(SOCK_DGRAM) error = %d", errno);
	}

	// 3.1) bind the requester to a particular SID_REQUESTER, on which it will
	// listen for RID responses

	// FIXME: will use Xgetaddrinfo(name = NULL, ...) to build
	// AD:HID:SIDx (= SID_REQUESTER) with local AD:HID
	struct addrinfo * local_addr_info;

	if (Xgetaddrinfo(
			NULL, 
			(const char *) SID_REQUESTER, 
			NULL, 
			&local_addr_info) != 0) {

		die(-1, "[rid_requester]: Xgetaddrinfo() error");
	}

	sockaddr_x * listen_addr = (sockaddr_x *) local_addr_info->ai_addr;

	// 3.2) Xbind() x_sock to listen_addr
	if (Xbind(
			x_sock,
			(struct sockaddr *) listen_addr,
			sizeof(*listen_addr)) < 0) {

		Xclose(x_sock);

		die(	-1,
				"[rid_requester]: unable to Xbind() to DAG %s error = %d",
				Graph(listen_addr).dag_string().c_str(),
				errno);
	}

	// 3.3) send the RID request
	int rid_packet_len = 0;
	sockaddr_x * rid_dest_addr = 
			to_rid_addr(
				rid_string, 
				RID_REQUEST_DEFAULT_AD,
				RID_REQUEST_DEFAULT_HID);

	// 3.3.1) TODO: what should be set in the payload? send the full name for 
	// now...
	char * rid_packet_payload; 
	strncpy(rid_packet_payload, name, RID_MAX_PACKET_SIZE);
	rid_packet_len = strlen(rid_packet_payload);

	say("[rid_requester]: sending RID request:"\
			"\n\t[DST_DAG] = %s"\
			"\n\t[PAYLOAD] = %s\n",
			Graph(rid_dest_addr).dag_string().c_str(),
			rid_packet_payload);
	
	int rc = 0;

	rc = Xsendto(
			x_sock,
			rid_packet_payload,
			rid_packet_len,
			0,
			(struct sockaddr *) rid_dest_addr,
			sizeof(*rid_dest_addr));

	// 3.2) check status of RID request
	if (rc < 0 || rc != rid_packet_len) {

		if (rc < 0) {
			die(-1, "[rid_requester]: Xsendto() error = %d", errno);
		}

		printf("[rid_requester]: sent RID request:"\
				"\n\t[DST_DAG] = %s"\
				"\n\t[PAYLOAD] = %s"\
				"\n\t[RETURN_CODE] = %d\n",
				Graph(rid_dest_addr).dag_string().c_str(),
				rid_packet_payload,
				rc);

		fflush(stdout);
	}

	// ************************************************************************
	// 4) listen to responses to the RID request 
	// ************************************************************************

	say("[rid_requester]: will listen to SID packets directed at: %s\n",
			Graph(listen_addr).dag_string().c_str());

	// 4.4) start listening to RID responses
	// FIXME: ... for now, in an endless loop. this is likely to change in the
	// future after i receive some feedback from prs on the RID protocols
	sockaddr_x rid_resp_src;
	socklen_t rid_resp_src_len = sizeof(rid_resp_src);

	// a buffer for the RID response(s)
	unsigned char rid_resp[RID_MAX_PACKET_SIZE];
	int rid_resp_len = sizeof(rid_resp);

	while (1) {

		say("[rid_requester]: listening for new responses...\n");

		int bytes_rcvd = -1;

		// cleanup rid_resp just in case...
		memset(rid_resp, 0, RID_MAX_PACKET_SIZE);

		// 4.4.1) Xrecvfrom() blocks while waiting for something to
		// appear on x_sock
		if ((bytes_rcvd = Xrecvfrom(
								x_sock,
								rid_resp,
								rid_resp_len,
								0,
								(struct sockaddr *) &rid_resp_src,
								&rid_resp_src_len)) < 0) {

			die(-1, "[rid_requester]: Xrecvfrom() error = %d", errno);

		} else {

			say("[rid_requester]: received RID response: "\
					"\n\t[SRC. ADDR]: %s"\
					"\n\t[PAYLOAD]: %s"\
					"\n\t[SIZE]: %d\n",
					Graph(rid_resp_src).dag_string().c_str(),
					rid_resp,
					rid_resp_len);

			// 4.3) use the CID given in the payload to fetch content from the 
			// local cache, using xcache interfaces
			sockaddr_x * cid_resp_addr = to_cid_addr(rid_resp);

			// cleanup rid_resp just in case...
			memset(rid_resp, 0, RID_MAX_PACKET_SIZE);

			// 4.4) use XfetchChunk() to get the CID's content
			if (XfetchChunk(
							xcache_handle, 
							rid_resp, rid_resp_len, 
							0, 
							cid_resp_addr, sizeof(*cid_resp_addr)) < 0) {

				warn("[rid_requester]: XfetchChunk() error = %d", errno);

			} else {

				say("[rid_requester]: contents of included CID: "\
						"\n\t[CID]: %s"\
						"\n\t[CONTENT]: %s"\
						"\n\t[SIZE]: %d\n",
						Graph(cid_resp_addr).dag_string().c_str(),
						rid_resp,
						rid_resp_len);
			}

			free(cid_resp_addr);
		}
	}

	// ************************************************************************
	// 5) close everything
	// ************************************************************************
	say("[rid_requester]: this is rid_requester, signing off...");

	Xclose(x_sock);

	free(name);
	free(rid_string);
	free(rid_dest_addr);

	exit(rc);
}
