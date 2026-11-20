#include "web_console.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "ota_service.h"
#include "web_console_internal.h"

static const char *TAG = "web_console";

httpd_handle_t g_web_console_server;
web_console_context_t g_web_console_context;
/* 切换互斥采用立即失败，OTA 可能持自己的锁回调，httpd_stop 又会等待 HTTP handler 退出。 */
static int s_web_transition;
static int s_application_ready;
static bool s_routes_complete;
/* 所有模式只保管启动所需标量，完整配置的路径、字符串和拥有资源均留在配置 owner。 */
static app_runtime_io_config_t s_web_io;
static int s_event_limit = 120;
static int s_max_input_bytes = 1024;

/**
 * @brief 发布已采用配置的两个热限制，HTTP 读取无需取得配置事务锁。
 * @param[in] event_limit 原配置事件条数值，保持调用方已验证的范围。
 * @param[in] max_input_bytes 原配置消息字节上限，保持调用方已验证的范围。
 * @post 两值各自原子可见，独立请求读取各自所需值，调用不等待 Web 切换或 HTTP 退出。
 * @pre 配置 owner 串行发布已采用版本，并在最终 destroy 前关闭发布入口。
 */
void web_console_publish_limits(int event_limit, int max_input_bytes)
{
    __atomic_store_n(&s_event_limit, event_limit, __ATOMIC_RELEASE);
    __atomic_store_n(&s_max_input_bytes, max_input_bytes, __ATOMIC_RELEASE);
}

/** @brief 取得事件请求的当前默认条数。 @return 最近原子发布的事件限制，无资源交接。 */
int web_console_event_limit(void)
{
    return __atomic_load_n(&s_event_limit, __ATOMIC_ACQUIRE);
}

/** @brief 取得消息请求的当前字节上限。 @return 最近原子发布的消息限制，无资源交接。 */
int web_console_max_input_bytes(void)
{
    return __atomic_load_n(&s_max_input_bytes, __ATOMIC_ACQUIRE);
}

/** @brief 非阻塞争取唯一 Web 生命周期切换权。 @return 取得为 true，已有切换时为 false，调用方立即返回错误。 */
static bool web_transition_begin(void)
{
    int expected=0;
    return __atomic_compare_exchange_n(&s_web_transition,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
}
/** @brief 发布本次生命周期切换结束。 @pre 当前调用已取得切换权。 @post 后续调用可争取切换权。 */
static void web_transition_end(void) { __atomic_store_n(&s_web_transition,0,__ATOMIC_RELEASE); }

/**
 * @brief 比较在用自有 IO 标量与本次借用配置，以及全部回调和业务身份。
 * @param[in] a 必需当前稳定上下文，其 IO 为模块自身快照或 NULL。
 * @param[in] b 必需本次稳定借用上下文，IO 仅在本次 start 调用内解引用。
 * @return IO 标量、模式、回调和 user_data 全部相同为 true，不修改上下文。
 */
static bool web_context_matches(const web_console_context_t *a,const web_console_context_t *b)
{
    bool same_io=a->io==b->io;
    if (!same_io && a->io && b->io) {
        same_io=a->io->web.enabled==b->io->web.enabled && a->io->web.port==b->io->web.port &&
            a->io->web.event_limit==b->io->web.event_limit && a->io->max_input_bytes==b->io->max_input_bytes;
    }
    return same_io && a->config_json==b->config_json && a->config_applied==b->config_applied &&
        a->status_json==b->status_json && a->events_json==b->events_json &&
        a->message_submitted==b->message_submitted && a->file_read_allowed==b->file_read_allowed &&
        a->file_mutation_allowed==b->file_mutation_allowed && a->user_data==b->user_data &&
        a->startup_only==b->startup_only;
}


static const char INDEX_HTML[] =
"<!doctype html><html><head><meta charset='utf-8'>"
"<meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>C-Claw</title><style>"
"body{font-family:system-ui,Segoe UI,Arial,sans-serif;margin:0;background:#101418;color:#e8edf2}"
"header{padding:12px 16px;background:#17202a;display:flex;gap:12px;align-items:center}"
"button,input,textarea{font:inherit}button{padding:6px 10px;border:1px solid #405060;background:#22303b;color:#fff;border-radius:4px}"
"main{display:grid;grid-template-columns:1fr 1fr;gap:12px;padding:12px}.panel{background:#151c23;border:1px solid #2a3844;border-radius:6px;padding:10px;min-width:0}"
"pre,textarea{background:#0b0f13;color:#dce7f0;border:1px solid #2c3a45;border-radius:4px;padding:8px;white-space:pre-wrap;overflow:auto}"
"textarea{width:100%;min-height:260px;box-sizing:border-box}.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}.wide{grid-column:1/-1}"
"a{color:#8cc8ff}.small{font-size:12px;color:#aab6c0}progress{width:280px;height:18px}</style></head><body>"
"<header><b>C-Claw Web Console</b><button onclick='refreshAll()'>Refresh</button><span id='eventMeta' class='small'></span></header>"
"<main><section class='panel wide'><h3>Status</h3><pre id='status'></pre></section>"
"<section class='panel wide'><h3>缓存观测</h3><pre id='cacheMetrics'>等待状态数据</pre></section>"
"<section class='panel wide'><h3>Events</h3><pre id='events'></pre><div class='row'><textarea id='inputText' placeholder='Message or command' style='min-height:72px;flex:1'></textarea><button onclick='sendInput()'>Send</button></div></section>"
"<section class='panel wide'><h3>Config</h3><div class='row'><button onclick='loadConfig()'>Load</button><button onclick='saveConfig()'>Save</button></div><textarea id='config'></textarea></section>"
"<section class='panel wide'><h3>Files</h3><div class='row'><input id='path' value='/usb0' style='flex:1'><button onclick='listFiles()'>List</button><button onclick='mkdirPath()'>Mkdir</button></div><pre id='files'></pre><div class='row'><input id='filePath' placeholder='/usb0/path.txt' style='flex:1'><button onclick='downloadFile()'>Download</button><button onclick='deleteFile()'>Delete</button></div><textarea id='fileText' placeholder='Edit text file here'></textarea><button onclick='saveFile()'>Save file</button></section>"
"<section class='panel wide'><h3>OTA</h3><div class='row'><input id='otaFile' type='file'><button onclick='refreshOta()'>Status</button><button onclick='uploadOta()'>Upload</button><select id='otaBootPartition'></select><button onclick='selectOtaBoot()'>Set boot</button><button onclick='bootOta()'>Reboot</button><button onclick='cancelOta()'>Cancel</button></div><div class='row'><progress id='otaProgress' max='100' value='0'></progress><span id='otaProgressText' class='small'>idle</span></div><pre id='ota'></pre></section></main>"
"<script>"
"async function webFetch(u,o){return fetch(u,o)}"
"let since=0,cache=[],viewCache=[],viewSeq=0,eventLoading=false,eventNotice='',clearGeneration=null;async function j(u,o){let r=await webFetch(u,o);let t=await r.text();try{return JSON.parse(t)}catch(e){return t}}"
"function renderEvents(e){if(!e||typeof e!=='object'||!Array.isArray(e.events)||!Number.isInteger(e.latest_seq)||(Array.isArray(e.view)&&(!Number.isInteger(e.view_seq)||e.view_seq>e.latest_seq)))throw new Error('invalid event response');let cleared=clearGeneration!==null&&Number.isInteger(e.clear_generation)&&e.clear_generation!==clearGeneration;if(Number.isInteger(e.clear_generation))clearGeneration=e.clear_generation;let reset=e.cursor_reset||e.latest_seq<since;let gap=e.gap||(since>0&&e.oldest_seq>since+1);if(cleared||reset||gap||e.truncated){cache=[];viewCache=[];viewSeq=0;eventNotice=cleared?'display cleared':reset?'device cursor reset; showing recent events':gap?'event history overwritten; showing recent events':'event response truncated; showing recent events'}if(Array.isArray(e.view)){viewCache=e.view.slice();viewSeq=e.view_seq}let last=cache.length?Math.max(viewSeq,cache[cache.length-1].seq):viewSeq;e.events.forEach(x=>{if(x.seq>last){cache.push(x);last=x.seq}});if(cache.length>160)cache=cache.slice(cache.length-160);since=e.latest_seq;eventMeta.textContent='seq='+since+' dropped='+(e.dropped||0)+(eventNotice?' | '+eventNotice:'');events.textContent=viewCache.concat(cache).map(x=>x.seq+' ['+x.source+'/'+x.kind+'/'+x.level+'] '+x.text).join('\\n')}"
"async function refreshEvents(){if(eventLoading)return;eventLoading=true;try{let r=await webFetch('/api/events?since='+since);if(!r.ok)throw new Error('HTTP '+r.status);renderEvents(await r.json())}catch(e){eventMeta.textContent='events unavailable: '+e.message}finally{eventLoading=false}}"
"function formatCacheMetrics(m){if(!m||!Array.isArray(m.models))return '缓存观测数据不可用';"
"let n=x=>Number.isSafeInteger(x)&&x>=0?x.toLocaleString('en-US'):'—',name=x=>typeof x==='string'&&x?x.replace(/\\s+/g,' ').slice(0,128):'未知';"
"let lines=['自启动以来观察到的事件；事件可能丢失，不能作为完整账单。'];"
"if(!m.models.length)lines.push('尚未观察到模型用量事件。');"
"m.models.forEach(x=>{if(!x||typeof x!=='object')return;let measured=Number.isSafeInteger(x.cache_measured)&&x.cache_measured>0;"
"let rate=measured&&!x.token_totals_saturated&&Number.isInteger(x.cache_hit_basis_points)&&x.cache_hit_basis_points>=0&&x.cache_hit_basis_points<=10000?(x.cache_hit_basis_points/100).toFixed(2)+'%':'不可计算';"
"lines.push(name(x.provider)+' / '+name(x.model),'  输入 token 复用比例：'+rate+(measured?'（'+n(x.cached_tokens)+' / '+n(x.prompt_tokens)+'）':''),'  可计量 / 已观察用量事件：'+n(x.cache_measured)+' / '+n(x.usage_events)+'；未报告：'+n(x.cache_unreported)+'；无效：'+n(x.cache_unusable));"
"let write=Number.isSafeInteger(x.cache_write_reported)&&x.cache_write_reported>0?n(x.cache_write_tokens)+(x.cache_write_tokens_saturated?'（累计值已饱和）':''):'无有效数据';"
"lines.push('  缓存写入 token：'+write+'；已报告 / 未报告 / 无效事件：'+n(x.cache_write_reported)+' / '+n(x.cache_write_unreported)+' / '+n(x.cache_write_unusable));});"
"if(m.summary_cache&&typeof m.summary_cache==='object'){let s=m.summary_cache;lines.push('本地摘要：完整命中 '+n(s.hit)+'；前缀命中 '+n(s.prefix_hit)+'；未命中 '+n(s.miss)+'；拒绝复用 '+n(s.rejected));}"
"if(Number.isSafeInteger(m.model_events_omitted)&&m.model_events_omitted>0)lines.push('未计入模型统计的事件：'+n(m.model_events_omitted));return lines.join('\\n')}"
"async function refreshAll(){let s=await j('/api/status');status.textContent=JSON.stringify(s,null,2);cacheMetrics.textContent=formatCacheMetrics(s&&s.profile&&s.profile.cache_metrics);refreshEvents()}"
"async function sendInput(){let text=inputText.value.trim();if(!text)return;inputText.value='';let r=await webFetch('/api/messages',{method:'POST',body:text});if(!r.ok)alert(await r.text());refreshEvents()}"
"async function loadConfig(){config.value=await (await webFetch('/api/config')).text()}"
"async function saveConfig(){alert(await (await webFetch('/api/config',{method:'PUT',body:config.value})).text())}"
"async function listFiles(){files.textContent=JSON.stringify(await j('/api/files?path='+encodeURIComponent(path.value)),null,2)}"
"async function downloadFile(){fileText.value=await (await webFetch('/api/file?path='+encodeURIComponent(filePath.value))).text()}"
"async function saveFile(){alert(await (await webFetch('/api/file?path='+encodeURIComponent(filePath.value),{method:'PUT',body:fileText.value})).text());listFiles()}"
"async function deleteFile(){alert(await (await webFetch('/api/file?path='+encodeURIComponent(filePath.value),{method:'DELETE'})).text());listFiles()}"
"async function mkdirPath(){alert(await (await webFetch('/api/mkdir?path='+encodeURIComponent(path.value),{method:'POST'})).text());listFiles()}"
"let otaTimer=0;function fmtBytes(n){n=Number(n)||0;if(n<1024)return n+' B';if(n<1048576)return Math.round(n/1024)+' KB';return (n/1048576).toFixed(1)+' MB'}"
"function renderOtaPartitions(parts){let old=otaBootPartition.value;if(!Array.isArray(parts))return;otaBootPartition.innerHTML=parts.map(p=>'<option value=\"'+p.label+'\">'+p.label+(p.running?' running':'')+(p.boot?' boot':'')+'</option>').join('');if(parts.some(p=>p.label===old))otaBootPartition.value=old;else{let b=parts.find(p=>p.boot)||parts.find(p=>p.running)||parts[0];if(b)otaBootPartition.value=b.label}}"
"function renderOtaStatus(s){if(typeof s==='string'){ota.textContent=s;return}renderOtaPartitions(s.partitions);let p=s.progress||0;otaProgress.value=p;let m=s.maintenance?' maintenance '+(s.maintenance_reason||'ota'):'';let b=s.boot?' boot '+s.boot:'';otaProgressText.textContent=s.state+' '+p+'% device '+fmtBytes(s.bytes)+'/'+fmtBytes(s.expected)+m+b;ota.textContent=JSON.stringify(s,null,2)}"
"async function refreshOta(){renderOtaStatus(await j('/api/ota/status'))}"
"function otaPoll(on){if(on&&!otaTimer)otaTimer=setInterval(refreshOta,1000);if(!on&&otaTimer){clearInterval(otaTimer);otaTimer=0}}"
"function badOtaName(n){return ['bootloader.bin','partition-table.bin','ota_data_initial.bin'].includes(n)||n.includes('CMakeDetermineCompilerABI')}"
    /* 局域网 HTTP 页面可能缺少 SubtleCrypto，采用 FIPS 180-4 SHA-256 回退，每 MiB 让出一次 UI 执行。 */
"async function otaSha256(buffer){let hex=a=>Array.from(a,b=>b.toString(16).padStart(2,'0')).join('');if(globalThis.crypto&&globalThis.crypto.subtle){try{return hex(new Uint8Array(await globalThis.crypto.subtle.digest('SHA-256',buffer)))}catch(e){}}"
"const k=new Uint32Array([0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2]);"
"let h=new Uint32Array([0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19]),data=new Uint8Array(buffer),n=data.length,total=Math.ceil((n+9)/64)*64,w=new Uint32Array(64),r=(v,s)=>(v>>>s)|(v<<(32-s));"
"for(let off=0;off<total;off+=64){w.fill(0);for(let j=0;j<64;j++){let p=off+j,b=p<n?data[p]:p===n?128:0;w[j>>>2]|=b<<(24-(j%4)*8)}if(off+64===total){w[14]=Math.floor(n/536870912);w[15]=(n*8)>>>0}for(let j=16;j<64;j++){let a=w[j-15],b=w[j-2];w[j]=(w[j-16]+(r(a,7)^r(a,18)^(a>>>3))+w[j-7]+(r(b,17)^r(b,19)^(b>>>10)))>>>0}let [a,b,c,d,e,f,g,z]=h;for(let j=0;j<64;j++){let t1=(z+(r(e,6)^r(e,11)^r(e,25))+((e&f)^(~e&g))+k[j]+w[j])>>>0,t2=((r(a,2)^r(a,13)^r(a,22))+((a&b)^(a&c)^(b&c)))>>>0;z=g;g=f;f=e;e=(d+t1)>>>0;d=c;c=b;b=a;a=(t1+t2)>>>0}let v=[a,b,c,d,e,f,g,z];for(let j=0;j<8;j++)h[j]=(h[j]+v[j])>>>0;if((off+64)%1048576===0)await new Promise(resolve=>setTimeout(resolve,0))}return Array.from(h,v=>v.toString(16).padStart(8,'0')).join('')}"
"async function uploadOta(){let f=otaFile.files[0];if(!f)return alert('choose bin');if(badOtaName(f.name))return alert('OTA requires the app image: use build/CClaw.bin');otaPoll(false);otaProgress.value=0;otaProgressText.textContent='hashing '+f.name;let h;try{h=await otaSha256(await f.arrayBuffer())}catch(e){otaProgressText.textContent='hashing failed: '+e.message;return}let t=encodeURIComponent(new Date().toISOString());otaProgressText.textContent='preparing OTA '+f.name;let pr=await webFetch('/api/ota/prepare?name='+encodeURIComponent(f.name)+'&size='+f.size+'&sha256='+h+'&time='+t,{method:'POST'});let pt=await pr.text();if(!pr.ok){ota.textContent=pt;otaProgressText.textContent='prepare failed HTTP '+pr.status;refreshOta();return}ota.textContent=pt;otaProgressText.textContent='uploading '+f.name;let x=new XMLHttpRequest();x.open('POST','/api/ota/upload?name='+encodeURIComponent(f.name)+'&sha256='+h+'&time='+t);x.upload.onprogress=e=>{if(e.lengthComputable){let p=Math.round(e.loaded*100/e.total);otaProgress.value=p;otaProgressText.textContent='browser sent '+p+'% '+fmtBytes(e.loaded)+'/'+fmtBytes(e.total)}};x.onload=()=>{otaProgressText.textContent=(x.status>=200&&x.status<300?'upload complete':'upload failed')+' HTTP '+x.status;ota.textContent=x.responseText;refreshOta()};x.onerror=async()=>{otaProgressText.textContent='upload network error';await webFetch('/api/ota/cancel',{method:'POST'});refreshOta()};x.send(f)}"
"async function selectOtaBoot(){let label=otaBootPartition.value;if(!label)return alert('choose partition');let r=await webFetch('/api/ota/select?label='+encodeURIComponent(label),{method:'POST'});let t=await r.text();let o={};try{o=JSON.parse(t)}catch(e){}if(!r.ok){alert(t);return false}otaProgressText.textContent=o.reboot_required===false?'boot set to '+(o.boot||label)+'; already running':'boot set to '+(o.boot||label)+'; reboot board to start it';refreshOta();return true}"
"async function bootOta(){otaProgressText.textContent='rebooting board; connection will drop';try{let r=await webFetch('/api/ota/reboot',{method:'POST'});let t=await r.text();ota.textContent=t;if(!r.ok)otaProgressText.textContent='reboot failed HTTP '+r.status}catch(e){ota.textContent='device is rebooting'}}"
"async function cancelOta(){alert(await (await webFetch('/api/ota/cancel',{method:'POST'})).text());refreshOta()}"
"refreshAll();loadConfig();setInterval(refreshEvents,2000);</script></body></html>";

static const char STARTUP_HTML[] =
"<!doctype html><html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>C-Claw 启动</title><style>body{font-family:system-ui;margin:2em;background:#101418;color:#e8edf2}"
"button{font:inherit;padding:.6em 1em}pre{white-space:pre-wrap;overflow-wrap:anywhere}</style></head><body>"
"<h1>C-Claw 启动</h1><p id='state'>正在检查存储设备…</p>"
"<pre id='events'></pre>"
"<script>const startupState=document.getElementById('state'),startupEvents=document.getElementById('events');"
"async function refreshStartup(){try{let r=await fetch('/api/status');if(!r.ok)throw Error('status');"
"let s=await r.json();if(s.phase==='running'||(!s.phase&&s.interaction&&s.interaction.started)){location.reload();return}startupState.textContent=s.message||s.phase||'正在启动';"
"let e=await (await fetch('/api/events?since=0&limit=120')).json();"
"let v=Array.isArray(e.view)?e.view:[],n=Number(e.view_seq)||0;startupEvents.textContent=v.concat((e.events||[]).filter(x=>x.seq>n)).map(x=>x.text).join('\\n')"
"}catch(e){startupState.textContent='设备正在启动或连接已中断，正在重试…'}}"
"refreshStartup();setInterval(refreshStartup,2000);</script></body></html>";

/**
 * @brief 按当前启动模式发送内嵌启动页或完整控制台页面。
 * @param[in,out] req HTTPD 借用请求，覆盖同步发送期。
 * @return 文本响应发送结果，页面为静态资源，无堆所有权交接。
 */
esp_err_t web_console_index_handler(httpd_req_t *req)
{
    return web_console_send_text(req, g_web_console_context.startup_only ? STARTUP_HTML : INDEX_HTML,
                                 "text/html; charset=utf-8");
}

/**
 * @brief 向当前 server 注册单条路由并记录失败日志。
 * @param[in] uri 必需静态路径，交由 HTTPD 注册过程使用。
 * @param[in] method HTTP 请求方法。
 * @param[in] handler 必需请求入口，覆盖 server 执行期。
 * @return 原生路由注册结果，不在本函数回收已安装路由。
 * @pre 调用者持切换权，当前 server 已启动。
 */
static esp_err_t register_uri(const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *))
{
    httpd_uri_t cfg = {.uri = uri, .method = method, .handler = handler};
    esp_err_t err = httpd_register_uri_handler(g_web_console_server, &cfg);
    if (err != ESP_OK) ESP_LOGW(TAG, "register %s failed: %s", uri, esp_err_to_name(err));
    return err;
}

/**
 * @brief 为每个应用路由生成相同的就绪检查入口。
 * @param[in,out] req HTTPD 借用请求，由生成的 ready_* 函数同步转交对应 handler。
 * @return 未就绪时发送 HTTP 500 的结果，就绪时返回对应 handler 的结果。
 * @note s_application_ready 的 acquire 读取配对路由发布，应用路由完整安装前保持拒绝。
 */
#define WEB_APPLICATION_HANDLER(name) \
    static esp_err_t ready_##name(httpd_req_t *req) { \
        if (!__atomic_load_n(&s_application_ready,__ATOMIC_ACQUIRE)) \
            return web_console_send_error(req,HTTPD_500_INTERNAL_SERVER_ERROR,"Console is not ready"); \
        return web_console_##name##_handler(req); \
    }
WEB_APPLICATION_HANDLER(config_get)
WEB_APPLICATION_HANDLER(config_put)
WEB_APPLICATION_HANDLER(files)
WEB_APPLICATION_HANDLER(file)
WEB_APPLICATION_HANDLER(mkdir)
WEB_APPLICATION_HANDLER(ota_status)
WEB_APPLICATION_HANDLER(ota_prepare)
WEB_APPLICATION_HANDLER(ota_upload)
WEB_APPLICATION_HANDLER(ota_select)
WEB_APPLICATION_HANDLER(ota_reboot)
WEB_APPLICATION_HANDLER(ota_cancel)
#undef WEB_APPLICATION_HANDLER

/**
 * @brief 允许启动模式输入，在完整模式下先核对路由发布状态。
 * @param[in,out] req HTTPD 借用请求，调用期间由 server 保活。
 * @return 未就绪时为错误响应结果，其余为输入 handler 的结果。
 */
static esp_err_t ready_input(httpd_req_t *req)
{
    if (!g_web_console_context.startup_only &&
        !__atomic_load_n(&s_application_ready, __ATOMIC_ACQUIRE)) {
        return web_console_send_error(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Console is not ready");
    }
    return web_console_input_handler(req);
}

/**
 * @brief 为 OTA 请求分配内部 DRAM I/O 缓冲。
 * @param[in] size 请求字节数。
 * @return 拥有缓冲或 NULL，由 OTA 按其 transport 契约释放。
 */
static char *web_ota_alloc(size_t size)
{
    return web_console_internal_alloc(size);
}

/**
 * @brief 将 OTA 的不透明请求转交文本响应入口。
 * @param[in,out] request 借用 httpd_req_t，请求寿命覆盖同步发送。
 * @param[in] text 借用正文，NULL 沿通用响应入口发送空串。
 * @param[in] type 借用 Content-Type，NULL 沿通用入口采用纯文本。
 * @return 通用文本发送结果，不接管正文。
 */
static esp_err_t web_ota_send_text(void *request, const char *text, const char *type)
{
    return web_console_send_text((httpd_req_t *)request, text, type);
}

/**
 * @brief 将 OTA 的整数 HTTP 错误转交通用错误响应入口。
 * @param[in,out] request 借用 httpd_req_t，覆盖同步发送。
 * @param[in] code 可转换到 httpd_err_code_t 的错误枚举。
 * @param[in] message 借用错误描述，沿通用入口处理 NULL。
 * @return 通用错误发送结果，不接管描述字符串。
 */
static esp_err_t web_ota_send_error(void *request, int code, const char *message)
{
    return web_console_send_error((httpd_req_t *)request, (httpd_err_code_t)code, message);
}

/**
 * @brief 为 OTA 提取 URL 查询值副本。
 * @param[in] request 借用 httpd_req_t，请求寿命覆盖查询。
 * @param[in] key 必需借用查询键名。
 * @return PSRAM 拥有字符串供调用者 heap_caps_free，缺失或分配失败为 NULL。
 */
static char *web_ota_query_value(void *request, const char *key)
{
    return web_console_query_value((httpd_req_t *)request, key);
}

/**
 * @brief 按 OTA 指定容量接收本次请求体片段。
 * @param[in,out] request 借用 httpd_req_t，覆盖接收调用。
 * @param[out] buffer 必需可写借用缓冲，容量至少 length 字节。
 * @param[in] length 最大接收字节数，调用者保证非负。
 * @return 原生接收字节数或 HTTPD 负错误码，不追加 NUL。
 */
static int web_ota_recv(void *request, char *buffer, int length)
{
    return httpd_req_recv((httpd_req_t *)request, buffer, length);
}

/**
 * @brief 将 OTA 工作任务的停止请求转交 Web 生命周期入口。
 * @param[in] user_data 未使用，允许 NULL。
 * @return web_console_stop 结果，切换忙立即失败。
 */
static esp_err_t web_ota_stop(void *user_data)
{
    (void)user_data;
    return web_console_stop();
}

/**
 * @brief 从 OTA 失败恢复路径重新开放保存的 Web 上下文。
 * @param[in] user_data 未使用，允许 NULL。
 * @return web_console_restart 结果，保存的自有 IO 和当前热限制继续有效。
 */
static esp_err_t web_ota_start(void *user_data)
{
    (void)user_data;
    return web_console_restart();
}

/** @brief 停止当前 server 的内部入口。 @return 停止结果，失败保留句柄。 @pre 当前调用已持切换权。 */
static esp_err_t web_console_stop_locked(void);

/**
 * @brief 复制首次输入标量后启动 HTTPD，完整注册成功才发布应用路由就绪。
 * @param[in] context 可选稳定借用上下文，NULL 使用零回调及默认限制；自身快照重启保留热限制。
 * @return 禁用、相同上下文已运行或全部路由成功为 ESP_OK，其他错误按原生结果返回。
 * @pre 当前调用已持切换权，外部 IO 在本次调用期间保持稳定。
 * @note HTTP task 固定内部内存能力和 8192 字节栈，注册失败尝试停止，停止失败保留部分 server 并拒绝应用路由。
 */
static esp_err_t web_console_start_locked(const web_console_context_t *context)
{
    web_console_context_t next_context;
    memset(&next_context, 0, sizeof(next_context));
    if (context) next_context = *context;
    if (g_web_console_server) {
        if (!s_routes_complete || !web_context_matches(&g_web_console_context,&next_context))
            return ESP_ERR_INVALID_STATE;
        __atomic_store_n(&s_application_ready,!next_context.startup_only,__ATOMIC_RELEASE);
        return ESP_OK;
    }
    __atomic_store_n(&s_application_ready,0,__ATOMIC_RELEASE);
    s_routes_complete=false;
    if (next_context.io && next_context.io != &s_web_io) {
        /* 输入仅在本次调用期间借用，先复制以允许重启或回滚传入模块自身快照。 */
        app_runtime_io_config_t copy={0};
        copy.web=next_context.io->web;
        copy.max_input_bytes=next_context.io->max_input_bytes;
        s_web_io=copy;
        next_context.io=&s_web_io;
        web_console_publish_limits(copy.web.event_limit, copy.max_input_bytes);
    } else if (!next_context.io) {
        web_console_publish_limits(120, 1024);
    }
    memset(&g_web_console_context, 0, sizeof(g_web_console_context));
    g_web_console_context = next_context;
    const app_runtime_io_config_t *io = g_web_console_context.io;
    if (io && !io->web.enabled) {
        ESP_LOGI(TAG, "web console disabled");
        s_routes_complete=true;
        return ESP_OK;
    }

    const ota_service_transport_t ota_transport = {
        .alloc_io = web_ota_alloc,
        .send_text = web_ota_send_text,
        .send_error = web_ota_send_error,
        .query_value = web_ota_query_value,
        .recv = web_ota_recv,
        .stop_web = web_ota_stop,
        .start_web = web_ota_start,
        .request_timeout_code = HTTPD_SOCK_ERR_TIMEOUT,
        .bad_request_code = HTTPD_400_BAD_REQUEST,
        .server_error_code = HTTPD_500_INTERNAL_SERVER_ERROR,
        .io_chunk = WEB_IO_CHUNK,
    };
    if (!g_web_console_context.startup_only) {
        esp_err_t ota_init_err = ota_service_init(&ota_transport);
        if (ota_init_err != ESP_OK) {
            ESP_LOGE(TAG, "OTA service init failed: %s", esp_err_to_name(ota_init_err));
            return ota_init_err;
        }
    }

    httpd_config_t httpd_config = HTTPD_DEFAULT_CONFIG();
    httpd_config.server_port = io && io->web.port > 0 ? (uint16_t)io->web.port : 80;
    httpd_config.uri_match_fn = httpd_uri_match_wildcard;
    httpd_config.lru_purge_enable = true;
    httpd_config.max_open_sockets = 8;
    httpd_config.recv_wait_timeout = 5;
    httpd_config.send_wait_timeout = 5;
    httpd_config.backlog_conn = 8;
    httpd_config.stack_size = 8192;
    httpd_config.max_uri_handlers = 20;
    httpd_config.task_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    esp_err_t err = httpd_start(&g_web_console_server, &httpd_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "web console start failed: %s", esp_err_to_name(err));
        g_web_console_server = NULL;
        return err;
    }
    #define REGISTER_REQUIRED(uri,method,handler) do { \
        err=register_uri(uri,method,handler); if (err!=ESP_OK) goto registration_failed; \
    } while (0)
    REGISTER_REQUIRED("/", HTTP_GET, web_console_index_handler);
    REGISTER_REQUIRED("/api/status", HTTP_GET, web_console_status_handler);
    REGISTER_REQUIRED("/api/events", HTTP_GET, web_console_events_handler);
    REGISTER_REQUIRED("/api/messages", HTTP_POST, ready_input);
    if (g_web_console_context.startup_only) {
        s_routes_complete=true;
        ESP_LOGI(TAG, "web startup console started on port %u", (unsigned)httpd_config.server_port);
        return ESP_OK;
    }
    REGISTER_REQUIRED("/api/config", HTTP_GET, ready_config_get);
    REGISTER_REQUIRED("/api/config", HTTP_PUT, ready_config_put);
    REGISTER_REQUIRED("/api/files", HTTP_GET, ready_files);
    REGISTER_REQUIRED("/api/file", HTTP_GET, ready_file);
    REGISTER_REQUIRED("/api/file", HTTP_PUT, ready_file);
    REGISTER_REQUIRED("/api/file", HTTP_DELETE, ready_file);
    REGISTER_REQUIRED("/api/mkdir", HTTP_POST, ready_mkdir);
    REGISTER_REQUIRED("/api/ota/status", HTTP_GET, ready_ota_status);
    REGISTER_REQUIRED("/api/ota/prepare", HTTP_POST, ready_ota_prepare);
    REGISTER_REQUIRED("/api/ota/upload", HTTP_POST, ready_ota_upload);
    REGISTER_REQUIRED("/api/ota/select", HTTP_POST, ready_ota_select);
    REGISTER_REQUIRED("/api/ota/reboot", HTTP_POST, ready_ota_reboot);
    REGISTER_REQUIRED("/api/ota/cancel", HTTP_POST, ready_ota_cancel);
    #undef REGISTER_REQUIRED
    s_routes_complete=true;
    __atomic_store_n(&s_application_ready,1,__ATOMIC_RELEASE);
    ESP_LOGI(TAG, "web console started on port %u", (unsigned)httpd_config.server_port);
    return ESP_OK;
registration_failed: {
    esp_err_t stop_err=web_console_stop_locked();
    if (stop_err!=ESP_OK) ESP_LOGE(TAG,"partial web server could not stop: %s",esp_err_to_name(stop_err));
    return err;
}
}

/**
 * @brief 非阻塞取得切换权后执行受限或完整启动。
 * @param[in] context 调用期间稳定借用上下文，复制与长期回调寿命见公共头。
 * @return 切换忙为 INVALID_STATE，其余返回内部启动结果，所有出口释放切换权。
 */
esp_err_t web_console_start(const web_console_context_t *context)
{
    if (!web_transition_begin()) return ESP_ERR_INVALID_STATE;
    esp_err_t err=web_console_start_locked(context);
    web_transition_end(); return err;
}

/**
 * @brief 先撤销应用路由就绪，再调用 HTTPD 停止并排空 handler。
 * @return 无 server 或停止成功为 ESP_OK，原生失败保留句柄和路由完整性供重试。
 * @pre 当前调用已持切换权，当前执行者不属于被停止 server 的 handler。
 * @note 成功保留上下文与自有 IO，停止失败的完整 server 可通过 restart 再次放行。
 */
static esp_err_t web_console_stop_locked(void)
{
    __atomic_store_n(&s_application_ready,0,__ATOMIC_RELEASE);
    if (!g_web_console_server) {
        s_routes_complete=false;
        return ESP_OK;
    }
    httpd_handle_t server = g_web_console_server;
    esp_err_t err=httpd_stop(server);
    if (err==ESP_OK) { g_web_console_server=NULL; s_routes_complete=false; }
    return err;
}

/** @brief 立即争取切换权并停止 server。 @return 切换忙为 INVALID_STATE，其余为内部停止结果，上下文继续保留。 */
esp_err_t web_console_stop(void)
{
    if (!web_transition_begin()) return ESP_ERR_INVALID_STATE;
    esp_err_t err=web_console_stop_locked();
    web_transition_end(); return err;
}

/** @brief 使用已保存自有 IO 与回调恢复服务。 @return 切换忙或无 IO 为 INVALID_STATE，其余为启动结果，所有出口释放切换权。 */
esp_err_t web_console_restart(void)
{
    if (!web_transition_begin()) return ESP_ERR_INVALID_STATE;
    if (!g_web_console_context.io) { web_transition_end(); return ESP_ERR_INVALID_STATE; }
    web_console_context_t context = g_web_console_context;
    esp_err_t err=web_console_start_locked(&context);
    web_transition_end(); return err;
}

/** @brief 停止成功后清空上下文、标量快照与热限制。 @return 切换忙或停止失败保留状态供重试，成功为 ESP_OK。 */
esp_err_t web_console_destroy(void)
{
    if (!web_transition_begin()) return ESP_ERR_INVALID_STATE;
    esp_err_t err = web_console_stop_locked();
    if (err == ESP_OK) {
        memset(&g_web_console_context, 0, sizeof(g_web_console_context));
        memset(&s_web_io,0,sizeof(s_web_io));
        web_console_publish_limits(120, 1024);
    }
    web_transition_end();
    return err;
}

/**
 * @brief 保存旧自有 IO 与热限制后替换启动 server，失败时尽可能恢复启动上下文。
 * @param[in] context 必需完整模式稳定借用上下文，IO 只在调用内复制。
 * @param[out] out_startup_restored 可选输出，进入置 false，旧 server 保留或恢复成功置 true。
 * @return 参数错误、切换错误或最初停止/启动错误原样返回，完整启动成功为 ESP_OK。
 * @note 旧 IO 用栈副本跨越新启动对模块快照的覆盖，恢复后模块再次拥有自己的副本。
 */
esp_err_t web_console_promote_startup(const web_console_context_t *context, bool *out_startup_restored)
{
    if (out_startup_restored) *out_startup_restored=false;
    if (!context || context->startup_only) return ESP_ERR_INVALID_ARG;
    if (!web_transition_begin()) return ESP_ERR_INVALID_STATE;
    if (!g_web_console_context.startup_only) { web_transition_end(); return ESP_ERR_INVALID_STATE; }
    web_console_context_t old=g_web_console_context;
    app_runtime_io_config_t old_io = {0};
    if (old.io) { old_io = *old.io; old.io = &old_io; }
    int old_event_limit = web_console_event_limit();
    int old_max_input_bytes = web_console_max_input_bytes();
    esp_err_t err=web_console_stop_locked();
    if (err!=ESP_OK) {
        if (out_startup_restored) *out_startup_restored=true;
        web_transition_end(); return err;
    }
    err=web_console_start_locked(context);
    if (err!=ESP_OK && !g_web_console_server) {
        esp_err_t restore=web_console_start_locked(&old);
        web_console_publish_limits(old_event_limit, old_max_input_bytes);
        if (out_startup_restored) *out_startup_restored=restore==ESP_OK;
        if (restore!=ESP_OK) ESP_LOGE(TAG,"startup console restore failed: %s",esp_err_to_name(restore));
    }
    web_transition_end(); return err;
}
