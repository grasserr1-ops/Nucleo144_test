#include "http_server.h"
#include "web_ipc_client.h"
#include "web_ipc_shared.h"
#include "lwip/api.h"
#include "lwip/opt.h"
#include "cmsis_os2.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#if LWIP_NETCONN

#define HTTP_PAGE_CACHE_SIZE  WEB_IPC_DATA_SIZE
#define HTTP_REQ_BUF_SIZE     512
#define HTTP_MAX_CONN         20

typedef struct {
  struct netconn *conn;
  uint32_t seq;
  uint8_t used;
} http_conn_slot_t;

static uint8_t s_page_cache[HTTP_PAGE_CACHE_SIZE];
static size_t s_page_len;
static uint32_t s_page_version;
static osMutexId_t s_page_mutex;
static osMutexId_t s_slots_mutex;
static osSemaphoreId_t s_conn_slots;
static http_conn_slot_t s_slots[HTTP_MAX_CONN];
static uint32_t s_conn_seq;

static int http_path_is_index(const char *req, size_t req_len)
{
  /* Expect: GET / HTTP/1.x  or  GET /index.html HTTP/1.x */
  if ((req_len < 5) || (memcmp(req, "GET ", 4) != 0)) {
    return 0;
  }
  const char *p = req + 4;
  if ((p[0] == '/') && ((p[1] == ' ') || (p[1] == '?'))) {
    return 1;
  }
  if (strncmp(p, "/index.html", 11) == 0) {
    char c = p[11];
    return (c == ' ' || c == '?' || c == '\0');
  }
  return 0;
}

static int http_path_is_click(const char *req, size_t req_len)
{
  /* Expect: POST /api/click HTTP/1.x */
  if ((req_len < 16) || (memcmp(req, "POST ", 5) != 0)) {
    return 0;
  }
  const char *p = req + 5;
  if (strncmp(p, "/api/click", 10) == 0) {
    char c = p[10];
    return (c == ' ' || c == '?' || c == '\0');
  }
  return 0;
}

static int http_path_is_events(const char *req, size_t req_len)
{
  /* Expect: GET /api/events HTTP/1.x */
  if ((req_len < 16) || (memcmp(req, "GET ", 4) != 0)) {
    return 0;
  }
  const char *p = req + 4;
  if (strncmp(p, "/api/events", 11) == 0) {
    char c = p[11];
    return (c == ' ' || c == '?' || c == '\0');
  }
  return 0;
}

static int http_path_is_love(const char *req, size_t req_len)
{
  /* Expect: GET /api/love HTTP/1.x — polling fallback for proxies */
  if ((req_len < 14) || (memcmp(req, "GET ", 4) != 0)) {
    return 0;
  }
  const char *p = req + 4;
  if (strncmp(p, "/api/love", 9) == 0) {
    char c = p[9];
    return (c == ' ' || c == '?' || c == '\0');
  }
  return 0;
}

static void http_send_response(struct netconn *conn, int ok,
                               const uint8_t *body, size_t body_len)
{
  char hdr[160];
  int n;

  if (ok) {
    n = snprintf(hdr, sizeof(hdr),
                 "HTTP/1.1 200 OK\r\n"
                 "Content-Type: text/html; charset=utf-8\r\n"
                 "Content-Length: %u\r\n"
                 "Connection: close\r\n"
                 "Cache-Control: no-cache\r\n"
                 "\r\n",
                 (unsigned)body_len);
  } else {
    static const char not_found[] =
      "<!DOCTYPE html><html><body><h1>404</h1></body></html>";
    body = (const uint8_t *)not_found;
    body_len = sizeof(not_found) - 1U;
    n = snprintf(hdr, sizeof(hdr),
                 "HTTP/1.1 404 Not Found\r\n"
                 "Content-Type: text/html; charset=utf-8\r\n"
                 "Content-Length: %u\r\n"
                 "Connection: close\r\n"
                 "\r\n",
                 (unsigned)body_len);
  }

  if (n > 0) {
    netconn_write(conn, hdr, (size_t)n, NETCONN_COPY);
    if ((body != NULL) && (body_len > 0U)) {
      netconn_write(conn, body, body_len, NETCONN_COPY);
    }
  }
}

static void http_send_no_content(struct netconn *conn)
{
  static const char hdr[] =
    "HTTP/1.1 204 No Content\r\n"
    "Connection: close\r\n"
    "Cache-Control: no-store\r\n"
    "\r\n";
  netconn_write(conn, hdr, sizeof(hdr) - 1U, NETCONN_COPY);
}

/** Long-lived Server-Sent Events stream; exits when client disconnects. */
static void http_serve_sse(struct netconn *conn)
{
  static const char hdr[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/event-stream\r\n"
    "Cache-Control: no-cache\r\n"
    "Connection: keep-alive\r\n"
    "\r\n";
  static const char hello[] = ": connected\n\n";
  static const char love_msg[] = "data: love you\n\n";
  uint32_t last_seq;
  err_t err;

  err = netconn_write(conn, hdr, sizeof(hdr) - 1U, NETCONN_COPY);
  if (err != ERR_OK) {
    return;
  }
  err = netconn_write(conn, hello, sizeof(hello) - 1U, NETCONN_COPY);
  if (err != ERR_OK) {
    return;
  }

  last_seq = web_ipc_client_love_seq();

  for (;;) {
    uint32_t cur = web_ipc_client_love_seq();
    if (cur != last_seq) {
      last_seq = cur;
      err = netconn_write(conn, love_msg, sizeof(love_msg) - 1U, NETCONN_COPY);
      if (err != ERR_OK) {
        break;
      }
    }

    /* Probe for client close without blocking forever */
#if LWIP_SO_RCVTIMEO
    {
      struct netbuf *buf = NULL;
      netconn_set_recvtimeout(conn, 50);
      err = netconn_recv(conn, &buf);
      if (buf != NULL) {
        netbuf_delete(buf);
      }
      if ((err == ERR_CLSD) || (err == ERR_RST) || (err == ERR_ABRT) ||
          (err == ERR_CONN)) {
        break;
      }
    }
#else
    osDelay(50);
#endif
  }
}

static void http_serve_conn(struct netconn *conn)
{
  struct netbuf *buf = NULL;
  char req[HTTP_REQ_BUF_SIZE];
  size_t req_len = 0;

  while (req_len + 1U < sizeof(req)) {
    err_t err = netconn_recv(conn, &buf);
    if (err != ERR_OK || buf == NULL) {
      break;
    }

    do {
      void *data = NULL;
      u16_t len = 0;
      netbuf_data(buf, &data, &len);
      if ((data != NULL) && (len > 0U)) {
        size_t copy = len;
        if (copy > (sizeof(req) - 1U - req_len)) {
          copy = sizeof(req) - 1U - req_len;
        }
        memcpy(req + req_len, data, copy);
        req_len += copy;
      }
    } while (netbuf_next(buf) >= 0);

    netbuf_delete(buf);
    buf = NULL;

    req[req_len] = '\0';
    /* Full HTTP headers end with blank line */
    if (strstr(req, "\r\n\r\n") != NULL) {
      break;
    }
  }

  if (http_path_is_events(req, req_len)) {
    http_serve_sse(conn);
    return;
  }

  if (http_path_is_love(req, req_len)) {
    char body[16];
    char hdr[256];
    int blen = snprintf(body, sizeof(body), "%lu",
                        (unsigned long)web_ipc_client_love_seq());
    int hlen = snprintf(hdr, sizeof(hdr),
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/plain; charset=utf-8\r\n"
                        "Content-Length: %d\r\n"
                        "Connection: close\r\n"
                        "Cache-Control: no-store, no-cache, must-revalidate\r\n"
                        "Pragma: no-cache\r\n"
                        "Expires: 0\r\n"
                        "\r\n",
                        blen);
    if (hlen > 0) {
      netconn_write(conn, hdr, (size_t)hlen, NETCONN_COPY);
    }
    if (blen > 0) {
      netconn_write(conn, body, (size_t)blen, NETCONN_COPY);
    }
    return;
  }

  if (http_path_is_click(req, req_len)) {
    (void)web_ipc_client_post_click();
    http_send_no_content(conn);
    return;
  }

  if (http_path_is_index(req, req_len)) {
    /* Always refresh from CM7 so HTML/JS updates are not stuck in CM4 cache */
    osMutexAcquire(s_page_mutex, osWaitForever);
    {
      size_t n = 0;
      if (web_ipc_client_fetch(s_page_cache, sizeof(s_page_cache), &n, 2000) == 0) {
        s_page_len = n;
        s_page_version = web_ipc_shared()->version;
      }
    }

    if (s_page_len > 0U) {
      http_send_response(conn, 1, s_page_cache, s_page_len);
      osMutexRelease(s_page_mutex);
    } else {
      osMutexRelease(s_page_mutex);
      static const char busy[] =
        "<!DOCTYPE html><html><body><h1>Content not ready</h1></body></html>";
      http_send_response(conn, 1, (const uint8_t *)busy, sizeof(busy) - 1U);
    }
  } else {
    http_send_response(conn, 0, NULL, 0);
  }
}

static int http_slot_register(struct netconn *conn)
{
  int idx = -1;
  uint32_t seq;

  osMutexAcquire(s_slots_mutex, osWaitForever);
  seq = ++s_conn_seq;
  for (int i = 0; i < HTTP_MAX_CONN; i++) {
    if (!s_slots[i].used) {
      s_slots[i].conn = conn;
      s_slots[i].seq = seq;
      s_slots[i].used = 1U;
      idx = i;
      break;
    }
  }
  osMutexRelease(s_slots_mutex);
  return idx;
}

static void http_slot_unregister(struct netconn *conn)
{
  osMutexAcquire(s_slots_mutex, osWaitForever);
  for (int i = 0; i < HTTP_MAX_CONN; i++) {
    if (s_slots[i].used && (s_slots[i].conn == conn)) {
      s_slots[i].used = 0U;
      s_slots[i].conn = NULL;
      s_slots[i].seq = 0U;
      break;
    }
  }
  osMutexRelease(s_slots_mutex);
}

/** Abort the oldest active connection so its worker exits and frees a slot. */
static void http_evict_oldest(void)
{
  struct netconn *victim = NULL;

  osMutexAcquire(s_slots_mutex, osWaitForever);
  {
    int best = -1;
    uint32_t best_seq = UINT32_MAX;
    for (int i = 0; i < HTTP_MAX_CONN; i++) {
      if (s_slots[i].used && (s_slots[i].conn != NULL) &&
          (s_slots[i].seq < best_seq)) {
        best_seq = s_slots[i].seq;
        best = i;
      }
    }
    if (best >= 0) {
      victim = s_slots[best].conn;
    }
  }
  osMutexRelease(s_slots_mutex);

  if (victim != NULL) {
    /* Wake blocked netconn_recv / write; worker owns close/delete */
    (void)netconn_shutdown(victim, 1, 1);
    (void)netconn_close(victim);
  }
}

static int http_acquire_slot(void)
{
  if (s_conn_slots == NULL) {
    return -1;
  }

  if (osSemaphoreAcquire(s_conn_slots, 0) == osOK) {
    return 0;
  }

  /* At capacity: drop oldest, wait for its worker to release the slot */
  http_evict_oldest();
  if (osSemaphoreAcquire(s_conn_slots, 2000) == osOK) {
    return 0;
  }
  return -1;
}

static void http_conn_worker(void *argument)
{
  struct netconn *conn = (struct netconn *)argument;

  if (conn != NULL) {
    http_serve_conn(conn);
    http_slot_unregister(conn);
    netconn_close(conn);
    netconn_delete(conn);
  }

  if (s_conn_slots != NULL) {
    osSemaphoreRelease(s_conn_slots);
  }
  osThreadExit();
}

static void http_server_task(void *argument)
{
  struct netconn *conn;
  struct netconn *newconn;
  err_t err;
  (void)argument;

  s_page_mutex = osMutexNew(NULL);
  s_slots_mutex = osMutexNew(NULL);
  s_conn_slots = osSemaphoreNew(HTTP_MAX_CONN, HTTP_MAX_CONN, NULL);
  memset(s_slots, 0, sizeof(s_slots));
  s_conn_seq = 0U;

  for (;;) {
    size_t n = 0;
    if (web_ipc_client_fetch(s_page_cache, sizeof(s_page_cache), &n, 500) == 0) {
      osMutexAcquire(s_page_mutex, osWaitForever);
      s_page_len = n;
      osMutexRelease(s_page_mutex);
      break;
    }
    osDelay(100);
  }

  conn = netconn_new(NETCONN_TCP);
  if (conn == NULL) {
    for (;;) {
      osDelay(1000);
    }
  }

  netconn_bind(conn, IP_ADDR_ANY, 80);
  netconn_listen_with_backlog(conn, HTTP_MAX_CONN);

  for (;;) {
    err = netconn_accept(conn, &newconn);
    if (err != ERR_OK || newconn == NULL) {
      continue;
    }

    if (http_acquire_slot() != 0) {
      netconn_close(newconn);
      netconn_delete(newconn);
      continue;
    }

    if (http_slot_register(newconn) < 0) {
      netconn_close(newconn);
      netconn_delete(newconn);
      osSemaphoreRelease(s_conn_slots);
      continue;
    }

    {
      const osThreadAttr_t worker_attr = {
        .name = "HttpConn",
        .stack_size = 1024 * 3,
        .priority = (osPriority_t)osPriorityNormal,
      };
      if (osThreadNew(http_conn_worker, newconn, &worker_attr) == NULL) {
        http_slot_unregister(newconn);
        netconn_close(newconn);
        netconn_delete(newconn);
        osSemaphoreRelease(s_conn_slots);
      }
    }
  }
}

void http_server_start(void)
{
  const osThreadAttr_t attr = {
    .name = "HttpSrv",
    .stack_size = 1024 * 2,
    .priority = (osPriority_t)osPriorityNormal,
  };
  osThreadNew(http_server_task, NULL, &attr);
}

#else /* !LWIP_NETCONN */

void http_server_start(void)
{
}

#endif /* LWIP_NETCONN */
