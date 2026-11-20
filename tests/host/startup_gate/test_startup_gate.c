#include "startup_gate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"check failed at %d\n",__LINE__); return 1; } } while (0)
static unsigned clears, forwards;
static app_event_source_t forwarded_source;
static char forwarded_text[64];
void app_event_bus_clear_conversation(void) { clears++; }
void app_event_publish(app_event_kind_t k, app_event_source_t s, cc_log_level_t l, const char *t)
{ (void)k; (void)s; (void)l; (void)t; }
esp_err_t interaction_service_submit_text(app_event_source_t source, const char *text)
{
    forwards++; forwarded_source=source; snprintf(forwarded_text,sizeof(forwarded_text),"%s",text); return ESP_OK;
}
int main(void)
{
    startup_gate_set_phase(STARTUP_GATE_LOADING);
    char *json=startup_gate_status_json(); CHECK(json && strstr(json,"\"phase\":\"loading\"")); free(json);
    CHECK(startup_gate_submit(APP_EVENT_SOURCE_WEB,"hello")==ESP_ERR_INVALID_STATE && forwards==0);
    CHECK(startup_gate_submit(APP_EVENT_SOURCE_SERIAL,"/clear")==ESP_OK && clears==1);
    startup_gate_set_phase(STARTUP_GATE_RUNNING);
    CHECK(startup_gate_submit(APP_EVENT_SOURCE_WEB,"hello")==ESP_OK && forwards==1 &&
          forwarded_source==APP_EVENT_SOURCE_WEB && strcmp(forwarded_text,"hello")==0);
    json=startup_gate_status_json(); CHECK(json && strstr(json,"\"phase\":\"running\"")); free(json);
    startup_gate_set_phase(STARTUP_GATE_USB_DISCONNECTED);
    CHECK(startup_gate_submit(APP_EVENT_SOURCE_WEB,"hello")==ESP_ERR_INVALID_STATE && forwards==1 &&
          startup_gate_submit(APP_EVENT_SOURCE_SERIAL,"/clear")==ESP_OK && clears==2);
    json=startup_gate_status_json(); CHECK(json && strstr(json,"\"phase\":\"usb_disconnected\"")); free(json);
    startup_gate_set_phase(STARTUP_GATE_USB_RELOADING);
    CHECK(startup_gate_submit(APP_EVENT_SOURCE_SERIAL,"hello")==ESP_ERR_INVALID_STATE && forwards==1);
    json=startup_gate_status_json(); CHECK(json && strstr(json,"\"phase\":\"usb_reloading\"")); free(json);
    return 0;
}
