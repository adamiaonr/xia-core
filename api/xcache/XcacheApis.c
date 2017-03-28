/*!
 @file XcacheApis.c
 @brief - content specific APIs
*/
#include "xcache.h"
#include <iostream>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "xcache_cmd.pb.h"
#include "xcache_sock.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include "dagaddr.hpp"

static void (*notif_handlers[XCE_MAX])(XcacheHandle *, int, sockaddr_x *, socklen_t) = {
	NULL,
	NULL,
};

/** Helper functions **/
static int get_connected_socket(void)
{
	int sock;
	struct sockaddr_un xcache_addr;
	char sock_name[512];

	sock = socket(AF_UNIX, SOCK_STREAM, 0);
	if(sock < 0) {
		return -1;
	}

	if(get_xcache_sock_name(sock_name, 512) < 0) {
		Xclose(sock);
		return -1;
	}

	/* Setup xcache's address */
	xcache_addr.sun_family = AF_UNIX;
	strcpy(xcache_addr.sun_path, sock_name);

	if(connect(sock, (struct sockaddr *)&xcache_addr, sizeof(xcache_addr)) < 0) {
		printf("%s:%d error:%s\n", __FILE__, __LINE__, strerror(errno));
		Xclose(sock);
		return -1;
	}

	Xclose(sock);
	return sock;
}

static int send_command(int xcache_sock, xcache_cmd *cmd)
{
	int ret;
	int remaining, sent;
	uint32_t msg_length;
	std::string cmd_on_wire;

	cmd->SerializeToString(&cmd_on_wire);

	remaining = cmd_on_wire.length();

	msg_length = htonl(remaining);
	send(xcache_sock, &msg_length, 4, 0);

	sent = 0;
	do {
		ret = send(xcache_sock, cmd_on_wire.c_str() + sent, remaining, 0);
		if (ret <= 0) {
			break;
		}
		remaining -= ret;
		sent += ret;
	} while(remaining > 0);

	//fprintf(stderr, "%s: Lib sent %d bytes\n", __func__, htonl(msg_length) + 4);

	if (ret < 0 || remaining > 0)
		return -1;
	else
		return htonl(msg_length) + 4;
}

static int read_bytes_to_buffer(int fd, std::string &buffer, int remaining)
{
	int ret;
	char *buf = (char *)malloc(remaining);
	int total = remaining;
	char *p = buf;

	while(remaining > 0) {

		ret = read(fd, p, remaining);
		if(ret <= 0)
			return ret;
		p += ret;
		remaining -= ret;
	}

	// FIXME: there must be a better way of doing this!
	std::string temp(buf, total);
	buffer = temp;
	free(buf);

	return 1;
}

static int get_response_blocking(int xcache_sock, xcache_cmd *cmd)
{
	std::string buffer;
	int ret;
	uint32_t msg_length, remaining;

	if (read(xcache_sock, &msg_length, 4) != 4) {
		fprintf(stderr, "%s: Error\n", __func__);
		return -1;
	}

	remaining = ntohl(msg_length);
	//fprintf(stderr, "Lib received msg of length %d\n", remaining);
	ret = read_bytes_to_buffer(xcache_sock, buffer, remaining);

	if (ret == 0) {
		cmd->set_cmd(xcache_cmd::XCACHE_ERROR);
		return -1;
	}
	cmd->ParseFromString(buffer);

	return 0;
}

/* Xbuf Functions */
int XbufInit(XcacheBuf *xbuf)
{
	xbuf->length = 0;
	xbuf->buf = NULL;

	return 0;
}

int XbufAdd(XcacheBuf *xbuf, void *data, size_t len)
{
	xbuf->buf = realloc(xbuf->buf, xbuf->length + len);

	if(!xbuf->buf)
		return -1;

	memcpy((char *)xbuf->buf + xbuf->length, data, len);
	xbuf->length += len;

	return 0;
}

void XbufFree(XcacheBuf *xbuf)
{
	xbuf->length = 0;
	free(xbuf->buf);
}

int XcacheHandleDestroy(XcacheHandle *h)
{
	xcache_cmd cmd;

	cmd.set_cmd(xcache_cmd::XCACHE_FREE_CONTEXT);
	cmd.set_context_id(h->contextID);
	send_command(h->xcacheSock, &cmd);

	close(h->xcacheSock);
	close(h->notifSock);

	return 0;
}


int XcacheHandleInit(XcacheHandle *h)
{
	xcache_cmd cmd;

	if (!h) {
		return -1;
	}

	h->ttl = 0;
	h->xcacheSock = get_connected_socket();

	if(h->xcacheSock < 0)
		return -1;

	h->notifSock = get_connected_socket();

	if(h->notifSock < 0) {
		close(h->xcacheSock);
		return -1;
	}

	cmd.set_cmd(xcache_cmd::XCACHE_ALLOC_CONTEXT);
	send_command(h->xcacheSock, &cmd);

	if (get_response_blocking(h->xcacheSock, &cmd) >= 0) {
		fprintf(stderr, "Msg type = %d\n", cmd.cmd());
		fprintf(stderr, "Library received context id = %d\n", cmd.context_id());

		h->contextID = cmd.context_id();
	} else {
		fprintf(stderr, "Library get_response_blocking failed\n");
	}

	cmd.set_cmd(xcache_cmd::XCACHE_FLAG_DATASOCKET);
	cmd.set_context_id(h->contextID);
	send_command(h->xcacheSock, &cmd);

	cmd.set_cmd(xcache_cmd::XCACHE_FLAG_NOTIFSOCK);
	cmd.set_context_id(h->contextID);
	send_command(h->notifSock, &cmd);

	return 0;
}

int XcacheHandleSetTtl(XcacheHandle *h, time_t ttl)
{
	int rc = -1;

	if (h) {
		h->ttl = ttl;
		rc = 0;
	}
	return rc;
}

int XevictChunk(XcacheHandle *h, const char *cid)
{
	int rc = xcache_cmd::XCACHE_OK;
	xcache_cmd cmd;

	if (strncasecmp(cid, "cid:", 4) == 0)
		cid += 4;
	if (strlen(cid) != (XID_SIZE * 2)) {
		return xcache_cmd::XCACHE_INVALID_CID;
	}

	cmd.set_cmd(xcache_cmd::XCACHE_EVICT);
	cmd.set_context_id(h->contextID);
	cmd.set_cid(cid);
	printf("evict sending\n");

	if(send_command(h->xcacheSock, &cmd) < 0) {
		fprintf(stderr, "%s: Error in sending command to xcache\n", __func__);
		/* Error in Sending chunk */
		return -1;
	}

	if(get_response_blocking(h->xcacheSock, &cmd) < 0) {
		fprintf(stderr, "Did not get a valid response from xcache\n");
		return -1;
	}

	return rc;
}

static int __XputChunk(XcacheHandle *h, const char *data, size_t length, sockaddr_x *addr, int flags)
{
	xcache_cmd cmd;

	cmd.set_cmd(xcache_cmd::XCACHE_STORE);
	cmd.set_context_id(h->contextID);
	cmd.set_data(data, length);
	cmd.set_flags(flags);
	cmd.set_ttl(h->ttl);

	if(send_command(h->xcacheSock, &cmd) < 0) {
		fprintf(stderr, "%s: Error in sending command to xcache\n", __func__);
		/* Error in Sending chunk */
		return -1;
	}

	if(get_response_blocking(h->xcacheSock, &cmd) < 0) {
		fprintf(stderr, "Did not get a valid response from xcache\n");
		return -1;
	}

	if(cmd.cmd() == xcache_cmd::XCACHE_ERROR) {
		printf("%s received an error from xcache\n", __func__);
		if(cmd.status() == xcache_cmd::XCACHE_ERR_EXISTS) {
			fprintf(stderr, "%s: Error this chunk already exists\n", __func__);
			return xcache_cmd::XCACHE_ERR_EXISTS;
		}
	}

	//fprintf(stderr, "%s: Got a response from server\n", __func__);
	memcpy(addr, cmd.dag().c_str(), cmd.dag().length());

	//Graph g(addr);
	//g.print_graph();

	return xcache_cmd::XCACHE_OK;
}

static inline int __XputDataChunk(XcacheHandle *h, const char *data, size_t length, sockaddr_x *addr)
{
	return __XputChunk(h, data, length, addr, XCF_DATACHUNK);
}

static inline int __XputMetaChunk(XcacheHandle *h, const char *data, size_t length, sockaddr_x *addr)
{
	return __XputChunk(h, data, length, addr, XCF_METACHUNK);
}

int XputChunk(XcacheHandle *h, const char *data, size_t length, sockaddr_x *addr)
{
	return __XputDataChunk(h, data, length, addr);
}

int XputMetaChunk(XcacheHandle *h, sockaddr_x *metachunk, sockaddr_x *addrs, socklen_t addrlen, int count)
{
	sockaddr_x *data = (sockaddr_x *)calloc(count, sizeof(sockaddr_x));
	xcache_cmd cmd;
	int i;

	if(!data)
		return -1;

	for(i = 0; i < count; i++) {
		memcpy(&data[i], &addrs[i], addrlen);
	}

	if (__XputMetaChunk(h, (char *)data, sizeof(sockaddr_x) * count, metachunk) < 0) {
		free(data);
		return -1;
	}

	free(data);

	return 0;
}

int XputFile(XcacheHandle *h, const char *fname, size_t chunkSize, sockaddr_x **addrs)
{
	FILE *fp;
	struct stat fs;
	sockaddr_x *addrlist;
	unsigned numChunks;
	unsigned i;
	int rc;
	int count;
	char *buf;

	if (h == NULL) {
		errno = EFAULT;
		return -1;
	}

	if (fname == NULL) {
		errno = EFAULT;
		return -1;
	}

	if (chunkSize == 0)
		chunkSize =  DEFAULT_CHUNK_SIZE;

	if (stat(fname, &fs) != 0)
		return -1;

	if (!(fp = fopen(fname, "rb")))
		return -1;

	numChunks = fs.st_size / chunkSize;
	if (fs.st_size % chunkSize)
		numChunks ++;

	if (!(addrlist = (sockaddr_x *)calloc(numChunks, sizeof(sockaddr_x)))) {
		fclose(fp);
		return -1;
	}

	if (!(buf = (char*)malloc(chunkSize))) {
		free(addrlist);
		fclose(fp);
		return -1;
	}

	i = 0;
	while (!feof(fp)) {
		if ((count = fread(buf, sizeof(char), chunkSize, fp)) > 0) {
			rc = XputChunk(h, buf, count, &addrlist[i]);
			if(rc < 0) {
				printf("Xputchunk failed in XputFile\n");
				break;
			}
			if(rc == xcache_cmd::XCACHE_ERR_EXISTS) {
				// TODO: does this cause us to not add the chunk to the list of what we send to the other end???
				printf("chunk already exists in the cache\n");
				continue;
			}
			i++;
		}
	}

	rc = i;

	*addrs = addrlist;
	fclose(fp);
	free(buf);

	return rc;
}

int XputBuffer(XcacheHandle *h, const char *data, size_t length, size_t chunkSize, sockaddr_x **addrs)
{
	sockaddr_x *addrlist;
	unsigned numChunks;
	unsigned i;
	int rc;
	char *buf;
	unsigned offset;

	if(h == NULL) {
		errno = EFAULT;
		return -1;
	}

	if(chunkSize == 0)
		chunkSize =  DEFAULT_CHUNK_SIZE;
	else if(chunkSize > XIA_MAXBUF)
		chunkSize = XIA_MAXBUF;

	numChunks = length / chunkSize;
	if(length % chunkSize)
		numChunks ++;

	if(!(addrlist = (sockaddr_x *)calloc(numChunks, sizeof(sockaddr_x)))) {
		return -1;
	}

	if(!(buf = (char*)malloc(chunkSize))) {
		free(addrlist);
		return -1;
	}

	i = 0;
	offset = 0;
#ifndef MIN
#define MIN(__x, __y) ((__x) < (__y) ? (__x) : (__y))
#endif
	while(offset < length) {
		int to_copy = MIN(length - offset, chunkSize);
		memcpy(buf, data + offset, to_copy);
		rc = XputChunk(h, buf, to_copy, &addrlist[i]);
		if(rc < 0)
			break;
		if(rc == xcache_cmd::XCACHE_ERR_EXISTS) {
			continue;
		}
		offset += to_copy;
		i++;
	}

	rc = i;

	*addrs = addrlist;
	free(buf);

	return rc;
}

/**
 * \brief send a datagram style chunk to an address (e.g. HID, SID)
 * 
 * the flow of this call goes like this:
 * 	-# store the data (a single chunk) and get its cid back
 * 	-# create a temp DGRAM socket, send the chunk to dest_addr
 * 
 * \return XCACHE_OK if successful
 */
int XpushChunkto(
		XcacheHandle * h, 
		const char * data, 
		size_t data_len, 
		sockaddr_x * cid_info,
		sockaddr_x * dst_addr, 
		socklen_t dst_addr_len)
{
	int rc = 0;

	// 1) store the chunk in the local cache, using XputChunk. this will 
	// give us back a cid, created during the store operation
	if ((rc = __XputChunk(h, data, data_len, cid_info, XCF_DATACHUNK)) < 0) {

		fprintf(stderr, "%s: error in storing chunk (%d)\n", __func__, rc);

		//return -1;
	}

	// 1.1) if data is too large, send only what we can
	if (data_len > XIA_MAXBUF) {

		fprintf(stderr, 
			"%s: truncating data from size %lu to XIA_MAXBUF %d byte\n", 
			__func__,
			data_len, XIA_MAXBUF);

		data_len = XIA_MAXBUF;
	}

	// 2) 'push' the chunk towards dest_addr
	xcache_cmd cmd;

	// 2.1) the attributes of the xcache command are:
	//	-# cmd code: XCACHE_PUSH
	//	-# context id
	//	-# cid: cid_info (in string form)
	//	-# dest dag: dst_addr, dst_addr_len
	//	-# data
	// 	-# flags
	cmd.set_cmd(xcache_cmd::XCACHE_PUSH);
	cmd.set_context_id(h->contextID);
	// FIXME: we pass cid in string form, since XputChunk does not return 
	// cid_addr_len
	cmd.set_cid(Graph((sockaddr_x *) cid_info).dag_string().c_str());

	// fprintf(stderr, 
	// 	"%s: dst_addr DAG : \n%s\ndst_addr_len : %d\n", 
	// 	__func__,
	// 	Graph(dst_addr).dag_string().c_str(), dst_addr_len);

	cmd.set_dag(dst_addr, dst_addr_len);
	cmd.set_data(data, data_len);
	cmd.set_flags(XCF_DATACHUNK);

	if((rc = send_command(h->xcacheSock, &cmd)) < 0) {

		fprintf(stderr, 
			"%s: Error in sending command to xcache (%d)\n", 
			__func__, 
			rc);

		return -1;
	}

	// FIXME: this isn't entirely correct, as we should confirm the 
	// success of the command via a response.


	// if((rc = get_response_blocking(h->xcacheSock, &cmd)) < 0) {
		
	// 	fprintf(stderr, 
	// 		"%s: Did not get a valid response from xcache (%d)\n", 
	// 		__func__,
	// 		rc);

	// 	return -1;
	// }

	// fprintf(stderr, 
	// 	"%s: Got a valid response from server (%d)\n", 
	// 	__func__, 
	// 	rc);

	return xcache_cmd::XCACHE_OK;
}

inline int XbufPut(XcacheHandle *h, XcacheBuf *xbuf, size_t chunkSize, sockaddr_x **addrs)
{
	return XputBuffer(h, xbuf->buf, xbuf->length, chunkSize, addrs);
}

/* Content Fetching APIs */
static int hopCount = -1;
int XgetPrevFetchHopCount(){
	return hopCount;
}

int XfetchChunk(XcacheHandle *h, void **buf, int flags, sockaddr_x *addr, socklen_t len)
{
	xcache_cmd cmd;

	fprintf(stderr, "Inside %s\n", __func__);

	cmd.set_cmd(xcache_cmd::XCACHE_FETCHCHUNK);
	cmd.set_context_id(h->contextID);
	cmd.set_dag(addr, len);
	cmd.set_flags(flags);

	if(send_command(h->xcacheSock, &cmd) < 0) {
		fprintf(stderr, "Error in sending command to xcache\n");
		/* Error in Sending chunk */
		return -1;
	}
	fprintf(stderr, "Command sent to xcache successfully\n");

	if(flags & XCF_BLOCK) {
		size_t to_copy;

		if (get_response_blocking(h->xcacheSock, &cmd) < 0) {
			fprintf(stderr, "Did not get a valid response from xcache\n");
			return -1;
		}

		if (cmd.cmd() == xcache_cmd::XCACHE_ERROR) {
			fprintf(stderr, "Chunk fetch failed\n");
			return -1;
		}

		to_copy = cmd.data().length();
		*buf = malloc(to_copy);
		memcpy(*buf, cmd.data().c_str(), to_copy);
		hopCount = cmd.hop_count();

		return to_copy;
	}

	return 0;
}

/* Notficiations */
int XregisterNotif(int event, void (*func)(XcacheHandle *, int event, sockaddr_x *addr, socklen_t addrlen))
{
	notif_handlers[event] = func;
	return 0;
}

static void *__notifThread(void *arg)
{
	XcacheHandle *h = (XcacheHandle *)arg;
	uint32_t msg_length;
	int ret;
	xcache_notif notif;

	do {
		std::cout << "Waiting for Notifications .... \n";
		ret = recv(h->notifSock, &msg_length, 4, 0);
		if(ret <= 0) {
			std::cout << "Notif Thread Killed.\n";
			pthread_exit(NULL);
		}

		std::cout << "[LIB] Received notification\n";

		msg_length = ntohl(msg_length);
		std::string buffer("");

		ret = read_bytes_to_buffer(h->notifSock, buffer, msg_length);
		if(ret == 0) {
			std::cout << "Error while receiving.\n";
			continue;
		}
		notif.ParseFromString(buffer);
		notif_handlers[notif.cmd()](h, notif.cmd(), (sockaddr_x *)notif.dag().c_str(), notif.dag().length());

	} while(1);
}

int XlaunchNotifThread(XcacheHandle *h)
{
	pthread_t thread;

	return pthread_create(&thread, NULL, __notifThread, (void *)h);
}

int XreadChunk(XcacheHandle *h, sockaddr_x *addr, socklen_t addrlen, void *buf, size_t buflen, off_t offset)
{
	size_t to_copy;
	xcache_cmd cmd;

	cmd.set_cmd(xcache_cmd::XCACHE_READ);
	cmd.set_context_id(h->contextID);
	cmd.set_dag(addr, addrlen);
	cmd.set_readoffset(offset);
	cmd.set_readlen(buflen);

	if(send_command(h->xcacheSock, &cmd) < 0) {
				fprintf(stderr, "Error in sending command to xcache\n");
		/* Error in Sending chunk */
		return -1;
	}

	if(get_response_blocking(h->xcacheSock, &cmd) < 0) {
		fprintf(stderr, "Did not get a valid response from xcache\n");
		return -1;
	}

	to_copy = MIN(cmd.data().length(), buflen);
	memcpy(buf, cmd.data().c_str(), to_copy);
	fprintf(stderr, "Copying %lu bytes of %lu to buffer\n", to_copy, buflen);

	return to_copy;
}
