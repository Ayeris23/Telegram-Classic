#include "queue.h"
#include "../mtx/include/api.h"
#include "../mtx/include/buf.h"
#include "../mtx/include/setup.h"
#include "../mtx/include/types.h"
#include "net.h"
#include "transport.h"
#include "../tl/alloc.h"
#include "list.h"
#include "tg.h"
#include "updates.h"
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include "stb_ds.h"
#include "errors.h"
#include <errno.h>

#if INTPTR_MAX == INT32_MAX
    #define THIS_IS_32_BIT_ENVIRONMENT
		#define _LD_ "%lld"
#elif INTPTR_MAX == INT64_MAX
    #define THIS_IS_64_BIT_ENVIRONMENT
		#define _LD_ "%ld"
#else
    #error "Environment not 32 or 64-bit."
#endif

enum RTL{
	RTL_EXIT,   // exit loop
	RTL_ERROR,  // error
	RTL_REREAD, // read socket again
	RTL_RESEND, // resend query
};

static void tg_send_query_sync_cb(void *d, const tl_t *tl);

static int cmp_msgid(void *msgidp, void *itemp)
{
	tg_queue_t *item = itemp;
	uint64_t *msgid = msgidp;
	if (*msgid == item->msgid)
		return 1;
	return 0;
}

static tg_queue_t * tg_queue_cut(tg_t *tg, uint64_t msg_id)
{
	tg_queue_t *queue = NULL;
	assert(tg);

	tg_mutex_lock(tg, &tg->queue_lock, 
			ON_ERR(tg, "%s: can't lock queue list", __func__);
			return queue);
	queue = list_cut(
				&tg->queue, 
				&msg_id, 
				cmp_msgid);
	tg_mutex_unlock(&tg->queue_lock);
	return queue;	
}

static void tg_queue_add(tg_t *tg, tg_queue_t * queue)
{
	assert(tg);
	assert(queue);

	tg_mutex_lock(tg, &tg->queue_lock, 
			ON_ERR(tg, "%s: can't lock queue list", __func__);
			return);
	list_add(&tg->queue, queue);
	tg_mutex_unlock(&tg->queue_lock);
}

static int flood_wait_for_seconds(
		tg_queue_t *queue, int seconds)
{
	ON_ERR(queue->tg, 
					"You are blocked for flooding. Wait %.2d:%.2d:%2d",
					seconds/3600, seconds%3600/60, seconds%3600%60);
					
	// sleep and resend query
	sleep(seconds);

	// resend queue
	tg_queue_new(
			queue->tg, 
			&queue->query, 
			queue->ip,
			queue->port,
			queue->multithread,
			queue->userdata, 
			queue->on_done, 
			queue->progressp, 
			queue->progress);
	
	return 0;
}

static int migrate_to_dc(
		tg_queue_t *queue, const struct dc_t *dc, tl_t *tl)
{
	ON_ERR(queue->tg, "%s: MIGRATE TO: %d", 
			__func__, dc->number);
	// get ip
	const char *ip = 
		tg_ip_address_for_dc(queue->tg, dc->number);
	if (ip == NULL){
		ON_ERR(queue->tg, "%s: can't get ip from config", __func__);
		ip = dc->ipv4;
	}

	// check if user/phone migrate
	if (tg_error_file_migrate(queue->tg, RPC_ERROR(tl)) == NULL)
	{
		// change main ip address
		tg_set_server_address(queue->tg, ip, queue->tg->port);
		ip_address_to_database(queue->tg, ip);
	}

	// resend queue
	tg_queue_new(
			queue->tg, 
			&queue->query, 
			ip,
			queue->port,
			queue->multithread,
			queue->userdata, 
			queue->on_done, 
			queue->progressp, 
			queue->progress);
	
	return 0;
}

static void catched_tl(tg_t *tg, uint64_t msg_id, tl_t *tl)
{
	tg_queue_t *queue = NULL;

	assert(tg);
	ON_LOG(tg, "%s", __func__);

	// get queue
	queue = tg_queue_cut(tg, msg_id);
	if (queue == NULL){
		ON_ERR(tg, "can't find queue for msg_id: "_LD_" with tl: %s"
				, msg_id, tl?TL_NAME_FROM_ID(tl->_id):"NULL");
		// drop answer
		//tg_add_todrop(tg, msg_id);
		return;
	}

	ON_ERR(tg, "%s: GOT queue for msg_id: "_LD_"!"
				, __func__, msg_id);

	// check if in loop
	if (!queue->loop)
		return;

	// lock queue
	tg_mutex_lock(tg, &queue->lock, 
			ON_LOG(tg, "%s: can't lock queue", __func__);
			return);

	if (tl == NULL){
		ON_ERR(tg, "%s: tl is NULL", __func__);
		if (queue->on_done)
			queue->on_done(queue->userdata, tl);
		tg_mutex_unlock(&queue->lock); // unlock
		return;
	}
		
	ON_LOG(tg, "%s: %s", __func__, TL_NAME_FROM_ID(tl->_id));

	switch (tl->_id) {
		case id_gzip_packed:
			{
				// handle gzip
				tl_gzip_packed_t *obj =
					(tl_gzip_packed_t *)tl;

				tl_t *ttl = NULL;
				buf_t buf;
				int _e = gunzip_buf(&buf, obj->packed_data_);
				if (_e)
				{
					char *err = gunzip_buf_err(_e);
					ON_ERR(tg, "%s: %s", __func__, err);
					free(err);
				} else {
					ttl = tl_deserialize(&buf);
					buf_free(buf);
				}
		
				if (queue->on_done)
					queue->on_done(queue->userdata, ttl);
				if (ttl)
					tl_free(ttl);

				queue->loop = false; // stop receive data!
				tg_mutex_unlock(&queue->lock); // unlock
				return; // do not run on_done!
			}
			break;
		case id_bad_msg_notification:
			{
				tl_bad_msg_notification_t *obj = 
					(tl_bad_msg_notification_t *)tl;
				// handle bad msg notification
				char *err = tg_strerr(tl);
				ON_ERR(queue->tg, "%s", err);
				free(err);
				tl = NULL;
				// add time diff
				tg_mutex_lock(tg, &queue->tg->seqnm, break);
				queue->tg->timediff = ntp_time_diff();
			  tg_mutex_unlock(&queue->tg->seqnm);
			}
			break;
		case id_rpc_error:
        {
            const char *error_message = RPC_ERROR(tl);
            
            printf("QUEUE: RPC_ERROR for msgid=%llu: %s\n",
                   (unsigned long long)msg_id,
                   error_message ? error_message : "(null)");
            fflush(stdout);
            
            /*
             * Synchronous calls must receive the RPC error through their
             * callback. Otherwise tg_send_query_sync() returns NULL while a
             * replacement queue retains a pointer to its expired stack.
             *
             * auth.c already handles PHONE_MIGRATE_* and can retry safely.
             */
            if (queue->on_done == tg_send_query_sync_cb) {
                break;
            }
            
            /*
             * Preserve the old automatic behavior for genuinely asynchronous
             * callers.
             */
            const struct dc_t *dc =
            tg_error_migrate(tg, error_message);
            
            if (dc) {
                ON_LOG(queue->tg,
                       "%s: %s",
                       __func__,
                       error_message);
                
                migrate_to_dc(queue, dc, tl);
                
                queue->loop = false;
                tg_mutex_unlock(&queue->lock);
                return;
            }
            
            int wait =
            tg_error_flood_wait(tg, error_message);
            
            if (wait) {
                ON_LOG(queue->tg,
                       "%s: %s",
                       __func__,
                       error_message);
                
                flood_wait_for_seconds(queue, wait);
                
                queue->loop = false;
                tg_mutex_unlock(&queue->lock);
                return;
            }
            
            char *err = tg_strerr(tl);
            
            if (err) {
                ON_ERR(queue->tg, "%s: %s", __func__, err);
                free(err);
            }
            
            /*
             * Fall through to the common queue->on_done call after the switch.
             */
        }
            break;
		
		default:
			break;
	}

	if (queue->on_done)
		queue->on_done(queue->userdata, tl);
	
	// stop query
	queue->loop = false;

	tg_mutex_unlock(&queue->lock); // unlock
}

static void handle_tl(tg_queue_t *queue, tl_t *tl)
{
	int i;
	if (tl == NULL){
		ON_ERR(queue->tg, "%s: tl is NULL", __func__);
		return;
	}
	ON_LOG(queue->tg, "%s: %s", __func__, 
			TL_NAME_FROM_ID(tl->_id));

	switch (tl->_id) {
		case id_gzip_packed:
            {
                tl_gzip_packed_t *obj =
                (tl_gzip_packed_t *)tl;
                
                buf_t buf;
                tl_t *ttl = NULL;
                int error;
                
                memset(&buf, 0, sizeof(buf));
                
                error = gunzip_buf(&buf, obj->packed_data_);
                
                if (error != 0) {
                    char *description = gunzip_buf_err(error);
                    
                    ON_ERR(queue->tg,
                           "%s: %s",
                           __func__,
                           description ? description : "gzip unpack failed");
                    
                    if (description)
                        free(description);
                    
                    break;
                }
                
                ttl = tl_deserialize(&buf);
                buf_free(buf);
                
                if (ttl == NULL) {
                    ON_ERR(queue->tg,
                           "%s: couldn't deserialize gzip payload",
                           __func__);
                    break;
                }
                
                handle_tl(queue, ttl);
                tl_free(ttl);
            }
            break;
		case id_msg_container:
			{
				tl_msg_container_t *container = 
					(tl_msg_container_t *)tl; 
				/*ON_LOG_BUF(queue->tg, container->_buf, "CONTAINER: ");*/
				ON_LOG(queue->tg, "%s: container %d long", 
						__func__, container->messages_len);
				for (i = 0; i < container->messages_len; ++i) {
					mtp_message_t m = container->messages_[i];
					// add to ack
					tg_add_msgid(queue->tg, m.msg_id);
					
					tl_t *ttl = tl_deserialize(&m.body);
					handle_tl(queue, ttl);
					if (ttl)
						tl_free(ttl);
				}
			}
			break;
		case id_new_session_created:
			{
				tl_new_session_created_t *obj = 
					(tl_new_session_created_t *)tl;
				// handle new session
				ON_LOG(queue->tg, "new session created...");
			}
			break;
		case id_pong:
			{
				tl_pong_t *obj = 
					(tl_pong_t *)tl;
				// handle pong
				ON_LOG(queue->tg, "pong...");
			}
			break;
		case id_bad_msg_notification:
			{
				tl_bad_msg_notification_t *obj = 
					(tl_bad_msg_notification_t *)tl;
				// handle bad msg notification
				char *err = tg_strerr(tl);
				ON_ERR(queue->tg, "%s", err);
				free(err);
				// add time diff
				tg_mutex_lock(queue->tg, &queue->tg->seqnm, break);
				queue->tg->timediff = ntp_time_diff();
				tg_mutex_unlock(&queue->tg->seqnm);
			}
			break;
		case id_rpc_error:
			{
				// check file/user/phone migrate
				const struct dc_t *dc = 
					tg_error_migrate(queue->tg, RPC_ERROR(tl));
				if (dc){
					ON_LOG(queue->tg, "%s: %s", __func__, RPC_ERROR(tl));
					// check if user/phone migrate
					migrate_to_dc(queue, dc, tl);
					queue->loop = false; // stop receive data!
					break;
				}

				// check flood wait
				int wait = tg_error_flood_wait(queue->tg, RPC_ERROR(tl));
				if (wait){
					ON_LOG(queue->tg, "%s: %s", __func__, RPC_ERROR(tl));
					flood_wait_for_seconds(queue, wait);
					queue->loop = false; // stop receive data!
					break;
				}

				// print error
				ON_ERR(queue->tg, "%s: %s", __func__, RPC_ERROR(tl));
			}
			break;
		case id_msgs_ack:
			{
				tl_msgs_ack_t *msgs_ack = 
					(tl_msgs_ack_t *)tl;
				/*ON_LOG_BUF(queue->tg, tl->_buf, "GOT ACK:");*/
			}
			break;
		case id_msg_detailed_info:
			{
				tl_msg_detailed_info_t *di = 
					(tl_msg_detailed_info_t *)tl;
				catched_tl(queue->tg, di->answer_msg_id_, NULL);
			}
			break;
		case id_msg_new_detailed_info:
			{
				tl_msg_new_detailed_info_t *di = 
					(tl_msg_new_detailed_info_t *)tl;
				catched_tl(queue->tg, di->answer_msg_id_, NULL);
			}
			break;
		case id_rpc_result:
        {
            tl_rpc_result_t *rpc_result =
            (tl_rpc_result_t *)tl;
            
            printf("QUEUE: rpc_result req_msg_id=%llu, result=%p\n",
                   (unsigned long long)rpc_result->req_msg_id_,
                   (void *)rpc_result->result_);
            
            if (rpc_result->result_ != NULL) {
                printf("QUEUE: nested result id=0x%08x\n",
                       (unsigned int)rpc_result->result_->_id);
            } else {
                unsigned int i;
                unsigned int count =
                rpc_result->_buf.size < 32
                ? (unsigned int)rpc_result->_buf.size
                : 32;
                
                printf("QUEUE: nested result was not deserialized\n");
                printf("QUEUE: rpc_result first %u bytes:", count);
                
                for (i = 0; i < count; ++i) {
                    printf(" %02x",
                           (unsigned int)rpc_result->_buf.data[i]);
                }
                
                printf("\n");
            }
            
            fflush(stdout);
            
            catched_tl(queue->tg,
                       rpc_result->req_msg_id_,
                       rpc_result->result_);
        }
            break;
		case id_updatesTooLong: case id_updateShort:
		case id_updateShortMessage: case id_updateShortChatMessage:
		case id_updateShortSentMessage: case id_updatesCombined:
		case id_updates:
			{
				// do updates
				tg_do_updates(queue->tg, tl);
			}
			break;;

		default:
			break;
	}
}

static int tg_recv_exact(
                         tg_queue_t *queue,
                         int sockfd,
                         void *destination,
                         size_t expected)
{
    unsigned char *output = destination;
    size_t received = 0;
    
    while (received < expected) {
        ssize_t count = recv(
                             sockfd,
                             output + received,
                             expected - received,
                             0);
        
        if (count > 0) {
            received += (size_t)count;
            continue;
        }
        
        if (count == 0) {
            ON_ERR(queue->tg,
                   "%s: server closed the connection",
                   __func__);
            return -1;
        }
        
        if (errno == EINTR)
            continue;
        
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            ON_ERR(queue->tg,
                   "%s: receive timed out",
                   __func__);
        } else {
            ON_ERR(queue->tg,
                   "%s: recv failed: errno=%d (%s)",
                   __func__,
                   errno,
                   strerror(errno));
        }
        
        return -1;
    }
    
    return 0;
}

static enum RTL _tg_receive(tg_queue_t *queue, int sockfd)
{
	ON_LOG(queue->tg, "%s", __func__);
	buf_t r = buf_new();
	// get length of the package
	uint32_t len = 0;
    
    if (tg_recv_exact(queue, sockfd, &len, sizeof(len)) != 0) {
        buf_free(r);
        return RTL_ERROR;
    }

	ON_LOG(queue->tg, "%s: prepare to receive len: %d", __func__, len);
	if (len < 0) {
		// this is error - report it
		ON_ERR(queue->tg, "%s: received wrong length: %d", __func__, len);
		buf_free(r);
		return RTL_ERROR;
	}
    
    if (len == 0 || len > 16 * 1024 * 1024) {
        ON_ERR(queue->tg,
               "%s: invalid response length: %u",
               __func__,
               len);
        buf_free(r);
        return RTL_ERROR;
    }

	// realloc buf to be enough size
	if (buf_realloc(&r, len)){
		// handle error
		ON_ERR(queue->tg, "%s: error buf realloc to size: %d", __func__, len);
		buf_free(r);
		return RTL_ERROR;
	}

	// get data
	if (tg_recv_exact(queue, sockfd, r.data, len) != 0) {
        buf_free(r);
        return RTL_ERROR;
    }
    
    if (queue->progress) {
        if (queue->progress(queue->progressp, len, len)) {
            buf_free(r);
            tg_add_todrop(queue->tg, queue->msgid);
            return RTL_EXIT;
        }
    }

	// get payload 
	r.size = len;
	if (r.size == 4 && buf_get_ui32(r) == 0xfffffe6c){
		buf_free(r);
		ON_ERR(queue->tg, "%s: 404 ERROR", __func__);
		return RTL_ERROR;
	}

	// decrypt
	buf_t d = tg_decrypt(queue->tg, r, true);
	if (!d.size){
		buf_free(r);
		return RTL_ERROR;
	}
	buf_free(r);

	// deheader
	buf_t msg = tg_deheader(queue->tg, d, true);
	buf_free(d);

	// deserialize 
	tl_t *tl = tl_deserialize(&msg);
    buf_free(msg);
    
    if (tl == NULL) {
        ON_ERR(queue->tg,
               "%s: couldn't deserialize response",
               __func__);
        
        return RTL_ERROR;
    }

	// check server salt
	if (tl->_id == id_bad_server_salt){
		ON_LOG(queue->tg, "BAD SERVER SALT: resend query");
        tl_free(tl);
		// resend query
		return RTL_RESEND;
	}

	// handle tl
	handle_tl(queue, tl);
	if (tl)
		tl_free(tl);
	
	return RTL_REREAD; // read again
}

static void tg_send_ack(void *data)
{
	tg_queue_t *queue = data;
	ON_LOG(queue->tg, "%s", __func__);
	
	// send ACK
	buf_t ack = tg_ack(queue->tg);	
	if (ack.size < 1){
		buf_free(ack);
		return;
	}
	buf_t query = tg_prepare_query(
			queue->tg, &ack, true, NULL);
	buf_free(ack);

	int s = 
		send(queue->socket, query.data, query.size, 0);
	buf_free(query);
	
	if (s < 0){
		ON_ERR(queue->tg, "%s: socket error", __func__);
		pthread_mutex_unlock(&queue->tg->msgidsm);
		return;
	}
}

static void tg_prepare_mtproto(tg_queue_t *queue)
{
	tg_t *tg = queue->tg;
	if (!tg->key.size){
		app_t app = api.app.open(queue->tg->ip, queue->tg->port);	
		tg->key = 
			buf_add(shared_rc.key.data, shared_rc.key.size);
		tg->salt = 
			buf_add(shared_rc.salt.data, shared_rc.salt.size);
		queue->socket = shared_rc.net.sockfd;
		tg->seqn = shared_rc.seqnh + 1;	
	}

	if (!tg->ssid.size)
		tg->ssid = buf_rand(8);

	if (!queue->tg->salt.size)
		tg->salt = buf_rand(8);
}

static int tg_send_all(
                       int sockfd,
                       const void *data,
                       size_t size)
{
    const unsigned char *bytes =
    (const unsigned char *)data;
    
    size_t sent_total = 0;
    
    while (sent_total < size) {
        ssize_t sent =
        send(sockfd,
             bytes + sent_total,
             size - sent_total,
             0);
        
        if (sent > 0) {
            sent_total += (size_t)sent;
            continue;
        }
        
        if (sent < 0 && errno == EINTR)
            continue;
        
        return -1;
    }
    
    return 0;
}

static int tg_send(void *data)
{
	// send query
	tg_queue_t *queue = data;
	tg_t *tg = queue->tg;
	ON_LOG(queue->tg, "%s", __func__);
		
	// prepare protocol
	tg_mutex_lock(tg, &tg->queue_lock,
		ON_ERR(tg, "%s: can't lock mutex", __func__);
		return 1);
	tg_prepare_mtproto(queue);
	tg_mutex_unlock(&tg->queue_lock);
	
	// prepare query
	buf_t b = tg_prepare_query(
			tg, 
			&queue->query, 
			true, 
			&queue->msgid);
	if (!b.size)
	{
		buf_free(b);
		tg_net_close(tg, queue->socket);
		return 1;
	}

	// log
	/*ON_LOG(queue->tg, "%s: %s, msgid: "_LD_"", */
	ON_ERR(tg, "%s: %s, msgid: "_LD_"", 
			__func__, 
			TL_NAME_FROM_ID(buf_get_ui32(queue->query)), 
			queue->msgid);
		
	// send query
    if (tg_send_all(queue->socket, b.data, b.size) != 0) {
        int socket_error = errno;
        
        ON_ERR(tg,
               "%s: send failed: errno=%d (%s)",
               __func__,
               socket_error,
               strerror(socket_error));
        
        buf_free(b);
        
        if (queue->socket >= 0) {
            int socket_to_close = queue->socket;
            queue->socket = -1;
            tg_net_close(tg, socket_to_close);
        }
        
        return 1;
    }
	
	buf_free(b);
	return 0;
}

static void tg_queue_free(tg_queue_t *queue)
{
    if (queue == NULL)
        return;
    
    buf_free(queue->query);
    
    pthread_mutex_destroy(&queue->lock);
    pthread_mutex_destroy(&queue->inloop_lock);
    
    free(queue);
}

static void *tg_run_queue_exit(tg_queue_t *queue, void *ret)
{
	tg_mutex_unlock(&queue->inloop_lock);
	return ret;
}

static void *tg_run_queue(void *data)
{
    tg_queue_t *queue = data;
    tg_t *tg = queue->tg;
    tg_queue_t *unfinished = NULL;
    
    tg_mutex_lock(
                  tg,
                  &queue->inloop_lock,
                  ON_ERR(tg, "%s: can't lock queue loop", __func__);
                  return NULL);
    
    queue->socket = tg_net_open(tg, queue->ip, queue->port);
    
    if (queue->socket < 0) {
        ON_ERR(tg, "%s: can't open socket", __func__);
        
        queue->loop = false;
        
        if (queue->on_done)
            queue->on_done(queue->userdata, NULL);
        
        return tg_run_queue_exit(queue, NULL);
    }
    
    tg_queue_add(tg, queue);
    
    if (tg_send(data) != 0) {
        queue->loop = false;
    } else {
        enum RTL res;
        
        while (queue->loop) {
            res = _tg_receive(queue, queue->socket);
            
            if (res == RTL_RESEND) {
                if (tg_send(data) == 0)
                    continue;
                
                break;
            }
            
            if (res == RTL_EXIT || res == RTL_ERROR)
                break;
        }
    }
    
    queue->loop = false;
    
    if (queue->socket >= 0) {
        int socket_to_close = queue->socket;
        queue->socket = -1;
        tg_net_close(tg, socket_to_close);
    }
    
    /*
     * A successful RPC result is removed by catched_tl().
     * If it is still present here, no completion result arrived.
     */
    unfinished = tg_queue_cut(tg, queue->msgid);
    
    if (unfinished == queue && queue->on_done) {
        queue->on_done(queue->userdata, NULL);
    }
    
    return tg_run_queue_exit(queue, NULL);
}

static void *tg_run_timer(void *data)
{
    tg_queue_t *queue = data;
    tg_t *tg = queue->tg;
    
    sleep(30);
    
    /*
     * If this succeeds, the worker already finished. This is normal
     * cleanup, not a timeout.
     */
    if (pthread_mutex_trylock(&queue->inloop_lock) == 0) {
        pthread_mutex_unlock(&queue->inloop_lock);
        tg_queue_free(queue);
        return NULL;
    }
    
    /*
     * The worker is still active after 30 seconds.
     */
    ON_ERR(tg, "%s: query timed out", __func__);
    
    queue->loop = false;
    
    if (queue->socket >= 0) {
        shutdown(queue->socket, SHUT_RDWR);
    }
    
    /*
     * Wait until the worker exits before freeing its queue object.
     */
    pthread_mutex_lock(&queue->inloop_lock);
    pthread_mutex_unlock(&queue->inloop_lock);
    
    tg_queue_free(queue);
    return NULL;
}

tg_queue_t * tg_queue_new(
		tg_t *tg, buf_t *query, 
		const char *ip, int port, bool multithread,
		void *userdata, void (*on_done)(void *userdata, const tl_t *tl),
		void *progressp, 
		int (*progress)(void *progressp, int size, int total))
{
	ON_LOG(tg, "%s", __func__);
	tg_queue_t *queue = NEW(tg_queue_t, 
			ON_ERR(tg, "%s: can't allocate memory", __func__);
			return NULL;);

	if (pthread_mutex_init(&queue->inloop_lock, NULL)){
		ON_ERR(tg, "%s: can't init mutex", __func__);
		return NULL;
	}
	if (pthread_mutex_init(&queue->lock, NULL)){
		ON_ERR(tg, "%s: can't init mutex", __func__);
		return NULL;
	}

	queue->socket = -1;
    queue->tg = tg;
    queue->loop = true;
	queue->query = buf_add_buf(*query);
	strncpy(queue->ip, ip, sizeof(queue->ip)-1);
	queue->port = port;
	queue->multithread = multithread,
	queue->userdata = userdata;
	queue->on_done = on_done;
	queue->progressp = progressp;
	queue->progress = progress;

	// Start the query worker.
    // queue->p must remain the handle for this worker because
    // tg_send_query_sync() joins it.
    int worker_error = pthread_create(
                                      &queue->p,
                                      NULL,
                                      tg_run_queue,
                                      queue);
    
    if (worker_error != 0)
    {
        ON_ERR(tg,
               "%s: can't create query thread: %d",
               __func__,
               worker_error);
        
        buf_free(queue->query);
        pthread_mutex_destroy(&queue->lock);
        pthread_mutex_destroy(&queue->inloop_lock);
        free(queue);
        
        return NULL;
    }
    
    // The timeout thread needs its own pthread_t.
    // Do not overwrite queue->p.
    pthread_t timer_thread;
    
    int timer_error = pthread_create(
                                     &timer_thread,
                                     NULL,
                                     tg_run_timer,
                                     queue);
    
    if (timer_error != 0)
    {
        ON_ERR(tg,
               "%s: can't create timer thread: %d",
               __func__,
               timer_error);
        
        /*
         * Continue for now. The query can still complete, but this
         * particular queue will not have its 60-second timeout cleanup.
         */
    }
    else
    {
        /*
         * Nobody joins the timer thread. Detaching prevents its pthread
         * resources from remaining allocated after it exits.
         */
        pthread_detach(timer_thread);
    }
    
    return queue;
}

pthread_t tg_send_query_async_with_progress(
		tg_t *tg, buf_t *query, bool multithread,
		void *userdata, void (*callback)(void *userdata, const tl_t *tl),
		void *progressp, 
		int (*progress)(void *progressp, int size, int total))
{
	ON_LOG(tg, "%s: tg: %p, query: %p, userdata: %p, callback: %p"
			       "progressp: %p, progress: %p",
		 	__func__, tg, query, userdata, callback, progressp, progress);
	tg_queue_t *queue =
    tg_queue_new(
                 tg,
                 query,
                 tg->ip,
                 tg->port,
                 multithread,
                 userdata,
                 callback,
                 progressp,
                 progress);
    
    if (queue == NULL)
    {
        ON_ERR(tg, "%s: couldn't create queue", __func__);
        return (pthread_t)0;
    }
    
    return queue->p;
}

pthread_t tg_send_query_async(
		tg_t *tg, buf_t *query, bool multithread,
		void *userdata, void (*callback)(void *userdata, const tl_t *tl))
{
	ON_LOG(tg, "%s: tg: %p, query: %p, userdata: %p, callback: %p",
		 	__func__, tg, query, userdata, callback);
	return tg_send_query_async_with_progress(
			tg, query, multithread, 
			userdata, callback,
			NULL, NULL);
}

static void tg_send_query_sync_cb(void *d, const tl_t *tl)
{
    tl_t **tlp = (tl_t **)d;
    
    printf("QUEUE: sync callback entered, input=%p\n", (void *)tl);
    fflush(stdout);
    
    if (tlp == NULL)
        return;
    
    *tlp = NULL;
    
    if (tl == NULL) {
        printf("QUEUE: callback received NULL nested result\n");
        fflush(stdout);
        return;
    }
    
    printf("QUEUE: callback input id=0x%08x, raw size=%u\n",
           (unsigned int)tl->_id,
           (unsigned int)tl->_buf.size);
    fflush(stdout);
    
    *tlp = tl_deserialize((buf_t *)&tl->_buf);
    
    printf("QUEUE: callback deserialized result=%p\n",
           (void *)*tlp);
    
    if (*tlp != NULL) {
        printf("QUEUE: callback output id=0x%08x\n",
               (unsigned int)(*tlp)->_id);
    }
    
    fflush(stdout);
}

tl_t *tg_send_query_sync_with_progress(
                                       tg_t *tg,
                                       buf_t *query,
                                       void *progressp,
                                       int (*progress)(void *progressp, int size, int total))
{
    tl_t *tl = NULL;
    
    pthread_t p =
    tg_send_query_async_with_progress(
                                      tg,
                                      query,
                                      false,
                                      &tl,
                                      tg_send_query_sync_cb,
                                      progressp,
                                      progress);
    
    if (!p)
    {
        ON_ERR(tg, "%s: query thread was not created", __func__);
        return NULL;
    }
    
    int join_error = pthread_join(p, NULL);
    
    if (join_error != 0)
    {
        ON_ERR(tg,
               "%s: pthread_join failed: %d",
               __func__,
               join_error);
        
        return NULL;
    }
    
    ON_LOG(tg,
           "%s got tl: %s",
           __func__,
           tl ? TL_NAME_FROM_ID(tl->_id) : "NULL");
    
    return tl;
}

tl_t *tg_send_query_sync(tg_t *tg, buf_t *query)
{
    tl_t *tl = NULL;
    
    pthread_t p =
    tg_send_query_async(
                        tg,
                        query,
                        false,
                        &tl,
                        tg_send_query_sync_cb);
    
    if (!p)
    {
        ON_ERR(tg, "%s: query thread was not created", __func__);
        return NULL;
    }
    
    int join_error = pthread_join(p, NULL);
    
    if (join_error != 0)
    {
        ON_ERR(tg,
               "%s: pthread_join failed: %d",
               __func__,
               join_error);
        
        return NULL;
    }
    
    ON_LOG(tg,
           "%s got tl: %s",
           __func__,
           tl ? TL_NAME_FROM_ID(tl->_id) : "NULL");
    
    return tl;
}

void tg_queue_cancell_all(tg_t *tg)
{
	tg_queue_t *queue = NULL;
	ON_LOG(tg, "%s", __func__);
	assert(tg);

	tg_mutex_lock(tg, &tg->queue_lock, 
			ON_ERR(tg, "%s: can't lock queue list", __func__);
			return);
	
	list_for_each(tg->queue, queue)
	{
		if (pthread_mutex_trylock(&queue->lock) == 0){
			queue->loop = false;
			tg_mutex_unlock(&queue->lock);
		}
	}

	list_free(&tg->queue);
	tg_mutex_unlock(&tg->queue_lock);
}

int tg_queue_cancell_queue(tg_t *tg, uint64_t msg_id)
{
	tg_queue_t *queue = tg_queue_cut(tg, msg_id);
	if (queue == NULL){
		ON_ERR(tg, "%s: can't find queue for msg_id: "_LD_""
				, __func__, msg_id);
		return 1;
	}

	// stop query
	tg_mutex_lock(tg, &queue->lock, 
		ON_ERR(tg, "%s: can't lock queue with msg_id: "_LD_"",
			__func__, msg_id);
		return 1);
	queue->loop = false;
	tg_mutex_unlock(&queue->lock); // unlock
	
	return 0;
}

tl_t *tg_send_query(tg_t *tg, buf_t *query){
	return tg_send_query_sync(tg, query);
}
