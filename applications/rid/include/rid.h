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
#include "bloom.h"

// @RID: the 'type string' for the RID principal type, as defined in /etc/xids
#define XID_TYPE_RID (const char *) "RID"

#define PREFIX_MAX_LENGTH 256
#define PREFIX_MAX_COUNT 32
#define PREFIX_DELIM "/"

// @RID: 'RID:[40 CHARACTER STRING]\0'
#define RID_STR_PREFIX "RID:"
#define RID_STR_SIZE 4 + (XID_SIZE * 2) + 1
#define RID_MAX_PACKET_SIZE 256

void say(const char *fmt, ...);
void warn(const char *fmt, ...);
void die(int ecode, const char * fmt, ...);

void bloomifyPrefix(struct bloom * bloomFilter, char * _prefix);
char * toRIDString(unsigned char * xid);

#endif
