/* SPDX-License-Identifier: LGPL-2.1-or-later */

#include <net/if.h>
#include <netinet/in.h>
#include <linux/if_arp.h>
#include <linux/if_bridge.h>

#include "bridge.h"
#include "netlink-util.h"
#include "networkd-manager.h"
#include "string-table.h"
#include "vlan-util.h"

assert_cc((int) MULTICAST_ROUTER_NONE            == (int) MDB_RTR_TYPE_DISABLED);
assert_cc((int) MULTICAST_ROUTER_TEMPORARY_QUERY == (int) MDB_RTR_TYPE_TEMP_QUERY);
assert_cc((int) MULTICAST_ROUTER_PERMANENT       == (int) MDB_RTR_TYPE_PERM);
assert_cc((int) MULTICAST_ROUTER_TEMPORARY       == (int) MDB_RTR_TYPE_TEMP);

static const char* const multicast_router_table[_MULTICAST_ROUTER_MAX] = {
        [MULTICAST_ROUTER_NONE]            = "no",
        [MULTICAST_ROUTER_TEMPORARY_QUERY] = "query",
        [MULTICAST_ROUTER_PERMANENT]       = "permanent",
        [MULTICAST_ROUTER_TEMPORARY]       = "temporary",
};

DEFINE_STRING_TABLE_LOOKUP_WITH_BOOLEAN(multicast_router, MulticastRouter, _MULTICAST_ROUTER_INVALID);
DEFINE_CONFIG_PARSE_ENUM(config_parse_multicast_router, multicast_router, MulticastRouter,
                         "Failed to parse bridge multicast router setting");

static BridgeVlan* bridge_vlan_free(BridgeVlan *vlan) {
        if (!vlan)
                return NULL;

        if (vlan->bridge && vlan->section)
                hashmap_remove(vlan->bridge->vlans, vlan->section);

        if (vlan->section)
                network_config_section_free(vlan->section);

        return mfree(vlan);
}

DEFINE_NETWORK_SECTION_FUNCTIONS(BridgeVlan, bridge_vlan_free);

static int bridge_vlan_new_static(Bridge *b, const char *filename, unsigned section_line, BridgeVlan **ret) {
        _cleanup_(network_config_section_freep) NetworkConfigSection *n = NULL;
        _cleanup_(bridge_vlan_freep) BridgeVlan *vlan = NULL;
        int r;

        assert(b);
        assert(ret);
        assert(filename);
        assert(section_line > 0);

        r = network_config_section_new(filename, section_line, &n);
        if (r < 0)
                return r;

        vlan = hashmap_get(b->vlans, n);
        if (vlan) {
                *ret = TAKE_PTR(vlan);
                return 0;
        }

        vlan = new(BridgeVlan, 1);
        if (!vlan)
                return -ENOMEM;

        *vlan = (BridgeVlan) {
                .bridge = b,
                .section = TAKE_PTR(n),
                .vid = -1,
                .vid_end = -1,
                .mcast_snooping = -1,
        };

        r = hashmap_ensure_put(&b->vlans, &network_config_hash_ops, vlan->section, vlan);
        if (r < 0)
                return r;

        *ret = TAKE_PTR(vlan);
        return 0;
}

/* callback for bridge netdev's parameter set */
static int netdev_bridge_set_handler(sd_netlink *rtnl, sd_netlink_message *m, NetDev *netdev) {
        int r;

        assert(netdev);
        assert(m);

        r = sd_netlink_message_get_errno(m);
        if (r < 0) {
                log_netdev_warning_errno(netdev, r, "Bridge parameters could not be set: %m");
                return 1;
        }

        log_netdev_debug(netdev, "Bridge parameters set success");

        return 1;
}

static int netdev_bridge_post_create(NetDev *netdev, Link *link, sd_netlink_message *m) {
        _cleanup_(sd_netlink_message_unrefp) sd_netlink_message *req = NULL;
        Bridge *b;
        int r;

        assert(netdev);

        b = BRIDGE(netdev);

        assert(b);

        r = sd_rtnl_message_new_link(netdev->manager->rtnl, &req, RTM_NEWLINK, netdev->ifindex);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not allocate RTM_SETLINK message: %m");

        r = sd_netlink_message_set_flags(req, NLM_F_REQUEST | NLM_F_ACK);
        if (r < 0)
                return log_link_error_errno(link, r, "Could not set netlink flags: %m");

        r = sd_netlink_message_open_container(req, IFLA_LINKINFO);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_LINKINFO attribute: %m");

        r = sd_netlink_message_open_container_union(req, IFLA_INFO_DATA, netdev_kind_to_string(netdev->kind));
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_INFO_DATA attribute: %m");

        /* convert to jiffes */
        if (b->forward_delay != USEC_INFINITY) {
                r = sd_netlink_message_append_u32(req, IFLA_BR_FORWARD_DELAY, usec_to_jiffies(b->forward_delay));
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_BR_FORWARD_DELAY attribute: %m");
        }

        if (b->hello_time > 0) {
                r = sd_netlink_message_append_u32(req, IFLA_BR_HELLO_TIME, usec_to_jiffies(b->hello_time));
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_BR_HELLO_TIME attribute: %m");
        }

        if (b->max_age > 0) {
                r = sd_netlink_message_append_u32(req, IFLA_BR_MAX_AGE, usec_to_jiffies(b->max_age));
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_BR_MAX_AGE attribute: %m");
        }

        if (b->ageing_time != USEC_INFINITY) {
                r = sd_netlink_message_append_u32(req, IFLA_BR_AGEING_TIME, usec_to_jiffies(b->ageing_time));
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_BR_AGEING_TIME attribute: %m");
        }

        if (b->priority > 0) {
                r = sd_netlink_message_append_u16(req, IFLA_BR_PRIORITY, b->priority);
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_BR_PRIORITY attribute: %m");
        }

        if (b->group_fwd_mask > 0) {
                r = sd_netlink_message_append_u16(req, IFLA_BR_GROUP_FWD_MASK, b->group_fwd_mask);
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_BR_GROUP_FWD_MASK attribute: %m");
        }

        if (b->default_pvid != VLANID_INVALID) {
                r = sd_netlink_message_append_u16(req, IFLA_BR_VLAN_DEFAULT_PVID, b->default_pvid);
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_BR_VLAN_DEFAULT_PVID attribute: %m");
        }

        if (b->mcast_querier >= 0) {
                r = sd_netlink_message_append_u8(req, IFLA_BR_MCAST_QUERIER, b->mcast_querier);
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_BR_MCAST_QUERIER attribute: %m");
        }

        if (b->mcast_snooping >= 0) {
                r = sd_netlink_message_append_u8(req, IFLA_BR_MCAST_SNOOPING, b->mcast_snooping);
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_BR_MCAST_SNOOPING attribute: %m");
        }

        if (b->vlan_filtering >= 0) {
                r = sd_netlink_message_append_u8(req, IFLA_BR_VLAN_FILTERING, b->vlan_filtering);
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_BR_VLAN_FILTERING attribute: %m");
        }

        if (b->vlan_protocol >= 0) {
                r = sd_netlink_message_append_u16(req, IFLA_BR_VLAN_PROTOCOL, htobe16(b->vlan_protocol));
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_BR_VLAN_PROTOCOL attribute: %m");
        }

        if (b->stp >= 0) {
                r = sd_netlink_message_append_u32(req, IFLA_BR_STP_STATE, b->stp);
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_BR_STP_STATE attribute: %m");
        }

        if (b->igmp_version > 0) {
                r = sd_netlink_message_append_u8(req, IFLA_BR_MCAST_IGMP_VERSION, b->igmp_version);
                if (r < 0)
                        return log_netdev_error_errno(netdev, r, "Could not append IFLA_BR_MCAST_IGMP_VERSION attribute: %m");
        }

        r = sd_netlink_message_close_container(req);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_LINKINFO attribute: %m");

        r = sd_netlink_message_close_container(req);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not append IFLA_INFO_DATA attribute: %m");

        r = netlink_call_async(netdev->manager->rtnl, NULL, req, netdev_bridge_set_handler,
                               netdev_destroy_callback, netdev);
        if (r < 0)
                return log_netdev_error_errno(netdev, r, "Could not send rtnetlink message: %m");

        netdev_ref(netdev);

        return r;
}

int config_parse_bridge_igmp_version(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        Bridge *b = userdata;
        uint8_t u;
        int r;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        if (isempty(rvalue)) {
                b->igmp_version = 0; /* 0 means unset. */
                return 0;
        }

        r = safe_atou8(rvalue, &u);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to parse bridge's multicast IGMP version number '%s', ignoring assignment: %m",
                           rvalue);
                return 0;
        }
        if (!IN_SET(u, 2, 3)) {
                log_syntax(unit, LOG_WARNING, filename, line, 0,
                           "Invalid bridge's multicast IGMP version number '%s', ignoring assignment.", rvalue);
                return 0;
        }

        b->igmp_version = u;

        return 0;
}

int config_parse_bridge_port_priority(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {

        uint16_t i;
        int r;

        assert(filename);
        assert(lvalue);
        assert(rvalue);
        assert(data);

        /* This is used in networkd-network-gperf.gperf. */

        r = safe_atou16(rvalue, &i);
        if (r < 0) {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Failed to parse bridge port priority, ignoring: %s", rvalue);
                return 0;
        }

        if (i > LINK_BRIDGE_PORT_PRIORITY_MAX) {
                log_syntax(unit, LOG_WARNING, filename, line, 0,
                           "Bridge port priority is larger than maximum %u, ignoring: %s",
                           LINK_BRIDGE_PORT_PRIORITY_MAX, rvalue);
                return 0;
        }

        *((uint16_t *)data) = i;

        return 0;
}

int config_parse_bridge_vlan(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {
        _cleanup_(bridge_vlan_free_or_set_invalidp) BridgeVlan *vlan = NULL;
        Bridge *b;
        uint16_t vid, vid_end;
        int r;

        assert(data);
        b = BRIDGE(data);
        assert(b);

        r = bridge_vlan_new_static(b, filename, section_line, &vlan);
        if (r < 0)
                return log_oom();

        r = parse_vid_range(rvalue, &vid, &vid_end);
        if (r >= 0) {
                vlan->vid = vid;
                vlan->vid_end = vid_end;
        } else {
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Could not parse %s=\"%s\", ignoring assignment: %m", lvalue, rvalue);
        }

        TAKE_PTR(vlan);
        return 0;
}

int config_parse_bridge_vlan_mcast_snooping(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {
        _cleanup_(bridge_vlan_free_or_set_invalidp) BridgeVlan *vlan = NULL;
        Bridge *b;
        int r;

        assert(data);
        b = BRIDGE(data);
        assert(b);

        r = bridge_vlan_new_static(b, filename, section_line, &vlan);
        if (r < 0)
                return log_oom();

        r = parse_boolean(rvalue);
        if (r >= 0)
                vlan->mcast_snooping = r;
        else
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Could not parse %s=\"%s\", ignoring assignment: %m", lvalue, rvalue);

        TAKE_PTR(vlan);
        return 0;
}

int config_parse_bridge_vlan_mcast_querier(
                const char *unit,
                const char *filename,
                unsigned line,
                const char *section,
                unsigned section_line,
                const char *lvalue,
                int ltype,
                const char *rvalue,
                void *data,
                void *userdata) {
        _cleanup_(bridge_vlan_free_or_set_invalidp) BridgeVlan *vlan = NULL;
        Bridge *b;
        int r;

        assert(data);
        b = BRIDGE(data);
        assert(b);

        r = bridge_vlan_new_static(b, filename, section_line, &vlan);
        if (r < 0)
                return log_oom();

        r = parse_boolean(rvalue);
        if (r >= 0)
                vlan->mcast_querier = r;
        else
                log_syntax(unit, LOG_WARNING, filename, line, r,
                           "Could not parse %s=\"%s\", ignoring assignment: %m", lvalue, rvalue);

        TAKE_PTR(vlan);
        return 0;
}

static void bridge_init(NetDev *n) {
        Bridge *b;

        b = BRIDGE(n);

        assert(b);

        b->mcast_querier = -1;
        b->mcast_snooping = -1;
        b->vlan_filtering = -1;
        b->vlan_protocol = -1;
        b->stp = -1;
        b->default_pvid = VLANID_INVALID;
        b->forward_delay = USEC_INFINITY;
        b->ageing_time = USEC_INFINITY;
}

static void bridge_done(NetDev *netdev) {
        Bridge *b;

        assert(netdev);
        b = BRIDGE(netdev);
        assert(b);

        hashmap_free_with_destructor(b->vlans, bridge_vlan_free);
}

static int bridge_verify(NetDev *netdev, const char *filename) {
        Bridge *b;
        BridgeVlan *vlan;

        assert(netdev);
        b = BRIDGE(netdev);
        assert(b);

        HASHMAP_FOREACH(vlan, b->vlans) {
                if (vlan->vid < 0) {
                        log_netdev_warning_errno(netdev, SYNTHETIC_ERRNO(EINVAL),
                                                 "%s: VLAN= is not set in BridgeVLAN. Ignoring section.", filename);
                }
        }

        return 0;
}

const NetDevVTable bridge_vtable = {
        .object_size = sizeof(Bridge),
        .init = bridge_init,
        .done = bridge_done,
        .sections = NETDEV_COMMON_SECTIONS "Bridge\0BridgeVLAN\0",
        .post_create = netdev_bridge_post_create,
        .create_type = NETDEV_CREATE_MASTER,
        .config_verify = bridge_verify,
        .iftype = ARPHRD_ETHER,
        .generate_mac = true,
};
