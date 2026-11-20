#pragma once
#include <stdarg.h>
enum { APP_EVENT_KIND_STATUS, APP_EVENT_KIND_ERROR, APP_EVENT_SOURCE_SERIAL, CC_LOG_INFO, CC_LOG_ERROR };
void app_event_publish(int kind,int source,int level,const char *message);
void app_event_publishf(int kind,int source,int level,const char *format,...);
