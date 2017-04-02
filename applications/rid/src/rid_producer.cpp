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
#include <arpa/inet.h>

#include <iostream>
#include <thread>
#include <string>
#include <unordered_map>

#include "Xsocket.h"
#include "dagaddr.hpp"
#include "rid.h"
#include "xcache.h"

#define VERSION "v2.0"
#define TITLE "XIA RID Producer Application"

// the name and SID used to connect to the rid producer 
// directly, e.g. via the fallback
#define ORIGIN_SERVER_NAME  "www_s.rid_producer.xia"
#define ORIGIN_SERVER_SID   "SID:0f00000000000000000000000000000000008888"
// max. size for a DAG (FIXME: does this make sense?)
#define MAX_DAG_SIZE    (int) (4 * (4 + (XID_SIZE * 2) + 1))

#define PRODUCTION_PERIOD   180

// a set of rid suffixes for demo purposes. the producer publishes content 
// under one of the suffixes [1, n] at random. suffix [0] - i.e. "latest" - 
// should always point to the last produced value.
const std::string SUFFIX[] = { "latest", "i202", "i210", "i203", "i205" };

class RID_Record {

    public:

        RID_Record() {}
        RID_Record(
            std::string rid,
            std::string full_name,
            std::string cid,
            sockaddr_x cid_addr) {

            this->rid = rid;
            this->full_name = full_name;
            this->cid = cid;
            this->cid_addr = cid_addr;
        }

        // rid of the record
        std::string rid;
        // the full name under which content has been published
        std::string full_name;
        // the cid associated with this record
        std::string cid;
        // the cid address associated with this record
        sockaddr_x cid_addr;
};

// hash table which keeps cids associated with a specific rid
// FIXME: this may not be the 
std::unordered_map<std::string, std::vector<RID_Record *>> RID_RECORD_TABLE;

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

std::string to_rid_str(std::string full_name) {

    // rids (still) are very C-like...
    char * aux = name_to_rid((char *) full_name.c_str());
    std::string rid_str = std::string(aux);
    free(aux);

    return rid_str;
}

void content_producer(
    std::string prefix,
    XcacheHandle * xcache_handle) {

    int cnt = 3;
    std::string latest_str = std::string("latest");

    do {

        // generate a sensor value, save it in value_str
        char value_str[128] = {'\0'};
        snprintf(
                value_str,
                128,
                "timestamp:%u:temperature:%d:unit:degC",
                (unsigned) time(NULL),
                (rand() % 111 + (-50)));

        // save value_str as cid on xcache, extract the cid
        sockaddr_x cid_addr;
        int rc = 0;
        if ((rc = XputChunk(
            xcache_handle, 
            value_str, strlen(value_str), &cid_addr)) < 0) {

            warn("[rid_producer]: XputChunk() error = %d", errno);

        } else {

            // XputChunk() was successful, review the result
            say("[rid_producer]: 'Xputted' content:"\
                    "\n\t[PAYLOAD (SIZE)] = %s (%d)"\
                    "\n\t[CID DAG] = %s\n",
                    value_str, strlen(value_str),
                    Graph(&cid_addr).dag_string().c_str());

            // pick a random rid under which to publish a new content value_str
            std::string full_name = prefix + "/" + SUFFIX[(rand() % 4) + 1];
            // generate the rid string out of full_name
            // FIXME: why doesn't name_to_rid() return an std::string?
            std::string rid_str = to_rid_str(full_name);

            // create a new rid record
            RID_Record * rid_record_ptr = new RID_Record(
                rid_str,
                full_name, 
                Graph(&cid_addr).get_final_intent().id_string(),
                cid_addr);

            // add it to the record table
            RID_RECORD_TABLE[rid_str].push_back(rid_record_ptr);
            // update the 'latest' record
            RID_RECORD_TABLE[latest_str].push_back(rid_record_ptr);

            // review the added record
            say("[rid_producer]: added RID record to RID_RECORD_TABLE:"\
                    "\n\t[RID] = %s"\
                    "\n\t[FULL NAME] = %s"\
                    "\n\t[CID] = %s\n",
                    RID_RECORD_TABLE[rid_str].back()->rid.c_str(),
                    RID_RECORD_TABLE[rid_str].back()->full_name.c_str(),
                    RID_RECORD_TABLE[rid_str].back()->cid.c_str());   
        }

        // sleep for a bit...
        std::this_thread::sleep_for(std::chrono::seconds(PRODUCTION_PERIOD));

    } while(--cnt > 0);
}

int send_rid_response(
        XcacheHandle * xcache_handle,
        std::string req_rid,
        sockaddr_x * rid_req_src,
        socklen_t rid_req_src_len) {

    // what does send_rid_response() do?
    //  1) fetch cids associated with requested rids
    //  2) send rid header + content to rid_req_src

    // we build the response packet by filling an rid_pckt struct, typecasted
    // over a char * (e.g. as done in other networking applications written 
    // in C) 
    char rid_rsp_raw[2048] = { '\0' };
    struct rid_pckt * rid_rsp_pckt = (struct rid_pckt *) rid_rsp_raw;
    rid_rsp_pckt->type = RID_PCKT_TYPE_REPLY;

    // copy the fields of an rid record associated with req_rid into the 
    // rid_pckt struct. if req_id exists in the table, we extract the 
    // latest record associated with it. if not, we simply use 'latest'.
    auto rid_record_ptr = RID_RECORD_TABLE.find(req_rid);
    sockaddr_x cid_addr;

    // if the record exists, co
    if (rid_record_ptr != RID_RECORD_TABLE.end()) {

        strncpy(rid_rsp_pckt->rid, rid_record_ptr->second.back()->rid.c_str(), RID_STR_SIZE);
        strncpy(rid_rsp_pckt->name, rid_record_ptr->second.back()->full_name.c_str(), PREFIX_MAX_LENGTH);
        strncpy(rid_rsp_pckt->cid, rid_record_ptr->second.back()->cid.c_str(), RID_STR_SIZE);

        cid_addr = rid_record_ptr->second.back()->cid_addr;

    } else {

        strncpy(rid_rsp_pckt->rid, RID_RECORD_TABLE["latest"].back()->rid.c_str(), RID_STR_SIZE);
        strncpy(rid_rsp_pckt->name, RID_RECORD_TABLE["latest"].back()->full_name.c_str(), PREFIX_MAX_LENGTH);
        strncpy(rid_rsp_pckt->cid, RID_RECORD_TABLE["latest"].back()->cid.c_str(), RID_STR_SIZE);

        cid_addr = RID_RECORD_TABLE["latest"].back()->cid_addr;
    }

    // fetch content from associated cid (using cid_addr) from xcache
    char * content_buffer = (char *) calloc(2048, sizeof(char));
    int content_len = 0;
    if ((content_len = XfetchChunk(
        xcache_handle,
        (void **) &content_buffer,
        XCF_BLOCK,
        &cid_addr, sizeof(cid_addr))) < 0) {

        warn("[rid_producer]: XfetchChunk() error = %d", errno);

    } else {

        // content extraction was successful
        say("[rid_producer]: fetched content:"\
                "\n\t[CID DAG] = %s"\
                "\n\t[PAYLOAD (SIZE)] = %s (%d)\n",
                Graph(&cid_addr).dag_string().c_str(),
                content_buffer, content_len);

        say("[rid_producer]: just to make sure:"\
                "\n\t[XID_SIZE] = %d"\
                "\n\t[RID_STR_SIZE] = %d"\
                "\n\t[PREFIX_MAX_LENGTH] = %d"\
                "\n\t[RID_PCKT_HDR_LEN] = %d"\
                "\n\t[sizeof(struct rid_pckt)] = %d\n",
                XID_SIZE, RID_STR_SIZE, PREFIX_MAX_LENGTH, RID_PCKT_HDR_LEN, sizeof(struct rid_pckt));

        // fill the datalen attribute of rid_pckt
        rid_rsp_pckt->datalen = htons((uint16_t) strlen(content_buffer));
        // fill the remaining bytes of rid_rsp_raw with the content
        // associated with the rid
        strncpy(&rid_rsp_raw[sizeof(struct rid_pckt)], content_buffer, 1024);

        // send it back using Xsendto() and an ephemeral SOCK_DGRAM
        int rid_rsp_sock = 0;
        if ((rid_rsp_sock = Xsocket(AF_XIA, SOCK_DGRAM, 0)) < 0) {

            warn("[rid_producer]: Xsocket(SOCK_DGRAM) (RID resp.) error = %d", errno);

        } else {

            int rc = 0;
            if ((rc = Xsendto(
                rid_rsp_sock,
                rid_rsp_raw, sizeof(struct rid_pckt) + content_len,
                0,
                (const sockaddr *) rid_req_src,
                rid_req_src_len)) < 0) {

                warn("[rid_producer]: Xsendto() error = %d", errno);

            } else {

                printf("[rid_producer]: sent RID reply (%d):"\
                        "\n\t[DST_DAG] = %s"\
                        "\n\t[RID] = %s"\
                        "\n\t[NAME] = %s"\
                        "\n\t[CID] = %s"\
                        "\n\t[PAYLOAD (SIZE)] = %s (%d)\n",
                        rc,
                        Graph(rid_req_src).dag_string().c_str(),
                        rid_rsp_pckt->rid,
                        rid_rsp_pckt->name,
                        rid_rsp_pckt->cid,
                        &rid_rsp_raw[sizeof(struct rid_pckt)], ntohs(rid_rsp_pckt->datalen));
            }
        }
    }

    free(content_buffer);
    return content_len;
}

int setup_rid_socket(char * rid_string)
{
    int rid_sock = 0;

    if ((rid_sock = Xsocket(AF_XIA, SOCK_DGRAM, 0)) < 0) {
        die(-1, "[rid_producer]: Xsocket(SOCK_DGRAM) error = %d", errno);
    }

    char localhost_dag_str[MAX_DAG_SIZE] = { 0 };
    char localhost_4id_str[MAX_DAG_SIZE] = { 0 };
    
    if (XreadLocalHostAddr(
            rid_sock,
            localhost_dag_str, MAX_DAG_SIZE,
            localhost_4id_str, MAX_DAG_SIZE) < 0 ) {

        die(    -1,
                "[rid_producer]: error reading localhost address\n");
    }

    say("[rid_producer]: extracted localhost DAG : %s\n",
            localhost_dag_str);

    // create a local RID DAG, in the style AD:HID:RID (= rid_string).
    sockaddr_x rid_local_addr;
    socklen_t rid_local_addr_len;

    to_rid_addr(
        rid_string,
        localhost_dag_str,
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

void process_rid_request(int socket, XcacheHandle * xcache_handle) {

    // create variables to hold src and dst addresses of the 
    // RID request. src will be directly set by Xrevfrom(), while dst 
    // is implicit (the x_sock socket should only get requests for the 
    // prefix bound to the socket)
    sockaddr_x req_src;
    socklen_t req_src_len;

    // a buffer for the whole RID request (have no idea what it should
    // contain for now)
    unsigned char buffer[2048];
    int buffer_len = sizeof(buffer);

    int rc = 0;
    if ((rc = Xrecvfrom(
            socket,
            buffer,
            buffer_len,
            0,
            (struct sockaddr *) &req_src,
            &req_src_len)) < 0) {

        if (errno == EINTR)
            return;

        warn("[rid_producer]: Xrecvfrom() error = %d", errno);
            return;
    }

    // we got something... read it as an rid_pckt struct
    struct rid_pckt * rid_req_pckt = (struct rid_pckt *) buffer;
    say("[rid_producer]: got request from:"\
            "\n\t[SRC DAG] = %s"\
            "\n\t[REQUESTED RID] = %s"\
            "\n\t[REQUESTED NAME] = %s\n",
            Graph((sockaddr_x *) &req_src).dag_string().c_str(),
            rid_req_pckt->rid,
            rid_req_pckt->name);

    send_rid_response(
            xcache_handle,
            std::string(rid_req_pckt->rid),
            &req_src,
            req_src_len);
}

int main(int argc, char **argv)
{
    // rid producer listens to RID requests and direct SID queries
    int rid_sock = 0, sid_sock = 0;
    char * name = (char *) calloc(PREFIX_MAX_LENGTH, sizeof(char));
    char * rid_string;

    // similarly to other XIA example apps, print some initial info on the app
    say("\n%s (%s): started\n\n", TITLE, VERSION);

    // read application arguments
    // if not enough arguments, stop immediately...
    if (argc < 3) {
        die(-1, "[rid_producer]: usage: rid_producer -n <URL-like (producer) name>\n");
    }
    // argc : nr. of arguments
    // argv : double char pointer for argument strings
    char **av = argv;
    // skip the name of the program in the list of arguments 
    // by decrementing argc and incrementing argv
    argc--, av++;
    // while there are arguments and the argument is 
    // an option (i.e. starts with '-'), keep collecting
    while (argc > 0 && *av[0] == '-') {

        int c = argc;
        // skip the '-' in the option name
        while (*++av[0]) {

            if (c != argc)
                break;

            switch (*av[0]) {

                // single URL-like name given as parameter to generate
                // RIDs (e.g. '/cmu/ece/rmcic/2221c/sensor/temperature')
                case 'n':
                    // shift to the option value, save the URL-like name
                    argc--, av++;
                    strncpy(name, av[0], strlen(av[0]));
                    say("[rid_producer]: user supplied prefix %s\n",
                        name);
                    break;
            }
        }

        argc--, av++;
    }

    // FIXME: this extra variable is just for testing purposes, get 'rid' of 
    // names in text form in the future 
    std::string extra_name = std::string(name);

    // create RID to be served by this producer
    if (name[0] != '\0') {

        rid_string = name_to_rid(name);

        say("[rid_producer]: created RID %s out of prefix %s\n",
            rid_string,
            name);

    } else {

        // if no name has been specified, no point going on...
        die(-1, "[rid_producer]: name string is empty. exiting.\n");
    }

    // initialize the producer's xcache handle
    say("[rid_producer]: initializing xcache handle\n");
    XcacheHandle xcache_handle; 
    XcacheHandleInit(&xcache_handle);
    say("[rid_producer]: xcache handle initialization done\n");

    // launch the content producer thread, which periodically
    // creates and saves content into xcache, updating the 
    // rid:cid map
    std::thread producer(content_producer, extra_name, &xcache_handle);

    // setup RID and SID sockets
    rid_sock = setup_rid_socket(rid_string);
    sid_sock = setup_sid_socket();

    // select() monitors both rid_sock and sid_sock

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

                say("[rid_producer]: ...on RID socket\n");
                process_rid_request(rid_sock, &xcache_handle);
            }

            if (FD_ISSET(sid_sock, &socket_fds)) {

                say("[rid_producer]: ...on SID socket");
                process_rid_request(sid_sock, &xcache_handle);
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

    producer.join();

    exit(rc);
}
