/*
Copyright (c) 2009-2013 Roger Light <roger@atchoo.org>
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice,
   this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.
3. Neither the name of mosquitto nor the names of its
   contributors may be used to endorse or promote products derived from
   this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.
*/

#include "config.h"

/*
 * adding all public XIA socket API function declarations, constants, and
 * types, contained in the Xsockets.h header file. inside an #ifdef clause,
 * in the fashion of the above.
 *
 * FIXME: note we only do this for non-WIN32 systems for now.
 */
#ifndef WIN32
#ifdef WITH_XIA
#include "Xsocket.h"
#include "dagaddr.hpp"
#else
#include <netdb.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#endif
#else
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#ifdef WITH_WRAP
#include <tcpd.h>
#endif

#ifdef __FreeBSD__
#  include <netinet/in.h>
#  include <sys/socket.h>
#endif

#ifdef __QNX__
#include <netinet/in.h>
#include <net/netbyte.h>
#include <sys/socket.h>
#endif

/*
 * adding all public XIA socket API function declarations, constants, and
 * types, contained in the Xsockets.h header file. inside an #ifdef clause,
 * in the fashion of the above.
 */
#ifdef WITH_XIA
#include "Xsocket.h"
#endif

#include "mosquitto_broker.h"
#include "mqtt3_protocol.h"
#include "memory_mosq.h"
#include "net_mosq.h"
#include "util_mosq.h"

#ifdef WITH_TLS
#include "tls_mosq.h"
#include <openssl/err.h>
static int tls_ex_index_context = -1;
static int tls_ex_index_listener = -1;
#endif

#ifdef WITH_SYS_TREE
extern unsigned int g_socket_connections;
#endif

int mqtt3_socket_accept(struct mosquitto_db * db, int listensock)
{
	int i;
	int j;
	int new_sock = -1;
	struct mosquitto **tmp_contexts = NULL;
	struct mosquitto *new_context;
#ifdef WITH_TLS
	BIO *bio;
	int rc;
	char ebuf[256];
	unsigned long e;
#endif
#ifdef WITH_WRAP
	struct request_info wrap_req;
	char address[1024];
#endif

	new_sock = accept(listensock, NULL, 0);
	if(new_sock == INVALID_SOCKET) return -1;

#ifdef WITH_SYS_TREE
	g_socket_connections++;
#endif

	if(_mosquitto_socket_nonblock(new_sock)){
		return INVALID_SOCKET;
	}

#ifdef WITH_WRAP
	/* Use tcpd / libwrap to determine whether a connection is allowed. */
	request_init(&wrap_req, RQ_FILE, new_sock, RQ_DAEMON, "mosquitto", 0);
	fromhost(&wrap_req);
	if(!hosts_access(&wrap_req)){
		/* Access is denied */
		if(!_mosquitto_socket_get_address(new_sock, address, 1024)){
			_mosquitto_log_printf(NULL, MOSQ_LOG_NOTICE, "Client connection from %s denied access by tcpd.", address);
		}
		COMPAT_CLOSE(new_sock);
		return -1;
	}else{
#endif
		new_context = mqtt3_context_init(new_sock);
		if(!new_context){
			COMPAT_CLOSE(new_sock);
			return -1;
		}
		for(i=0; i<db->config->listener_count; i++){
			for(j=0; j<db->config->listeners[i].sock_count; j++){
				if(db->config->listeners[i].socks[j] == listensock){
					new_context->listener = &db->config->listeners[i];
					new_context->listener->client_count++;
					break;
				}
			}
		}
		if(!new_context->listener){
			mqtt3_context_cleanup(NULL, new_context, true);
			return -1;
		}

		if(new_context->listener->max_connections > 0 && new_context->listener->client_count > new_context->listener->max_connections){
			_mosquitto_log_printf(NULL, MOSQ_LOG_NOTICE, "Client connection from %s denied: max_connections exceeded.", new_context->address);
			mqtt3_context_cleanup(NULL, new_context, true);
			return -1;
		}

#ifdef WITH_TLS
		/* TLS init */
		for(i=0; i<db->config->listener_count; i++){
			for(j=0; j<db->config->listeners[i].sock_count; j++){
				if(db->config->listeners[i].socks[j] == listensock){
					if(db->config->listeners[i].ssl_ctx){
						new_context->ssl = SSL_new(db->config->listeners[i].ssl_ctx);
						if(!new_context->ssl){
							mqtt3_context_cleanup(NULL, new_context, true);
							return -1;
						}
						SSL_set_ex_data(new_context->ssl, tls_ex_index_context, new_context);
						SSL_set_ex_data(new_context->ssl, tls_ex_index_listener, &db->config->listeners[i]);
						new_context->want_write = true;
						bio = BIO_new_socket(new_sock, BIO_NOCLOSE);
						SSL_set_bio(new_context->ssl, bio, bio);
						rc = SSL_accept(new_context->ssl);
						if(rc != 1){
							rc = SSL_get_error(new_context->ssl, rc);
							if(rc == SSL_ERROR_WANT_READ){
								/* We always want to read. */
							}else if(rc == SSL_ERROR_WANT_WRITE){
								new_context->want_write = true;
							}else{
								e = ERR_get_error();
								while(e){
									_mosquitto_log_printf(NULL, MOSQ_LOG_NOTICE,
											"Client connection from %s failed: %s.",
											new_context->address, ERR_error_string(e, ebuf));
									e = ERR_get_error();
								}
								mqtt3_context_cleanup(NULL, new_context, true);
								return -1;
							}
						}
					}
				}
			}
		}
#endif

		_mosquitto_log_printf(NULL, MOSQ_LOG_NOTICE, "New connection from %s on port %d.", new_context->address, new_context->listener->port);
		for(i=0; i<db->context_count; i++){
			if(db->contexts[i] == NULL){
				db->contexts[i] = new_context;
				break;
			}
		}
		if(i==db->context_count){
			tmp_contexts = (struct mosquitto **) _mosquitto_realloc(db->contexts, sizeof(struct mosquitto*)*(db->context_count+1));
			if(tmp_contexts){
				db->context_count++;
				db->contexts = tmp_contexts;
				db->contexts[i] = new_context;
			}else{
				// Out of memory
				mqtt3_context_cleanup(NULL, new_context, true);
				return -1;
			}
		}
		// If we got here then the context's DB index is "i" regardless of how we got here
		new_context->db_index = i;

#ifdef WITH_WRAP
	}
#endif
	return new_sock;
}

/**
 * \brief accept() sys call wrapper for mosquitto
 *
 * re-implementation of mqtt3_socket_accept(), XIA-style.
 *
 * @param db
 * @param listensock
 * @return
 */
int xmqtt3_socket_accept(struct mosquitto_db * db, int listensock)
{
	int i;
	int j;
	int new_sock = -1;
	struct mosquitto ** tmp_contexts = NULL;
	struct mosquitto * new_context;

	/*#ifdef WITH_TLS
	BIO *bio;
	int rc;
	char ebuf[256];
	unsigned long e;
	#endif*/


	/*#ifdef WITH_WRAP
	struct request_info wrap_req;
	char address[1024];
	#endif*/

	// replace accept() with Xaccept()
	/*new_sock = accept(listensock, NULL, 0);*/
	sockaddr_x sa;
	socklen_t sz = sizeof(sa);

	if ((new_sock = Xaccept(
						listensock,
						(struct sockaddr *) &sa,
						&sz)) == INVALID_SOCKET) {
		return -1;
	}

	// FIXME: i'm not sure about leaving this but this seems not problematic
	#ifdef WITH_SYS_TREE
	g_socket_connections++;
	#endif

	// FIXME: we'll probably have to ignore this, as the XIA API does not
	// implement non-blocking sockets in the current release. on
	// http://bit.ly/VPxF0X, the devs note that 'because the xsocket
	// identifier is a standard socket, the normal poll and select
	// functions can be used to provide similar behavior'. this is probably
	// what we'll have to do (somewhere)...
	if (_mosquitto_socket_nonblock(new_sock)) {

		_mosquitto_log_printf(
				NULL,
				MOSQ_LOG_ERR,
				"Error while setting non-blocking socket.\n");

		return INVALID_SOCKET;
	}

//	#ifdef WITH_WRAP
//	/* Use tcpd / libwrap to determine whether a connection is allowed. */
//	request_init(&wrap_req, RQ_FILE, new_sock, RQ_DAEMON, "mosquitto", 0);
//	fromhost(&wrap_req);
//	if(!hosts_access(&wrap_req)){
//		/* Access is denied */
//		if(!_mosquitto_socket_get_address(new_sock, address, 1024)){
//			_mosquitto_log_printf(NULL, MOSQ_LOG_NOTICE, "Client connection from %s denied access by tcpd.", address);
//		}
//		COMPAT_CLOSE(new_sock);
//		return -1;
//	}else{
//	#endif

		// mqtt3_context_init() is the function which creates new mosquitto
		// structs, aka contexts. note that we initialize a new context for
		// each new connection we receive.
		new_context = mqtt3_context_init(new_sock);

		Graph g((sockaddr_x *) &sa);

		/*_mosquitto_log_printf(
				NULL,
				MOSQ_LOG_NOTICE,
				"Accepted connection from %s",
				g.dag_string().c_str());*/

		if(!new_context){

			_mosquitto_log_printf(
					NULL,
					MOSQ_LOG_ERR,
					"Error: _mqtt3_context_init() returned NULL.\n");

			COMPAT_CLOSE(new_sock);

			return -1;

		} else {

			// FIXME: the new_context->address field must be set at this point,
			// since the Xgetpeername() function doesn't work so far.
			// check if the pre-defined size for the DAG is enough...
			char dag[1024];
			snprintf(dag, 1024, g.dag_string().c_str());

			new_context->address = _mosquitto_strdup(dag);
		}

		for (i = 0; i < db->config->listener_count; i++) {

			for (j = 0; j < db->config->listeners[i].sock_count; j++) {

				if (db->config->listeners[i].socks[j] == listensock) {

					new_context->listener = &db->config->listeners[i];
					new_context->listener->client_count++;
					break;
				}
			}
		}

		if(!new_context->listener){
			mqtt3_context_cleanup(NULL, new_context, true);
			return -1;
		}

		if(new_context->listener->max_connections > 0
				&& new_context->listener->client_count > new_context->listener->max_connections) {

			_mosquitto_log_printf(
					NULL,
					MOSQ_LOG_NOTICE,
					"Client connection from %s denied: max_connections exceeded.",
					new_context->address);

			mqtt3_context_cleanup(NULL, new_context, true);

			return -1;
		}

//		#ifdef WITH_TLS
//		/* TLS init */
//		for(i=0; i<db->config->listener_count; i++){
//			for(j=0; j<db->config->listeners[i].sock_count; j++){
//				if(db->config->listeners[i].socks[j] == listensock){
//					if(db->config->listeners[i].ssl_ctx){
//						new_context->ssl = SSL_new(db->config->listeners[i].ssl_ctx);
//						if(!new_context->ssl){
//							mqtt3_context_cleanup(NULL, new_context, true);
//							return -1;
//						}
//						SSL_set_ex_data(new_context->ssl, tls_ex_index_context, new_context);
//						SSL_set_ex_data(new_context->ssl, tls_ex_index_listener, &db->config->listeners[i]);
//						new_context->want_write = true;
//						bio = BIO_new_socket(new_sock, BIO_NOCLOSE);
//						SSL_set_bio(new_context->ssl, bio, bio);
//						rc = SSL_accept(new_context->ssl);
//						if(rc != 1){
//							rc = SSL_get_error(new_context->ssl, rc);
//							if(rc == SSL_ERROR_WANT_READ){
//								/* We always want to read. */
//							}else if(rc == SSL_ERROR_WANT_WRITE){
//								new_context->want_write = true;
//							}else{
//								e = ERR_get_error();
//								while(e){
//									_mosquitto_log_printf(NULL, MOSQ_LOG_NOTICE,
//											"Client connection from %s failed: %s.",
//											new_context->address, ERR_error_string(e, ebuf));
//									e = ERR_get_error();
//								}
//								mqtt3_context_cleanup(NULL, new_context, true);
//								return -1;
//							}
//						}
//					}
//				}
//			}
//		}
//		#endif

		_mosquitto_log_printf(
				NULL,
				MOSQ_LOG_NOTICE,
				"New connection from %s on SID %s.",
				new_context->address,
				new_context->listener->sid);

		for (i = 0; i < db->context_count; i++) {

			if (db->contexts[i] == NULL) {
				db->contexts[i] = new_context;
				break;
			}
		}

		// the context created for the incoming connection (from the
		// incoming Xaccept() call above) is approved for addition to the
		// database of the broker.
		if (i == db->context_count) {

			// find room for one more context (struct mosquitto)
			tmp_contexts = (struct mosquitto**) _mosquitto_realloc(
					db->contexts,
					sizeof(struct mosquitto *) *(db->context_count+1));

			// adjust the counters, add the new context to the end of the
			// db->context array
			if (tmp_contexts) {

				db->context_count++;
				db->contexts = tmp_contexts;
				db->contexts[i] = new_context;

			} else {
				// Out of memory
				mqtt3_context_cleanup(NULL, new_context, true);
				return -1;
			}
		}
		// If we got here then the context's DB index is "i" regardless of how we got here
		new_context->db_index = i;

#ifdef WITH_WRAP
	}
#endif
	return new_sock;
}

#ifdef WITH_TLS
static int client_certificate_verify(int preverify_ok, X509_STORE_CTX *ctx)
{
	/* Preverify should check expiry, revocation. */
	return preverify_ok;
}
#endif

#ifdef REAL_WITH_TLS_PSK
static unsigned int psk_server_callback(SSL *ssl, const char *identity, unsigned char *psk, unsigned int max_psk_len)
{
	struct mosquitto_db *db;
	struct mosquitto *context;
	struct _mqtt3_listener *listener;
	char *psk_key = NULL;
	int len;
	const char *psk_hint;

	if(!identity) return 0;

	db = _mosquitto_get_db();

	context = SSL_get_ex_data(ssl, tls_ex_index_context);
	if(!context) return 0;

	listener = SSL_get_ex_data(ssl, tls_ex_index_listener);
	if(!listener) return 0;

	psk_hint = listener->psk_hint;

	/* The hex to BN conversion results in the length halving, so we can pass
	 * max_psk_len*2 as the max hex key here. */
	psk_key = _mosquitto_calloc(1, max_psk_len*2 + 1);
	if(!psk_key) return 0;

	if(mosquitto_psk_key_get(db, psk_hint, identity, psk_key, max_psk_len*2) != MOSQ_ERR_SUCCESS){
		return 0;
	}

	len = _mosquitto_hex2bin(psk_key, psk, max_psk_len);
	if (len < 0) return 0;

	if(listener->use_identity_as_username){
		context->username = _mosquitto_strdup(identity);
		if(!context->username) return 0;
	}

	return len;
}
#endif

/* Creates a socket and listens on port 'port'.
 * Returns 1 on failure
 * Returns 0 on success.
 */
int mqtt3_socket_listen(struct _mqtt3_listener *listener)
{
	int sock = -1;
	struct addrinfo hints;
	struct addrinfo *ainfo, *rp;
	char service[10];
#ifndef WIN32
	int ss_opt = 1;
#else
	char ss_opt = 1;
#endif
#ifdef WITH_TLS
	int rc;
	X509_STORE *store;
	X509_LOOKUP *lookup;
	int ssl_options = 0;
#endif
	char buf[256];

	if(!listener) return MOSQ_ERR_INVAL;

	snprintf(service, 10, "%d", listener->port);
	memset(&hints, 0, sizeof(struct addrinfo));
	hints.ai_family = PF_UNSPEC;
	hints.ai_flags = AI_PASSIVE;
	hints.ai_socktype = SOCK_STREAM;

	if(getaddrinfo(listener->host, service, &hints, &ainfo)) return INVALID_SOCKET;

	listener->sock_count = 0;
	listener->socks = NULL;

	for(rp = ainfo; rp; rp = rp->ai_next){
		if(rp->ai_family == AF_INET){
			_mosquitto_log_printf(NULL, MOSQ_LOG_INFO, "Opening ipv4 listen socket on port %d.", ntohs(((struct sockaddr_in *)rp->ai_addr)->sin_port));
		}else if(rp->ai_family == AF_INET6){
			_mosquitto_log_printf(NULL, MOSQ_LOG_INFO, "Opening ipv6 listen socket on port %d.", ntohs(((struct sockaddr_in6 *)rp->ai_addr)->sin6_port));
		}else{
			continue;
		}

		sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if(sock == -1){
			strerror_r(errno, buf, 256);
			_mosquitto_log_printf(NULL, MOSQ_LOG_WARNING, "Warning: %s", buf);
			continue;
		}
		listener->sock_count++;
		listener->socks = (int *) _mosquitto_realloc(listener->socks, sizeof(int)*listener->sock_count);
		if(!listener->socks){
			_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: Out of memory.");
			return MOSQ_ERR_NOMEM;
		}
		listener->socks[listener->sock_count-1] = sock;

#ifndef WIN32
		ss_opt = 1;
		setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &ss_opt, sizeof(ss_opt));
#endif
		ss_opt = 1;
		setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &ss_opt, sizeof(ss_opt));

		if(_mosquitto_socket_nonblock(sock)){
			return 1;
		}

		if(bind(sock, rp->ai_addr, rp->ai_addrlen) == -1){
			strerror_r(errno, buf, 256);
			_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: %s", buf);
			COMPAT_CLOSE(sock);
			return 1;
		}

		if(listen(sock, 100) == -1){
			strerror_r(errno, buf, 256);
			_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: %s", buf);
			COMPAT_CLOSE(sock);
			return 1;
		}
	}
	freeaddrinfo(ainfo);

	/* We need to have at least one working socket. */
	if(listener->sock_count > 0){
#ifdef WITH_TLS
		if((listener->cafile || listener->capath) && listener->certfile && listener->keyfile){
#if OPENSSL_VERSION_NUMBER >= 0x10001000L
			if(listener->tls_version == NULL){
				listener->ssl_ctx = SSL_CTX_new(TLSv1_2_server_method());
			}else if(!strcmp(listener->tls_version, "tlsv1.2")){
				listener->ssl_ctx = SSL_CTX_new(TLSv1_2_server_method());
			}else if(!strcmp(listener->tls_version, "tlsv1.1")){
				listener->ssl_ctx = SSL_CTX_new(TLSv1_1_server_method());
			}else if(!strcmp(listener->tls_version, "tlsv1")){
				listener->ssl_ctx = SSL_CTX_new(SSLv23_server_method());
			}
#else
			listener->ssl_ctx = SSL_CTX_new(SSLv23_server_method());
#endif
			if(!listener->ssl_ctx){
				_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: Unable to create TLS context.");
				COMPAT_CLOSE(sock);
				return 1;
			}

			/* Don't accept SSLv2 or SSLv3 */
			ssl_options = SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3;
#ifdef SSL_OP_NO_COMPRESSION
			/* Disable compression */
			ssl_options |= SSL_OP_NO_COMPRESSION;
#endif
#ifdef SSL_OP_CIPHER_SERVER_PREFERENCE
			/* Server chooses cipher */
			ssl_options |= SSL_OP_CIPHER_SERVER_PREFERENCE;
#endif
			SSL_CTX_set_options(listener->ssl_ctx, ssl_options);

#ifdef SSL_MODE_RELEASE_BUFFERS
			/* Use even less memory per SSL connection. */
			SSL_CTX_set_mode(listener->ssl_ctx, SSL_MODE_RELEASE_BUFFERS);
#endif
			snprintf(buf, 256, "mosquitto-%d", listener->port);
			SSL_CTX_set_session_id_context(listener->ssl_ctx, (unsigned char *)buf, strlen(buf));

			if(listener->ciphers){
				rc = SSL_CTX_set_cipher_list(listener->ssl_ctx, listener->ciphers);
				if(rc == 0){
					_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: Unable to set TLS ciphers. Check cipher list \"%s\".", listener->ciphers);
					COMPAT_CLOSE(sock);
					return 1;
				}
			}else{
				rc = SSL_CTX_set_cipher_list(listener->ssl_ctx, "DEFAULT:!aNULL:!eNULL:!LOW:!EXPORT:!SSLv2:@STRENGTH");
				if(rc == 0){
					_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: Unable to set TLS ciphers. Check cipher list \"%s\".", listener->ciphers);
					COMPAT_CLOSE(sock);
					return 1;
				}
			}
			rc = SSL_CTX_load_verify_locations(listener->ssl_ctx, listener->cafile, listener->capath);
			if(rc == 0){
				if(listener->cafile && listener->capath){
					_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: Unable to load CA certificates. Check cafile \"%s\" and capath \"%s\".", listener->cafile, listener->capath);
				}else if(listener->cafile){
					_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: Unable to load CA certificates. Check cafile \"%s\".", listener->cafile);
				}else{
					_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: Unable to load CA certificates. Check capath \"%s\".", listener->capath);
				}
				COMPAT_CLOSE(sock);
				return 1;
			}
			/* FIXME user data? */
			if(listener->require_certificate){
				SSL_CTX_set_verify(listener->ssl_ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, client_certificate_verify);
			}else{
				SSL_CTX_set_verify(listener->ssl_ctx, SSL_VERIFY_NONE, client_certificate_verify);
			}
			rc = SSL_CTX_use_certificate_chain_file(listener->ssl_ctx, listener->certfile);
			if(rc != 1){
				_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: Unable to load server certificate \"%s\". Check certfile.", listener->certfile);
				COMPAT_CLOSE(sock);
				return 1;
			}
			rc = SSL_CTX_use_PrivateKey_file(listener->ssl_ctx, listener->keyfile, SSL_FILETYPE_PEM);
			if(rc != 1){
				_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: Unable to load server key file \"%s\". Check keyfile.", listener->keyfile);
				COMPAT_CLOSE(sock);
				return 1;
			}
			rc = SSL_CTX_check_private_key(listener->ssl_ctx);
			if(rc != 1){
				_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: Server certificate/key are inconsistent.");
				COMPAT_CLOSE(sock);
				return 1;
			}
			/* Load CRLs if they exist. */
			if(listener->crlfile){
				store = SSL_CTX_get_cert_store(listener->ssl_ctx);
				if(!store){
					_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: Unable to obtain TLS store.");
					COMPAT_CLOSE(sock);
					return 1;
				}
				lookup = X509_STORE_add_lookup(store, X509_LOOKUP_file());
				rc = X509_load_crl_file(lookup, listener->crlfile, X509_FILETYPE_PEM);
				if(rc != 1){
					_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: Unable to load certificate revocation file \"%s\". Check crlfile.", listener->crlfile);
					COMPAT_CLOSE(sock);
					return 1;
				}
				X509_STORE_set_flags(store, X509_V_FLAG_CRL_CHECK);
			}

#  ifdef REAL_WITH_TLS_PSK
		}else if(listener->psk_hint){
			if(tls_ex_index_context == -1){
				tls_ex_index_context = SSL_get_ex_new_index(0, "client context", NULL, NULL, NULL);
			}
			if(tls_ex_index_listener == -1){
				tls_ex_index_listener = SSL_get_ex_new_index(0, "listener", NULL, NULL, NULL);
			}

#if OPENSSL_VERSION_NUMBER >= 0x10001000L
			if(listener->tls_version == NULL){
				listener->ssl_ctx = SSL_CTX_new(TLSv1_2_server_method());
			}else if(!strcmp(listener->tls_version, "tlsv1.2")){
				listener->ssl_ctx = SSL_CTX_new(TLSv1_2_server_method());
			}else if(!strcmp(listener->tls_version, "tlsv1.1")){
				listener->ssl_ctx = SSL_CTX_new(TLSv1_1_server_method());
			}else if(!strcmp(listener->tls_version, "tlsv1")){
				listener->ssl_ctx = SSL_CTX_new(TLSv1_server_method());
			}
#else
			listener->ssl_ctx = SSL_CTX_new(TLSv1_server_method());
#endif
			if(!listener->ssl_ctx){
				_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: Unable to create TLS context.");
				COMPAT_CLOSE(sock);
				return 1;
			}
			SSL_CTX_set_psk_server_callback(listener->ssl_ctx, psk_server_callback);
			if(listener->psk_hint){
				rc = SSL_CTX_use_psk_identity_hint(listener->ssl_ctx, listener->psk_hint);
				if(rc == 0){
					_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: Unable to set TLS PSK hint.");
					COMPAT_CLOSE(sock);
					return 1;
				}
			}
			if(listener->ciphers){
				rc = SSL_CTX_set_cipher_list(listener->ssl_ctx, listener->ciphers);
				if(rc == 0){
					_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: Unable to set TLS ciphers. Check cipher list \"%s\".", listener->ciphers);
					COMPAT_CLOSE(sock);
					return 1;
				}
			}
#  endif /* REAL_WITH_TLS_PSK */
		}
#endif /* WITH_TLS */
		return 0;
	}else{
		return 1;
	}
}

#ifdef WITH_XIA
/**
 * \brief wrapper for the socket(), bind(), listen() sequence of operations
 * for mosquitto.
 *
 * FIXME: the Xbind() operation is preceeded by an XregisterName()
 * operation, similarly to echoserver.c. don't know if this is somewhat
 * wrong.
 *
 * @param 	listener	mosquitto's description for the listener socket
 * @return	an integer, 0 if the a listening socket is correctly initialized,
 * 			otherwise an MOSQ_ERR_* error code or 1 is returned.
 */
int xmqtt3_socket_listen(struct _mqtt3_listener * listener)
{
	int sock = -1;

	// we have to make sure we'll be using the addrinfo structs used by XIA and
	// not those defined in netdb.h.
	struct addrinfo hints;
	struct addrinfo * ainfo, * rp;

	char service[10];

	#ifndef WIN32
	int ss_opt = 1;
	#else
	char ss_opt = 1;
	#endif

	// we'll leave TLS out of the game for the moment
	/*
	#ifdef WITH_TLS
	int rc;
	X509_STORE *store;
	X509_LOOKUP *lookup;
	int ssl_options = 0;
	#endif
	*/

	char buf[256];

	if (!listener) return MOSQ_ERR_INVAL;

	snprintf(service, 10, "%d", listener->port);
	memset(&hints, 0, sizeof(struct addrinfo));

	// note we're here setting XIA options for our socket address
	// filter, according to those previously specified in the original
	// mqtt3_socket_listen() method:
	// 		(1) AF_UNSPEC (it could be AF_XIA)
	// 		(2) AI_PASSIVE
	// 		(3) XSOCK_STREAM
	hints.ai_family = AF_XIA;
	hints.ai_flags = AI_PASSIVE;
	hints.ai_socktype = XSOCK_STREAM;

	// replace getaddrinfo by Xgetaddrinfo
	/*if(getaddrinfo(listener->host, service, &hints, &ainfo))
		return INVALID_SOCKET;*/
	_mosquitto_log_printf(
			NULL,
			MOSQ_LOG_INFO,
			"Listener hostname: %s, SID %s\n",
			listener->host,
			listener->sid);

	if (Xgetaddrinfo(NULL, listener->sid, &hints, &ainfo) != 0) {

		_mosquitto_log_printf(
				NULL,
				MOSQ_LOG_ERR,
				"Error in Xgetaddrinfo().");

		return INVALID_SOCKET;
	}

	listener->sock_count = 0;
	listener->socks = NULL;

	// cycle the list of returned addrinfo structs which matched the
	// hints criteria. we'll be adding an if clause for XIA sockets, and
	// i'll leave the IPv4 and IPv6 if clauses, just in case.
	for (rp = ainfo; rp; rp = rp->ai_next) {

		if(rp->ai_family == AF_INET) {

			_mosquitto_log_printf(
					NULL,
					MOSQ_LOG_INFO,
					"Opening ipv4 listen socket on port %d.",
					ntohs(((struct sockaddr_in *)rp->ai_addr)->sin_port));

		} else if (rp->ai_family == AF_INET6) {

			_mosquitto_log_printf(
					NULL,
					MOSQ_LOG_INFO,
					"Opening ipv6 listen socket on port %d.",
					ntohs(((struct sockaddr_in6 *)rp->ai_addr)->sin6_port));

		} else if (rp->ai_family == AF_XIA) {

			// notice that in XIA, sockaddr_x structs have no port field: in
			// fact, the concept of ports is captured by the SID principal
			// type, whose 160 bit XID can be interpreted as a port number
			Graph g((sockaddr_x *) rp->ai_addr);
			_mosquitto_log_printf(
					NULL,
					MOSQ_LOG_INFO,
					"Opening mosquitto broker's XIA-style listensocket on DAG\n%s.",
					g.dag_string().c_str());

		} else {

			continue;
		}

		// replace socket() by Xsocket()
		/*sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);*/
		if ((sock = Xsocket(
						rp->ai_family,
						rp->ai_socktype,
						rp->ai_protocol)) == -1) {

			strerror_r(errno, buf, 256);
			_mosquitto_log_printf(NULL, MOSQ_LOG_WARNING, "Warning: %s", buf);
			continue;
		}

		listener->sock_count++;
		listener->socks = (int *) _mosquitto_realloc(
				listener->socks,
				sizeof(int) * listener->sock_count);

		if(!listener->socks) {

			_mosquitto_log_printf(
					NULL,
					MOSQ_LOG_ERR,
					"Error: Memory FAIL - Out of Memory.");

			return MOSQ_ERR_NOMEM;
		}

		// important step: socket we've just created with Xsocket() will be
		// pointed to by the .socks pointer array from the listener struct.
		listener->socks[listener->sock_count - 1] = sock;

#ifndef WIN32

		ss_opt = 1;
		//setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &ss_opt, sizeof(ss_opt));
#endif
		ss_opt = 1;

		// replace setsockopt with Xsetsockopt(). plus, not sure if we're
		// actually need to do this.

		// TODO: check this out and try to understand if a call to
		// Xsetsockopt() is necessary here. notice that the call to setsockopt()
		// of the original method was only relative to the IPPROTO_IPV6
		// protocol level
		// only, therefore i would say this is not needed... XOPT_NEXT_PROTO
		// sets the next
		// proto field in the XIA header, the other option is XOPT_HLIM,
		// which sets the hop limit on an XIA packet header (i guess this
		// is useful for the xtraceroute application). check this out too
		// http://bit.ly/XPe2Id.
		/*setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &ss_opt, sizeof(ss_opt));*/
		/*Xsetsockopt(
				sock,
				XOPT_NEXT_PROTO,
				(const void*) &ss_opt,
				sizeof(ss_opt));*/

		// we'll probably have to ignore this, as the XIA API does not
		// implement non-blocking sockets in the current release. on
		// http://bit.ly/VPxF0X, the devs note that 'because the xsocket
		// identifier is a standard socket, the normal poll and select
		// functions can be used to provide similar behavior'. this is probably
		// what we'll have to do (somewhere)...
		/*if(_mosquitto_socket_nonblock(sock)) {

			return 1;
		}*/

		// replace bind() with Xbind(). the call to Xbind() is weird, in the
		// sense that it uses a sockaddr struct instead of a sockaddr_x
		// struct.

		/*if(bind(sock, rp->ai_addr, rp->ai_addrlen) == -1){
			strerror_r(errno, buf, 256);
			_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: %s", buf);
			COMPAT_CLOSE(sock);
			return 1;
		}*/

		sockaddr_x * sa = (sockaddr_x *) rp->ai_addr;

		// FIXME: similarly to echoserver.c, we place here a call to
		// XregisterName().
		if (listener->host) {

		    if (XregisterName(listener->host, sa) < 0 ) {

				_mosquitto_log_printf(
						NULL,
						MOSQ_LOG_ERR,
						"Error: Couldn't register name %s\n",
						STREAM_NAME);
		    } else {

				_mosquitto_log_printf(
						NULL,
						MOSQ_LOG_INFO,
						"Registered name %s\n",
						listener->host);
		    }
		}

		if (Xbind(sock, (struct sockaddr *) sa, sizeof(sa)) < 0) {

			strerror_r(errno, buf, 256);

			_mosquitto_log_printf(
					NULL,
					MOSQ_LOG_ERR,
					"Error: Xbind() FAIL. Description: %s",
					buf);

			COMPAT_CLOSE(sock);

			return 1;
		}

		// TODO: so XIA doesn't use the listen() sys call: e.g. in the
		// echoserver.c example, we jump directly from a bind() to an accept()
		// call. i wonder why: in the listen() MAN page, one can read that
		// the listen() sys call sets the 'willingness to accept incoming
		// connections and a queue limit for incoming connections'. in XIA
		// we may not need to do this then, so i'll ignore it for
		// the moment.
		/*if(listen(sock, 100) == -1){
			strerror_r(errno, buf, 256);
			_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: %s", buf);
			COMPAT_CLOSE(sock);
			return 1;
		}*/
	}

	// replace freeaddrinfo() with Xfreeaddrinfo(). in fact, this function is
	// just a wrapper for freeaddrinfo(), no XIA specific processing occurs
	// here.
	//freeaddrinfo(ainfo);
	Xfreeaddrinfo(ainfo);

	/* We need to have at least one working socket. */
	if(listener->sock_count > 0) {

		// again, we'll discard the TLS stuff for now...
//#ifdef WITH_TLS
//		if((listener->cafile || listener->capath) && listener->certfile && listener->keyfile){
//#if OPENSSL_VERSION_NUMBER >= 0x10001000L
//			if(listener->tls_version == NULL){
//				listener->ssl_ctx = SSL_CTX_new(TLSv1_2_server_method());
//			}else if(!strcmp(listener->tls_version, "tlsv1.2")){
//				listener->ssl_ctx = SSL_CTX_new(TLSv1_2_server_method());
//			}else if(!strcmp(listener->tls_version, "tlsv1.1")){
//				listener->ssl_ctx = SSL_CTX_new(TLSv1_1_server_method());
//			}else if(!strcmp(listener->tls_version, "tlsv1")){
//				listener->ssl_ctx = SSL_CTX_new(SSLv23_server_method());
//			}
//#else
//			listener->ssl_ctx = SSL_CTX_new(SSLv23_server_method());
//#endif
//			if(!listener->ssl_ctx){
//				_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: Unable to create TLS context.");
//				COMPAT_CLOSE(sock);
//				return 1;
//			}
//
//			/* Don't accept SSLv2 or SSLv3 */
//			ssl_options = SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3;
//#ifdef SSL_OP_NO_COMPRESSION
//			/* Disable compression */
//			ssl_options |= SSL_OP_NO_COMPRESSION;
//#endif
//#ifdef SSL_OP_CIPHER_SERVER_PREFERENCE
//			/* Server chooses cipher */
//			ssl_options |= SSL_OP_CIPHER_SERVER_PREFERENCE;
//#endif
//			SSL_CTX_set_options(listener->ssl_ctx, ssl_options);
//
//#ifdef SSL_MODE_RELEASE_BUFFERS
//			/* Use even less memory per SSL connection. */
//			SSL_CTX_set_mode(listener->ssl_ctx, SSL_MODE_RELEASE_BUFFERS);
//#endif
//			snprintf(buf, 256, "mosquitto-%d", listener->port);
//			SSL_CTX_set_session_id_context(listener->ssl_ctx, (unsigned char *)buf, strlen(buf));
//
//			if(listener->ciphers){
//				rc = SSL_CTX_set_cipher_list(listener->ssl_ctx, listener->ciphers);
//				if(rc == 0){
//					_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: Unable to set TLS ciphers. Check cipher list \"%s\".", listener->ciphers);
//					COMPAT_CLOSE(sock);
//					return 1;
//				}
//			}else{
//				rc = SSL_CTX_set_cipher_list(listener->ssl_ctx, "DEFAULT:!aNULL:!eNULL:!LOW:!EXPORT:!SSLv2:@STRENGTH");
//				if(rc == 0){
//					_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: Unable to set TLS ciphers. Check cipher list \"%s\".", listener->ciphers);
//					COMPAT_CLOSE(sock);
//					return 1;
//				}
//			}
//			rc = SSL_CTX_load_verify_locations(listener->ssl_ctx, listener->cafile, listener->capath);
//			if(rc == 0){
//				if(listener->cafile && listener->capath){
//					_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: Unable to load CA certificates. Check cafile \"%s\" and capath \"%s\".", listener->cafile, listener->capath);
//				}else if(listener->cafile){
//					_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: Unable to load CA certificates. Check cafile \"%s\".", listener->cafile);
//				}else{
//					_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: Unable to load CA certificates. Check capath \"%s\".", listener->capath);
//				}
//				COMPAT_CLOSE(sock);
//				return 1;
//			}
//			/* FIXME user data? */
//			if(listener->require_certificate){
//				SSL_CTX_set_verify(listener->ssl_ctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, client_certificate_verify);
//			}else{
//				SSL_CTX_set_verify(listener->ssl_ctx, SSL_VERIFY_NONE, client_certificate_verify);
//			}
//			rc = SSL_CTX_use_certificate_chain_file(listener->ssl_ctx, listener->certfile);
//			if(rc != 1){
//				_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: Unable to load server certificate \"%s\". Check certfile.", listener->certfile);
//				COMPAT_CLOSE(sock);
//				return 1;
//			}
//			rc = SSL_CTX_use_PrivateKey_file(listener->ssl_ctx, listener->keyfile, SSL_FILETYPE_PEM);
//			if(rc != 1){
//				_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: Unable to load server key file \"%s\". Check keyfile.", listener->keyfile);
//				COMPAT_CLOSE(sock);
//				return 1;
//			}
//			rc = SSL_CTX_check_private_key(listener->ssl_ctx);
//			if(rc != 1){
//				_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: Server certificate/key are inconsistent.");
//				COMPAT_CLOSE(sock);
//				return 1;
//			}
//			/* Load CRLs if they exist. */
//			if(listener->crlfile){
//				store = SSL_CTX_get_cert_store(listener->ssl_ctx);
//				if(!store){
//					_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: Unable to obtain TLS store.");
//					COMPAT_CLOSE(sock);
//					return 1;
//				}
//				lookup = X509_STORE_add_lookup(store, X509_LOOKUP_file());
//				rc = X509_load_crl_file(lookup, listener->crlfile, X509_FILETYPE_PEM);
//				if(rc != 1){
//					_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: Unable to load certificate revocation file \"%s\". Check crlfile.", listener->crlfile);
//					COMPAT_CLOSE(sock);
//					return 1;
//				}
//				X509_STORE_set_flags(store, X509_V_FLAG_CRL_CHECK);
//			}
//
//#  ifdef REAL_WITH_TLS_PSK
//		}else if(listener->psk_hint){
//			if(tls_ex_index_context == -1){
//				tls_ex_index_context = SSL_get_ex_new_index(0, "client context", NULL, NULL, NULL);
//			}
//			if(tls_ex_index_listener == -1){
//				tls_ex_index_listener = SSL_get_ex_new_index(0, "listener", NULL, NULL, NULL);
//			}
//
//#if OPENSSL_VERSION_NUMBER >= 0x10001000L
//			if(listener->tls_version == NULL){
//				listener->ssl_ctx = SSL_CTX_new(TLSv1_2_server_method());
//			}else if(!strcmp(listener->tls_version, "tlsv1.2")){
//				listener->ssl_ctx = SSL_CTX_new(TLSv1_2_server_method());
//			}else if(!strcmp(listener->tls_version, "tlsv1.1")){
//				listener->ssl_ctx = SSL_CTX_new(TLSv1_1_server_method());
//			}else if(!strcmp(listener->tls_version, "tlsv1")){
//				listener->ssl_ctx = SSL_CTX_new(TLSv1_server_method());
//			}
//#else
//			listener->ssl_ctx = SSL_CTX_new(TLSv1_server_method());
//#endif
//			if(!listener->ssl_ctx){
//				_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: Unable to create TLS context.");
//				COMPAT_CLOSE(sock);
//				return 1;
//			}
//			SSL_CTX_set_psk_server_callback(listener->ssl_ctx, psk_server_callback);
//			if(listener->psk_hint){
//				rc = SSL_CTX_use_psk_identity_hint(listener->ssl_ctx, listener->psk_hint);
//				if(rc == 0){
//					_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: Unable to set TLS PSK hint.");
//					COMPAT_CLOSE(sock);
//					return 1;
//				}
//			}
//			if(listener->ciphers){
//				rc = SSL_CTX_set_cipher_list(listener->ssl_ctx, listener->ciphers);
//				if(rc == 0){
//					_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: Unable to set TLS ciphers. Check cipher list \"%s\".", listener->ciphers);
//					COMPAT_CLOSE(sock);
//					return 1;
//				}
//			}
//#  endif /* REAL_WITH_TLS_PSK */
//		}
//#endif /* WITH_TLS */

		return 0;

	} else {

		return 1;
	}
}
#endif

int _mosquitto_socket_get_address(int sock, char *buf, int len)
{
	struct sockaddr_storage addr;
	socklen_t addrlen;

	addrlen = sizeof(addr);
	if(!getpeername(sock, (struct sockaddr *)&addr, &addrlen)){
		if(addr.ss_family == AF_INET){
			if(inet_ntop(AF_INET, &((struct sockaddr_in *)&addr)->sin_addr.s_addr, buf, len)){
				return 0;
			}
		}else if(addr.ss_family == AF_INET6){
			if(inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&addr)->sin6_addr.s6_addr, buf, len)){
				return 0;
			}
		}
	}
	return 1;
}

/**
 * \brief extracts the XIA address (i.e. a full DAG) of the remote socket.
 *
 * re-implementation of _mosquitto_socket_get_address(), XIA style.
 *
 * @param sock
 * @param buf
 * @param len
 * @return
 */
int _xmosquitto_socket_get_address(int sock, char * buf, int len)
{
	// Xgetpeername() uses a struct sockaddr_x
	//struct sockaddr_storage addr;
	sockaddr_x addr;
	socklen_t addrlen = sizeof(sockaddr_x);

	// replace getpeername with Xgetpeername
	//if (!getpeername(sock, (struct sockaddr *) &addr, &addrlen)) {
	int rc = Xgetpeername(sock, (struct sockaddr *) &addr, &addrlen);

	if (!rc) {

		// FIXME: should we check for this in here? for now, let's not use
		// this, as it may cause confusion when debugging in the future...
		//if (addr.sa_family == AF_XIA || addr.sa_family == AF_UNSPEC) {

			// get the DAG string and copy it to the buffer

			// FIXME: what are we looking for here? the complete DAG? the
			// final intent (e.g. the HID or SID at the last node of the
			// DAG)? for now i'll assume it's everything.
			Graph g(&addr);

			// FIXME: we don't know if the pre-allocated
			// size of buf are appropriate for the lenghts of DAGs.
			// nevertheless, as the pre-allocated size is passed to
			// _xmosquitto_socket_get_address() via 'len', there's no
			// danger of a out of bounds error.
			snprintf(buf, (size_t) len, "%s", g.dag_string().c_str());

			return 0;
		//}
	} else {

		_mosquitto_log_printf(
				NULL,
				MOSQ_LOG_ERR,
				"Error: Xgetpeername() returned %d, errno %d",
				rc, errno);
	}

	return 1;
}
