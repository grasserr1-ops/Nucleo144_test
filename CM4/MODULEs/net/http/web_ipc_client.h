#ifndef NET_HTTP_WEB_IPC_CLIENT_H_
#define NET_HTTP_WEB_IPC_CLIENT_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Copy current web page from CM7 shared buffer into local cache. */
int web_ipc_client_fetch(uint8_t *dst, size_t dst_cap, size_t *out_len,
                         uint32_t timeout_ms);

/** Notify CM7 that the web page was clicked (red LED pulse). */
int web_ipc_client_post_click(void);

/** Current board-button "love you" event sequence (for SSE). */
uint32_t web_ipc_client_love_seq(void);

#ifdef __cplusplus
}
#endif

#endif /* NET_HTTP_WEB_IPC_CLIENT_H_ */
