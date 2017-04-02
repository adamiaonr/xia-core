#ifndef _RID_H
#define _RID_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/param.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/file.h>

#include <netinet/in_systm.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ip_icmp.h>
#include <netdb.h>

#include <string.h>
#include <signal.h>

#include "xia.h"
#include "Xsocket.h"

// @RID: the type string for the RID principal type, as defined in /etc/xids
#define XID_TYPE_RID (const char *) "RID"

#define PREFIX_MAX_LENGTH 256
#define PREFIX_MAX_COUNT 32
#define PREFIX_DELIM "/"

// @RID: 'RID:[40 CHARACTER STRING]\0'
#define RID_STR_PREFIX      "RID:"
#define RID_STR_SIZE        (int) 4 + (2 * XID_SIZE) + 1
#define RID_MAX_PACKET_SIZE (int) 256

#define RID_PCKT_TYPE_REQUEST   0x01
#define RID_PCKT_TYPE_REPLY     0x10

struct rid_pckt {
    
    // rid request or response
    uint8_t type;
    // the requested rid OR rid in response
    char rid[RID_STR_SIZE];
    // the full name (in text form) used in request or reply.
    // FIXME : you may ask 'why do we waste bytes sending the prefix, 
    // when the whole purpose was to avoid names of variable and unbounded 
    // lenght?'. i've included this field for testing purposes, but its use 
    // may be deprecated or made optional in the future
    char name[PREFIX_MAX_LENGTH];
    // the CID associated with the payload in the packet
    char cid[RID_STR_SIZE];
    // length of the content associated with the rid (transmitted 
    // after struct rid_pckt)
    uint16_t datalen;

} __attribute__((packed));

extern char * name_to_rid(char * name);
extern char * to_rid_string(unsigned char * bf);
extern void to_rid_addr(
    char * rid_str,
    char * dag_str,
    sockaddr_x * rid_addr,
    socklen_t * rid_addr_len);
extern void to_cid_addr(
    char * cid_string,
    sockaddr_x * cid_addr,
    socklen_t * cid_addr_len);

#endif
