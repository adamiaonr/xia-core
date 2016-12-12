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

// XXX: 0 = 'permanent', i.e. cached objects live forever
#define DEFAULT_CACHE_SLICE_TTL     (unsigned) 0
#define DEFAULT_CACHE_SLICE_SIZE    (unsigned) 200000000

// the name and SID used to contect the producer directly: i.e. 
// using SID forwarding (e.g. via fallback), not via RIDs
#define ORIGIN_SERVER_NAME  "www_s.dgram_producer.aaa.xia"
#define ORIGIN_SERVER_SID   "SID:0f00000000000000000000000000000000008888"

char PRODUCER_AID[4 + (XID_SIZE * 2) + 1];
char PRODUCER_HID[4 + (XID_SIZE * 2) + 1];
char PRODUCER_4ID[4 + (XID_SIZE * 2) + 1];

/*
** write the message to stdout unless in quiet mode
*/
void say(const char * fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

/*
** always write the message to stdout
*/
void warn(const char * fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);
}

/*
** write the message to stdout, and exit the app
*/
void die(int ecode, const char * fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    vfprintf(stdout, fmt, args);
    va_end(args);
    fprintf(stdout, "[rid_producer]: exiting\n");
    exit(ecode);
}

int send_rid_response(
        XcacheHandle * xcache_handle,
        sockaddr_x * rid_req_src,
        socklen_t rid_req_src_len) {

    // what does send_rid_response() do?
    //     1) generate a sensor value
    //    2) send it to rid_req_src using xcache's interfaces

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
                Graph(&cid_value_addr).dag_string().c_str(),
                value_str);
    }

    return rc;
}

int setup_rid_socket(char * rid_string)
{
    int rid_sock = 0;

    if ((rid_sock = Xsocket(AF_XIA, SOCK_DGRAM, 0)) < 0) {
        die(-1, "[rid_producer]: Xsocket(SOCK_DGRAM) error = %d", errno);
    }

    // get the localhost address of the producer (AD:HID components)
    if (XreadLocalHostAddr(
            rid_sock,
            PRODUCER_AID, sizeof(PRODUCER_AID),
            PRODUCER_HID, sizeof(PRODUCER_HID),
            PRODUCER_4ID, sizeof(PRODUCER_4ID)) < 0 ) {

        die(    -1,
                "[rid_producer]: error reading localhost address\n");
    }

    // create a local RID DAG, in the style AD:HID:RID (= rid_string).
    sockaddr_x rid_local_addr;
    socklen_t rid_local_addr_len;

    to_rid_addr(
        rid_string, 
        PRODUCER_AID,
        PRODUCER_HID,
        &rid_local_addr,
        &rid_local_addr_len);

    // 3.3) bind rid_sock to rid_local_addr
    if (Xbind(
            rid_sock,
            (struct sockaddr *) &rid_local_addr,
            rid_local_addr_len) < 0) {

        Xclose(rid_sock);

        die(    -1,
                "[rid_producer]: unable to Xbind() to DAG %s error = %d\n",
                Graph(&rid_local_addr).dag_string().c_str(),
                errno);
    }

    say("[rid_producer]: will listen to RID packets directed at: \n%s\n\n",
        Graph(&rid_local_addr).dag_string().c_str());

    return rid_sock;
}

int setup_sid_socket()
{
    int sid_sock;

    if ((sid_sock = Xsocket(AF_XIA, SOCK_DGRAM, 0)) < 0) {
        die(-1, "[rid_producer::]: Xsocket(SOCK_DGRAM) error = %d", errno);
    }

    // we want to associate PRODUCER_NAME to a DAG with the following 
    // structure:
    //
    //  DAG 2 0 - 
    //  AD:     <local AD> 2 1 - 
    //  HID:    <local HID> 2 - 
    //  SID:    <ORIGIN_SERVER_SID>
    //
    // we do it via Xgetaddrinfo()
    struct addrinfo * local_addr_info;
    if (Xgetaddrinfo(NULL, ORIGIN_SERVER_SID, NULL, &local_addr_info) != 0)
        die(-1, "[rid_producer]: Xgetaddrinfo() error\n");

    sockaddr_x * listen_addr = (sockaddr_x *) local_addr_info->ai_addr;

    // print the generated DAG
    printf("[rid_producer]: binding RID producer to (secondary) DAG:\n%s\n\n", 
        Graph(listen_addr).dag_string().c_str());

    // associate the name PRODUCER_NAME to the DAG address
    if (XregisterName(ORIGIN_SERVER_NAME, listen_addr) < 0 )
        die(-1, "[rid_producer]: error registering name: %s\n", ORIGIN_SERVER_NAME);
    say("[rid_producer]: producer registered name: \n%s\n\n", ORIGIN_SERVER_NAME);

    // finally, bind sid_socket to the generated DAG
    if (Xbind(
            sid_sock,
            (struct sockaddr *) listen_addr,
            sizeof(*listen_addr)) < 0) {

        Xclose(sid_sock);

        die(    -1,
                "[rid_producer]: unable to Xbind() to DAG %s error = %d",
                Graph(listen_addr).dag_string().c_str(),
                errno);
    }

    return sid_sock;
}

int main(int argc, char **argv)
{
    // rid producer listens to RID requests and direct SID queries
    int rid_sock = 0, sid_sock = 0;

    char * name = (char *) calloc(PREFIX_MAX_LENGTH, sizeof(char));
    char * rid_string;

    // similarly to other XIA example apps, print some initial info on the app
    say("\n%s (%s): started\n\n", TITLE, VERSION);

    // 1) gather the application arguments

    // if not enough arguments, stop immediately...
    if (argc < 3) {
        die(-1, "[rid_producer]: usage: rid_producer -n <URL-like (producer) name>\n");
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
                case 'n':
                    // 1.2.1) shift to the argument value, save the URL-like
                    // name in prefix
                    argc--, av++;
                    strncpy(name, av[0], strlen(av[0]));

                    say("[rid_producer]: user supplied prefix %s\n",
                        name);

                    break;

                // 1.3) file with list of URL-like names to generate RIDs
                // FIXME: will be used when binding to multiple RIDs is
                // implemented
                // case 'f':
                //     argc--, av++;
                //     //npackets = atoi(av[0]);
                //     break;
            }
        }

        argc--, av++;
    }

    // 1.4) create RID to be served by this producer
    if (name[0] != '\0') {

        rid_string = name_to_rid(name);

        say("[rid_producer]: created RID %s out of prefix %s\n",
            rid_string,
            name);

    } else {

        // 1.4.1) if no name has been specified, no point going on...
        die(-1, "[rid_producer]: name string is empty\n");
    }

    // 2) initialize the producer's xcache handle
    say("[rid_producer]: initializing xcache handle\n");
    XcacheHandle xcache_handle; 
    XcacheHandleInit(&xcache_handle);
    say("[rid_producer]: done!\n");

    // 3) setup RID and SID sockets
    rid_sock = setup_rid_socket(rid_string);
    sid_sock = setup_sid_socket();

    // 4) select() monitors both rid_sock and sid_sock

    // create variables to hold src and dst addresses of the 
    // RID request. src will be directly set by Xrevfrom(), while dst 
    // is implicit (the x_sock socket should only get requests for the 
    // prefix bound to the socket)
    sockaddr_x rid_req_src, sid_req_src;
    socklen_t rid_req_src_len, sid_req_src_len;

    // a buffer for the whole RID request (have no idea what it should
    // contain for now)
    unsigned char rid_req[RID_MAX_PACKET_SIZE], sid_req[RID_MAX_PACKET_SIZE];
    int rid_req_len = sizeof(rid_req), sid_req_len = sizeof(sid_req);

    // create a file descriptor set
    fd_set socket_fds;
    // max fd required for select() call
    int max_sock = 0;
    if (rid_sock > sid_sock)
        max_sock = rid_sock;
    else
        max_sock = sid_sock;

    // set a small timeout for the select() call (e.g. 500 ms)
    struct timeval timeout;

    int rc = 0;
    while(1) {

        // you need to 're-arm' timeout and socket_fds every time (Xselect() 
        // alters these):

        // check what's up with the sockets
        timeout.tv_sec = 0;
        timeout.tv_usec = 500000;
        // clear socket_fds, then add both rid_sock and sid_sock to it
        FD_ZERO(&socket_fds);
        FD_SET(rid_sock, &socket_fds);
        FD_SET(sid_sock, &socket_fds);

        rc = Xselect(max_sock + 1, &socket_fds, 0, 0, &timeout);

        if (rc == 0) {

            // say("[rid_producer]: Xselect() timeout\n");
            continue;

        } else if (rc > 0) {

            say("[rid_producer]: Xselect() has something...\n");

            // check rid_sock and sid_sock separately
            if (FD_ISSET(rid_sock, &socket_fds)) {

                if ((rc = Xrecvfrom(
                        rid_sock,
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

                say("[rid_producer]: got RID request from: \n%s\n\n",
                        Graph((sockaddr_x *) &rid_req_src).dag_string().c_str());

                send_rid_response(
                        &xcache_handle,
                        &rid_req_src,
                        rid_req_src_len);
            }

            if (FD_ISSET(sid_sock, &socket_fds)) {

                if ((rc = Xrecvfrom(
                        sid_sock,
                        sid_req,
                        sid_req_len,
                        0,
                        (struct sockaddr *) &sid_req_src,
                        &sid_req_src_len)) < 0) {

                    if (errno == EINTR)
                        continue;

                    warn("[rid_producer]: Xrecvfrom() error = %d", errno);
                        continue;
                }

                say("[rid_producer]: got SID request from:"\
                        "\n\t[SRC_DAG] = %s",
                        Graph((sockaddr_x *) &sid_req_src).dag_string().c_str());

                // TODO: the application should also be able to verify other issues
                // e.g. if the requested name in its `original' form, etc. to
                // distinguish BF false positives (FPs). this information should
                // somehow be included in an RID extension header (e.g. like
                // a ContentHeader).
                send_rid_response(
                        &xcache_handle,
                        &sid_req_src,
                        sid_req_src_len);
            }

        } else {

            die(-1, "[rid_producer]: Xselect() rc = %d, error = %d (%s)\n", rc, errno, strerror(errno));
        }
    }

    // ************************************************************************
    // 5) close everything
    // ************************************************************************
    say("[rid_producer]: this is rid_requester, signing off...");

    Xclose(rid_sock);
    Xclose(sid_sock);

    free(name);
    free(rid_string);

    exit(rc);
}
