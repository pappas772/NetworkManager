/* -*- Mode: C; tab-width: 4; indent-tabs-mode: t; c-basic-offset: 4 -*- */
/* nm-dhcp-manager.c - Handle the DHCP daemon for NetworkManager
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Copyright (C) 2005 - 2013 Red Hat, Inc.
 * Copyright (C) 2006 - 2008 Novell, Inc.
 *
 */

#include "nm-default.h"

#include "nm-dhcp-manager.h"

#include <sys/socket.h>
#include <sys/wait.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>

#include "nm-config.h"
#include "NetworkManagerUtils.h"

#define DHCP_TIMEOUT 45 /* default DHCP timeout, in seconds */

/*****************************************************************************/

typedef struct {
	const NMDhcpClientFactory *client_factory;
	GHashTable *        clients;
	char *              default_hostname;
} NMDhcpManagerPrivate;

struct _NMDhcpManager {
	GObject parent;
	NMDhcpManagerPrivate _priv;
};

struct _NMDhcpManagerClass {
	GObjectClass parent;
};

G_DEFINE_TYPE (NMDhcpManager, nm_dhcp_manager, G_TYPE_OBJECT)

#define NM_DHCP_MANAGER_GET_PRIVATE(self) _NM_GET_PRIVATE (self, NMDhcpManager, NM_IS_DHCP_MANAGER)

/*****************************************************************************/

/* default to installed helper, but can be modified for testing */
const char *nm_dhcp_helper_path = LIBEXECDIR "/nm-dhcp-helper";

/*****************************************************************************/

static const NMDhcpClientFactory *
_client_factory_find_by_name (const char *name)
{
	int i;

	g_return_val_if_fail (name, NULL);

	for (i = 0; i < G_N_ELEMENTS (_nm_dhcp_manager_factories); i++) {
		const NMDhcpClientFactory *f = _nm_dhcp_manager_factories[i];

		if (f && nm_streq (f->name, name))
			return f;
	}
	return NULL;
}

static const NMDhcpClientFactory *
_client_factory_available (const NMDhcpClientFactory *client_factory)
{
	if (   client_factory
	    && (!client_factory->get_path || client_factory->get_path ()))
		return client_factory;
	return NULL;
}

/*****************************************************************************/

static NMDhcpClient *
get_client_for_ifindex (NMDhcpManager *manager, int ifindex, gboolean ip6)
{
	NMDhcpManagerPrivate *priv;
	GHashTableIter iter;
	gpointer value;

	g_return_val_if_fail (NM_IS_DHCP_MANAGER (manager), NULL);
	g_return_val_if_fail (ifindex > 0, NULL);

	priv = NM_DHCP_MANAGER_GET_PRIVATE (manager);

	g_hash_table_iter_init (&iter, priv->clients);
	while (g_hash_table_iter_next (&iter, NULL, &value)) {
		NMDhcpClient *candidate = NM_DHCP_CLIENT (value);

		if (   nm_dhcp_client_get_ifindex (candidate) == ifindex
		    && nm_dhcp_client_get_ipv6 (candidate) == ip6)
			return candidate;
	}

	return NULL;
}

static void client_state_changed (NMDhcpClient *client,
                                  NMDhcpState state,
                                  GObject *ip_config,
                                  GVariant *options,
                                  const char *event_id,
                                  NMDhcpManager *self);

static void
remove_client (NMDhcpManager *self, NMDhcpClient *client)
{
	g_signal_handlers_disconnect_by_func (client, client_state_changed, self);

	/* Stopping the client is left up to the controlling device
	 * explicitly since we may want to quit NetworkManager but not terminate
	 * the DHCP client.
	 */

	g_hash_table_remove (NM_DHCP_MANAGER_GET_PRIVATE (self)->clients, client);
}

static void
client_state_changed (NMDhcpClient *client,
                      NMDhcpState state,
                      GObject *ip_config,
                      GVariant *options,
                      const char *event_id,
                      NMDhcpManager *self)
{
	if (state >= NM_DHCP_STATE_TIMEOUT)
		remove_client (self, client);
}

static NMDhcpClient *
client_start (NMDhcpManager *self,
              const char *iface,
              int ifindex,
              const GByteArray *hwaddr,
              const char *uuid,
              guint32 priority,
              gboolean ipv6,
              const struct in6_addr *ipv6_ll_addr,
              const char *dhcp_client_id,
              guint32 timeout,
              const char *dhcp_anycast_addr,
              const char *hostname,
              const char *fqdn,
              gboolean info_only,
              NMSettingIP6ConfigPrivacy privacy,
              const char *last_ip4_address)
{
	NMDhcpManagerPrivate *priv;
	NMDhcpClient *client;
	gboolean success = FALSE;

	g_return_val_if_fail (self, NULL);
	g_return_val_if_fail (NM_IS_DHCP_MANAGER (self), NULL);
	g_return_val_if_fail (ifindex > 0, NULL);
	g_return_val_if_fail (uuid != NULL, NULL);

	priv = NM_DHCP_MANAGER_GET_PRIVATE (self);

	/* Ensure we have a usable DHCP client */
	if (!priv->client_factory)
		return NULL;

	/* Kill any old client instance */
	client = get_client_for_ifindex (self, ifindex, ipv6);
	if (client) {
		g_object_ref (client);
		remove_client (self, client);
		nm_dhcp_client_stop (client, FALSE);
		g_object_unref (client);
	}

	/* And make a new one */
	client = g_object_new (priv->client_factory->get_type (),
	                       NM_DHCP_CLIENT_INTERFACE, iface,
	                       NM_DHCP_CLIENT_IFINDEX, ifindex,
	                       NM_DHCP_CLIENT_HWADDR, hwaddr,
	                       NM_DHCP_CLIENT_IPV6, ipv6,
	                       NM_DHCP_CLIENT_UUID, uuid,
	                       NM_DHCP_CLIENT_PRIORITY, priority,
	                       NM_DHCP_CLIENT_TIMEOUT, timeout ? timeout : DHCP_TIMEOUT,
	                       NULL);
	g_hash_table_insert (NM_DHCP_MANAGER_GET_PRIVATE (self)->clients, client, g_object_ref (client));
	g_signal_connect (client, NM_DHCP_CLIENT_SIGNAL_STATE_CHANGED, G_CALLBACK (client_state_changed), self);

	if (ipv6)
		success = nm_dhcp_client_start_ip6 (client, dhcp_anycast_addr, ipv6_ll_addr, hostname, info_only, privacy);
	else
		success = nm_dhcp_client_start_ip4 (client, dhcp_client_id, dhcp_anycast_addr, hostname, fqdn, last_ip4_address);

	if (!success) {
		remove_client (self, client);
		client = NULL;
	}

	return client;
}

static const char *
get_send_hostname (NMDhcpManager *self, const char *setting_hostname)
{
	NMDhcpManagerPrivate *priv = NM_DHCP_MANAGER_GET_PRIVATE (self);

	/* Always prefer the explicit dhcp-send-hostname if given */
	return setting_hostname ? setting_hostname : priv->default_hostname;
}

/* Caller owns a reference to the NMDhcpClient on return */
NMDhcpClient *
nm_dhcp_manager_start_ip4 (NMDhcpManager *self,
                           const char *iface,
                           int ifindex,
                           const GByteArray *hwaddr,
                           const char *uuid,
                           guint32 priority,
                           gboolean send_hostname,
                           const char *dhcp_hostname,
                           const char *dhcp_fqdn,
                           const char *dhcp_client_id,
                           guint32 timeout,
                           const char *dhcp_anycast_addr,
                           const char *last_ip_address)
{
	const char *hostname = NULL;
	const char *fqdn = NULL;

	g_return_val_if_fail (NM_IS_DHCP_MANAGER (self), NULL);

	if (send_hostname) {
		hostname = get_send_hostname (self, dhcp_hostname);
		fqdn = dhcp_fqdn;
	}
	return client_start (self, iface, ifindex, hwaddr, uuid, priority, FALSE, NULL,
	                     dhcp_client_id, timeout, dhcp_anycast_addr, hostname,
	                     fqdn, FALSE, 0, last_ip_address);
}

/* Caller owns a reference to the NMDhcpClient on return */
NMDhcpClient *
nm_dhcp_manager_start_ip6 (NMDhcpManager *self,
                           const char *iface,
                           int ifindex,
                           const GByteArray *hwaddr,
                           const struct in6_addr *ll_addr,
                           const char *uuid,
                           guint32 priority,
                           gboolean send_hostname,
                           const char *dhcp_hostname,
                           guint32 timeout,
                           const char *dhcp_anycast_addr,
                           gboolean info_only,
                           NMSettingIP6ConfigPrivacy privacy)
{
	const char *hostname = NULL;

	g_return_val_if_fail (NM_IS_DHCP_MANAGER (self), NULL);

	if (send_hostname)
		hostname = get_send_hostname (self, dhcp_hostname);
	return client_start (self, iface, ifindex, hwaddr, uuid, priority, TRUE,
	                     ll_addr, NULL, timeout, dhcp_anycast_addr, hostname, NULL, info_only,
	                     privacy, NULL);
}

void
nm_dhcp_manager_set_default_hostname (NMDhcpManager *manager, const char *hostname)
{
	NMDhcpManagerPrivate *priv = NM_DHCP_MANAGER_GET_PRIVATE (manager);

	g_clear_pointer (&priv->default_hostname, g_free);

	/* Never send 'localhost'-type names to the DHCP server */
	if (!nm_utils_is_specific_hostname (hostname))
		return;

	priv->default_hostname = g_strdup (hostname);
}

GSList *
nm_dhcp_manager_get_lease_ip_configs (NMDhcpManager *self,
                                      const char *iface,
                                      int ifindex,
                                      const char *uuid,
                                      gboolean ipv6,
                                      guint32 default_route_metric)
{
	NMDhcpManagerPrivate *priv;

	g_return_val_if_fail (NM_IS_DHCP_MANAGER (self), NULL);
	g_return_val_if_fail (iface != NULL, NULL);
	g_return_val_if_fail (ifindex >= -1, NULL);
	g_return_val_if_fail (uuid != NULL, NULL);

	priv = NM_DHCP_MANAGER_GET_PRIVATE (self);
	if (   priv->client_factory
	    && priv->client_factory->get_lease_ip_configs)
		return priv->client_factory->get_lease_ip_configs (iface, ifindex, uuid, ipv6, default_route_metric);
	return NULL;
}

/*****************************************************************************/

NM_DEFINE_SINGLETON_GETTER (NMDhcpManager, nm_dhcp_manager_get, NM_TYPE_DHCP_MANAGER);

static void
nm_dhcp_manager_init (NMDhcpManager *self)
{
	NMDhcpManagerPrivate *priv = NM_DHCP_MANAGER_GET_PRIVATE (self);
	NMConfig *config = nm_config_get ();
	const char *client;
	int i;
	const NMDhcpClientFactory *client_factory = NULL;

	for (i = 0; i < G_N_ELEMENTS (_nm_dhcp_manager_factories); i++) {
		const NMDhcpClientFactory *f = _nm_dhcp_manager_factories[i];

		if (!f)
			continue;

		nm_log_dbg (LOGD_DHCP, "dhcp-init: enabled DHCP client '%s' (%s)%s",
		            f->name, g_type_name (f->get_type ()),
		            _client_factory_available (f) ? "" : " (not available)");
	}

	/* Client-specific setup */
	client = nm_config_get_dhcp_client (config);
	if (nm_config_get_configure_and_quit (config)) {
		client_factory = &_nm_dhcp_client_factory_internal;
		if (client && !nm_streq (client, client_factory->name))
			nm_log_warn (LOGD_DHCP, "dhcp-init: Using internal DHCP client since configure-and-quit is set.");
	} else {
		if (client) {
			client_factory = _client_factory_available (_client_factory_find_by_name (client));
			if (!client_factory)
				nm_log_warn (LOGD_DHCP, "dhcp-init: DHCP client '%s' not available", client);
		}
		if (!client_factory) {
			for (i = 0; i < G_N_ELEMENTS (_nm_dhcp_manager_factories); i++) {
				client_factory = _client_factory_available (_nm_dhcp_manager_factories[i]);
				if (client_factory)
					break;
			}
		}
	}

	nm_assert (client_factory);

	nm_log_info (LOGD_DHCP, "dhcp-init: Using DHCP client '%s'", client_factory->name);

	priv->client_factory = client_factory;
	priv->clients = g_hash_table_new_full (g_direct_hash, g_direct_equal,
	                                       NULL,
	                                       (GDestroyNotify) g_object_unref);
}

static void
dispose (GObject *object)
{
	NMDhcpManagerPrivate *priv = NM_DHCP_MANAGER_GET_PRIVATE ((NMDhcpManager *) object);
	GList *values, *iter;

	if (priv->clients) {
		values = g_hash_table_get_values (priv->clients);
	for (iter = values; iter; iter = g_list_next (iter))
			remove_client (NM_DHCP_MANAGER (object), NM_DHCP_CLIENT (iter->data));
		g_list_free (values);
	}

	G_OBJECT_CLASS (nm_dhcp_manager_parent_class)->dispose (object);
}

static void
finalize (GObject *object)
{
	NMDhcpManagerPrivate *priv = NM_DHCP_MANAGER_GET_PRIVATE ((NMDhcpManager *) object);

	g_free (priv->default_hostname);

	if (priv->clients)
		g_hash_table_destroy (priv->clients);

	G_OBJECT_CLASS (nm_dhcp_manager_parent_class)->finalize (object);
}

static void
nm_dhcp_manager_class_init (NMDhcpManagerClass *manager_class)
{
	GObjectClass *object_class = G_OBJECT_CLASS (manager_class);

	object_class->finalize = finalize;
	object_class->dispose = dispose;
}
