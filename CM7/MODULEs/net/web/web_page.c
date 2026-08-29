#include "web_page.h"

/* Fat-client page: CM7 only stores/serves bytes; browser draws the heart. */
static const char web_index_html[] =
"<!DOCTYPE html>\n"
"<html lang=\"en\">\n"
"<head>\n"
"<meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
"<title>STM32H755</title>\n"
"<style>\n"
"html,body{margin:0;height:100%;background:#111;color:#eee;"
"font-family:system-ui,sans-serif;cursor:pointer;overflow:hidden;"
"user-select:none;-webkit-user-select:none}\n"
".hint{position:fixed;left:50%;top:50%;transform:translate(-50%,-50%);"
"opacity:.45;font-size:18px;pointer-events:none}\n"
".heart{position:fixed;font-size:48px;line-height:1;pointer-events:none;"
"animation:pop .8s ease-out forwards;will-change:transform,opacity}\n"
"@keyframes pop{0%{opacity:1;transform:scale(.5) translateY(0)}"
"100%{opacity:0;transform:scale(1.6) translateY(-40px)}}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<div class=\"hint\">tap for a heart</div>\n"
"<script>\n"
"document.body.addEventListener('click',function(e){\n"
"  var h=document.createElement('div');\n"
"  h.className='heart';\n"
"  h.textContent='\\u2764';\n"
"  h.style.left=(e.clientX-24)+'px';\n"
"  h.style.top=(e.clientY-24)+'px';\n"
"  document.body.appendChild(h);\n"
"  setTimeout(function(){h.remove();},800);\n"
"});\n"
"</script>\n"
"</body>\n"
"</html>\n";

const char *web_page_get_html(void)
{
  return web_index_html;
}

size_t web_page_get_html_len(void)
{
  return sizeof(web_index_html) - 1U;
}

const char *web_page_get_path(void)
{
  return "/index.html";
}
