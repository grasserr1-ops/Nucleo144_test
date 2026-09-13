#ifndef NET_DHCP_DHCP_SERVER_H_
#define NET_DHCP_DHCP_SERVER_H_

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Start a minimal DHCP server on UDP/67 for direct PC↔board links.
 * Board is expected to already have a static IPv4 (e.g. 192.168.11.1/24).
 * Call after MX_LWIP_Init() / netif is up.
 */
void dhcp_server_start(void);

#ifdef __cplusplus
}
#endif

#endif /* NET_DHCP_DHCP_SERVER_H_ */
