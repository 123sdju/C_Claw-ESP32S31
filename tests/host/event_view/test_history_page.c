#include "history_view.h"
#include "cc/util/cc_redaction.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"check failed at %d\n",__LINE__); return 1; } } while (0)
static int ok(cc_result_t rc) { int good=rc.code==CC_OK; cc_result_free(&rc); return good; }

int main(void)
{
    char *long_text=malloc(36001); CHECK(long_text);
    for (size_t i=0;i<36000;i+=3) memcpy(long_text+i,"\xe6\xb5\x8b",3);
    long_text[36000]='\0';
    cc_message_t *created[3]={0}; cc_message_t messages[3]={0};
    CHECK(ok(cc_message_create_text("a","s",CC_ROLE_USER,"first",NULL,&created[0])));
    CHECK(ok(cc_message_create_text("b","s",CC_ROLE_ASSISTANT,long_text,NULL,&created[1])));
    CHECK(ok(cc_message_create_text("c","s",CC_ROLE_TOOL,"last",NULL,&created[2])));
    for (size_t i=0;i<3;i++) { CHECK(ok(cc_message_copy(created[i],&messages[i]))); cc_message_destroy(created[i]); }

    app_history_page_t page={0}; app_history_cursor_t cursor={1,1};
    CHECK(app_history_page_build(messages,3,cursor,2,4097,&page)==ESP_ERR_INVALID_ARG && page.count==0);
    cursor=(app_history_cursor_t){1,0};
    CHECK(app_history_page_build(messages,3,cursor,1,5,&page)==ESP_OK && page.count==1 &&
          strlen(page.items[0].text)==3 && page.has_more && page.next.message_index==1 && page.next.byte_offset==3);
    app_history_page_cleanup(&page);
    CHECK(app_history_page_build(messages,3,cursor,1,4,&page)==ESP_ERR_INVALID_ARG);

    cursor=(app_history_cursor_t){0,0};
    char *reconstructed=calloc(36001,1); CHECK(reconstructed);
    size_t copied=0, pages=0, first=0, last=0;
    do {
        CHECK(app_history_page_build(messages,3,cursor,2,4097,&page)==ESP_OK && page.count>0);
        for (size_t i=0;i<page.count;i++) {
            if (page.items[i].kind==APP_EVENT_KIND_USER) { CHECK(strcmp(page.items[i].text,"first")==0); first++; }
            else if (page.items[i].kind==APP_EVENT_KIND_TOOL) { CHECK(strcmp(page.items[i].text,"last")==0); last++; }
            else {
                CHECK(page.items[i].kind==APP_EVENT_KIND_ASSISTANT);
                size_t n=strlen(page.items[i].text); CHECK(n%3==0 && copied+n<=36000);
                memcpy(reconstructed+copied,page.items[i].text,n); copied+=n;
            }
        }
        int more=page.has_more; app_history_cursor_t next=page.next;
        CHECK(next.message_index>cursor.message_index || next.byte_offset>cursor.byte_offset);
        app_history_page_cleanup(&page); cursor=next; pages++;
        if (!more) break;
        CHECK(pages<32);
    } while (1);
    CHECK(pages>1 && first==1 && last==1 && copied==36000 && strcmp(reconstructed,long_text)==0 &&
          cursor.message_index==3 && cursor.byte_offset==0);
    CHECK(app_history_page_build(messages,3,cursor,2,4097,&page)==ESP_OK && !page.has_more && page.count==0);
    app_history_page_cleanup(&page);
    CHECK(app_history_tail_cursor(messages,3,2,4097,&cursor)==ESP_OK &&
          cursor.message_index==1 && cursor.byte_offset>0 && cursor.byte_offset%3==0);
    CHECK(app_history_page_build(messages,3,cursor,2,4097,&page)==ESP_OK && page.count==2 &&
          !page.has_more && strlen(page.items[0].text)%3==0 && strcmp(page.items[1].text,"last")==0);
    app_history_page_cleanup(&page);

    /* A credential crosses several small pages. Redact the complete message
     * before splitting it, or neither individual page can recognize it. */
    cc_message_t *secret=NULL;
    CHECK(ok(cc_message_create_text("secret","s",CC_ROLE_USER,"token=fallback-token",NULL,&secret)));
    char *expected=cc_redact_secrets("token=fallback-token"); CHECK(expected && !strstr(expected,"fallback-token"));
    char safe_text[128]={0}; size_t safe_len=0; cursor=(app_history_cursor_t){0,0};
    do {
        CHECK(app_history_page_build(secret,1,cursor,1,8,&page)==ESP_OK && page.count==1);
        size_t n=strlen(page.items[0].text); CHECK(safe_len+n<sizeof(safe_text));
        memcpy(safe_text+safe_len,page.items[0].text,n); safe_len+=n;
        int more=page.has_more; cursor=page.next; app_history_page_cleanup(&page);
        if (!more) break;
    } while (1);
    CHECK(strcmp(safe_text,expected)==0 && !strstr(safe_text,"fallback-token"));
    CHECK(app_history_tail_cursor(secret,1,1,8,&cursor)==ESP_OK && cursor.byte_offset>0);
    CHECK(app_history_page_build(secret,1,cursor,1,8,&page)==ESP_OK && !page.has_more &&
          !strstr(page.items[0].text,"fallback-token"));
    app_history_page_cleanup(&page);
    free(expected); cc_message_destroy(secret);
    for (size_t i=0;i<3;i++) cc_message_cleanup(&messages[i]);
    free(reconstructed); free(long_text); return 0;
}
