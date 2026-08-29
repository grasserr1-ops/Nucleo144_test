#ifndef NET_WEB_WEB_IPC_SERVER_H_
#define NET_WEB_WEB_IPC_SERVER_H_

#ifdef __cplusplus
extern "C" {
#endif

/** Publish page once and start FreeRTOS thread that serves IPC requests. */
void web_ipc_server_start(void);

#ifdef __cplusplus
}
#endif

#endif /* NET_WEB_WEB_IPC_SERVER_H_ */
