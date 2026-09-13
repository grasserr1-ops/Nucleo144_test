#include "web_page.h"

/* Tap → local heart + LED; USER button → all clients show "love you" heart (poll only). */
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
"animation:pop .8s ease-out forwards;will-change:transform,opacity;"
"text-align:center}\n"
".heart .t{display:block;font-size:14px;margin-top:4px;opacity:.95}\n"
"@keyframes pop{0%{opacity:1;transform:scale(.5) translateY(0)}"
"100%{opacity:0;transform:scale(1.6) translateY(-40px)}}\n"
"</style>\n"
"</head>\n"
"<body>\n"
"<script>\n"
"function spawnHeart(x,y,label){\n"
"  var h=document.createElement('div');\n"
"  h.className='heart';\n"
"  h.style.left=(x-24)+'px';\n"
"  h.style.top=(y-24)+'px';\n"
"  h.innerHTML='\\u2764'+(label?'<span class=\"t\"></span>':'');\n"
"  if(label){h.querySelector('.t').textContent=label;}\n"
"  document.body.appendChild(h);\n"
"  setTimeout(function(){h.remove();},label?2000:800);\n"
"}\n"
"function loveHeart(){\n"
"  var now=Date.now();\n"
"  if(now-loveHeart._t<400)return;\n"
"  loveHeart._t=now;\n"
"  spawnHeart(window.innerWidth/2,window.innerHeight/2,'love you');\n"
"}\n"
"loveHeart._t=0;\n"
"document.body.addEventListener('click',function(e){\n"
"  spawnHeart(e.clientX,e.clientY,null);\n"
"  try{fetch('/api/click',{method:'POST',keepalive:true});}catch(err){}\n"
"});\n"
"var lastLove=-1;\n"
"var loveInFlight=false;\n"
"function pollLove(){\n"
"  if(loveInFlight)return;\n"
"  loveInFlight=true;\n"
"  fetch('/api/love?t='+Date.now(),{cache:'no-store',headers:{'Cache-Control':'no-cache','Pragma':'no-cache'}})\n"
"  .then(function(r){return r.text();})\n"
"  .then(function(t){\n"
"    var n=parseInt(t,10);\n"
"    if(isNaN(n))return;\n"
"    if(lastLove<0){lastLove=n;return;}\n"
"    if(n!==lastLove){lastLove=n;loveHeart();}\n"
"  }).catch(function(){})\n"
"  .then(function(){loveInFlight=false;});\n"
"}\n"
"setInterval(pollLove,500);\n"
"pollLove();\n"
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
