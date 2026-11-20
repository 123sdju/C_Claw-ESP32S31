"""编译实际 Web 入口，验证正文边界、目录构造失败与长度转交，端口使用同步替身。"""
from pathlib import Path
import unittest

from test_event_ring_ownership import run_c_test


ROOT = Path(__file__).resolve().parents[1]


def function(path, signature):
    """读取一个实际 C 定义，包含函数体，供独立 fixture 编译。"""
    text = (ROOT / path).read_text(encoding="utf-8")
    start = text.index(signature)
    return text[start:text.index("\n}\n", start) + 3]


BASE = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <limits.h>
#include <errno.h>
#include <stdarg.h>
typedef int esp_err_t;
typedef int httpd_err_code_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_SIZE 2
#define ESP_ERR_NO_MEM 3
#define ESP_ERR_TIMEOUT 4
#define ESP_ERR_INVALID_STATE 5
#define ESP_ERR_NOT_FOUND 6
#define HTTPD_400_BAD_REQUEST 400
#define HTTPD_403_FORBIDDEN 403
#define HTTPD_404_NOT_FOUND 404
#define HTTPD_405_METHOD_NOT_ALLOWED 405
#define HTTPD_408_REQ_TIMEOUT 408
#define HTTPD_500_INTERNAL_SERVER_ERROR 500
#define HTTPD_SOCK_ERR_TIMEOUT -2
#define HTTP_GET 1
#define HTTP_PUT 2
#define HTTP_DELETE 3
#define WEB_ROOT "/usb0"
#define WEB_MAX_PATH 256
#define WEB_IO_CHUNK 4096
#define WEB_MAX_UPLOAD_BYTES (8U * 1024U * 1024U)
#define WEB_REQUEST_TIMEOUT_US (60LL * 1000LL * 1000LL)
#define CC_OK 0
#define CC_ERR_OUT_OF_MEMORY 1
#define CC_ERR_JSON 2
typedef struct { int code; char *message; } cc_result_t;
typedef struct {
    size_t content_len, offset;
    const char *body, *query;
    int method;
} httpd_req_t;
static int response_status, recv_mode, allocation_failure;
static int psram_allocations,fail_psram_at;
static int64_t now_us;
static char response_text[8192];
/** @brief 捕获错误响应。 @param[in,out] req 请求借用。 @param[in] code HTTP状态。 @param[in] message 描述借用。 @return ESP_OK表示已记录。 */
static esp_err_t web_console_send_error(httpd_req_t *req, int code, const char *message)
{ (void)req; response_status=code; snprintf(response_text,sizeof(response_text),"%s",message); return ESP_OK; }
/** @brief 捕获成功正文。 @param[in,out] req 请求借用。 @param[in] text 正文借用。 @param[in] type 类型借用。 @return ESP_OK表示已记录。 */
static esp_err_t web_console_send_text(httpd_req_t *req,const char *text,const char *type)
{ (void)req; (void)type; snprintf(response_text,sizeof(response_text),"%s",text?text:""); return ESP_OK; }
/** @brief 记录显式响应状态。 @param[in,out] req 请求借用。 @param[in] text 状态借用。 @return ESP_OK。 */
static int httpd_resp_set_status(httpd_req_t *req,const char *text)
{ (void)req; response_status=atoi(text); return ESP_OK; }
/** @brief 提供可故障注入的 PSRAM 替身。 @param[in] size 字节数。 @return 独占内存或NULL。 */
static char *web_console_psram_alloc(size_t size)
{ psram_allocations++; return allocation_failure || psram_allocations==fail_psram_at ? NULL : malloc(size); }
/** @brief 提供内部RAM替身。 @param[in] size 字节数。 @return 独占内存或NULL。 */
static char *web_console_internal_alloc(size_t size)
{ return allocation_failure ? NULL : malloc(size); }
/** @brief 归还替身分配。 @param[in,out] ptr 可空拥有内存。 @post 内存已释放。 */
static void heap_caps_free(void *ptr) { free(ptr); }
/** @brief 读取受控微秒时钟。 @return 当前测试读数。 */
static int64_t esp_timer_get_time(void) { return now_us; }
/** @brief 按请求片段接收或返回可控异常。 @param[in,out] req 请求及偏移。 @param[out] out 至少size字节缓冲。 @param[in] size 本次容量。 @return 实际字节、超时或过量返回的异常替身。 */
static int httpd_req_recv(httpd_req_t *req,char *out,size_t size)
{
    if(recv_mode==1) return (int)size+1;
    if(recv_mode==2) return 0;
    if(recv_mode==3) { now_us=WEB_REQUEST_TIMEOUT_US; return HTTPD_SOCK_ERR_TIMEOUT; }
    if(recv_mode==4) { recv_mode=0; return HTTPD_SOCK_ERR_TIMEOUT; }
    size_t count=req->content_len-req->offset;
    if(count>size) count=size;
    if(count>7) count=7;
    memcpy(out,req->body+req->offset,count); req->offset+=count;
    return (int)count;
}
'''


class WebTransportContracts(unittest.TestCase):
    def test_config_body_boundaries(self):
        """验证实际配置 handler 在回调前拒绝截断表示、非完整对象及异常接收。"""
        scanner = (ROOT / "third_party/cclaw-sdk/cclaw/core/include/cc/internal/cc_json_syntax_check.h").read_text(encoding="utf-8")
        prefix = BASE + scanner + r'''
typedef struct { int object; } cc_json_value_t;
static cc_json_value_t parsed_root;
static int parse_calls, apply_calls;
typedef struct {
    bool parsed,event_bus_applied,interaction_applied,network_applied,profile_applied;
    bool font_applied,runtime_controls_applied,durable_committed;
    bool rollback_attempted,rollback_succeeded,restart_required;
    esp_err_t error;
    char failed_subsystem[24];
} app_config_apply_result_t;
/** @brief 按共享无分配语法识别器模拟严格解析，确认入口使用strict符号。 @param[in] text 正文借用。 @param[out] out 根借用槽。 @return 模拟解析结果，无拥有诊断。 */
static cc_result_t cc_json_parse_strict(const char *text,cc_json_value_t **out)
{
    parse_calls++; *out=NULL;
    if(cc_json_syntax_check(text)!=CC_JSON_SYNTAX_VALID) return (cc_result_t){CC_ERR_JSON,NULL};
    parsed_root.object=*text=='{'; *out=&parsed_root; return (cc_result_t){CC_OK,NULL};
}
/** @brief 查询根类型。 @param[in] root 根借用。 @return 对象为真。 */
static int cc_json_is_object(const cc_json_value_t *root) { return root->object; }
/** @brief 结束模拟根借用。 @param[in,out] root 模拟根或NULL。 @post 无动态资源。 */
static void cc_json_destroy(cc_json_value_t *root) { (void)root; }
/** @brief 清理模拟结果。 @param[in,out] result 结果槽。 @post 结果清零。 */
static void cc_result_free(cc_result_t *result) { memset(result,0,sizeof(*result)); }
/** @brief 提供可定位测试错误。 @param[in] code 模拟状态。 @return 静态描述。 */
static const char *cc_error_string(int code) { (void)code; return "invalid JSON"; }
/** @brief 接纳有效配置并填写报告。 @param[in] text 调用期正文借用。 @param[out] report 空报告槽。 @param[in,out] user 忽略上下文。 @return ESP_OK。 */
static esp_err_t apply(const char *text,app_config_apply_result_t *report,void *user)
{ (void)user; assert(text); apply_calls++; report->parsed=true; report->durable_committed=true; return ESP_OK; }
static struct {
    esp_err_t (*config_applied)(const char *,app_config_apply_result_t *,void *);
    void *user_data;
} g_web_console_context={apply,NULL};
'''
        handler = function("main/transports/web_console_config.c", "esp_err_t web_console_config_put_handler(")
        tests = r'''
/** @brief 执行一个独立请求。 @param[in] text 有len字节的借用正文。 @param[in] len 显式长度。 @param[in] status 预期状态。 @param[in] accepted 是否可交事务。 @post 请求结束且计数匹配。 */
static void run_request(const char *text,size_t len,int status,int accepted)
{
    httpd_req_t req={.content_len=len,.body=text};
    int before=apply_calls; response_status=200; response_text[0]=0; now_us=0;
    assert(web_console_config_put_handler(&req)==ESP_OK);
    assert(response_status==status && apply_calls==before+accepted);
}
/** @brief 运行完整正文、NUL、尾部、大小和接收异常。 @return 断言通过为零。 */
int main(void)
{
    run_request("{}",2,200,1);
    const char literal[]="{\"value\":\"literal\\\\u0000\"}";
    run_request(literal,sizeof(literal)-1,200,1);
    const char decoded[]="{\"value\":\"\\u0000\"}";
    int before=parse_calls; run_request(decoded,sizeof(decoded)-1,400,0); assert(parse_calls==before);
    const char raw[]={'{','}',0,'x'};
    before=parse_calls; run_request(raw,sizeof(raw),400,0); assert(parse_calls==before);
    run_request("{} tail",7,400,0);
    run_request("[]",2,400,0); assert(strstr(response_text,"object"));
    const char unicode[]="{\"value\":\"\\u00G0\"}";
    run_request(unicode,sizeof(unicode)-1,400,0);
    run_request("",0,400,0); run_request("{}",65537,400,0);
    char *limit=malloc(65536); assert(limit); memset(limit,' ',65536); memcpy(limit,"{}",2);
    run_request(limit,65536,200,1); free(limit);
    recv_mode=1; run_request("{}",2,500,0);
    recv_mode=2; run_request("{}",2,500,0);
    recv_mode=3; run_request("{}",2,408,0);
    recv_mode=4; run_request("{}",2,200,1);
    allocation_failure=1; run_request("{}",2,500,0);
    return 0;
}
'''
        run_c_test(prefix + handler + tests)

    def test_directory_output_and_mkdir(self):
        """验证目录构造的每次追加失败返回500并消费诊断，mkdir不把同名文件当目录。"""
        prefix = BASE + r'''
typedef struct { char *data; size_t length; int error; } cc_string_builder_t;
static int operation,fail_operation,diagnostics_live,directories,entry_index,existing_mode,query_status;
typedef struct { int unused; } DIR;
struct dirent { char d_name[80]; };
struct stat { int st_mode; unsigned st_size; };
#define S_ISDIR(mode) ((mode)==2)
static DIR directory;
static struct dirent entry;
/** @brief 为每个builder操作返回拥有诊断，成功路径也需消费。 @param[in] code 状态。 @return 独占诊断结果。 */
static cc_result_t result(int code)
{ char *text=malloc(2); assert(text); strcpy(text,"x"); diagnostics_live++; return (cc_result_t){code,text}; }
/** @brief 消费模拟诊断。 @param[in,out] rc 独占结果槽。 @post 诊断归还且清零。 */
static void cc_result_free(cc_result_t *rc)
{ if(rc->message) { diagnostics_live--; free(rc->message); } memset(rc,0,sizeof(*rc)); }
/** @brief 创建构造器并注入首分配失败。 @param[out] sb 新槽。 @return 拥有结果。 */
static cc_result_t cc_string_builder_init(cc_string_builder_t *sb)
{ memset(sb,0,sizeof(*sb)); operation++; if(operation==fail_operation) {sb->error=1;return result(1);} sb->data=malloc(8192); assert(sb->data); sb->data[0]=0; return result(0); }
/** @brief 注入追加故障并维护sticky状态。 @param[in,out] sb 构造器。 @param[in] text 文本借用。 @return 拥有结果。 */
static cc_result_t cc_string_builder_append(cc_string_builder_t *sb,const char *text)
{ operation++; if(operation==fail_operation) sb->error=1; if(sb->error) return result(1); size_t len=strlen(text); assert(sb->length+len<8192); memcpy(sb->data+sb->length,text,len+1);sb->length+=len;return result(0); }
/** @brief 追加单字节。 @param[in,out] sb 构造器。 @param[in] ch 字节。 @return 拥有结果。 */
static cc_result_t cc_string_builder_append_char(cc_string_builder_t *sb,char ch)
{ char text[2]={ch,0};return cc_string_builder_append(sb,text); }
/** @brief 追加格式化文本。 @param[in,out] sb 构造器。 @param[in] format 格式。 @param[in] ... 匹配参数。 @return 拥有结果。 */
static cc_result_t cc_string_builder_appendf(cc_string_builder_t *sb,const char *format,...)
{ char text[256];va_list args;va_start(args,format);vsnprintf(text,sizeof(text),format,args);va_end(args);return cc_string_builder_append(sb,text); }
/** @brief 读取sticky状态。 @param[in] sb 构造器。 @return 当前状态。 */
static int cc_string_builder_error(const cc_string_builder_t *sb) { return sb->error; }
/** @brief 移交完整正文或撤销失败缓冲。 @param[in,out] sb 构造器。 @return 拥有正文或NULL。 */
static char *cc_string_builder_take(cc_string_builder_t *sb)
{ if(sb->error) { free(sb->data);sb->data=NULL;return NULL; } char *text=sb->data;sb->data=NULL;return text; }
/** @brief 返回拥有路径或明确查询状态。 @param[in] req 请求借用。 @param[in] key 键名借用。 @param[out] out 独立路径或NULL。 @return 模拟查询状态。 */
static int web_console_query_value_checked(httpd_req_t *req,const char *key,char **out)
{ (void)req;(void)key;*out=NULL;if(query_status)return query_status;char *path=malloc(6);assert(path);strcpy(path,"/usb0");*out=path;return ESP_OK; }
/** @brief 此fixture只验证已获许可路径。 @param[in] path 借用路径。 @return 真。 */
static bool web_console_path_is_safe(const char *path) { return path!=NULL; }
/** @brief 此fixture保持owner许可。 @param[in] path 路径借用。 @param[in] write 修改标记。 @return 真。 */
static bool web_console_file_access_allowed(const char *path,bool write) { (void)path;(void)write;return true; }
/** @brief 打开合成目录。 @param[in] path 路径借用。 @return 目录借用。 */
static DIR *opendir(const char *path) { (void)path;directories++;entry_index=0;return &directory; }
/** @brief 顺序借出含转义字符和普通名称。 @param[in,out] dir 目录借用。 @return 当前条目或结束NULL。 */
static struct dirent *readdir(DIR *dir)
{ (void)dir;if(entry_index==2)return NULL;strcpy(entry.d_name,entry_index++==0?"quote\"line\n":"file");return &entry; }
/** @brief 关闭合成目录。 @param[in,out] dir 借用目录。 @return 零。 */
static int closedir(DIR *dir) { (void)dir;directories--;return 0; }
/** @brief 查询合成条目类型。 @param[in] path 路径借用。 @param[out] st 元数据。 @return 零。 */
static int fake_stat(const char *path,struct stat *st) { (void)path;st->st_mode=existing_mode;st->st_size=17;return 0; }
#define stat(path,out) fake_stat(path,out)
/** @brief 模拟目标已存在。 @param[in] path 路径借用。 @param[in] mode 权限。 @return 负一并设置EEXIST。 */
static int mkdir(const char *path,int mode) { (void)path;(void)mode;errno=EEXIST;return -1; }
/** @brief 监测错误查询不应进入上传。 @param[in,out] req 请求。 @param[in] path 路径。 @return 未到达时无返回。 */
static int web_console_upload_file(httpd_req_t *req,const char *path) { (void)req;(void)path;assert(0);return ESP_FAIL; }
/** @brief 监测错误查询不应设置下载类型。 @param[in,out] req 请求。 @param[in] type 类型。 @return 未到达时无返回。 */
static int httpd_resp_set_type(httpd_req_t *req,const char *type) { (void)req;(void)type;assert(0);return ESP_FAIL; }
/** @brief 监测错误查询不应发送文件块。 @param[in,out] req 请求。 @param[in] data 内容。 @param[in] len 字节数。 @return 未到达时无返回。 */
static int httpd_resp_send_chunk(httpd_req_t *req,const char *data,size_t len) { (void)req;(void)data;(void)len;assert(0);return ESP_FAIL; }
'''
        common = "main/transports/web_console_common.c"
        files = "main/transports/web_console_files.c"
        code = function(common, "bool web_console_join_path(")
        code += function(common, "void web_console_json_escape_append(")
        code += function(files, "esp_err_t web_console_files_handler(")
        code += function(files, "esp_err_t web_console_file_handler(")
        code += function(files, "esp_err_t web_console_mkdir_handler(")
        tests = r'''
/** @brief 覆盖每个实际构造步骤和同名对象类型。 @return 断言通过为零。 */
int main(void)
{
    httpd_req_t req={0}; response_status=200;
    assert(web_console_files_handler(&req)==ESP_OK);
    assert(response_status==200 && strstr(response_text,"quote\\\"line\\n") && !diagnostics_live && !directories);
    int attempts=operation;
    for(int failed=1;failed<=attempts;failed++) {
        operation=0;fail_operation=failed;response_status=200;response_text[0]=0;
        assert(web_console_files_handler(&req)==ESP_OK);
        assert(response_status==500 && response_text[0] && !diagnostics_live && !directories);
    }
    fail_operation=0; existing_mode=1;response_status=200;
    assert(web_console_mkdir_handler(&req)==ESP_OK && response_status==500);
    existing_mode=2;response_status=200;
    assert(web_console_mkdir_handler(&req)==ESP_OK && response_status==200);
    query_status=ESP_ERR_NOT_FOUND;response_status=200;
    assert(web_console_files_handler(&req)==ESP_OK&&response_status==200&&!diagnostics_live);
    assert(web_console_file_handler(&req)==ESP_OK&&response_status==400);
    query_status=ESP_ERR_INVALID_ARG;response_status=200;
    assert(web_console_files_handler(&req)==ESP_OK&&response_status==400&&!directories);
    assert(web_console_mkdir_handler(&req)==ESP_OK&&response_status==400);
    assert(web_console_file_handler(&req)==ESP_OK&&response_status==400);
    query_status=ESP_ERR_NO_MEM;response_status=200;
    assert(web_console_files_handler(&req)==ESP_OK&&response_status==500&&!directories);
    assert(web_console_mkdir_handler(&req)==ESP_OK&&response_status==500);
    assert(web_console_file_handler(&req)==ESP_OK&&response_status==500);
    return 0;
}
'''
        run_c_test(prefix + code + tests)

    def test_query_join_and_receive_bounds(self):
        """核对query解码、路径完整容量、文件接收块长与总期限。"""
        prefix = BASE + r'''
static int oversized_query;
/** @brief 返回查询原文字节数或容量极值。 @param[in] req 请求借用。 @return 查询长度。 */
static size_t httpd_req_get_url_query_len(httpd_req_t *req) { return oversized_query?SIZE_MAX:strlen(req->query); }
/** @brief 复制查询原文。 @param[in] req 请求借用。 @param[out] out 字符缓冲。 @param[in] size 含NUL容量。 @return 成功或失败。 */
static int httpd_req_get_url_query_str(httpd_req_t *req,char *out,size_t size)
{ if(strlen(req->query)>=size)return ESP_FAIL;strcpy(out,req->query);return ESP_OK; }
/** @brief 只模拟path键的原始取值，实际URL解码留给生产helper。 @param[in] query 原文。 @param[in] key 键名。 @param[out] out 缓冲。 @param[in] size 容量。 @return 成功或失败。 */
static int httpd_query_key_value(const char *query,const char *key,char *out,size_t size)
{ (void)key;if(strncmp(query,"path=",5))return ESP_ERR_NOT_FOUND;if(strlen(query+5)>=size)return ESP_FAIL;strcpy(out,query+5);return ESP_OK; }
'''
        path = "main/transports/web_console_common.c"
        code = "".join(function(path, signature) for signature in (
            "static int url_hex_value(", "static bool url_decode_in_place(",
            "esp_err_t web_console_query_value_checked(",
            "char *web_console_query_value(", "bool web_console_join_path(",
            "esp_err_t web_console_read_request_to_file("))
        tests = r'''
/** @brief 验证边界和失败后无额外文件写入。 @return 断言通过为零。 */
int main(void)
{
    httpd_req_t req={.query="path=%2Fusb0%2Fa+b"};
    char *value=web_console_query_value(&req,"path");assert(value&&!strcmp(value,"/usb0/a b"));heap_caps_free(value);
    req.query="path=%00";assert(!web_console_query_value(&req,"path"));
    req.query="path=%G0";assert(!web_console_query_value(&req,"path"));
    req.query="path=%2";assert(!web_console_query_value(&req,"path"));
    char *checked=(void *)1;
    assert(web_console_query_value_checked(&req,"path",&checked)==ESP_ERR_INVALID_ARG&&!checked);
    req.query="other=missing";assert(web_console_query_value_checked(&req,"path",&checked)==ESP_ERR_NOT_FOUND&&!checked);
    req.query="";assert(web_console_query_value_checked(&req,"path",&checked)==ESP_ERR_NOT_FOUND&&!checked);
    req.query="path=ok";fail_psram_at=psram_allocations+2;
    assert(web_console_query_value_checked(&req,"path",&checked)==ESP_ERR_NO_MEM&&!checked);fail_psram_at=0;
    char long_query[WEB_MAX_PATH+6];memcpy(long_query,"path=",5);memset(long_query+5,'a',WEB_MAX_PATH);long_query[sizeof(long_query)-1]=0;
    req.query=long_query;assert(web_console_query_value_checked(&req,"path",&checked)==ESP_ERR_INVALID_ARG&&!checked);
    oversized_query=1;assert(!web_console_query_value(&req,"path"));oversized_query=0;
    assert(!web_console_query_value(NULL,"path")&&!web_console_query_value(&req,NULL));
    char joined[5]="old";assert(web_console_join_path(joined,sizeof(joined),"a","bc")&&!strcmp(joined,"a/bc"));
    strcpy(joined,"old");assert(!web_console_join_path(joined,4,"a","bc")&&!strcmp(joined,"old"));
    assert(web_console_join_path(joined,1,"","")&&!joined[0]);
    char body[8193];memset(body,'v',sizeof(body)); req=(httpd_req_t){.body=body,.content_len=sizeof(body)};
    FILE *file=tmpfile();assert(file);assert(web_console_read_request_to_file(&req,file)==ESP_OK&&ftell(file)==sizeof(body));fclose(file);
    req.offset=0;recv_mode=1;file=tmpfile();assert(file);
    assert(web_console_read_request_to_file(&req,file)==ESP_FAIL&&ftell(file)==0);fclose(file);
    req.offset=0;recv_mode=3;now_us=0;file=tmpfile();assert(file);
    assert(web_console_read_request_to_file(&req,file)==ESP_ERR_TIMEOUT&&ftell(file)==0);fclose(file);
    req.offset=0;recv_mode=4;now_us=0;file=tmpfile();assert(file);
    assert(web_console_read_request_to_file(&req,file)==ESP_OK);fclose(file);
    req.offset=0;allocation_failure=1;file=tmpfile();assert(file);
    assert(web_console_read_request_to_file(&req,file)==ESP_ERR_NO_MEM&&ftell(file)==0);fclose(file);
    return 0;
}
'''
        run_c_test(prefix + code + tests)

    def test_ota_length_handoff(self):
        """检查HTTP长度在缩窄到OTA服务int参数前被拒绝。"""
        prefix = BASE + r'''
static int uploads,last_length;
/** @brief 捕获服务收到的长度。 @param[in,out] request 请求借用。 @param[in] length 原生int字节数。 @return ESP_OK。 */
static int ota_service_upload(void *request,int length) { (void)request;uploads++;last_length=length;return ESP_OK; }
'''
        code = function("main/transports/web_console_ota.c", "esp_err_t web_console_ota_upload_handler(")
        tests = r'''
/** @brief 检查合法极值与越界值，NULL保留旧零长度转交。 @return 断言通过为零。 */
int main(void)
{
    httpd_req_t req={.content_len=INT_MAX};response_status=200;
    assert(web_console_ota_upload_handler(&req)==ESP_OK&&uploads==1&&last_length==INT_MAX);
    req.content_len=(size_t)INT_MAX+1;assert(web_console_ota_upload_handler(&req)==ESP_OK&&uploads==1&&response_status==400);
    req.content_len=SIZE_MAX;assert(web_console_ota_upload_handler(&req)==ESP_OK&&uploads==1&&response_status==400);
    req.content_len=0;assert(web_console_ota_upload_handler(&req)==ESP_OK&&uploads==2&&last_length==0);
    assert(web_console_ota_upload_handler(NULL)==ESP_OK&&uploads==3&&last_length==0);
    return 0;
}
'''
        run_c_test(prefix + code + tests)


if __name__ == "__main__":
    unittest.main()
