#ifndef NET_WEB_WEB_PAGE_H_
#define NET_WEB_WEB_PAGE_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

const char *web_page_get_html(void);
size_t web_page_get_html_len(void);
const char *web_page_get_path(void);

#ifdef __cplusplus
}
#endif

#endif /* NET_WEB_WEB_PAGE_H_ */
