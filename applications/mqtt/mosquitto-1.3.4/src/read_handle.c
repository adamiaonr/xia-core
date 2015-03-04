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

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "config.h"

#include "mosquitto_broker.h"
#include "mqtt3_protocol.h"
#include "memory_mosq.h"
#include "read_handle.h"
#include "send_mosq.h"
#include "util_mosq.h"

#ifdef WITH_SYS_TREE
extern uint64_t g_pub_bytes_received;
#endif

int mqtt3_packet_handle(struct mosquitto_db * db, struct mosquitto * context)
{
	if(!context) return MOSQ_ERR_INVAL;

	switch((context->in_packet.command)&0xF0){
		case PINGREQ:
			return _mosquitto_handle_pingreq(context);
		case PINGRESP:
			return _mosquitto_handle_pingresp(context);
		case PUBACK:
			return _mosquitto_handle_pubackcomp(context, "PUBACK");
		case PUBCOMP:
			return _mosquitto_handle_pubackcomp(context, "PUBCOMP");
		case PUBLISH:
			return mqtt3_handle_publish(db, context);
		case PUBREC:
			return _mosquitto_handle_pubrec(context);
		case PUBREL:
			return _mosquitto_handle_pubrel(db, context);
		case CONNECT:
			return mqtt3_handle_connect(db, context);
		case DISCONNECT:
			return mqtt3_handle_disconnect(db, context);
		case SUBSCRIBE:
			return mqtt3_handle_subscribe(db, context);
		case UNSUBSCRIBE:
			return mqtt3_handle_unsubscribe(db, context);
#ifdef WITH_BRIDGE
		case CONNACK:
			return mqtt3_handle_connack(db, context);
		case SUBACK:
			return _mosquitto_handle_suback(context);
		case UNSUBACK:
			return _mosquitto_handle_unsuback(context);
#endif
		default:
			/* If we don't recognise the command, return an error straight away. */
			return MOSQ_ERR_PROTOCOL;
	}
}

/**
 * \brief handles a publish event in a mosquitto broker, given an incoming
 * packet, accessible via the in_packet field of a mosquitto context (i.e.
 * a representation of a connected client or broker (?)).
 *
 * @param db		the mosquitto broker's database
 * @param context	the context of the connected client which has triggered the
 * 					call to PUBLISH
 * @return an integer, 1 if errors occur, 0 if succesful.
 */
int mqtt3_handle_publish(struct mosquitto_db * db, struct mosquitto * context)
{
	char *topic;
	void *payload = NULL;
	uint32_t payloadlen;
	uint8_t dup, qos, retain;
	uint16_t mid = 0;
	int rc = 0;

	// 1) gather the header's 1st byte from the mosquitto_packet in_packet
	// member of the context struct, i.e. the last incoming packet
	uint8_t header = context->in_packet.command;
	int res = 0;
	struct mosquitto_msg_store * stored = NULL;
	int len;
	char * topic_mount;

#ifdef WITH_BRIDGE
	char * topic_temp;
	int i;
	struct _mqtt3_bridge_topic * cur_topic;
	bool match;
#endif

	// 2) gather the DUP and QOS flags (bytes 3 and 2:1, respectively)
	dup = (header & 0x08)>>3;
	qos = (header & 0x06)>>1;

	// 3) notice how, according to http://ibm.co/1q6Lkgf, the value 3 is
	// reserved and shouldn't be used
	if(qos == 3){
		_mosquitto_log_printf(NULL, MOSQ_LOG_INFO,
				"Invalid QoS in PUBLISH from %s, disconnecting.", context->id);
		return 1;
	}

	// 4) the RETAIN flag: used by MQTT in publish messages to make the broker
	// hold the message even after delivering it to interested subscribers:
	// the idea is for new subscribers to receive the last published message
	// immediately after establishing the subscription
	retain = (header & 0x01);

	// 5) extract the topic from the in_packet member. to do so, one must look
	// into an MQTT packet's variable header. the fixed header's 2nd byte
	// provides the length of variable header + payload.
	if(_mosquitto_read_string(&context->in_packet, &topic)) return 1;

	if(strlen(topic) == 0){
		/* Invalid publish topic, disconnect client. */
		_mosquitto_free(topic);
		return 1;
	}

	if(!strlen(topic)){
		_mosquitto_free(topic);
		return 1;
	}

#ifdef WITH_BRIDGE

	// if the context triggering this PUBLISH call is a bridge (why not
	// testing with the is_bridge flag? it's probably not reliable and
	// may be deprecated in the future), handle its PUBLISH message with
	// prefix and suffix alterations...
	if (context->bridge
			&& context->bridge->topics
			&& context->bridge->topic_remapping) {

		for(i = 0; i < context->bridge->topic_count; i++) {

			cur_topic = &context->bridge->topics[i];

			if ((cur_topic->direction == bd_both
						|| cur_topic->direction == bd_in)
					&& (cur_topic->remote_prefix || cur_topic->local_prefix)) {

				/* Topic mapping required on this topic if the message matches */

				rc = mosquitto_topic_matches_sub(
						cur_topic->remote_topic,
						topic,
						&match);

				if (rc) {

					_mosquitto_free(topic);
					return rc;
				}

				if (match) {
					if (cur_topic->remote_prefix) {
						/* This prefix needs removing. */
						if (!strncmp(
								cur_topic->remote_prefix,
								topic,
								strlen(cur_topic->remote_prefix))) {

							// TODO: pay attention to this piece of code, since
							// it is quite interesting... it removes part of
							// a string, in this case a prefix of some
							// topic. what it does is (1) takes the entire
							// prefix/suffix char *; (2) duplicates the
							// string, adjusting the *topic pointer position
							// to (char *) topic + strlen(prefix), i.e starting
							// from the 's' of prefix/suffix till the
							// terminating character '\0'.
							topic_temp = _mosquitto_strdup(
									topic + strlen(cur_topic->remote_prefix));

							if (!topic_temp) {

								_mosquitto_free(topic);
								return MOSQ_ERR_NOMEM;
							}

							_mosquitto_free(topic);
							topic = topic_temp;
						}
					}

					if (cur_topic->local_prefix) {
						/* This prefix needs adding. */
						len = strlen(topic)
								+ strlen(cur_topic->local_prefix) + 1;

						topic_temp = (char *) _mosquitto_calloc(
								len + 1,
								sizeof(char));

						if (!topic_temp) {
							_mosquitto_free(topic);
							return MOSQ_ERR_NOMEM;
						}

						// TODO: here you concatenate the local prefix with the
						// extracted topic, which we have done just above
						snprintf(
								topic_temp,
								len,
								"%s%s",
								cur_topic->local_prefix,
								topic);

						_mosquitto_free(topic);
						topic = topic_temp;
					}

					break;
				}
			}
		}
	}
#endif

	if (_mosquitto_topic_wildcard_len_check(topic) != MOSQ_ERR_SUCCESS) {
		/* Invalid publish topic, just swallow it. */
		_mosquitto_free(topic);
		return 1;
	}

	if (qos > 0) {
		if (_mosquitto_read_uint16(&context->in_packet, &mid)) {
			_mosquitto_free(topic);
			return 1;
		}
	}

	payloadlen = context->in_packet.remaining_length - context->in_packet.pos;

#ifdef WITH_SYS_TREE
	g_pub_bytes_received += payloadlen;
#endif

	if (context->listener && context->listener->mount_point) {
		len = strlen(context->listener->mount_point) + strlen(topic) + 1;
		topic_mount = (char *) _mosquitto_calloc(len, sizeof(char));

		if (!topic_mount) {

			_mosquitto_free(topic);
			return MOSQ_ERR_NOMEM;
		}

		snprintf(topic_mount, len, "%s%s", context->listener->mount_point, topic);
		_mosquitto_free(topic);
		topic = topic_mount;
	}

	if (payloadlen) {

		if (db->config->message_size_limit
				&& payloadlen > db->config->message_size_limit) {

			_mosquitto_log_printf(
					NULL,
					MOSQ_LOG_DEBUG,
					"Dropped too large PUBLISH from %s (d%d, q%d, r%d, m%d, '%s', ... (%ld bytes))",
					context->id,
					dup,
					qos,
					retain,
					mid,
					topic,
					(long) payloadlen);

			goto process_bad_message;
		}

		payload = _mosquitto_calloc(payloadlen + 1, sizeof(uint8_t));

		if (!payload) {
			_mosquitto_free(topic);
			return 1;
		}

		if (_mosquitto_read_bytes(&context->in_packet, payload, payloadlen)) {
			_mosquitto_free(topic);
			_mosquitto_free(payload);

			return 1;
		}
	}

	/* Check for topic access */
	rc = mosquitto_acl_check(db, context, topic, MOSQ_ACL_WRITE);

	if (rc == MOSQ_ERR_ACL_DENIED) {
		_mosquitto_log_printf(
				NULL,
				MOSQ_LOG_DEBUG,
				"Denied PUBLISH from %s (d%d, q%d, r%d, m%d, '%s', ... (%ld bytes))",
				context->id,
				dup,
				qos,
				retain,
				mid,
				topic,
				(long) payloadlen);

		goto process_bad_message;

	} else if(rc != MOSQ_ERR_SUCCESS) {

		_mosquitto_free(topic);

		if(payload)
			_mosquitto_free(payload);

		return rc;
	}

	_mosquitto_log_printf(
			NULL,
			MOSQ_LOG_DEBUG,
			"Received PUBLISH from %s (d%d, q%d, r%d, m%d, '%s', ... (%ld bytes))",
			context->id,
			dup,
			qos,
			retain,
			mid,
			topic,
			(long) payloadlen);

	// if qos > 0, i.e. the PUBLISH message shall generate notifications
	// to subscribers 'at least once' (qos = 1) or 'exactly once' (qos = 2),
	// we call mqtt3_db_message_store_find(), which attempts to find out if
	// the message ID just published is already stored in the broker (context)

	// TODO: ok, this one confuses me, what if context does not refer to
	// a broker? didn't we just realized that it can happen?
	if (qos > 0) {

		mqtt3_db_message_store_find(context, mid, &stored);
	}

	// if the message isn't stored, store it (no matter the qos level?).
	if (!stored) {

		dup = 0;

		// at this point, memory is allocated to save the actual message in
		// memory, via the creation of a mosquitto_msg_store record. a pointer
		// to that record shall be returned in the argument stored, passed
		// by reference.

		// TODO: what about having references to content objects (e.g.
		// ChunkStatus structs), saved in XIA CID caches, instead of having
		// the content saved at the mosquitto level?
		if (mqtt3_db_message_store(
				db,
				context->id,
				mid,
				topic,
				qos,
				payloadlen,
				payload,
				retain,
				&stored,
				0)) {

			_mosquitto_free(topic);

			if(payload)
				_mosquitto_free(payload);

			return 1;
		}
	} else {

		// if it is, set the 'dup'lication flag
		dup = 1;
	}

	// at this point
	switch(qos) {

		// 1) qos = 0, fire and forget, i.e. don't call
		// _mosquitto_send_puback(), as with qos = 1.
		case 0:

			if (mqtt3_db_messages_queue(
					db,
					context->id,
					topic,
					qos,
					retain,
					stored))

				// note how rc, i.e. the return code, is set to 1, so
				// that mqtt3_context_disconnect() is eventually called by
				// loop_handle_reads_writes() when handling PUBLISH messages...
				rc = 1;

			break;

		case 1:

			if (mqtt3_db_messages_queue(
					db,
					context->id,
					topic,
					qos,
					retain,
					stored))

				rc = 1;

			if (_mosquitto_send_puback(
					context,
					mid))

				rc = 1;

			break;

		case 2:

			if (!dup) {

				res = mqtt3_db_message_insert(
						db,
						context,
						mid,
						mosq_md_in,
						qos,
						retain,
						stored);
			} else {

				res = 0;
			}

			// interesting... we only let rc != 1 if mqtt3_db_message_insert()
			// returns something else than {0, 1}.
			// TODO: explain this later...

			/* mqtt3_db_message_insert() returns 2 to indicate dropped message
			 * due to queue. This isn't an error so don't disconnect them. */
			if(!res) {

				if(_mosquitto_send_pubrec(context, mid))
					rc = 1;

			} else if(res == 1) {

				rc = 1;
			}

			break;
	}

	_mosquitto_free(topic);
	if(payload) _mosquitto_free(payload);

	return rc;

process_bad_message:

	_mosquitto_free(topic);

	if(payload) _mosquitto_free(payload);

	switch(qos){
		case 0:
			return MOSQ_ERR_SUCCESS;
		case 1:
			return _mosquitto_send_puback(context, mid);
		case 2:
			mqtt3_db_message_store_find(context, mid, &stored);
			if(!stored){
				if(mqtt3_db_message_store(db, context->id, mid, NULL, qos, 0, NULL, false, &stored, 0)){
					return 1;
				}
				res = mqtt3_db_message_insert(db, context, mid, mosq_md_in, qos, false, stored);
			}else{
				res = 0;
			}

			if(!res){
				res = _mosquitto_send_pubrec(context, mid);
			}

			return res;
	}

	return 1;
}
