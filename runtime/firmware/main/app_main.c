/* FlightWall owned firmware, skeleton stage.
 *
 * Purpose: measure what an owned build actually costs in RAM, with the real
 * Wi-Fi stack, the source-built TLS engine and its ESP32 transport, before any
 * display or settings work is written. It has no panel output yet.
 *
 * Safety properties (these matter more than features at this stage):
 *  - Never erases NVS, and writes it in two places only: setup mode with
 *    CONFIG_FW_SETUP_SAVE saves Wi-Fi credentials the user submitted, after a
 *    successful test join, exactly as stock does (setup_server.c); and the
 *    dashboard's owned settings go to their own namespace "fw_owned", only
 *    on a change (owned_settings.c). Otherwise the driver runs with RAM-only
 *    storage. On an NVS error it stays offline.
 *  - Wi-Fi follows stock: it joins from stock's saved network list and
 *    starts setup mode only when that fails (wifi_link.c).
 *  - Never confirms itself. Installed as a pending OTA image, it restarts after
 *    its view window (or a deadline) so the installed bootloader rolls back to
 *    the previous known-good firmware. Only a keepable build
 *    (CONFIG_FW_KEEPABLE) is kept, and only when the owner confirms it
 *    through the keyed update service (owner_update.c).
 *  - Never prints credentials, the SSID or response bodies.
 */
#include <string.h>
#include "sdkconfig.h"
#include "wall_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "esp32_transport.h"
#include "tls_port.h"
#include "memory_probe.h"
#include "route_probe.h"
#include "owner_descriptor.h"
#include "display.h"
#include "setup_server.h"
#include "wifi_link.h"
#include "games_server.h"
#include "feed.h"
#include "aliases.h"
#include "settings_store.h"
#include "owner_update.h"
#include "community.h"
#include "big_buffer.h"
#include "stats.h"
#include "crash_guard.h"
#include "panic_note.h"
#include "board_check.h"
#include "sync_store.h"

/* The TLS engine's arena is the shared block (big_buffer.h), its context is
 * in community.cpp: the community source and, in probe builds, the route probe
 * use them, never at the same time. */

#define BIT_PROBE_DONE BIT1
static EventGroupHandle_t s_events;
static volatile bool s_probe_ok;

static bool running_is_pending(void){
    esp_ota_img_states_t state;
    const esp_partition_t *running=esp_ota_get_running_partition();
    return running&&esp_ota_get_state_partition(running,&state)==ESP_OK&&
           state==ESP_OTA_IMG_PENDING_VERIFY;
}

static void report_identity(void){
    const esp_partition_t *running=esp_ota_get_running_partition();
    esp_ota_img_states_t state=ESP_OTA_IMG_UNDEFINED;
    if(running)esp_ota_get_state_partition(running,&state);
    fw_state("{\"partition\":\"%s\",\"offset\":%u,\"ota_state\":%d,\"pending\":%s,\"reset_reason\":%d,"
           "\"descriptor_version\":%u,\"descriptor_keyed\":%s}",
           running?running->label:"?",running?(unsigned)running->address:0u,(int)state,
           state==ESP_OTA_IMG_PENDING_VERIFY?"true":"false",(int)esp_reset_reason(),
           (unsigned)fw_owner_descriptor_version(),fw_owner_descriptor_keyed()?"true":"false");
}

/* Backstop for a hang anywhere: an unconfirmed image must not linger. */
static void pending_deadline(void *arg){
    (void)arg;
    if(running_is_pending()){
        fw_state("{\"event\":\"pending_deadline_restart\"}");
        esp_restart();
    }
}

static bool sync_clock(void){
    /* esp_sntp_* exists in v4.4.7 and is the non-deprecated name in v5.x. */
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0,"pool.ntp.org");
    esp_sntp_init();
    for(int i=0;i<60&&!fw_esp_wall_clock_valid();++i)vTaskDelay(pdMS_TO_TICKS(500));
    return fw_esp_wall_clock_valid();
}

static void __attribute__((unused)) probe_task(void *arg){   /* unused in Wi-Fi test builds */
    (void)arg;
    fw_mem_report("probe_start",NULL);
    uint8_t *arena=fw_big_take(UINT32_MAX);
    s_probe_ok=fw_route_probe_run(arena,fw_big_bytes,fw_tls_context,fw_tls_context_bytes_reserved);
    fw_big_give();
    fw_mem_report("probe_end",NULL);   /* stack_free here = this task's lowest margin */
    xEventGroupSetBits(s_events,BIT_PROBE_DONE);
    vTaskDelete(NULL);
}

void app_main(void){
    char extra[160];
    esp_timer_handle_t deadline=NULL;
    const esp_timer_create_args_t deadline_args={.callback=pending_deadline,.name="fw_pending"};

    fw_log_init();      /* status lines also kept in RTC memory for GET /v1/log */
    fw_panic_note_init();   /* the task watchdog's FWPANIC line says which CPU starved */
    report_identity();
    const bool safe=fw_crash_guard_boot();   /* crash loop: Wi-Fi and updates only */
    snprintf(extra,sizeof extra,"\"tls_arena\":%u,\"tls_context_reserved\":%u,\"tls_context_needed\":%u",
             (unsigned)fw_big_bytes,(unsigned)fw_tls_context_bytes_reserved,(unsigned)fw_tls_context_bytes());
    fw_mem_report("boot",extra);

    if(running_is_pending()){
        ESP_ERROR_CHECK(esp_timer_create(&deadline_args,&deadline));
        ESP_ERROR_CHECK(esp_timer_start_once(deadline,(uint64_t)CONFIG_FW_PENDING_RESTART_S*1000000ull));
    }

    s_events=xEventGroupCreate();
    fw_big_init();      /* the shared 48 KiB block: logos (from the splash on) and TLS */

    /* NVS holds the device identity, certificates, pairing key and Wi-Fi
     * credentials. A fresh-project template would erase it on these errors;
     * this firmware must never do that. Opened before the panels only so the
     * board check can read the vendor's model name (a small, freed read). */
    esp_err_t nvs=nvs_flash_init();

    /* Is this the board the panel pins belong to? Checked before any panel
     * pin is touched (board_check.h). */
    const bool board_ok=fw_board_check(NULL);
    const bool limited=safe||!board_ok;

    /* Panels first after that, as the vendor firmware does: the 64 KiB DMA
     * frame buffer is the largest single allocation and is easiest to place
     * before Wi-Fi and TLS fragment the heap. */
    bool display_ok=false;
#if CONFIG_FW_DISPLAY_ENABLE
    if(!limited)display_ok=fw_display_start();
#endif

    if(nvs!=ESP_OK){
        fw_state("{\"nvs\":\"init_failed\",\"err\":%d,\"action\":\"offline_no_erase\"}",(int)nvs);
    }else if(!fw_wifi_link_start()){
        fw_mem_report("wifi_unavailable",NULL);
    }else{
        /* RF is running from here on: the hardware RNG is a true entropy source. */
        fw_esp_entropy_ready(true);
        /* The owner's update service (UPDATES.md): new builds over Wi-Fi,
         * keeping a keepable build, switching back to stock. */
        if(!fw_owner_update_start(display_ok))fw_mem_report("update_service_failed",NULL);
        if(limited){
            /* Nothing else: whatever crashed must not run again, and an
             * unexpected board must not see the panel driver. The desk can
             * read status and the log, upload a fix or switch to stock. */
            fw_state("{\"event\":\"%s\",\"note\":\"Wi-Fi and the update service only\"}",
                     safe?"safe_mode":"board_mismatch");
            for(;;){
                vTaskDelay(pdMS_TO_TICKS(30000));
                fw_mem_report("safe_mode_idle",NULL);
            }
        }
        /* Stock's join / lose / reconnect rules run in the link task; setup
         * mode starts only when Wi-Fi fails (SETUP.md). */
        if(!fw_wifi_link_wait_online((uint32_t)CONFIG_FW_VIEW_SECONDS*1000u)){
            fw_mem_report("wifi_not_online",NULL);
        }else{
            fw_mem_report("wifi_online",NULL);
            fw_games_start();   /* phones on the home network can play (GAMES.md) */
            fw_store_init();    /* owned settings, imported once from the vendor app's copy */
            {   /* the dashboard's address: on the console (the installer reads it) and,
                 * while no browser is signed in, on the panel for a minute */
                char url[40];
                FwPair *p = fw_store_pair_lock();
                unsigned paired = p ? p->store.count : 0;
                fw_store_pair_unlock(false);
                if (fw_dashboard_url(url, sizeof url)) {
                    fw_state("{\"dashboard\":\"%s\",\"paired\":%u}", url, paired);
                    if (!paired) fw_dashboard_hint(60000);
                }
            }
            fw_aliases_load();  /* owned airport aliases */
            fw_sync_store_init();   /* owned Sync schedule (SYNC.md) */
            fw_stats_init();    /* flight stats in memory (ANALYTICS.md) */
            fw_feed_start();    /* live flights from the vendor feed (FEED.md) */
            if(!sync_clock()){
                fw_mem_report("clock_unsynced",NULL);
            }else{
                fw_mem_report("clock_synced",NULL);
#if CONFIG_FW_PROBE && CONFIG_FW_WIFI_FAIL_ATTEMPTS == 0 && CONFIG_FW_WIFI_DROP_AFTER_S == 0
                xTaskCreate(probe_task,"fw_probe",CONFIG_FW_PROBE_TASK_STACK,NULL,5,NULL);
                xEventGroupWaitBits(s_events,BIT_PROBE_DONE,pdFALSE,pdTRUE,pdMS_TO_TICKS(120000));
#endif
            }
        }
    }

    snprintf(extra,sizeof extra,"\"probe_ok\":%s,\"wifi_attempts\":%u,\"display_ok\":%s,"
             "\"panel_frames\":%u,\"panel_stack_free\":%u",
             s_probe_ok?"true":"false",fw_wifi_link_attempts(),display_ok?"true":"false",
             (unsigned)fw_display_frames(),(unsigned)fw_display_task_stack_free());
    fw_mem_report("final",extra);

    if(running_is_pending()){
        /* Leave the test pattern up long enough to look at it. The owner may
         * keep a keepable build meanwhile (owner_update.c): then it stays. */
        while(esp_timer_get_time()<(int64_t)CONFIG_FW_VIEW_SECONDS*1000000&&!fw_owner_update_confirmed())
            vTaskDelay(pdMS_TO_TICKS(500));
        snprintf(extra,sizeof extra,"\"panel_frames\":%u,\"panel_stack_free\":%u",
                 (unsigned)fw_display_frames(),(unsigned)fw_display_task_stack_free());
        fw_mem_report("view_end",extra);
        if(!fw_owner_update_confirmed()){
            fw_state("{\"event\":\"pending_restart\",\"note\":\"never self-confirms; bootloader rolls back\"}");
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
        }
        fw_state("{\"event\":\"kept\",\"note\":\"confirmed by the owner; staying up\"}");
    }
    /* Flashed directly (not pending): stay up and keep reporting, so long-run
     * heap drift is visible on the console. */
    for(;;){
        vTaskDelay(pdMS_TO_TICKS(600000));   /* every 10 min: the 4 KiB log then spans hours */
        fw_mem_report("idle",NULL);
    }
}
