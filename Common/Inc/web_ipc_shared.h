/**
 * @file web_ipc_shared.h
 * @brief CM7↔CM4 web content IPC over SRAM4 + HSEM
 *
 * Layout lives at fixed SRAM4 base (0x38000000), visible to both cores.
 * CM7 owns HTML content; CM4 fetches a snapshot and serves HTTP.
 */
#ifndef WEB_IPC_SHARED_H
#define WEB_IPC_SHARED_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEB_IPC_MAGIC           (0x57454231u) /* 'WEB1' */
#define WEB_IPC_BASE_ADDR       (0x38000000u)
#define WEB_IPC_DATA_SIZE       (4096u)
#define WEB_IPC_PATH_SIZE       (48u)

/* HSEM_ID_0 is reserved for dual-core boot sync */
#ifndef HSEM_ID_WEB
#define HSEM_ID_WEB             (1U)
#endif

enum {
  WEB_IPC_STATE_EMPTY = 0,
  WEB_IPC_STATE_READY = 1,
  WEB_IPC_STATE_REQ   = 2
};

enum {
  WEB_IPC_CTYPE_HTML = 0
};

typedef struct {
  volatile uint32_t magic;
  volatile uint32_t state;
  volatile uint32_t version;
  volatile uint32_t size;
  volatile uint32_t content_type;
  char path[WEB_IPC_PATH_SIZE];
  uint8_t data[WEB_IPC_DATA_SIZE];
} web_ipc_shared_t;

static inline web_ipc_shared_t *web_ipc_shared(void)
{
  return (web_ipc_shared_t *)WEB_IPC_BASE_ADDR;
}

#ifdef __cplusplus
}
#endif

#endif /* WEB_IPC_SHARED_H */
