#ifndef _NETSOCK_H
#define _NETSOCK_H

#include "types.h"
#include "epoll.h"
#include "tools.h"

#define DDHCP_MULTICAST_PORT 1234
#define DDHCP_UNICAST_PORT 1235

extern const struct in6_addr in6addr_localmast;

/**
 * Derive our node id from the interface identifier of the link local
 * address of the given interface, or from the systems entropy source
 * when the interface has none.
 * Returns a value below 0 when no node id could be determined.
 */
ATTR_NONNULL_ALL int netsock_node_id_init(char* interface, ddhcp_config* config);

// ddhcpd_socket_init_t implementations
ATTR_NONNULL_ALL int netsock_multicast_init(epoll_data_t data,ddhcp_config* config);
ATTR_NONNULL_ALL int netsock_server_init(epoll_data_t data,ddhcp_config* config);
ATTR_NONNULL_ALL int netsock_dhcp_init(epoll_data_t data,ddhcp_config* config);
ATTR_NONNULL_ALL int netsock_control_init(epoll_data_t data,ddhcp_config* config);

#endif
