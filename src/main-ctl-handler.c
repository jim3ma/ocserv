/*
 * Copyright (C) 2013 Red Hat
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
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <main.h>
#include <vpn.h>
#include <ip-lease.h>

#include <errno.h>

#ifdef HAVE_DBUS

#include <dbus/dbus.h>
#include <str.h>

static void method_status(main_server_st * s, DBusConnection * conn,
			  DBusMessage * msg);
static void method_list_users(main_server_st * s, DBusConnection * conn,
			      DBusMessage * msg);
static void method_disconnect_user_name(main_server_st * s,
					DBusConnection * conn,
					DBusMessage * msg);
static void method_disconnect_user_id(main_server_st * s, DBusConnection * conn,
				      DBusMessage * msg);
static void method_introspect(main_server_st * s, DBusConnection * conn,
			      DBusMessage * msg);
static void method_stop(main_server_st * s, DBusConnection * conn,
			DBusMessage * msg);
static void method_reload(main_server_st * s, DBusConnection * conn,
			  DBusMessage * msg);
static void method_user_info(main_server_st * s, DBusConnection * conn,
			     DBusMessage * msg);
static void method_id_info(main_server_st * s, DBusConnection * conn,
			   DBusMessage * msg);

typedef void (*method_func) (main_server_st * s, DBusConnection * conn,
			     DBusMessage * msg);

typedef struct {
	char *name;
	unsigned name_size;
	char *iface;
	unsigned iface_size;
	char *desc;
	unsigned desc_size;
	method_func func;
} ctl_method_st;

#define ENTRY(name, iface, desc, func) \
	{name, sizeof(name)-1, iface, sizeof(iface)-1, desc, sizeof(desc)-1, func}

#define LIST_USERS_SIG "(ussssssssuss)"

#define DESC_LIST \
		"    <method name=\"list\">\n" \
		"      <arg name=\"user-info\" direction=\"out\" type=\"a"LIST_USERS_SIG"\"/>\n" \
		"    </method>\n"

#define DESC_USER_INFO \
		"    <method name=\"user_info\">\n" \
		"      <arg name=\"user-info\" direction=\"out\" type=\"a"LIST_USERS_SIG"\"/>\n" \
		"    </method>\n"

/* ID-INFO returns an array of 0 or 1 elements */
#define DESC_ID_INFO \
		"    <method name=\"id_info\">\n" \
		"      <arg name=\"id-info\" direction=\"out\" type=\"a"LIST_USERS_SIG"\"/>\n" \
		"    </method>\n"

#define DESC_DISC_NAME \
		"    <method name=\"disconnect_name\">\n" \
		"      <arg name=\"user-name\" direction=\"in\" type=\"s\"/>\n" \
		"      <arg name=\"status\" direction=\"out\" type=\"b\"/>\n" \
        	"    </method>\n"

#define DESC_RELOAD \
		"    <method name=\"reload\">\n" \
		"      <arg name=\"status\" direction=\"out\" type=\"b\"/>\n" \
        	"    </method>\n"

#define DESC_STOP \
		"    <method name=\"stop\">\n" \
		"      <arg name=\"status\" direction=\"out\" type=\"b\"/>\n" \
        	"    </method>\n"

#define DESC_DISC_ID \
		"    <method name=\"disconnect_id\">\n" \
		"      <arg name=\"user-id\" direction=\"in\" type=\"u\"/>\n" \
		"      <arg name=\"status\" direction=\"out\" type=\"b\"/>\n" \
		"    </method>\n"

#define DESC_STATUS \
		"    <method name=\"status\">\n" \
		"      <arg name=\"status\" direction=\"out\" type=\"b\"/>\n" \
		"      <arg name=\"pid\" direction=\"out\" type=\"u\"/>\n" \
		"      <arg name=\"sec-mod-pid\" direction=\"out\" type=\"u\"/>\n" \
		"      <arg name=\"clients\" direction=\"out\" type=\"u\"/>\n" \
		"    </method>\n"

static const ctl_method_st methods[] = {
	ENTRY("Introspect", "org.freedesktop.DBus.Introspectable", NULL,
	      method_introspect),
	ENTRY("status", "org.infradead.ocserv", DESC_STATUS, method_status),
	ENTRY("reload", "org.infradead.ocserv", DESC_RELOAD, method_reload),
	ENTRY("stop", "org.infradead.ocserv", DESC_RELOAD, method_stop),
	ENTRY("list", "org.infradead.ocserv", DESC_LIST, method_list_users),
	ENTRY("user_info", "org.infradead.ocserv", DESC_USER_INFO, method_user_info),
	ENTRY("id_info", "org.infradead.ocserv", DESC_ID_INFO, method_id_info),
	ENTRY("disconnect_name", "org.infradead.ocserv", DESC_DISC_NAME,
	      method_disconnect_user_name),
	ENTRY("disconnect_id", "org.infradead.ocserv", DESC_DISC_ID,
	      method_disconnect_user_id),
	{NULL, 0, NULL, 0, NULL}
};

static void add_ctl_fd(main_server_st * s, int fd, void *watch, unsigned type)
{
	struct ctl_handler_st *tmp;

	tmp = malloc(sizeof(*tmp));
	if (tmp == NULL)
		return;

	tmp->fd = fd;
	if (dbus_watch_get_enabled(watch))
		tmp->enabled = 1;
	else
		tmp->enabled = 0;
	tmp->watch = watch;
	tmp->type = type;

	mslog(s, NULL, LOG_DEBUG, "dbus: adding %s %swatch for fd: %d",
	      (type == CTL_READ) ? "read" : "write",
	      (tmp->enabled) ? "" : "(disabled) ", fd);

	list_add(&s->ctl_list.head, &(tmp->list));
}

static dbus_bool_t add_watch(DBusWatch * watch, void *data)
{
	int fd = dbus_watch_get_unix_fd(watch);
	main_server_st *s = data;
	unsigned flags;

	flags = dbus_watch_get_flags(watch);

	if (flags & DBUS_WATCH_READABLE) {
		add_ctl_fd(s, fd, watch, CTL_READ);
	} else {
		add_ctl_fd(s, fd, watch, CTL_WRITE);
	}

	return 1;
}

static void remove_watch(DBusWatch * watch, void *data)
{
	main_server_st *s = data;
	struct ctl_handler_st *btmp = NULL, *bpos;

	list_for_each_safe(&s->ctl_list.head, btmp, bpos, list) {
		if (btmp->watch == watch) {
			mslog(s, NULL, LOG_DEBUG,
			      "dbus: removing %s watch for fd: %d",
			      (btmp->type == CTL_READ) ? "read" : "write",
			      btmp->fd);

			list_del(&btmp->list);
			free(btmp);
			return;
		}
	}
}

static void toggle_watch(DBusWatch * watch, void *data)
{
	main_server_st *s = data;
	struct ctl_handler_st *btmp = NULL;

	list_for_each(&s->ctl_list.head, btmp, list) {
		if (btmp->watch == watch) {
			if (dbus_watch_get_enabled(watch)) {
				btmp->enabled = 1;
			} else
				btmp->enabled = 0;

			mslog(s, NULL, LOG_DEBUG,
			      "dbus: %s %s watch for fd: %d",
			      (btmp->enabled) ? "enabling" : "disabling",
			      (btmp->type == CTL_READ) ? "read" : "write",
			      btmp->fd);
			return;
		}
	}
}

#define OCSERV_DBUS_NAME "org.infradead.ocserv"

void ctl_handler_deinit(main_server_st * s)
{
	if (s->config->use_dbus != 0 && s->ctl_ctx != NULL) {
		mslog(s, NULL, LOG_DEBUG, "closing DBUS connection");
		dbus_connection_close(s->ctl_ctx);
		dbus_bus_release_name(s->ctl_ctx, OCSERV_DBUS_NAME, NULL);
		dbus_connection_unref(s->ctl_ctx);
	}
}

/* Initializes unix socket and stores the fd.
 */
int ctl_handler_init(main_server_st * s)
{
	int ret;
	DBusError err;
	DBusConnection *conn;

	if (s->config->use_dbus == 0)
		return 0;

	dbus_error_init(&err);

	conn = dbus_bus_get_private(DBUS_BUS_SYSTEM, &err);
	if (conn == NULL)
		goto error;

	ret = dbus_bus_request_name(conn, OCSERV_DBUS_NAME,
				    DBUS_NAME_FLAG_REPLACE_EXISTING, &err);
	if (ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER)
		goto error;

	s->ctl_ctx = conn;

	if (!dbus_connection_set_watch_functions(conn,
						 add_watch, remove_watch,
						 toggle_watch, s, NULL)) {
		goto error;
	}

	return 0;

 error:
	if (dbus_error_is_set(&err)) {
		fprintf(stderr, "DBUS connection error (%s)", err.message);
		dbus_error_free(&err);
	}
	ctl_handler_deinit(s);

	return ERR_CTL;
}

static void method_status(main_server_st * s, DBusConnection * conn,
			  DBusMessage * msg)
{
	DBusMessage *reply;
	DBusMessageIter args;
	dbus_bool_t status = true;
	dbus_uint32_t tmp;

	mslog(s, NULL, LOG_DEBUG, "ctl: status");

	/* no arguments needed */
	reply = dbus_message_new_method_return(msg);
	if (reply == NULL) {
		mslog(s, NULL, LOG_ERR, "error generating dbus reply");
		return;
	}

	dbus_message_iter_init_append(reply, &args);

	if (dbus_message_iter_append_basic(&args, DBUS_TYPE_BOOLEAN, &status) ==
	    0) {
		mslog(s, NULL, LOG_ERR, "error appending to dbus reply");
		goto error;
	}

	tmp = getpid();
	if (dbus_message_iter_append_basic(&args, DBUS_TYPE_UINT32, &tmp) == 0) {
		mslog(s, NULL, LOG_ERR, "error appending to dbus reply");
		goto error;
	}

	tmp = s->sec_mod_pid;
	if (dbus_message_iter_append_basic(&args, DBUS_TYPE_UINT32, &tmp) == 0) {
		mslog(s, NULL, LOG_ERR, "error appending to dbus reply");
		goto error;
	}

	tmp = s->active_clients;
	if (dbus_message_iter_append_basic(&args, DBUS_TYPE_UINT32, &tmp) == 0) {
		mslog(s, NULL, LOG_ERR, "error appending to dbus reply");
		goto error;
	}

	if (!dbus_connection_send(conn, reply, NULL)) {
		mslog(s, NULL, LOG_ERR, "error sending dbus reply");
		goto error;
	}

 error:
	dbus_message_unref(reply);

	return;
}

static void method_reload(main_server_st * s, DBusConnection * conn,
			  DBusMessage * msg)
{
	DBusMessage *reply;
	DBusMessageIter args;
	dbus_bool_t status = true;

	mslog(s, NULL, LOG_DEBUG, "ctl: reload");

	/* no arguments needed */
	reply = dbus_message_new_method_return(msg);
	if (reply == NULL) {
		mslog(s, NULL, LOG_ERR, "error generating dbus reply");
		return;
	}

	dbus_message_iter_init_append(reply, &args);

	if (dbus_message_iter_append_basic(&args, DBUS_TYPE_BOOLEAN, &status) ==
	    0) {
		mslog(s, NULL, LOG_ERR, "error appending to dbus reply");
		goto error;
	}

	request_reload(0);

	if (!dbus_connection_send(conn, reply, NULL)) {
		mslog(s, NULL, LOG_ERR, "error sending dbus reply");
		goto error;
	}

 error:
	dbus_message_unref(reply);

	return;
}

static void method_stop(main_server_st * s, DBusConnection * conn,
			DBusMessage * msg)
{
	DBusMessage *reply;
	DBusMessageIter args;
	dbus_bool_t status = true;

	mslog(s, NULL, LOG_DEBUG, "ctl: stop");

	/* no arguments needed */
	reply = dbus_message_new_method_return(msg);
	if (reply == NULL) {
		mslog(s, NULL, LOG_ERR, "error generating dbus reply");
		return;
	}

	dbus_message_iter_init_append(reply, &args);

	if (dbus_message_iter_append_basic(&args, DBUS_TYPE_BOOLEAN, &status) ==
	    0) {
		mslog(s, NULL, LOG_ERR, "error appending to dbus reply");
		goto error;
	}

	request_stop(0);

	if (!dbus_connection_send(conn, reply, NULL)) {
		mslog(s, NULL, LOG_ERR, "error sending dbus reply");
		goto error;
	}

 error:
	dbus_message_unref(reply);

	return;
}

static void method_list_users(main_server_st * s, DBusConnection * conn,
			      DBusMessage * msg)
{
	DBusMessage *reply;
	DBusMessageIter args;
	DBusMessageIter suba;
	DBusMessageIter subs;
	dbus_uint32_t tmp;
	struct proc_st *ctmp = NULL;
	char ipbuf[128];
	const char *strtmp;

	mslog(s, NULL, LOG_DEBUG, "ctl: list-users");

	/* no arguments needed */
	reply = dbus_message_new_method_return(msg);
	if (reply == NULL) {
		mslog(s, NULL, LOG_ERR, "error generating dbus reply");
		return;
	}

	dbus_message_iter_init_append(reply, &args);
	if (dbus_message_iter_open_container
	    (&args, DBUS_TYPE_ARRAY, LIST_USERS_SIG, &suba) == 0) {
		mslog(s, NULL, LOG_ERR,
		      "error appending container to dbus reply");
		goto error;
	}

	list_for_each(&s->proc_list.head, ctmp, list) {

		if (dbus_message_iter_open_container
		    (&suba, DBUS_TYPE_STRUCT, NULL, &subs) == 0) {
			mslog(s, NULL, LOG_ERR,
			      "error appending container to dbus reply");
			goto error;
		}

		/* ID: pid */
		tmp = ctmp->pid;
		if (dbus_message_iter_append_basic
		    (&subs, DBUS_TYPE_UINT32, &tmp) == 0) {
			mslog(s, NULL, LOG_ERR,
			      "error appending to dbus reply");
			goto error;
		}

		strtmp = ctmp->username;
		if (dbus_message_iter_append_basic
		    (&subs, DBUS_TYPE_STRING, &strtmp) == 0) {
			mslog(s, NULL, LOG_ERR,
			      "error appending to dbus reply");
			goto error;
		}

		strtmp = ctmp->groupname;
		if (dbus_message_iter_append_basic
		    (&subs, DBUS_TYPE_STRING, &strtmp) == 0) {
			mslog(s, NULL, LOG_ERR,
			      "error appending to dbus reply");
			goto error;
		}

		strtmp =
		    human_addr((struct sockaddr *)&ctmp->remote_addr,
			       ctmp->remote_addr_len, ipbuf, sizeof(ipbuf));
		if (strtmp == NULL)
			strtmp = "";
		if (dbus_message_iter_append_basic
		    (&subs, DBUS_TYPE_STRING, &strtmp) == 0) {
			mslog(s, NULL, LOG_ERR,
			      "error appending to dbus reply");
			goto error;
		}
		strtmp = ctmp->tun_lease.name;
		if (dbus_message_iter_append_basic
		    (&subs, DBUS_TYPE_STRING, &strtmp) == 0) {
			mslog(s, NULL, LOG_ERR,
			      "error appending to dbus reply");
			goto error;
		}

		strtmp = NULL;
		if (ctmp->ipv4 != NULL)
			strtmp =
			    human_addr((struct sockaddr *)&ctmp->ipv4->rip,
				       ctmp->ipv4->rip_len, ipbuf,
				       sizeof(ipbuf));
		if (strtmp == NULL)
			strtmp = "";
		if (dbus_message_iter_append_basic
		    (&subs, DBUS_TYPE_STRING, &strtmp) == 0) {
			mslog(s, NULL, LOG_ERR,
			      "error appending to dbus reply");
			goto error;
		}

		strtmp = NULL;
		if (ctmp->ipv4 != NULL)
			strtmp =
			    human_addr((struct sockaddr *)&ctmp->ipv4->lip,
				       ctmp->ipv4->lip_len, ipbuf,
				       sizeof(ipbuf));
		if (strtmp == NULL)
			strtmp = "";
		if (dbus_message_iter_append_basic
		    (&subs, DBUS_TYPE_STRING, &strtmp) == 0) {
			mslog(s, NULL, LOG_ERR,
			      "error appending to dbus reply");
			goto error;
		}

		strtmp = NULL;
		if (ctmp->ipv6 != NULL)
			strtmp =
			    human_addr((struct sockaddr *)&ctmp->ipv6->rip,
				       ctmp->ipv6->rip_len, ipbuf,
				       sizeof(ipbuf));
		if (strtmp == NULL)
			strtmp = "";
		if (dbus_message_iter_append_basic
		    (&subs, DBUS_TYPE_STRING, &strtmp) == 0) {
			mslog(s, NULL, LOG_ERR,
			      "error appending to dbus reply");
			goto error;
		}

		strtmp = NULL;
		if (ctmp->ipv6 != NULL)
			strtmp =
			    human_addr((struct sockaddr *)&ctmp->ipv6->lip,
				       ctmp->ipv6->lip_len, ipbuf,
				       sizeof(ipbuf));
		if (strtmp == NULL)
			strtmp = "";
		if (dbus_message_iter_append_basic
		    (&subs, DBUS_TYPE_STRING, &strtmp) == 0) {
			mslog(s, NULL, LOG_ERR,
			      "error appending to dbus reply");
			goto error;
		}
		tmp = ctmp->conn_time;
		if (dbus_message_iter_append_basic
		    (&subs, DBUS_TYPE_UINT32, &tmp) == 0) {
			mslog(s, NULL, LOG_ERR,
			      "error appending to dbus reply");
			goto error;
		}

		strtmp = ctmp->hostname;
		if (dbus_message_iter_append_basic
		    (&subs, DBUS_TYPE_STRING, &strtmp) == 0) {
			mslog(s, NULL, LOG_ERR,
			      "error appending to dbus reply");
			goto error;
		}

		if (ctmp->auth_status == PS_AUTH_COMPLETED)
			strtmp = "authenticated";
		else if (ctmp->auth_status == PS_AUTH_INIT)
			strtmp = "authenticating";
		else
			strtmp = "pre-auth";
		if (dbus_message_iter_append_basic
		    (&subs, DBUS_TYPE_STRING, &strtmp) == 0) {
			mslog(s, NULL, LOG_ERR,
			      "error appending to dbus reply");
			goto error;
		}

		if (dbus_message_iter_close_container(&suba, &subs) == 0) {
			mslog(s, NULL, LOG_ERR,
			      "error closing container in dbus reply");
			goto error;
		}
	}

	if (dbus_message_iter_close_container(&args, &suba) == 0) {
		mslog(s, NULL, LOG_ERR,
		      "error closing container in dbus reply");
		goto error;
	}

	if (!dbus_connection_send(conn, reply, NULL)) {
		mslog(s, NULL, LOG_ERR, "error sending dbus reply");
		goto error;
	}

 error:
	dbus_message_unref(reply);

	return;
}

static void info_common(main_server_st * s, DBusConnection * conn,
			     DBusMessage * msg, const char* user,
			     unsigned id)
{
	DBusMessage *reply;
	DBusMessageIter args;
	DBusMessageIter suba;
	DBusMessageIter subs;
	dbus_uint32_t tmp;
	struct proc_st *ctmp = NULL;
	char ipbuf[128];
	const char *strtmp;

	/* no arguments needed */
	reply = dbus_message_new_method_return(msg);
	if (reply == NULL) {
		mslog(s, NULL, LOG_ERR, "error generating dbus reply");
		return;
	}

	dbus_message_iter_init_append(reply, &args);
	if (dbus_message_iter_open_container
	    (&args, DBUS_TYPE_ARRAY, LIST_USERS_SIG, &suba) == 0) {
		mslog(s, NULL, LOG_ERR,
		      "error appending container to dbus reply");
		goto error;
	}

	list_for_each(&s->proc_list.head, ctmp, list) {
		if (id != 0) { /* id */
			if (id != ctmp->pid) {
				continue;
			}
		} else { /* username */
			if (strcmp(ctmp->username, user) != 0) {
				continue;
			}
		}

		if (dbus_message_iter_open_container
		    (&suba, DBUS_TYPE_STRUCT, NULL, &subs) == 0) {
			mslog(s, NULL, LOG_ERR,
			      "error appending container to dbus reply");
			goto error;
		}

		/* ID: pid */
		tmp = ctmp->pid;
		if (dbus_message_iter_append_basic
		    (&subs, DBUS_TYPE_UINT32, &tmp) == 0) {
			mslog(s, NULL, LOG_ERR,
			      "error appending to dbus reply");
			goto error;
		}

		strtmp = ctmp->username;
		if (dbus_message_iter_append_basic
		    (&subs, DBUS_TYPE_STRING, &strtmp) == 0) {
			mslog(s, NULL, LOG_ERR,
			      "error appending to dbus reply");
			goto error;
		}

		strtmp = ctmp->groupname;
		if (dbus_message_iter_append_basic
		    (&subs, DBUS_TYPE_STRING, &strtmp) == 0) {
			mslog(s, NULL, LOG_ERR,
			      "error appending to dbus reply");
			goto error;
		}

		strtmp =
		    human_addr((struct sockaddr *)&ctmp->remote_addr,
			       ctmp->remote_addr_len, ipbuf, sizeof(ipbuf));
		if (strtmp == NULL)
			strtmp = "";
		if (dbus_message_iter_append_basic
		    (&subs, DBUS_TYPE_STRING, &strtmp) == 0) {
			mslog(s, NULL, LOG_ERR,
			      "error appending to dbus reply");
			goto error;
		}
		strtmp = ctmp->tun_lease.name;
		if (dbus_message_iter_append_basic
		    (&subs, DBUS_TYPE_STRING, &strtmp) == 0) {
			mslog(s, NULL, LOG_ERR,
			      "error appending to dbus reply");
			goto error;
		}

		strtmp = NULL;
		if (ctmp->ipv4 != NULL)
			strtmp =
			    human_addr((struct sockaddr *)&ctmp->ipv4->rip,
				       ctmp->ipv4->rip_len, ipbuf,
				       sizeof(ipbuf));
		if (strtmp == NULL)
			strtmp = "";
		if (dbus_message_iter_append_basic
		    (&subs, DBUS_TYPE_STRING, &strtmp) == 0) {
			mslog(s, NULL, LOG_ERR,
			      "error appending to dbus reply");
			goto error;
		}

		strtmp = NULL;
		if (ctmp->ipv4 != NULL)
			strtmp =
			    human_addr((struct sockaddr *)&ctmp->ipv4->lip,
				       ctmp->ipv4->lip_len, ipbuf,
				       sizeof(ipbuf));
		if (strtmp == NULL)
			strtmp = "";
		if (dbus_message_iter_append_basic
		    (&subs, DBUS_TYPE_STRING, &strtmp) == 0) {
			mslog(s, NULL, LOG_ERR,
			      "error appending to dbus reply");
			goto error;
		}

		strtmp = NULL;
		if (ctmp->ipv6 != NULL)
			strtmp =
			    human_addr((struct sockaddr *)&ctmp->ipv6->rip,
				       ctmp->ipv6->rip_len, ipbuf,
				       sizeof(ipbuf));
		if (strtmp == NULL)
			strtmp = "";
		if (dbus_message_iter_append_basic
		    (&subs, DBUS_TYPE_STRING, &strtmp) == 0) {
			mslog(s, NULL, LOG_ERR,
			      "error appending to dbus reply");
			goto error;
		}

		strtmp = NULL;
		if (ctmp->ipv6 != NULL)
			strtmp =
			    human_addr((struct sockaddr *)&ctmp->ipv6->lip,
				       ctmp->ipv6->lip_len, ipbuf,
				       sizeof(ipbuf));
		if (strtmp == NULL)
			strtmp = "";
		if (dbus_message_iter_append_basic
		    (&subs, DBUS_TYPE_STRING, &strtmp) == 0) {
			mslog(s, NULL, LOG_ERR,
			      "error appending to dbus reply");
			goto error;
		}
		tmp = ctmp->conn_time;
		if (dbus_message_iter_append_basic
		    (&subs, DBUS_TYPE_UINT32, &tmp) == 0) {
			mslog(s, NULL, LOG_ERR,
			      "error appending to dbus reply");
			goto error;
		}

		strtmp = ctmp->hostname;
		if (dbus_message_iter_append_basic
		    (&subs, DBUS_TYPE_STRING, &strtmp) == 0) {
			mslog(s, NULL, LOG_ERR,
			      "error appending to dbus reply");
			goto error;
		}

		if (ctmp->auth_status == PS_AUTH_COMPLETED)
			strtmp = "authenticated";
		else if (ctmp->auth_status == PS_AUTH_INIT)
			strtmp = "authenticating";
		else
			strtmp = "pre-auth";
		if (dbus_message_iter_append_basic
		    (&subs, DBUS_TYPE_STRING, &strtmp) == 0) {
			mslog(s, NULL, LOG_ERR,
			      "error appending to dbus reply");
			goto error;
		}

		if (dbus_message_iter_close_container(&suba, &subs) == 0) {
			mslog(s, NULL, LOG_ERR,
			      "error closing container in dbus reply");
			goto error;
		}

		if (id != 0) /* id -> one a single element */
			break;
	}

	if (dbus_message_iter_close_container(&args, &suba) == 0) {
		mslog(s, NULL, LOG_ERR,
		      "error closing container in dbus reply");
		goto error;
	}

	if (!dbus_connection_send(conn, reply, NULL)) {
		mslog(s, NULL, LOG_ERR, "error sending dbus reply");
		goto error;
	}

 error:
	dbus_message_unref(reply);

	return;
}

static void method_user_info(main_server_st * s, DBusConnection * conn,
			     DBusMessage * msg)
{
	DBusMessageIter args;
	const char *name;

	mslog(s, NULL, LOG_DEBUG, "ctl: user_info");

	if (dbus_message_iter_init(msg, &args) == 0) {
		mslog(s, NULL, LOG_ERR,
		      "no arguments provided in user_info");
		return;
	}

	if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING) {
		mslog(s, NULL, LOG_ERR,
		      "wrong argument provided in user_info");
		return;
	}

	dbus_message_iter_get_basic(&args, &name);
	
	info_common(s, conn, msg, name, 0);

	return;
}

static void method_id_info(main_server_st * s, DBusConnection * conn,
			     DBusMessage * msg)
{
	DBusMessageIter args;
	dbus_uint32_t id;

	mslog(s, NULL, LOG_DEBUG, "ctl: user_info");

	if (dbus_message_iter_init(msg, &args) == 0) {
		mslog(s, NULL, LOG_ERR,
		      "no arguments provided in user_info");
		return;
	}

	if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_UINT32) {
		mslog(s, NULL, LOG_ERR,
		      "wrong argument provided in user_info");
		return;
	}

	dbus_message_iter_get_basic(&args, &id);
	
	info_common(s, conn, msg, NULL, id);

	return;
}


static void method_disconnect_user_name(main_server_st * s,
					DBusConnection * conn,
					DBusMessage * msg)
{
	DBusMessage *reply;
	DBusMessageIter args;
	dbus_bool_t status = 0;
	struct proc_st *ctmp = NULL;
	char *name = "";

	mslog(s, NULL, LOG_DEBUG, "ctl: disconnect_name");

	if (dbus_message_iter_init(msg, &args) == 0) {
		mslog(s, NULL, LOG_ERR,
		      "no arguments provided in disconnect_name");
		return;
	}

	if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_STRING) {
		mslog(s, NULL, LOG_ERR,
		      "wrong argument provided in disconnect_name");
		return;
	}

	dbus_message_iter_get_basic(&args, &name);

	/* got the name. Try to disconnect */
	list_for_each(&s->proc_list.head, ctmp, list) {
		if (strcmp(ctmp->username, name) == 0) {
			remove_proc(s, ctmp, 1);
			status = 1;
		}
	}

	/* reply */
	reply = dbus_message_new_method_return(msg);
	if (reply == NULL) {
		mslog(s, NULL, LOG_ERR, "error generating dbus reply");
		return;
	}

	dbus_message_iter_init_append(reply, &args);

	if (dbus_message_iter_append_basic(&args, DBUS_TYPE_BOOLEAN, &status) ==
	    0) {
		mslog(s, NULL, LOG_ERR, "error appending to dbus reply");
		goto error;
	}

	if (!dbus_connection_send(conn, reply, NULL)) {
		mslog(s, NULL, LOG_ERR, "error sending dbus reply");
		goto error;
	}

 error:
	dbus_message_unref(reply);

	return;
}

static void method_disconnect_user_id(main_server_st * s, DBusConnection * conn,
				      DBusMessage * msg)
{
	DBusMessage *reply;
	DBusMessageIter args;
	dbus_bool_t status = 0;
	struct proc_st *ctmp = NULL;
	dbus_uint32_t id = 0;

	mslog(s, NULL, LOG_DEBUG, "ctl: disconnect_id");

	if (dbus_message_iter_init(msg, &args) == 0) {
		mslog(s, NULL, LOG_ERR,
		      "no arguments provided in disconnect_id");
		return;
	}

	if (dbus_message_iter_get_arg_type(&args) != DBUS_TYPE_UINT32) {
		mslog(s, NULL, LOG_ERR,
		      "wrong argument provided in disconnect_id");
		return;
	}

	dbus_message_iter_get_basic(&args, &id);

	/* got the ID. Try to disconnect */
	list_for_each(&s->proc_list.head, ctmp, list) {
		if (ctmp->pid == id) {
			remove_proc(s, ctmp, 1);
			status = 1;
			break;
		}
	}

	/* reply */
	reply = dbus_message_new_method_return(msg);
	if (reply == NULL) {
		mslog(s, NULL, LOG_ERR, "error generating dbus reply");
		return;
	}

	dbus_message_iter_init_append(reply, &args);

	if (dbus_message_iter_append_basic(&args, DBUS_TYPE_BOOLEAN, &status) ==
	    0) {
		mslog(s, NULL, LOG_ERR, "error appending to dbus reply");
		goto error;
	}

	if (!dbus_connection_send(conn, reply, NULL)) {
		mslog(s, NULL, LOG_ERR, "error sending dbus reply");
		goto error;
	}

 error:
	dbus_message_unref(reply);

	return;
}

#define XML_HEAD \
	"<!DOCTYPE node PUBLIC \"-//freedesktop//DTD D-BUS Object Introspection 1.0//EN\"\n" \
	"\"http://www.freedesktop.org/standards/dbus/1.0/introspect.dtd\">\n" \
        "<node name=\"/org/infradead/ocserv\">\n" \
	"<interface name=\"org.infradead.ocserv\">\n"

#define XML_FOOT \
	"</interface>" \
	"</node>\n"

static void method_introspect(main_server_st * s, DBusConnection * conn,
			      DBusMessage * msg)
{
	DBusMessage *reply = NULL;
	const char *xml;
	str_st buf;
	int ret;
	unsigned i;

	mslog(s, NULL, LOG_DEBUG, "ctl: introspect");

	str_init(&buf);

	ret = str_append_data(&buf, XML_HEAD, sizeof(XML_HEAD) - 1);
	if (ret < 0) {
		mslog(s, NULL, LOG_ERR, "error generating dbus reply");
		goto error;
	}

	for (i = 0; methods[i].name != NULL; i++) {
		ret =
		    str_append_data(&buf, methods[i].desc,
				    methods[i].desc_size);
		if (ret < 0) {
			mslog(s, NULL, LOG_ERR, "error generating dbus reply");
			goto error;
		}
	}

	ret = str_append_data(&buf, XML_FOOT, sizeof(XML_FOOT) - 1);
	if (ret < 0) {
		mslog(s, NULL, LOG_ERR, "error generating dbus reply");
		goto error;
	}

	/* no arguments needed */
	reply = dbus_message_new_method_return(msg);
	if (reply == NULL) {
		mslog(s, NULL, LOG_ERR, "error generating dbus reply");
		goto error;
	}

	xml = (char *)buf.data;
	if (dbus_message_append_args
	    (reply, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID) == 0) {
		mslog(s, NULL, LOG_ERR, "error in introspect to dbus reply");
		goto error;
	}

	if (!dbus_connection_send(conn, reply, NULL)) {
		mslog(s, NULL, LOG_ERR, "error sending dbus reply");
		goto error;
	}

 error:
	str_clear(&buf);
	if (reply)
		dbus_message_unref(reply);

	return;

}

void ctl_handle_commands(main_server_st * s, struct ctl_handler_st *ctl)
{
	DBusConnection *conn = s->ctl_ctx;
	DBusMessage *msg;
	int ret;
	unsigned flags, i;

	if (s->config->use_dbus == 0) {
		mslog(s, NULL, LOG_ERR, "%s called when D-BUS is disabled!", __func__);
		return;
	}

	if (ctl->type == CTL_READ)
		flags = DBUS_WATCH_READABLE;
	else
		flags = DBUS_WATCH_WRITABLE;

	dbus_connection_ref(conn);
	ret = dbus_watch_handle(ctl->watch, flags);
	dbus_connection_unref(conn);

	if (ret == 0) {
		mslog(s, NULL, LOG_ERR, "error handling watch");
		return;
	}

	do {
		if (dbus_connection_read_write(conn, 0) == 0) {
			mslog(s, NULL, LOG_ERR,
			      "error handling dbus_connection_read_write");
			return;
		}

		msg = dbus_connection_pop_message(conn);
		if (msg == NULL)
			return;

		for (i = 0;; i++) {
			if (methods[i].name == NULL) {
				mslog(s, NULL, LOG_INFO,
				      "unknown D-BUS message: %s.%s: %s",
				      dbus_message_get_interface(msg),
				      dbus_message_get_member(msg),
				      dbus_message_get_path(msg));
				break;
			}
			if (dbus_message_is_method_call
			    (msg, methods[i].iface, methods[i].name)) {
				methods[i].func(s, conn, msg);
				break;
			}
		}

		dbus_message_unref(msg);
	} while (msg != NULL);
}
#else

void ctl_handler_deinit(main_server_st * s)
{
	return;
}

/* Initializes unix socket and stores the fd.
 */
int ctl_handler_init(main_server_st * s)
{
	return 0;
}

void ctl_handle_commands(main_server_st * s, struct ctl_handler_st *ctl)
{
	return;
}

#endif