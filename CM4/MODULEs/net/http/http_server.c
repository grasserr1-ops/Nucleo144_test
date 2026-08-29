#include "http_server.h"
#include "web_ipc_client.h"
#include "web_ipc_shared.h"
#include "lwip/api.h"
#include "lwip/opt.h"
#include "cmsis_os2.h"
#include <stdio.h>
#include <string.h>

#if LWIP_NETCONN

#define HTTP_PAGE_CACHE_SIZE  WEB_IPC_DATA_SIZE
#define HTTP_REQ_BUF_SIZE     512

static uint8_t s_page_cache[HTTP_PAGE_CACHE_SIZE];
static size_t s_page_len;
static osMutexId_t s_page_mutex;

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

static void http_serve_conn(struct netconn *conn)
{
  struct netbuf *buf = NULL;
  char req[HTTP_REQ_BUF_SIZE];
  size_t req_len = 0;
  int is_index = 0;

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
    if (strstr(req, "\r\n") != NULL) {
      break;
    }
  }

  is_index = http_path_is_index(req, req_len);

  if (is_index) {
    osMutexAcquire(s_page_mutex, osWaitForever);
    if (s_page_len == 0U) {
      size_t n = 0;
      if (web_ipc_client_fetch(s_page_cache, sizeof(s_page_cache), &n, 2000) == 0) {
        s_page_len = n;
      }
    }

    if (s_page_len > 0U) {
      /* NETCONN_COPY: body is duplicated into pbufs before return */
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

static void http_server_task(void *argument)
{
  struct netconn *conn;
  struct netconn *newconn;
  err_t err;
  (void)argument;

  s_page_mutex = osMutexNew(NULL);

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
  netconn_listen(conn);

  for (;;) {
    err = netconn_accept(conn, &newconn);
    if (err == ERR_OK) {
      http_serve_conn(newconn);
      netconn_close(newconn);
      netconn_delete(newconn);
    }
  }
}

void http_server_start(void)
{
  const osThreadAttr_t attr = {
    .name = "HttpSrv",
    .stack_size = 1024 * 4,
    .priority = (osPriority_t)osPriorityNormal,
  };
  osThreadNew(http_server_task, NULL, &attr);
}

#else /* !LWIP_NETCONN */

void http_server_start(void)
{
}

#endif /* LWIP_NETCONN */
