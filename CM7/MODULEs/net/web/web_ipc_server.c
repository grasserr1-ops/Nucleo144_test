#include "web_ipc_server.h"
#include "web_page.h"
#include "web_ipc_shared.h"
#include "cmsis_os2.h"
#include "stm32h7xx_hal.h"
#include <string.h>

static uint32_t s_version;

static void web_ipc_publish_locked(web_ipc_shared_t *sh)
{
  const char *html = web_page_get_html();
  size_t len = web_page_get_html_len();
  const char *path = web_page_get_path();

  if (len > WEB_IPC_DATA_SIZE) {
    len = WEB_IPC_DATA_SIZE;
  }

  memset((void *)sh->path, 0, WEB_IPC_PATH_SIZE);
  strncpy(sh->path, path, WEB_IPC_PATH_SIZE - 1U);
  memcpy((void *)sh->data, html, len);

  sh->size = (uint32_t)len;
  sh->content_type = WEB_IPC_CTYPE_HTML;
  sh->version = ++s_version;
  sh->magic = WEB_IPC_MAGIC;
  sh->state = WEB_IPC_STATE_READY;
  __DSB();
}

static void web_ipc_publish(void)
{
  web_ipc_shared_t *sh = web_ipc_shared();

  while (HAL_HSEM_Take(HSEM_ID_WEB, 0) != HAL_OK) {
    osDelay(1);
  }
  web_ipc_publish_locked(sh);
  HAL_HSEM_Release(HSEM_ID_WEB, 0);
}

static void web_ipc_server_task(void *argument)
{
  (void)argument;
  web_ipc_shared_t *sh = web_ipc_shared();

  __HAL_RCC_HSEM_CLK_ENABLE();
  web_ipc_publish();

  for (;;) {
    if (HAL_HSEM_Take(HSEM_ID_WEB, 0) == HAL_OK) {
      if (sh->state == WEB_IPC_STATE_REQ) {
        web_ipc_publish_locked(sh);
      }
      HAL_HSEM_Release(HSEM_ID_WEB, 0);
    }
    osDelay(20);
  }
}

void web_ipc_server_start(void)
{
  const osThreadAttr_t attr = {
    .name = "WebIpc",
    .stack_size = 512 * 4,
    .priority = (osPriority_t)osPriorityBelowNormal,
  };
  osThreadNew(web_ipc_server_task, NULL, &attr);
}
