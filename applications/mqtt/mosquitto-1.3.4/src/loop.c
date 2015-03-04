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

#define _GNU_SOURCE

#include "config.h"

#include <assert.h>
#ifndef WIN32
#ifdef WITH_XIA
#include "Xsocket.h"
#endif
#include <poll.h>
#else
#include <process.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "mosquitto_broker.h"
#include "memory_mosq.h"
#include "time_mosq.h"
#include "util_mosq.h"

extern bool flag_reload;
#ifdef WITH_PERSISTENCE
extern bool flag_db_backup;
#endif
extern bool flag_tree_print;
extern int run;
#ifdef WITH_SYS_TREE
extern int g_clients_expired;
#endif

static void loop_handle_errors(struct mosquitto_db *db, struct pollfd *pollfds);
static void loop_handle_reads_writes(struct mosquitto_db *db, struct pollfd *pollfds);

int mosquitto_main_loop(
		struct mosquitto_db * db,
		int * listensock,
		int listensock_count,
		int listener_max)
{
	time_t start_time = mosquitto_time();
	time_t last_backup = mosquitto_time();
	time_t last_store_clean = mosquitto_time();
	time_t now;
	int time_count;
	int fdcount;
#ifndef WIN32
	sigset_t sigblock, origsig;
#endif
	int i;
	struct pollfd * pollfds = NULL;
	int pollfd_count = 0;
	int pollfd_index;
#ifdef WITH_BRIDGE
	int bridge_sock;
	int rc;
#endif

#ifndef WIN32
	sigemptyset(&sigblock);
	sigaddset(&sigblock, SIGINT);
#endif

	_mosquitto_log_printf(
			NULL,
			MOSQ_LOG_INFO,
			"Entering The Loop...");

	// here the loop starts
	while (run) {
#ifdef WITH_SYS_TREE
		if (db->config->sys_interval > 0) {
			mqtt3_db_sys_update(db, db->config->sys_interval, start_time);
		}
#endif

		// initialize the array of pollf sructs, pollfds, which is used
		// to monitor several file descriptors, either listen sockets or
		// context->sock
		if (listensock_count + db->context_count > pollfd_count || !pollfds) {
			pollfd_count = listensock_count + db->context_count;
			pollfds =
					(struct pollfd *) _mosquitto_realloc(
							pollfds,
							sizeof(struct pollfd) * pollfd_count);

			if (!pollfds) {
				_mosquitto_log_printf(NULL, MOSQ_LOG_ERR, "Error: Out of memory.");
				return MOSQ_ERR_NOMEM;
			}
		}

		memset(pollfds, -1, sizeof(struct pollfd)*pollfd_count);

		pollfd_index = 0;

		/*
		 * the pollfds structs corresponding to listen sockets are
		 * initialized, according to (1) their fd values, (2) the type of event
		 * we're interested in monitoring, i.e. 'POLLIN', and (3) a variable
		 * which holds the type of the latest occurring event, which may or not
		 * be equal to the type specified in 'events'.
		 *
		 * Just by curiosity, here's a list of some of the types one could set
		 * on 'events' (chek the rest on http://linux.die.net/man/2/poll):
		 *
		 * 1) POLLIN: 		Data to read.
		 * 2) POLLPRI:		Urgent data to read.
		 * 3) POLLOUT:		Writing now will not block.
		 * 4) POLLRDHUP:	Stream socket peer closed connection.
		 */
		for (i = 0; i < listensock_count; i++) {

			pollfds[pollfd_index].fd = listensock[i];
			pollfds[pollfd_index].events = POLLIN;
			pollfds[pollfd_index].revents = 0;
			pollfd_index++;
		}

		time_count = 0;

		// now cycle through the contexts (i.e. representations of mosquitto
		// entities) saved in the database, either subscribers, publishers or
		// other brokers connected via bridges
		for (i = 0; i < db->context_count; i++) {

			if (db->contexts[i]) {

				if (time_count > 0) {

					time_count--;

				} else {

					time_count = 1000;
					now = mosquitto_time();
				}

				db->contexts[i]->pollfd_index = -1;

				if (db->contexts[i]->sock != INVALID_SOCKET) {

#ifdef WITH_BRIDGE
					// here we look for all contexts of type bridge, connected
					// to this broker, check if the connections are still
					// active, disconnect them if not
					if (db->contexts[i]->bridge) {

						_mosquitto_check_keepalive(db->contexts[i]);

						if (db->contexts[i]->bridge->round_robin == false
								&& db->contexts[i]->bridge->cur_address != 0
								&& now > db->contexts[i]->bridge->primary_retry) {

							/* FIXME - this should be non-blocking */
							if (_mosquitto_try_connect(
									db->contexts[i]->bridge->addresses[0].address,
									db->contexts[i]->bridge->addresses[0].port,
									&bridge_sock, NULL, true) == MOSQ_ERR_SUCCESS) {

								COMPAT_CLOSE(bridge_sock);
								_mosquitto_socket_close(db->contexts[i]);
								db->contexts[i]->bridge->cur_address =
										db->contexts[i]->bridge->address_count-1;
							}
						}
					}
#endif

					/* Local bridges never time out in this fashion. */

					// TODO: this part is important: here we test for the
					// liveness of several contexts kept in the brokers
					// database (e.g. if the period of time between now and
					// the last activity is smaller than a keepalive period,
					// set by the --keepalive option on mosquitto
					// subscribers, the time between PING commands)
					// and check if (inbound) messages have been received
					if (!(db->contexts[i]->keepalive)
							|| db->contexts[i]->bridge
							|| (now - db->contexts[i]->last_msg_in
									< (time_t)(db->contexts[i]->keepalive) * 3/2)) {

						// the mqtt3_db_message_write() method cycles through
						// all pending mosquitto_client_msg * on
						// db->contexts[i], and responds with notifications,
						// ACKs, etc. if necessary.
						// TODO: in short, this is kind of an handler method
						// which periodically checks some context's
						// mosquitto_client_msg list and responds to
						// pending incoming messages
						if (mqtt3_db_message_write(
								db->contexts[i]) == MOSQ_ERR_SUCCESS
								) {

							// notice how pollfd_index starts with
							// the value left after filling pollfds with the
							// 'listening' sockets...
							pollfds[pollfd_index].fd = db->contexts[i]->sock;
							pollfds[pollfd_index].events = POLLIN;
							pollfds[pollfd_index].revents = 0;

							if (db->contexts[i]->current_out_packet) {
								pollfds[pollfd_index].events |= POLLOUT;
							}

							db->contexts[i]->pollfd_index = pollfd_index;

							pollfd_index++;

						} else {

							// disconnect the mosquitto entity represented
							// by the context under inspection if the above
							// mqtt3_db_message_write() fails
							mqtt3_context_disconnect(db, db->contexts[i]);
						}

					} else {

						if (db->config->connection_messages == true) {

							_mosquitto_log_printf(
									NULL,
									MOSQ_LOG_NOTICE,
									"Client %s has exceeded timeout, disconnecting.",
									db->contexts[i]->id);
						}

						/* Client has exceeded keepalive*1.5 */
						mqtt3_context_disconnect(db, db->contexts[i]);
					}

				} else {

#ifdef WITH_BRIDGE
					if(db->contexts[i]->bridge){
						/* Want to try to restart the bridge connection */
						if(!db->contexts[i]->bridge->restart_t){
							db->contexts[i]->bridge->restart_t = now+db->contexts[i]->bridge->restart_timeout;
							db->contexts[i]->bridge->cur_address++;
							if(db->contexts[i]->bridge->cur_address == db->contexts[i]->bridge->address_count){
								db->contexts[i]->bridge->cur_address = 0;
							}
							if(db->contexts[i]->bridge->round_robin == false && db->contexts[i]->bridge->cur_address != 0){
								db->contexts[i]->bridge->primary_retry = now + 5;
							}
						}else{
							if(db->contexts[i]->bridge->start_type == bst_lazy && db->contexts[i]->bridge->lazy_reconnect){
								rc = mqtt3_bridge_connect(db, db->contexts[i]);
								if(rc){
									db->contexts[i]->bridge->cur_address++;
									if(db->contexts[i]->bridge->cur_address == db->contexts[i]->bridge->address_count){
										db->contexts[i]->bridge->cur_address = 0;
									}
								}
							}
							if(db->contexts[i]->bridge->start_type == bst_automatic && now > db->contexts[i]->bridge->restart_t){
								db->contexts[i]->bridge->restart_t = 0;
								rc = mqtt3_bridge_connect(db, db->contexts[i]);
								if(rc == MOSQ_ERR_SUCCESS){
									pollfds[pollfd_index].fd = db->contexts[i]->sock;
									pollfds[pollfd_index].events = POLLIN;
									pollfds[pollfd_index].revents = 0;
									if(db->contexts[i]->current_out_packet){
										pollfds[pollfd_index].events |= POLLOUT;
									}
									db->contexts[i]->pollfd_index = pollfd_index;
									pollfd_index++;
								}else{
									/* Retry later. */
									db->contexts[i]->bridge->restart_t = now+db->contexts[i]->bridge->restart_timeout;

									db->contexts[i]->bridge->cur_address++;
									if(db->contexts[i]->bridge->cur_address == db->contexts[i]->bridge->address_count){
										db->contexts[i]->bridge->cur_address = 0;
									}
								}
							}
						}
					}else{
#endif
						if(db->contexts[i]->clean_session == true){
							mqtt3_context_cleanup(db, db->contexts[i], true);
							db->contexts[i] = NULL;
						}else if(db->config->persistent_client_expiration > 0){
							/* This is a persistent client, check to see if the
							 * last time it connected was longer than
							 * persistent_client_expiration seconds ago. If so,
							 * expire it and clean up.
							 */
							if(now > db->contexts[i]->disconnect_t+db->config->persistent_client_expiration){
								_mosquitto_log_printf(NULL, MOSQ_LOG_NOTICE, "Expiring persistent client %s due to timeout.", db->contexts[i]->id);
#ifdef WITH_SYS_TREE
								g_clients_expired++;
#endif
								db->contexts[i]->clean_session = true;
								mqtt3_context_cleanup(db, db->contexts[i], true);
								db->contexts[i] = NULL;
							}
						}
#ifdef WITH_BRIDGE
					}
#endif
				}
			}
		}

		mqtt3_db_message_timeout_check(db, db->config->retry_interval);

#ifndef WIN32

		/*
		 * 1.4) ok, at this point, the poll() method, performs a similar
		 * task to select(), it waits for one of a set of
		 * fds in pollfds to become ready to perform I/O, by polling them
		 * every 100 milliseconds...
		 *
		 * ... and this is where the pollfd struct comes into play, as poll()
		 * specifically asks for arguments of type pollfd (see
		 * http://linux.die.net/man/2/poll).
		 *
		 * in addition, what are the sigprocmask() methods doing here? i guess
		 * these are either blocking or unblocking some set of interesting
		 * signals for mosquitto.
		 */
		sigprocmask(SIG_SETMASK, &sigblock, &origsig);
		fdcount = poll(pollfds, pollfd_index, 100);
		sigprocmask(SIG_SETMASK, &origsig, NULL);
#else
		fdcount = WSAPoll(pollfds, pollfd_index, 100);
#endif

		/*
		 * 1.5) ok, so after the polling results, we either (1) handle error
		 * events or (2) successful read/write events
		 */
		if(fdcount == -1){

			/*
			 * 1.5.1) ERRORS, for 'contexts'
			 */
			loop_handle_errors(db, pollfds);

		} else {

			/*
			 * 1.5.2) READ/WRITES
			 */

			/*
			 * 1.5.3) this is for 'context' event handling
			 */

			loop_handle_reads_writes(db, pollfds);

			/*
			 * 1.5.4) for listener socket handling, notice how the .revents of
			 * interest are 'POLLIN' and 'POLLPRI', which mean incoming
			 * connections
			 */

			for(i = 0; i < listensock_count; i++) {

				if(pollfds[i].revents & (POLLIN | POLLPRI)) {

					_mosquitto_log_printf(
							NULL,
							MOSQ_LOG_INFO,
							"xmqtt3_socket_accept(%d)", i);

					/*
					 * 1.5.4.1) accept() the incoming connections.
					 * FIXME: the while() cycle will keep calling
					 * xmqtt3_socket_accept(), which blocks the program, since
					 * Xsockets are ALWAYS blocking (i.e. the first time the
					 * Xaccept() call doesn't block, but does it the next
					 * time it is called). to solve it, either (1) use a single
					 * xmqtt3_socket_accept() call, or (2) use select()?
					 */
//					while (xmqtt3_socket_accept(db, listensock[i]) != -1) {
//					}
					xmqtt3_socket_accept(db, listensock[i]);
				}
			}
		}
#ifdef WITH_PERSISTENCE
		if(db->config->persistence && db->config->autosave_interval){
			if(db->config->autosave_on_changes){
				if(db->persistence_changes > db->config->autosave_interval){
					mqtt3_db_backup(db, false, false);
					db->persistence_changes = 0;
				}
			}else{
				if(last_backup + db->config->autosave_interval < mosquitto_time()){
					mqtt3_db_backup(db, false, false);
					last_backup = mosquitto_time();
				}
			}
		}
#endif
		if(!db->config->store_clean_interval || last_store_clean + db->config->store_clean_interval < mosquitto_time()){
			mqtt3_db_store_clean(db);
			last_store_clean = mosquitto_time();
		}
#ifdef WITH_PERSISTENCE
		if(flag_db_backup){
			mqtt3_db_backup(db, false, false);
			flag_db_backup = false;
		}
#endif
		if(flag_reload){
			_mosquitto_log_printf(NULL, MOSQ_LOG_INFO, "Reloading config.");
			mqtt3_config_read(db->config, true);
			mosquitto_security_cleanup(db, true);
			mosquitto_security_init(db, true);
			mosquitto_security_apply(db);
			mqtt3_log_init(db->config->log_type, db->config->log_dest);
			flag_reload = false;
		}
		if(flag_tree_print){
			mqtt3_sub_tree_print(&db->subs, 0);
			flag_tree_print = false;
		}
	}

	if(pollfds) _mosquitto_free(pollfds);
	return MOSQ_ERR_SUCCESS;
}

static void do_disconnect(struct mosquitto_db *db, int context_index)
{
	if(db->config->connection_messages == true){
		if(db->contexts[context_index]->state != mosq_cs_disconnecting){
			_mosquitto_log_printf(NULL, MOSQ_LOG_NOTICE, "Socket error on client %s, disconnecting.", db->contexts[context_index]->id);
		}else{
			_mosquitto_log_printf(NULL, MOSQ_LOG_NOTICE, "Client %s disconnected.", db->contexts[context_index]->id);
		}
	}
	mqtt3_context_disconnect(db, db->contexts[context_index]);
}

/*
 * Error ocurred, probably an fd has been closed.
 * Loop through and check them all.
 */
static void loop_handle_errors(struct mosquitto_db *db, struct pollfd *pollfds)
{
	int i;

	for(i=0; i<db->context_count; i++){
		if(db->contexts[i] && db->contexts[i]->sock != INVALID_SOCKET){
			if(pollfds[db->contexts[i]->pollfd_index].revents & (POLLERR | POLLNVAL)){
				do_disconnect(db, i);
			}
		}
	}
}

static void loop_handle_reads_writes(
		struct mosquitto_db * db,
		struct pollfd * pollfds)
{
	int i;

	for (i = 0; i < db->context_count; i++) {

		if (db->contexts[i] && db->contexts[i]->sock != INVALID_SOCKET) {

			assert(pollfds[db->contexts[i]->pollfd_index].fd == db->contexts[i]->sock);
#ifdef WITH_TLS
			if (pollfds[db->contexts[i]->pollfd_index].revents & POLLOUT ||
					db->contexts[i]->want_write ||
					(db->contexts[i]->ssl && db->contexts[i]->state
							== mosq_cs_new)) {
#else
			if (pollfds[db->contexts[i]->pollfd_index].revents & POLLOUT) {
#endif
				if (_mosquitto_packet_write(db->contexts[i])) {
					do_disconnect(db, i);
				}
			}
		}

		if (db->contexts[i] && db->contexts[i]->sock != INVALID_SOCKET) {

			assert(pollfds[db->contexts[i]->pollfd_index].fd == db->contexts[i]->sock);
#ifdef WITH_TLS

			if (pollfds[db->contexts[i]->pollfd_index].revents & POLLIN ||
					(db->contexts[i]->ssl && db->contexts[i]->state == mosq_cs_new)){
#else
			if (pollfds[db->contexts[i]->pollfd_index].revents & POLLIN) {
#endif

				// TODO: to fully understand this, one must understand what
				// exactly is the variable db->contexts[i]. what does this
				// context represent? the broker itself? the client which
				// has just sent the packet?

				// the answer is in net.c:436, method xmqtt3_socket_accept():
				// for every accepted connection, the broker creates a new
				// context and adds it to the db, therefore, here the
				// broker cycles through all active mosquitto connections
				// which have been verified by poll() to generate a POLLIN
				// event (up above we have the same for POLLOUT events).
				if (_mosquitto_packet_read(db, db->contexts[i])) {
					do_disconnect(db, i);
				}
			}
		}

		if (db->contexts[i] && db->contexts[i]->sock != INVALID_SOCKET) {
			if (pollfds[db->contexts[i]->pollfd_index].revents
					& (POLLERR | POLLNVAL)) {
				do_disconnect(db, i);
			}
		}
	}
}
