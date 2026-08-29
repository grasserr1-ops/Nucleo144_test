#include "web_ipc_client.h"
#include "web_ipc_shared.h"
#include "cmsis_os2.h"
#include "stm32h7xx_hal.h"
#include <string.h>

int web_ipc_client_fetch(uint8_t *dst, size_t dst_cap, size_t *out_len,
                         uint32_t timeout_ms)
{
  web_ipc_shared_t *sh = web_ipc_shared();
  uint32_t waited = 0;

  if ((dst == NULL) || (out_len == NULL) || (dst_cap == 0U)) {
    return -1;
  }

  __HAL_RCC_HSEM_CLK_ENABLE();

  while (waited <= timeout_ms) {
    if (HAL_HSEM_Take(HSEM_ID_WEB, 0) == HAL_OK) {
      if ((sh->magic == WEB_IPC_MAGIC) &&
          (sh->state == WEB_IPC_STATE_READY) &&
          (sh->size > 0U) &&
          (sh->size <= WEB_IPC_DATA_SIZE)) {
        size_t n = sh->size;
        if (n > dst_cap) {
          n = dst_cap;
        }
        memcpy(dst, (const void *)sh->data, n);
        *out_len = n;
        HAL_HSEM_Release(HSEM_ID_WEB, 0);
        return 0;
      }

      /* Ask CM7 to (re)publish */
      sh->state = WEB_IPC_STATE_REQ;
      HAL_HSEM_Release(HSEM_ID_WEB, 0);
    }

    osDelay(20);
    waited += 20U;
  }

  return -1;
}
