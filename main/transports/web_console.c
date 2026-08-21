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
"<section class='panel wide'><h3>Events</h3><pre id='events'></pre><div class='row'><textarea id='inputText' placeholder='Message or command' style='min-height:72px;flex:1'></textarea><button onclick='sendInput()'>Send</button></div></section>"
"<section class='panel wide'><h3>Config</h3><div class='row'><button onclick='loadConfig()'>Load</button><button onclick='saveConfig()'>Save</button></div><textarea id='config'></textarea></section>"
"<section class='panel wide'><h3>Files</h3><div class='row'><input id='path' value='/usb0' style='flex:1'><button onclick='listFiles()'>List</button><button onclick='mkdirPath()'>Mkdir</button></div><pre id='files'></pre><div class='row'><input id='filePath' placeholder='/usb0/path.txt' style='flex:1'><button onclick='downloadFile()'>Download</button><button onclick='deleteFile()'>Delete</button></div><textarea id='fileText' placeholder='Edit text file here'></textarea><button onclick='saveFile()'>Save file</button></section>"
"<section class='panel wide'><h3>OTA</h3><div class='row'><input id='otaFile' type='file'><button onclick='refreshOta()'>Status</button><button onclick='uploadOta()'>Upload</button><select id='otaBootPartition'></select><button onclick='selectOtaBoot()'>Set boot</button><button onclick='bootOta()'>Reboot</button><button onclick='cancelOta()'>Cancel</button></div><div class='row'><progress id='otaProgress' max='100' value='0'></progress><span id='otaProgressText' class='small'>idle</span></div><pre id='ota'></pre></section></main>"
"<script>"
"let since=0,cache=[];async function j(u,o){let r=await fetch(u,o);let t=await r.text();try{return JSON.parse(t)}catch(e){return t}}"
"function renderEvents(e){if(typeof e==='string'){events.textContent=e;return}(e.events||[]).forEach(x=>cache.push(x));if(cache.length>160)cache=cache.slice(cache.length-160);since=e.latest_seq||since;eventMeta.textContent='seq='+since+' dropped='+(e.dropped||0);events.textContent=cache.map(x=>x.seq+' ['+x.source+'/'+x.kind+'/'+x.level+'] '+x.text).join('\\n')}"
"async function refreshEvents(){renderEvents(await j('/api/events?since='+since))}"
"async function refreshAll(){status.textContent=JSON.stringify(await j('/api/status'),null,2);refreshEvents()}"
"async function sendInput(){let text=inputText.value.trim();if(!text)return;inputText.value='';let r=await fetch('/api/messages',{method:'POST',body:text});if(!r.ok)alert(await r.text());refreshEvents()}"
"async function loadConfig(){config.value=await (await fetch('/api/config')).text()}"
"async function saveConfig(){alert(await (await fetch('/api/config',{method:'PUT',body:config.value})).text())}"
"async function listFiles(){files.textContent=JSON.stringify(await j('/api/files?path='+encodeURIComponent(path.value)),null,2)}"
"async function downloadFile(){fileText.value=await (await fetch('/api/file?path='+encodeURIComponent(filePath.value))).text()}"
"async function saveFile(){alert(await (await fetch('/api/file?path='+encodeURIComponent(filePath.value),{method:'PUT',body:fileText.value})).text());listFiles()}"
"async function deleteFile(){alert(await (await fetch('/api/file?path='+encodeURIComponent(filePath.value),{method:'DELETE'})).text());listFiles()}"
"async function mkdirPath(){alert(await (await fetch('/api/mkdir?path='+encodeURIComponent(path.value),{method:'POST'})).text());listFiles()}"
"let otaTimer=0;function fmtBytes(n){n=Number(n)||0;if(n<1024)return n+' B';if(n<1048576)return Math.round(n/1024)+' KB';return (n/1048576).toFixed(1)+' MB'}"
"function renderOtaPartitions(parts){let old=otaBootPartition.value;if(!Array.isArray(parts))return;otaBootPartition.innerHTML=parts.map(p=>'<option value=\"'+p.label+'\">'+p.label+(p.running?' running':'')+(p.boot?' boot':'')+'</option>').join('');if(parts.some(p=>p.label===old))otaBootPartition.value=old;else{let b=parts.find(p=>p.boot)||parts.find(p=>p.running)||parts[0];if(b)otaBootPartition.value=b.label}}"
"function renderOtaStatus(s){if(typeof s==='string'){ota.textContent=s;return}renderOtaPartitions(s.partitions);let p=s.progress||0;otaProgress.value=p;let m=s.maintenance?' maintenance '+(s.maintenance_reason||'ota'):'';let b=s.boot?' boot '+s.boot:'';otaProgressText.textContent=s.state+' '+p+'% device '+fmtBytes(s.bytes)+'/'+fmtBytes(s.expected)+m+b;ota.textContent=JSON.stringify(s,null,2)}"
"async function refreshOta(){renderOtaStatus(await j('/api/ota/status'))}"
"function otaPoll(on){if(on&&!otaTimer)otaTimer=setInterval(refreshOta,1000);if(!on&&otaTimer){clearInterval(otaTimer);otaTimer=0}}"
"function badOtaName(n){return ['bootloader.bin','partition-table.bin','ota_data_initial.bin'].includes(n)||n.includes('CMakeDetermineCompilerABI')}"
"async function uploadOta(){let f=otaFile.files[0];if(!f)return alert('choose bin');if(badOtaName(f.name))return alert('OTA requires the app image: use build/CClaw.bin');if(!crypto.subtle)return alert('browser SHA-256 is unavailable');otaPoll(false);otaProgress.value=0;otaProgressText.textContent='hashing '+f.name;let d=await crypto.subtle.digest('SHA-256',await f.arrayBuffer());let h=Array.from(new Uint8Array(d),b=>b.toString(16).padStart(2,'0')).join('');let t=encodeURIComponent(new Date().toISOString());otaProgressText.textContent='preparing OTA '+f.name;let pr=await fetch('/api/ota/prepare?name='+encodeURIComponent(f.name)+'&size='+f.size+'&sha256='+h+'&time='+t,{method:'POST'});let pt=await pr.text();if(!pr.ok){ota.textContent=pt;otaProgressText.textContent='prepare failed HTTP '+pr.status;refreshOta();return}ota.textContent=pt;otaProgressText.textContent='uploading '+f.name;let x=new XMLHttpRequest();x.open('POST','/api/ota/upload?name='+encodeURIComponent(f.name)+'&sha256='+h+'&time='+t);x.upload.onprogress=e=>{if(e.lengthComputable){let p=Math.round(e.loaded*100/e.total);otaProgress.value=p;otaProgressText.textContent='browser sent '+p+'% '+fmtBytes(e.loaded)+'/'+fmtBytes(e.total)}};x.onload=()=>{otaProgressText.textContent=(x.status>=200&&x.status<300?'upload complete':'upload failed')+' HTTP '+x.status;ota.textContent=x.responseText;refreshOta()};x.onerror=async()=>{otaProgressText.textContent='upload network error';await fetch('/api/ota/cancel',{method:'POST'});refreshOta()};x.send(f)}"
"async function selectOtaBoot(){let label=otaBootPartition.value;if(!label)return alert('choose partition');let r=await fetch('/api/ota/select?label='+encodeURIComponent(label),{method:'POST'});let t=await r.text();let o={};try{o=JSON.parse(t)}catch(e){}if(!r.ok){alert(t);return false}otaProgressText.textContent=o.reboot_required===false?'boot set to '+(o.boot||label)+'; already running':'boot set to '+(o.boot||label)+'; reboot board to start it';refreshOta();return true}"
"async function bootOta(){otaProgressText.textContent='rebooting board; connection will drop';try{let r=await fetch('/api/ota/reboot',{method:'POST'});let t=await r.text();ota.textContent=t;if(!r.ok)otaProgressText.textContent='reboot failed HTTP '+r.status}catch(e){ota.textContent='device is rebooting'}}"
"async function cancelOta(){alert(await (await fetch('/api/ota/cancel',{method:'POST'})).text());refreshOta()}"
"refreshAll();loadConfig();setInterval(refreshEvents,2000);</script></body></html>";

/*
 * 处理根路径 GET / 请求。返回内嵌的 SPA HTML 页面（INDEX_HTML），
 * Content-Type 为 "text/html; charset=utf-8"。页面包含状态、事件、消息输入、
 * 配置、文件管理和 OTA 功能面板。
 * 参数: req - HTTP 请求句柄
 * 返回: ESP_OK 表示页面发送成功
 */
esp_err_t web_console_index_handler(httpd_req_t *req)
{
    return web_console_send_text(req, INDEX_HTML, "text/html; charset=utf-8");
}

static void register_uri(const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *))
{
    httpd_uri_t cfg = {.uri = uri, .method = method, .handler = handler};
    esp_err_t err = httpd_register_uri_handler(g_web_console_server, &cfg);
    if (err != ESP_OK) ESP_LOGW(TAG, "register %s failed: %s", uri, esp_err_to_name(err));
}

/*
 * OTA 子系统内存分配适配器。将 ota_service 的分配请求转发到 web_console_internal_alloc，
 * 使用内部 DRAM 分配。
 * 参数: size - 需要的字节数
 * 返回: 内存指针或 NULL
 */
static char *web_ota_alloc(size_t size)
{
    return web_console_internal_alloc(size);
}

/*
 * OTA 子系统文本响应适配器。将 void* request 转为 httpd_req_t*，
 * 调用 web_console_send_text 发送响应。
 * 参数: request - ota_service 传递的 HTTP 请求（实际为 httpd_req_t*）
 *       text    - 响应文本
 *       type    - Content-Type
 * 返回: ESP_OK 或错误码
 */
static esp_err_t web_ota_send_text(void *request, const char *text, const char *type)
{
    return web_console_send_text((httpd_req_t *)request, text, type);
}

/*
 * OTA 子系统错误响应适配器。将 void* request 和 int code 转为 httpd_req_t*
 * 和 httpd_err_code_t，调用 web_console_send_error。
 * 参数: request - ota_service 传递的 HTTP 请求（实际为 httpd_req_t*）
 *       code    - HTTP 错误码（int 形式）
 *       message - 错误描述
 * 返回: ESP_OK 或错误码
 */
static esp_err_t web_ota_send_error(void *request, int code, const char *message)
{
    return web_console_send_error((httpd_req_t *)request, (httpd_err_code_t)code, message);
}

/*
 * OTA 子系统查询参数适配器。将 void* request 转为 httpd_req_t*，
 * 调用 web_console_query_value 提取 URL 查询参数。
 * 参数: request - ota_service 传递的 HTTP 请求（实际为 httpd_req_t*）
 *       key     - 要查询的参数名
 * 返回: PSRAM 中分配的值字符串副本，调用方负责释放
 */
static char *web_ota_query_value(void *request, const char *key)
{
    return web_console_query_value((httpd_req_t *)request, key);
}

/*
 * OTA 子系统数据接收适配器。将 void* request 转为 httpd_req_t*，
 * 调用 httpd_req_recv 接收请求体数据。
 * 参数: request - ota_service 传递的 HTTP 请求（实际为 httpd_req_t*）
 *       buffer  - 接收缓冲区
 *       length  - 期望接收的最大字节数
 * 返回: 实际接收的字节数，负值表示错误
 */
static int web_ota_recv(void *request, char *buffer, int length)
{
    return httpd_req_recv((httpd_req_t *)request, buffer, length);
}

/*
 * OTA 子系统停止 Web 服务器适配器。当 OTA 流程需要重启时，ota_service
 * 通过此回调停止当前 HTTP 服务器以便后续操作。
 * 参数: user_data - 未使用
 * 返回: web_console_stop 的返回值
 */
static esp_err_t web_ota_stop(void *user_data)
{
    (void)user_data;
    return web_console_stop();
}

static esp_err_t web_ota_start(void *user_data)
{
    (void)user_data;
    return web_console_restart();
}

/*
 * 启动 Web 控制台 HTTP 服务器。若服务器已在运行则直接返回 ESP_OK。
 * 若 io->web.enabled 为 false 则跳过启动。初始化 OTA 服务传输适配器，
 * 然后启动 ESP HTTP Server（默认端口 80 或 io->web.port），
 * 注册全部 API 路由（/、/api/status、/api/events、/api/messages、
 * /api/config、/api/files、/api/file、/api/mkdir、/api/ota/...）。
 * 参数: context - Web 控制台上下文，包含 IO 配置和业务回调函数指针
 * 返回: ESP_OK 表示启动成功或已运行；否则返回 ESP 错误码
 */
esp_err_t web_console_start(const web_console_context_t *context)
{
    if (g_web_console_server) return ESP_OK;
    web_console_context_t next_context;
    memset(&next_context, 0, sizeof(next_context));
    if (context) next_context = *context;
    memset(&g_web_console_context, 0, sizeof(g_web_console_context));
    g_web_console_context = next_context;
    const app_runtime_io_config_t *io = g_web_console_context.io;
    if (io && !io->web.enabled) {
        ESP_LOGI(TAG, "web console disabled");
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
    esp_err_t ota_init_err = ota_service_init(&ota_transport);
    if (ota_init_err != ESP_OK) {
        ESP_LOGE(TAG, "OTA service init failed: %s", esp_err_to_name(ota_init_err));
        return ota_init_err;
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
    register_uri("/", HTTP_GET, web_console_index_handler);
    register_uri("/api/status", HTTP_GET, web_console_status_handler);
    register_uri("/api/events", HTTP_GET, web_console_events_handler);
    register_uri("/api/messages", HTTP_POST, web_console_input_handler);
    register_uri("/api/config", HTTP_GET, web_console_config_get_handler);
    register_uri("/api/config", HTTP_PUT, web_console_config_put_handler);
    register_uri("/api/files", HTTP_GET, web_console_files_handler);
    register_uri("/api/file", HTTP_GET, web_console_file_handler);
    register_uri("/api/file", HTTP_PUT, web_console_file_handler);
    register_uri("/api/file", HTTP_DELETE, web_console_file_handler);
    register_uri("/api/mkdir", HTTP_POST, web_console_mkdir_handler);
    register_uri("/api/ota/status", HTTP_GET, web_console_ota_status_handler);
    register_uri("/api/ota/prepare", HTTP_POST, web_console_ota_prepare_handler);
    register_uri("/api/ota/upload", HTTP_POST, web_console_ota_upload_handler);
    register_uri("/api/ota/select", HTTP_POST, web_console_ota_select_handler);
    register_uri("/api/ota/reboot", HTTP_POST, web_console_ota_reboot_handler);
    register_uri("/api/ota/cancel", HTTP_POST, web_console_ota_cancel_handler);
    ESP_LOGI(TAG, "web console started on port %u", (unsigned)httpd_config.server_port);
    return ESP_OK;
}

/*
 * 停止 Web 控制台 HTTP 服务器。若服务器未运行则直接返回 ESP_OK。
 * 将全局句柄置 NULL并调用 httpd_stop；保存上下文供 OTA 失败恢复。
 * 返回: ESP_OK 表示停止成功；若已停止也返回 ESP_OK
 */
esp_err_t web_console_stop(void)
{
    if (!g_web_console_server) {
        return ESP_OK;
    }
    httpd_handle_t server = g_web_console_server;
    g_web_console_server = NULL;
    return httpd_stop(server);
}

esp_err_t web_console_restart(void)
{
    if (g_web_console_server) return ESP_OK;
    if (!g_web_console_context.io) return ESP_ERR_INVALID_STATE;
    web_console_context_t context = g_web_console_context;
    return web_console_start(&context);
}

esp_err_t web_console_destroy(void)
{
    esp_err_t err = web_console_stop();
    if (err == ESP_OK) memset(&g_web_console_context, 0, sizeof(g_web_console_context));
    return err;
}
