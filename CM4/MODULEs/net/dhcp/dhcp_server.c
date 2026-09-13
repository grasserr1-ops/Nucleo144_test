/**
 * @file dhcp_server.c
 * @brief Minimal DHCP server for Nucleo↔PC direct Ethernet (option A).
 *
 * Hands out leases in 192.168.11.100–119; server/gateway = netif IP (.1).
 */
#include "dhcp_server.h"
#include "lwip/opt.h"
#include "lwip/api.h"
#include "lwip/netif.h"
#include "lwip/prot/dhcp.h"
#include "cmsis_os2.h"
#include <string.h>

#if LWIP_NETCONN && LWIP_IPV4

#define DHCP_SERVER_PORT          67U
#define DHCP_CLIENT_PORT          68U
#define DHCP_LEASE_SECS           (3600U)
#define DHCP_POOL_START           (100U)
#define DHCP_POOL_SIZE            (20U)

typedef struct {
  uint8_t mac[6];
  uint8_t used;
  uint8_t ip_host; /* last octet */
} dhcp_lease_t;

static dhcp_lease_t s_leases[DHCP_POOL_SIZE];
static struct netconn *s_conn;

static int dhcp_find_option(const struct dhcp_msg *msg, size_t msg_len,
                            uint8_t opt, uint8_t *out, size_t out_max)
{
  const uint8_t *opts;
  size_t i;
  size_t opts_len;

  if (msg_len < DHCP_OPTIONS_OFS) {
    return -1;
  }
  opts = msg->options;
  opts_len = msg_len - DHCP_OPTIONS_OFS;

  for (i = 0; i < opts_len; ) {
    uint8_t code = opts[i++];
    if (code == DHCP_OPTION_PAD) {
      continue;
    }
    if (code == DHCP_OPTION_END || (i >= opts_len)) {
      break;
    }
    uint8_t len = opts[i++];
    if ((i + len) > opts_len) {
      break;
    }
    if (code == opt) {
      size_t n = len;
      if (n > out_max) {
        n = out_max;
      }
      if ((out != NULL) && (n > 0U)) {
        memcpy(out, &opts[i], n);
      }
      return (int)len;
    }
    i += len;
  }
  return -1;
}

static uint8_t dhcp_alloc_ip(const uint8_t mac[6], uint8_t prefer_host)
{
  int i;

  for (i = 0; i < DHCP_POOL_SIZE; i++) {
    if (s_leases[i].used && (memcmp(s_leases[i].mac, mac, 6) == 0)) {
      return s_leases[i].ip_host;
    }
  }

  if ((prefer_host >= DHCP_POOL_START) &&
      (prefer_host < (DHCP_POOL_START + DHCP_POOL_SIZE))) {
    int idx = (int)prefer_host - (int)DHCP_POOL_START;
    if (!s_leases[idx].used || (memcmp(s_leases[idx].mac, mac, 6) == 0)) {
      memcpy(s_leases[idx].mac, mac, 6);
      s_leases[idx].used = 1U;
      s_leases[idx].ip_host = prefer_host;
      return prefer_host;
    }
  }

  for (i = 0; i < DHCP_POOL_SIZE; i++) {
    if (!s_leases[i].used) {
      memcpy(s_leases[i].mac, mac, 6);
      s_leases[i].used = 1U;
      s_leases[i].ip_host = (uint8_t)(DHCP_POOL_START + i);
      return s_leases[i].ip_host;
    }
  }

  memcpy(s_leases[0].mac, mac, 6);
  s_leases[0].used = 1U;
  s_leases[0].ip_host = DHCP_POOL_START;
  return DHCP_POOL_START;
}

static size_t dhcp_build_reply(struct dhcp_msg *out, const struct dhcp_msg *req,
                               uint8_t msg_type, const ip4_addr_t *server_ip,
                               uint8_t yi_host)
{
  uint8_t *opt;
  size_t opt_len;
  u32_t lease = lwip_htonl(DHCP_LEASE_SECS);
  u32_t mask = lwip_htonl(0xFFFFFF00UL);
  u32_t sip = ip4_addr_get_u32(server_ip);
  u32_t net = sip & lwip_htonl(0xFFFFFF00UL);
  u32_t yi = net | lwip_htonl((u32_t)yi_host);

  memset(out, 0, sizeof(*out));
  out->op = DHCP_BOOTREPLY;
  out->htype = req->htype ? req->htype : 1U;
  out->hlen = req->hlen ? req->hlen : 6U;
  out->xid = req->xid;
  out->flags = req->flags;
  out->yiaddr.addr = yi;
  out->siaddr.addr = sip;
  memcpy(out->chaddr, req->chaddr, DHCP_CHADDR_LEN);
  out->cookie = lwip_htonl(DHCP_MAGIC_COOKIE);

  opt = out->options;
  opt_len = 0;

  opt[opt_len++] = DHCP_OPTION_MESSAGE_TYPE;
  opt[opt_len++] = 1;
  opt[opt_len++] = msg_type;

  opt[opt_len++] = DHCP_OPTION_SERVER_ID;
  opt[opt_len++] = 4;
  memcpy(&opt[opt_len], &sip, 4);
  opt_len += 4;

  opt[opt_len++] = DHCP_OPTION_LEASE_TIME;
  opt[opt_len++] = 4;
  memcpy(&opt[opt_len], &lease, 4);
  opt_len += 4;

  opt[opt_len++] = DHCP_OPTION_SUBNET_MASK;
  opt[opt_len++] = 4;
  memcpy(&opt[opt_len], &mask, 4);
  opt_len += 4;

  opt[opt_len++] = DHCP_OPTION_ROUTER;
  opt[opt_len++] = 4;
  memcpy(&opt[opt_len], &sip, 4);
  opt_len += 4;

  opt[opt_len++] = DHCP_OPTION_END;

  return DHCP_OPTIONS_OFS + opt_len;
}

static void dhcp_handle_packet(struct netbuf *buf)
{
  struct dhcp_msg req;
  struct dhcp_msg reply;
  struct netif *netif = netif_default;
  ip4_addr_t server_ip;
  ip_addr_t dst_ip;
  uint8_t msg_type = 0;
  uint8_t req_ip[4];
  uint8_t prefer = 0;
  uint8_t yi_host;
  size_t req_len;
  size_t reply_len;
  void *data;
  u16_t len;
  struct netbuf *outbuf;
  void *payload;

  if ((netif == NULL) || !netif_is_up(netif)) {
    return;
  }
  server_ip = *netif_ip4_addr(netif);

  if ((netbuf_data(buf, &data, &len) != ERR_OK) || (data == NULL) ||
      (len < DHCP_OPTIONS_OFS)) {
    return;
  }
  if (len > sizeof(req)) {
    len = (u16_t)sizeof(req);
  }
  memcpy(&req, data, len);
  req_len = len;

  if (req.op != DHCP_BOOTREQUEST) {
    return;
  }
  if (req.cookie != lwip_htonl(DHCP_MAGIC_COOKIE)) {
    return;
  }
  if (dhcp_find_option(&req, req_len, DHCP_OPTION_MESSAGE_TYPE, &msg_type, 1) < 1) {
    return;
  }

  if (dhcp_find_option(&req, req_len, DHCP_OPTION_REQUESTED_IP, req_ip, 4) == 4) {
    prefer = req_ip[3];
  }

  yi_host = dhcp_alloc_ip(req.chaddr, prefer);

  if (msg_type == DHCP_DISCOVER) {
    reply_len = dhcp_build_reply(&reply, &req, DHCP_OFFER, &server_ip, yi_host);
  } else if (msg_type == DHCP_REQUEST) {
    reply_len = dhcp_build_reply(&reply, &req, DHCP_ACK, &server_ip, yi_host);
  } else {
    return;
  }

  IP_ADDR4(&dst_ip, 255, 255, 255, 255);

  outbuf = netbuf_new();
  if (outbuf == NULL) {
    return;
  }
  payload = netbuf_alloc(outbuf, (u16_t)reply_len);
  if (payload == NULL) {
    netbuf_delete(outbuf);
    return;
  }
  memcpy(payload, &reply, reply_len);
  (void)netconn_sendto(s_conn, outbuf, &dst_ip, DHCP_CLIENT_PORT);
  netbuf_delete(outbuf);
}

static void dhcp_server_task(void *argument)
{
  err_t err;
  (void)argument;

  memset(s_leases, 0, sizeof(s_leases));

  s_conn = netconn_new(NETCONN_UDP);
  if (s_conn == NULL) {
    for (;;) {
      osDelay(1000);
    }
  }

  err = netconn_bind(s_conn, IP_ADDR_ANY, DHCP_SERVER_PORT);
  if (err != ERR_OK) {
    for (;;) {
      osDelay(1000);
    }
  }

  for (;;) {
    struct netbuf *buf = NULL;
    err = netconn_recv(s_conn, &buf);
    if ((err == ERR_OK) && (buf != NULL)) {
      dhcp_handle_packet(buf);
      netbuf_delete(buf);
    }
  }
}

void dhcp_server_start(void)
{
  const osThreadAttr_t attr = {
    .name = "DhcpSrv",
    .stack_size = 1024 * 2,
    .priority = (osPriority_t)osPriorityNormal,
  };
  osThreadNew(dhcp_server_task, NULL, &attr);
}

#else /* !LWIP_NETCONN || !LWIP_IPV4 */

void dhcp_server_start(void)
{
}

#endif
