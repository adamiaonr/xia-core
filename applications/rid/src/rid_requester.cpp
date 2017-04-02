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

#include <iostream>
#include <thread>
#include <string>
#include <unordered_map>

#include "rid.h"
#include "xcache.h"
#include "dagaddr.hpp"

#define VERSION "v2.0"
#define TITLE "XIA RID Producer Application"

#define RID_REQUEST_DEFAULT_AD     (char *) "1000000000000000000000000000000000000002"
#define RID_REQUEST_DEFAULT_HID    (char *) "0000000000000000000000000000000000000005"
#define SID_REQUESTER       "SID:00000000dd41b924c1001cfa1e1117a812492434"
#define ORIGIN_SERVER_NAME  "www_s.rid_producer.xia"

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
    fprintf(stdout, "[rid]: exiting\n");
    exit(ecode);
}

int setup_sid_sock() {

    int x_sock = 0;

    if ((x_sock = Xsocket(AF_XIA, SOCK_DGRAM, 0)) < 0) {
        die(-1, "[rid_requester]: Xsocket(SOCK_DGRAM) error = %d", errno);
    }

    // FIXME (1): will use Xgetaddrinfo(name = NULL, ...) to build
    // AD:HID:SIDx (= SID_REQUESTER) with local AD:HID
    // FIXME (2, several months afterwards): why do we do this???
    struct addrinfo * local_addr_info;

    if (Xgetaddrinfo(
            NULL, 
            (const char *) SID_REQUESTER, 
            NULL, 
            &local_addr_info) != 0) {

        die(-1, "[rid_requester]: Xgetaddrinfo() error");
    }

    sockaddr_x * listen_addr = (sockaddr_x *) local_addr_info->ai_addr;

    // bind the socket to the local DAG
    if (Xbind(
            x_sock,
            (struct sockaddr *) listen_addr,
            sizeof(*listen_addr)) < 0) {

        Xclose(x_sock);

        die(    -1,
                "[rid_requester]: unable to Xbind() to DAG %s error = %d",
                Graph(listen_addr).dag_string().c_str(),
                errno);
    }

    say("[rid_requester]: will listen to SID packets directed at: \n%s\n\n",
            Graph(listen_addr).dag_string().c_str());

    return x_sock;
}

int feth_remote_dag(const char * origin_server_name, Graph & remote_dag) {

    struct addrinfo * remote_address_info;
    sockaddr_x * remote_socket_address;

    if (Xgetaddrinfo(
        origin_server_name, 
        NULL, 
        NULL, 
        &remote_address_info) != 0) {

        say("[rid_requester] unable to lookup name %s\n", origin_server_name);

        return -1;
    }

    remote_socket_address = (sockaddr_x *) remote_address_info->ai_addr;

    remote_dag = Graph(remote_socket_address);
    say("[rid_requester] remote DAG for name %s is:\n%s\n\n", 
        origin_server_name, remote_dag.dag_string().c_str());

    return 0;
}

int main(int argc, char **argv)
{
    // file descriptors for 'SOCK_DGRAM' Xsocket:
    //    -# 'SOCK_DGRAM': send RID request and listen for responses
    int x_sock = 0;

    char * name = (char *) calloc(PREFIX_MAX_LENGTH, sizeof(char));
    char * rid_string;

    // similarly to other XIA example apps, print some initial info on the app
    say("\n%s (%s): started\n", TITLE, VERSION);

    // gather the application arguments
    // if not enough arguments, stop immediately...
    if (argc < 3) {
        die(-1, "usage: rid_requester -n <URL-like name>\n");
    }

    char **av = argv;
    argc--, av++;

    while (argc > 0 && *av[0] == '-') {

        int c = argc;

        while (*++av[0]) {

            if (c != argc)
                break;

            switch (*av[0]) {

                // single URL-like name given as parameter to generate
                // RIDs (e.g. uof/cs/r9x/r9001/)
                case 'n':
                    // 1.1.1) shift to the argument value, save the url-like
                    // name
                    argc--, av++;
                    strncpy(name, av[0], strlen(av[0]));
                    break;
            }
        }

        argc--, av++;
    }

    // FIXME: this extra variable is just for testing purposes, get 'rid' of 
    // names in text form in the future 
    std::string extra_name = std::string(name);

    // create RID out of the provided name
    if (name[0] != '\0') {

        rid_string = name_to_rid(name);
        say("[rid_requester]: created RID %s out of name %s\n",
            rid_string,
            name);

    } else {

        // if no name has been specified, no point going on...
        die(-1, "[rid_requester]: name string is empty\n");
    }

    // // initialize the requester's xcache handle
    // say("[rid_requester]: initializing xcache handle...\n");
    // XcacheHandle xcache_handle; 
    // XcacheHandleInit(&xcache_handle);
    // say("[rid_requester]: ... done!\n");

    // open a 'SOCK_DGRAM' Xsocket, make it listen on SID_REQUESTER, on 
    // which it will listen for RID responses
    x_sock = setup_sid_sock();

    // build the rid dag, which will be used to obtain a sockaddr_x struct 
    // holding the destination address of the rid request packet
    sockaddr_x rid_dest_addr;
    socklen_t rid_dest_addr_len;

    // we want to generate and rid dag like this:
    //
    // direct   :    (SRC) -----------------------------> (RID)
    // fallback :      \--> (AD_origin) --> (HID_origin) -->/
    // 
    // to do so, we fetch the dag of the remote origin server, based on 
    // a name that we assume to know previously. note that the goal of 
    // the rid request is to be served by any router in-between which holds 
    // content (i.e. cids) associated with the rid, and not to go all the way 
    // to the origin server
    Graph remote_dag;
    if (feth_remote_dag(ORIGIN_SERVER_NAME, remote_dag) < 0) {

        Xclose(x_sock);
        free(name);
        free(rid_string);

        die(-1, "[rid_requester]: could not fetch remote dag. exiting.\n");
    }

    // replace the final intent of remote_dag w/ an rid node and add a 
    // fallback address
    to_rid_addr(
        rid_string, 
        (char *) remote_dag.dag_string().c_str(),
        &rid_dest_addr,
        &rid_dest_addr_len);

    // build the rid request packet, using the rid_pckt struct
    char rid_req_raw[2048] = { '\0' };
    struct rid_pckt * rid_req_pckt = (struct rid_pckt *) rid_req_raw;

    // set rid_req_pckt to the request type, set the rid field and name, with 
    // empty cid and payload
    rid_req_pckt->type = RID_PCKT_TYPE_REQUEST;
    strncpy(rid_req_pckt->rid, rid_string, RID_STR_SIZE);
    strncpy(rid_req_pckt->name, extra_name.c_str(), PREFIX_MAX_LENGTH);
    rid_req_pckt->datalen = 0;

    say("[rid_requester]: sending RID request:"\
            "\n\t[DST_DAG] = %s"\
            "\n\t[RID] = %s"\
            "\n\t[NAME] = %s\n",
            Graph(&rid_dest_addr).dag_string().c_str(),
            rid_req_pckt->rid,
            rid_req_pckt->name);
    
    // finally, send the rid request packet, towards the 
    // rid destination address build above
    int rc = 0;
    rc = Xsendto(
            x_sock,
            rid_req_pckt, RID_PCKT_HDR_LEN,
            0,
            (struct sockaddr *) &rid_dest_addr,
            rid_dest_addr_len);

    // check status of RID request
    if (rc < 0) {
        die(-1, "[rid_requester]: Xsendto() error = %d", errno);
    }

    printf("[rid_requester]: sent RID request:"\
            "\n\t[DST_DAG] = %s"\
            "\n\t[RETURN_CODE] = %d\n",
            Graph(&rid_dest_addr).dag_string().c_str(),
            rc);

    fflush(stdout);

    // listen to responses to the RID request 
    // FIXME: ... for now, in an endless loop. this is likely to change in the
    // future after i receive some feedback from prs on the rid protocols
    sockaddr_x rid_resp_src;
    socklen_t rid_resp_src_len = sizeof(sockaddr_x);

    // a buffer for the RID response(s)
    unsigned char rid_resp[2048]; 
    int rid_resp_len = sizeof(rid_resp);

    while (1) {

        say("[rid_requester]: listening for new responses...\n");

        int bytes_rcvd = -1;
        // cleanup rid_resp just in case...
        memset(rid_resp, 0, 2048);

        // Xrecvfrom() blocks while waiting for something to
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

            // we got something... read it as an rid_pckt struct
            struct rid_pckt * rid_rsp_pckt = (struct rid_pckt *) rid_resp;
            say("[rid_requester]: received RID response: "\
                    "\n\t[SRC. ADDR]: %s"\
                    "\n\t[RID]: %s"\
                    "\n\t[NAME]: %s"\
                    "\n\t[CID]: %s"\
                    "\n\t[PAYLOAD (SIZE)]: %s %d\n",
                    Graph(&rid_resp_src).dag_string().c_str(),
                    rid_rsp_pckt->rid,
                    rid_rsp_pckt->name,
                    rid_rsp_pckt->cid,
                    rid_rsp_pckt->data, rid_rsp_pckt->datalen);
        }
    }

    // ************************************************************************
    // 5) close everything
    // ************************************************************************
    say("[rid_requester]: this is rid_requester, signing off...");

    Xclose(x_sock);
    free(name);
    free(rid_string);

    exit(rc);
}
