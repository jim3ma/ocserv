/*
 * Copyright (C) 2013 Nikos Mavrogiannopoulos
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netdb.h>
#include <signal.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <gnutls/gnutls.h>
#include <gnutls/crypto.h>
#include <tlslib.h>
#include "ipc.h"
#include <ccan/hash/hash.h>

#include <vpn.h>
#include <main.h>
#include <tlslib.h>

int send_resume_fetch_reply(main_server_st* s, struct proc_st * proc,
		cmd_resume_reply_t r, struct cmd_resume_fetch_reply_st * reply)
{
	struct iovec iov[3];
	uint8_t cmd = RESUME_FETCH_REP;
	struct msghdr hdr;

	memset(&hdr, 0, sizeof(hdr));
	
	iov[0].iov_base = &cmd;
	iov[0].iov_len = 1;
	hdr.msg_iovlen++;

	iov[1].iov_base = reply;
	iov[1].iov_len = 3 + reply->session_data_size;
	hdr.msg_iovlen++;

	hdr.msg_iov = iov;

	return(sendmsg(proc->fd, &hdr, 0));
}

int send_udp_fd(main_server_st* s, struct proc_st * proc, 
		void* cli_addr, socklen_t cli_addr_size, int fd)
{
	struct iovec iov[2];
	uint8_t cmd = CMD_UDP_FD;
	struct msghdr hdr;
	union {
		struct cmsghdr    cm;
		char control[CMSG_SPACE(sizeof(int))];
	} control_un;
	struct cmsghdr  *cmptr;	


	memset(&hdr, 0, sizeof(hdr));
	iov[0].iov_base = &cmd;
	iov[0].iov_len = 1;
	hdr.msg_iovlen++;

	iov[1].iov_base = cli_addr;
	iov[1].iov_len = cli_addr_size;
	hdr.msg_iovlen++;

	hdr.msg_iov = iov;

	hdr.msg_control = control_un.control;
	hdr.msg_controllen = sizeof(control_un.control);
	
	cmptr = CMSG_FIRSTHDR(&hdr);
	cmptr->cmsg_len = CMSG_LEN(sizeof(int));
	cmptr->cmsg_level = SOL_SOCKET;
	cmptr->cmsg_type = SCM_RIGHTS;
	memcpy(CMSG_DATA(cmptr), &fd, sizeof(int));
	
	return(sendmsg(proc->fd, &hdr, 0));
}

int handle_resume_delete_req(main_server_st* s, struct proc_st * proc,
  			   const struct cmd_resume_fetch_req_st * req)
{
tls_cache_st* cache;
struct htable_iter iter;
size_t key;

	key = hash_stable_8(req->session_id, req->session_id_size, 0);

	cache = htable_firstval(&s->tls_db->ht, &iter, key);
	while(cache != NULL) {
		if (req->session_id_size == cache->session_id_size &&
	          memcmp (req->session_id, cache->session_id, req->session_id_size) == 0) {

	          	cache->session_data_size = 0;
	          	cache->session_id_size = 0;
	          
	          	htable_delval(&s->tls_db->ht, &iter);
	          	free(cache);
			s->tls_db->entries--;
	          	return 0;
		}
          	
          	cache = htable_nextval(&s->tls_db->ht, &iter, key);
        }

        return 0;
}

static int ip_cmp(const struct sockaddr_storage *s1, const struct sockaddr_storage *s2, size_t n)
{
	if (((struct sockaddr*)s1)->sa_family == AF_INET) {
		return memcmp(SA_IN_P(s1), SA_IN_P(s2), sizeof(struct in_addr));
	} else { /* inet6 */
		return memcmp(SA_IN6_P(s1), SA_IN6_P(s2), sizeof(struct in6_addr));
	}
}

int handle_resume_fetch_req(main_server_st* s, struct proc_st * proc,
  			   const struct cmd_resume_fetch_req_st * req, 
  			   struct cmd_resume_fetch_reply_st * rep)
{
tls_cache_st* cache;
struct htable_iter iter;
size_t key;

	rep->reply = REP_RESUME_FAILED;

	key = hash_stable_8(req->session_id, req->session_id_size, 0);

	cache = htable_firstval(&s->tls_db->ht, &iter, key);
	while(cache != NULL) {
		if (req->session_id_size == cache->session_id_size &&
	          memcmp (req->session_id, cache->session_id, req->session_id_size) == 0) {

	          	if (proc->remote_addr_len == cache->remote_addr_len &&
		          	ip_cmp(&proc->remote_addr, &cache->remote_addr, proc->remote_addr_len) == 0) {

		          	rep->reply = REP_RESUME_OK;
	          	
		          	memcpy(rep->session_data, cache->session_data, cache->session_data_size);
	        	  	rep->session_data_size = cache->session_data_size;

		          	return 0;
			}
		}

          	cache = htable_nextval(&s->tls_db->ht, &iter, key);
        }

        return 0;

}

int handle_resume_store_req(main_server_st* s, struct proc_st * proc,
  			   const struct cmd_resume_store_req_st * req)
{
tls_cache_st* cache;
size_t key;

	if (req->session_id_size > GNUTLS_MAX_SESSION_ID)
		return -1;
	if (req->session_data_size > MAX_SESSION_DATA_SIZE)
		return -1;

	if ((s->config->max_clients != 0 && 
		s->tls_db->entries >= MAX(s->config->max_clients, DEFAULT_MAX_CACHED_TLS_SESSIONS(s->tls_db))) ||
		(s->config->max_clients == 0 && 
		s->tls_db->entries >= DEFAULT_MAX_CACHED_TLS_SESSIONS(s->tls_db)))
		return -1;

	key = hash_stable_8(req->session_id, req->session_id_size, 0);
	
	cache = malloc(sizeof(*cache));
	if (cache == NULL)
		return -1;
		
	cache->session_id_size = req->session_id_size;
	cache->session_data_size = req->session_data_size;
	cache->remote_addr_len = proc->remote_addr_len;

	memcpy(cache->session_id, req->session_id, req->session_id_size);
	memcpy(cache->session_data, req->session_data, req->session_data_size);
	memcpy(&cache->remote_addr, &proc->remote_addr, proc->remote_addr_len);
	
	htable_add(&s->tls_db->ht, key, cache);
	s->tls_db->entries++;

	return 0;
}

void expire_tls_sessions(main_server_st *s)
{
tls_cache_st* cache;
struct htable_iter iter;
time_t now, exp;

	now = time(0);

	cache = htable_first(&s->tls_db->ht, &iter);
	while(cache != NULL) {
#if GNUTLS_VERSION_NUMBER >= 0x030107
		gnutls_datum_t d;

		d.data = (void*)cache->session_data;
		d.size = cache->session_data_size;

		exp = gnutls_db_check_entry_time(&d);
#else
		exp = 0;
#endif
		if (now-exp > TLS_SESSION_EXPIRATION_TIME) {
	          	cache->session_data_size = 0;
	          	cache->session_id_size = 0;

	          	htable_delval(&s->tls_db->ht, &iter);
	          	free(cache);
			s->tls_db->entries--;
		}
          	cache = htable_next(&s->tls_db->ht, &iter);
        }

        return;
}
