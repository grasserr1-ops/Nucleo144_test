#ifndef NET_HTTP_HTTP_SERVER_H_
#define NET_HTTP_HTTP_SERVER_H_

#ifdef __cplusplus
extern "C" {
#endif

/** Start HTTP server thread (port 80). Call after MX_LWIP_Init(). */
void http_server_start(void);

#ifdef __cplusplus
}
#endif

#endif /* NET_HTTP_HTTP_SERVER_H_ */
