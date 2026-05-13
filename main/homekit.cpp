// Copyright 2023 Brandon Matthews <thenewwazoo@optimaltour.us>
// All rights reserved. GPLv3 License
#define TAG ("HOMEKIT")

#include <cstring>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include <esp_log.h>
#include <esp_system.h>
#include <esp_mac.h>
#include <esp_err.h>
#include <esp_app_desc.h>

#include <hap.h>
#include <hap_apple_servs.h>
#include <hap_apple_chars.h>

#include "homekit_decl.h"
#include "homekit.h"
#include <gdo.h>

#include "wifi.h"
#include "app_health.h"
#include "app_events.h"

#define DEVICE_NAME_SIZE 19
#define SERIAL_NAME_SIZE 18
#define HOMEKIT_SETUP_CODE "251-02-023"
#define HOMEKIT_SETUP_ID "KCTD"

// Make device_name available
char device_name[DEVICE_NAME_SIZE];

// Make serial_number available
char serial_number[SERIAL_NAME_SIZE];

// Queue to store GDO notification events
static QueueHandle_t gdo_notif_event_q;
static bool s_homekit_initialized;

enum class HomeKitNotifDest {
    DoorCurrentState,
    DoorTargetState,
    LockCurrentState,
    LockTargetState,
    Obstruction,
    Light,
    Motion,
    Battery,
    WallButton,
};

struct GDOEvent {
    HomeKitNotifDest dest;
    union {
        bool b;
        uint8_t u;
    } value;
};

static int gdo_svc_set(hap_write_data_t write_data[], int count, void *serv_priv, void *write_priv);
static int light_svc_set(hap_write_data_t write_data[], int count, void *serv_priv, void *write_priv);
static void sync_homekit_from_gdo_status(void);

static constexpr uint8_t HAP_CHARGING_NOT_CHARGING = 0;
static constexpr uint8_t HAP_CHARGING_CHARGING = 1;
static constexpr uint8_t HAP_CHARGING_NOT_CHARGEABLE = 2;
static constexpr uint8_t HAP_LOW_BATTERY_NORMAL = 0;
static constexpr uint8_t HAP_STATUS_FAULT_NONE = 0;
static constexpr uint8_t HAP_STATUS_FAULT_GENERAL = 1;
static constexpr uint8_t HAP_PROGRAMMABLE_SWITCH_SINGLE_PRESS = 0;

typedef struct {
    uint8_t level;
    uint8_t charging_state;
    uint8_t low_battery;
    uint8_t fault;
} homekit_battery_values_t;

static homekit_battery_values_t map_gdo_battery_to_homekit(gdo_battery_state_t battery)
{
    switch (battery) {
    case GDO_BATT_STATE_FULL:
        return {100, HAP_CHARGING_NOT_CHARGING, HAP_LOW_BATTERY_NORMAL, HAP_STATUS_FAULT_NONE};
    case GDO_BATT_STATE_CHARGING:
        return {50, HAP_CHARGING_CHARGING, HAP_LOW_BATTERY_NORMAL, HAP_STATUS_FAULT_NONE};
    case GDO_BATT_STATE_UNKNOWN:
    default:
        return {0, HAP_CHARGING_NOT_CHARGEABLE, HAP_LOW_BATTERY_NORMAL, HAP_STATUS_FAULT_GENERAL};
    }
}

static hap_status_t hap_status_from_esp_err(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return HAP_STATUS_SUCCESS;
    case ESP_ERR_NO_MEM:
        return HAP_STATUS_OO_RES;
    case ESP_ERR_TIMEOUT:
        return HAP_STATUS_TIMEOUT;
    case ESP_ERR_INVALID_ARG:
        return HAP_STATUS_VAL_INVALID;
    case ESP_ERR_INVALID_STATE:
    case ESP_ERR_NOT_FOUND:
    case ESP_ERR_NOT_SUPPORTED:
        return HAP_STATUS_RES_BUSY;
    default:
        return HAP_STATUS_COMM_ERR;
    }
}

static uint8_t map_lock_current_to_homekit(gdo_lock_state_t lock)
{
    if (lock == GDO_LOCK_STATE_UNLOCKED) {
        return HOMEKIT_CHARACTERISTIC_CURRENT_LOCK_STATE_UNSECURED;
    }
    if (lock == GDO_LOCK_STATE_LOCKED) {
        return HOMEKIT_CHARACTERISTIC_CURRENT_LOCK_STATE_SECURED;
    }
    return HOMEKIT_CHARACTERISTIC_CURRENT_LOCK_STATE_UNKNOWN;
}

static bool map_lock_target_to_homekit(gdo_lock_state_t lock, uint8_t *target)
{
    if (!target) {
        return false;
    }
    if (lock == GDO_LOCK_STATE_LOCKED) {
        *target = HOMEKIT_CHARACTERISTIC_TARGET_LOCK_STATE_SECURED;
        return true;
    }
    if (lock == GDO_LOCK_STATE_UNLOCKED) {
        *target = HOMEKIT_CHARACTERISTIC_TARGET_LOCK_STATE_UNSECURED;
        return true;
    }
    return false;
}

static void set_write_result(hap_write_data_t *write, esp_err_t err, const char *operation)
{
    *(write->status) = hap_status_from_esp_err(err);
    if (err == ESP_OK) {
        hap_char_update_val(write->hc, &(write->val));
    } else {
        ESP_LOGE(TAG, "%s failed: %s", operation, esp_err_to_name(err));
    }
}


/********************************** MAIN LOOP CODE *****************************************/

int identify(hap_acc_t *acc) {
    ESP_LOGI(TAG, "identify called");
    return HAP_SUCCESS;
}

static void homekit_event_handler(hap_event_t event, void *data)
{
    (void)data;
    switch (event) {
    case HAP_EVENT_CTRL_PAIRED:
        app_events_log("homekit", "warn", "controller_paired", "HomeKit controller paired", "{}", true);
        break;
    case HAP_EVENT_CTRL_UNPAIRED:
        app_events_log("homekit", "warn", "controller_unpaired", "HomeKit controller unpaired", "{}", true);
        break;
    case HAP_EVENT_PAIRING_STARTED:
        app_events_log("homekit", "info", "pairing_started", "HomeKit pairing started", "{}", false);
        break;
    case HAP_EVENT_PAIRING_ABORTED:
        app_events_log("homekit", "warn", "pairing_aborted", "HomeKit pairing aborted", "{}", true);
        break;
    case HAP_EVENT_ACC_REBOOTING:
        app_events_log("homekit", "warn", "accessory_rebooting", "HomeKit accessory requested reboot", "{}", true);
        break;
    default:
        break;
    }
}

void homekit_task_entry(void* ctx) {

    uint8_t mac[8] = {0};
    ESP_ERROR_CHECK(esp_efuse_mac_get_default(mac));

    snprintf(device_name, DEVICE_NAME_SIZE, "Garage Door %02X%02X%02X", mac[2], mac[1], mac[0]);
    snprintf(
        serial_number,
        SERIAL_NAME_SIZE,
        "%02X:%02X:%02X:%02X:%02X:%02X",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]
    );

    /*
    hap_reset_homekit_data();
    while (1) {}
    */

    hap_acc_t *accessory;
    hap_serv_t *gdo_svc;
    hap_serv_t *motion_svc;
    hap_serv_t *light_svc;
    hap_serv_t *battery_svc;
    hap_serv_t *lock_svc;
    hap_serv_t *wall_button_svc;

    gdo_notif_event_q = xQueueCreate(16, sizeof(GDOEvent));
    if (!gdo_notif_event_q) {
        ESP_LOGE(TAG, "failed to create HomeKit notification queue");
        vTaskDelete(NULL);
        return;
    }

    if (hap_init(HAP_TRANSPORT_WIFI) != HAP_SUCCESS) {
        ESP_LOGE(TAG, "failed to initialize HomeKit");
        vTaskDelete(NULL);
        return;
    }
    hap_register_event_handler(homekit_event_handler);

    hap_acc_cfg_t config;
    const esp_app_desc_t *app_desc = esp_app_get_description();
    static char hw_rev[32];
    snprintf(hw_rev, sizeof(hw_rev), "%s", CONFIG_IDF_TARGET);
    config.name = device_name;
    config.manufacturer = const_cast<char*>("Konnected Inc");
    config.model = const_cast<char*>("GDO blaQ HomeKit");
    config.serial_num = serial_number;
    config.fw_rev = const_cast<char*>(app_desc && app_desc->version[0] ? app_desc->version : "dev");
    config.hw_rev = hw_rev;
    config.identify_routine = identify;
    config.cid = HAP_CID_GARAGE_DOOR_OPENER;

    accessory = hap_acc_create(&config);

    // create garage door opener service with optional lock characteristics
    gdo_svc = hap_serv_garage_door_opener_create(
            HOMEKIT_CHARACTERISTIC_CURRENT_DOOR_STATE_OPEN,
            HOMEKIT_CHARACTERISTIC_TARGET_DOOR_STATE_OPEN,
            HOMEKIT_CHARACTERISTIC_OBSTRUCTION_SENSOR_OBSTRUCTED);
    hap_serv_add_char(gdo_svc, hap_char_name_create(const_cast<char*>("Konnected blaQ")));
    hap_serv_add_char(gdo_svc, hap_char_lock_current_state_create(0));
    hap_serv_add_char(gdo_svc, hap_char_lock_target_state_create(0));

    hap_serv_set_write_cb(gdo_svc, gdo_svc_set);

    hap_acc_add_serv(accessory, gdo_svc);

    // create the motion sensor service with no optional characteristics (e.g. active)
    motion_svc = hap_serv_motion_sensor_create(false);

    hap_acc_add_serv(accessory, motion_svc);

    // create the light service with no optional characteristics (e.g. brightness)
    light_svc = hap_serv_lightbulb_create(false);

    hap_serv_set_write_cb(light_svc, light_svc_set);

    hap_acc_add_serv(accessory, light_svc);

    gdo_status_t initial_status = {};
    uint8_t initial_lock_current = HOMEKIT_CHARACTERISTIC_CURRENT_LOCK_STATE_UNKNOWN;
    uint8_t initial_lock_target = HOMEKIT_CHARACTERISTIC_TARGET_LOCK_STATE_UNSECURED;
    if (gdo_get_status(&initial_status) == ESP_OK) {
        initial_lock_current = map_lock_current_to_homekit(initial_status.lock);
        uint8_t mapped_target = 0;
        if (map_lock_target_to_homekit(initial_status.lock, &mapped_target)) {
            initial_lock_target = mapped_target;
        }
    }

    lock_svc = hap_serv_lock_mechanism_create(initial_lock_current, initial_lock_target);
    if (lock_svc) {
        hap_serv_add_char(lock_svc, hap_char_name_create(const_cast<char*>("GDO Lock")));
        hap_serv_set_write_cb(lock_svc, gdo_svc_set);
        hap_acc_add_serv(accessory, lock_svc);
    }

    wall_button_svc = hap_serv_stateless_programmable_switch_create(HAP_PROGRAMMABLE_SWITCH_SINGLE_PRESS);
    if (wall_button_svc) {
        hap_serv_add_char(wall_button_svc, hap_char_name_create(const_cast<char*>("Wall Button")));
        hap_acc_add_serv(accessory, wall_button_svc);
    }

    homekit_battery_values_t battery_values = map_gdo_battery_to_homekit(GDO_BATT_STATE_UNKNOWN);
    battery_svc = hap_serv_battery_service_create(
        battery_values.level, battery_values.charging_state, battery_values.low_battery);
    if (battery_svc) {
        hap_serv_add_char(battery_svc, hap_char_name_create(const_cast<char*>("GDO Battery")));
        hap_serv_add_char(battery_svc, hap_char_status_fault_create(battery_values.fault));
        hap_acc_add_serv(accessory, battery_svc);
    }

    hap_add_accessory(accessory);

    // initialize and start homekit
    hap_set_setup_code(HOMEKIT_SETUP_CODE);  // On Oct 25, 2023, Chamberlain announced they were disabling API
                                       // access for "unauthorized" third parties.
    hap_set_setup_id(HOMEKIT_SETUP_ID);

    // wifi setup is stuck in the homekit code because homekit sets up some event handlers, and the
    // ordering matters.
    app_wifi_init();

    if (hap_start() != HAP_SUCCESS) {
        ESP_LOGE(TAG, "failed to start HomeKit");
        s_homekit_initialized = false;
        vTaskDelete(NULL);
        return;
    }
    s_homekit_initialized = true;
    app_health_mark_homekit_started();
    sync_homekit_from_gdo_status();

    GDOEvent e;

    while (true) {
        hap_val_t value;
        hap_char_t* dest = NULL;
        hap_char_t* extra = NULL;

        if (xQueueReceive(gdo_notif_event_q, &e, portMAX_DELAY)) {
            switch (e.dest) {
                case HomeKitNotifDest::DoorCurrentState:
                    dest = hap_serv_get_char_by_uuid(gdo_svc, HAP_CHAR_UUID_CURRENT_DOOR_STATE);
                    value.u = e.value.u;
                    break;
                case HomeKitNotifDest::DoorTargetState:
                    dest = hap_serv_get_char_by_uuid(gdo_svc, HAP_CHAR_UUID_TARGET_DOOR_STATE);
                    value.u = e.value.u;
                    break;
                case HomeKitNotifDest::LockCurrentState:
                    dest = hap_serv_get_char_by_uuid(gdo_svc, HAP_CHAR_UUID_LOCK_CURRENT_STATE);
                    extra = lock_svc ? hap_serv_get_char_by_uuid(lock_svc, HAP_CHAR_UUID_LOCK_CURRENT_STATE) : NULL;
                    value.u = e.value.u;
                    break;
                case HomeKitNotifDest::LockTargetState:
                    dest = hap_serv_get_char_by_uuid(gdo_svc, HAP_CHAR_UUID_LOCK_TARGET_STATE);
                    extra = lock_svc ? hap_serv_get_char_by_uuid(lock_svc, HAP_CHAR_UUID_LOCK_TARGET_STATE) : NULL;
                    value.u = e.value.u;
                    break;
                case HomeKitNotifDest::Obstruction:
                    dest = hap_serv_get_char_by_uuid(gdo_svc, HAP_CHAR_UUID_OBSTRUCTION_DETECTED);
                    value.b = e.value.b;
                    break;
                case HomeKitNotifDest::Light:
                    dest = hap_serv_get_char_by_uuid(light_svc, HAP_CHAR_UUID_ON);
                    value.b = e.value.b;
                    break;
                case HomeKitNotifDest::Motion:
                    dest = hap_serv_get_char_by_uuid(motion_svc, HAP_CHAR_UUID_MOTION_DETECTED);
                    value.b = e.value.b;
                    break;
                case HomeKitNotifDest::Battery: {
                    if (!battery_svc) {
                        break;
                    }
                    homekit_battery_values_t values = map_gdo_battery_to_homekit((gdo_battery_state_t)e.value.u);
                    hap_char_t *battery_level = hap_serv_get_char_by_uuid(battery_svc, HAP_CHAR_UUID_BATTERY_LEVEL);
                    hap_char_t *charging = hap_serv_get_char_by_uuid(battery_svc, HAP_CHAR_UUID_CHARGING_STATE);
                    hap_char_t *low_battery = hap_serv_get_char_by_uuid(battery_svc, HAP_CHAR_UUID_STATUS_LOW_BATTERY);
                    hap_char_t *fault = hap_serv_get_char_by_uuid(battery_svc, HAP_CHAR_UUID_STATUS_FAULT);
                    hap_val_t battery_val;
                    battery_val.u = 0;
                    if (battery_level) {
                        battery_val.u = values.level;
                        hap_char_update_val(battery_level, &battery_val);
                    }
                    if (charging) {
                        battery_val.u = values.charging_state;
                        hap_char_update_val(charging, &battery_val);
                    }
                    if (low_battery) {
                        battery_val.u = values.low_battery;
                        hap_char_update_val(low_battery, &battery_val);
                    }
                    if (fault) {
                        battery_val.u = values.fault;
                        hap_char_update_val(fault, &battery_val);
                    }
                    dest = NULL;
                    break;
                }
                case HomeKitNotifDest::WallButton:
                    if (!wall_button_svc) {
                        break;
                    }
                    dest = hap_serv_get_char_by_uuid(wall_button_svc, HAP_CHAR_UUID_PROGRAMMABLE_SWITCH_EVENT);
                    value.u = e.value.u;
                    break;
            }
            if (dest) {
                ESP_LOGI(TAG, "updating characteristic");
                if (hap_char_update_val(dest, &value) == HAP_FAIL) {
                    ESP_LOGE(TAG, "failed to update characteristic");
                }
            }
            if (extra) {
                if (hap_char_update_val(extra, &value) == HAP_FAIL) {
                    ESP_LOGE(TAG, "failed to update secondary characteristic");
                }
            }
        }
    }
}

/******************************** GETTERS AND SETTERS ***************************************/

// this function is called by HomeKit when the value of a characteristic changes (i.e. has been set
// by the user) for the garage door service. It effectuates the value of the characteristic.
static int gdo_svc_set(hap_write_data_t write_data[], int count, void *serv_priv, void *write_priv) {

    int i, ret = HAP_SUCCESS;
    hap_write_data_t *write;

    for (i = 0; i < count; i++) {
        write = &write_data[i];

        if (!strcmp(hap_char_get_type_uuid(write->hc), HAP_CHAR_UUID_TARGET_DOOR_STATE)) {
            ESP_LOGI(TAG, "set door state: %" PRIu32, write->val.u);
            char data[48];
            snprintf(data, sizeof(data), "{\"target\":%" PRIu32 "}", write->val.u);
            app_events_log("homekit", "info", "door_command", "HomeKit door target requested", data, false);
            switch (write->val.u) {
                case TGT_OPEN:
                    set_write_result(write, gdo_door_open(), "open door");
                    if (*(write->status) != HAP_STATUS_SUCCESS) {
                        ret = HAP_FAIL;
                    }
                    break;
                case TGT_CLOSED:
                    set_write_result(write, gdo_door_close(), "close door");
                    if (*(write->status) != HAP_STATUS_SUCCESS) {
                        ret = HAP_FAIL;
                    }
                    break;
                default:
                    ESP_LOGE(TAG, "invalid target door state set requested: %" PRIu32, write->val.u);
                    *(write->status) = HAP_STATUS_VAL_INVALID;
                    ret = HAP_FAIL;
                    break;
            }

        } else if (!strcmp(hap_char_get_type_uuid(write->hc), HAP_CHAR_UUID_LOCK_TARGET_STATE)) {
            ESP_LOGI(TAG, "set lock state: %s", write->val.u == HOMEKIT_CHARACTERISTIC_TARGET_LOCK_STATE_SECURED ? "Locked" : "Unlocked");
            char data[48];
            snprintf(data, sizeof(data), "{\"target\":%" PRIu32 "}", write->val.u);
            app_events_log("homekit", "info", "lock_command", "HomeKit lock target requested", data, false);
            esp_err_t err;
            if (write->val.u == HOMEKIT_CHARACTERISTIC_TARGET_LOCK_STATE_SECURED) {
                err = gdo_lock();
            } else if (write->val.u == HOMEKIT_CHARACTERISTIC_TARGET_LOCK_STATE_UNSECURED) {
                err = gdo_unlock();
            } else {
                ESP_LOGE(TAG, "invalid target lock state set requested: %" PRIu32, write->val.u);
                *(write->status) = HAP_STATUS_VAL_INVALID;
                ret = HAP_FAIL;
                continue;
            }
            set_write_result(write, err, "set lock");
            if (*(write->status) != HAP_STATUS_SUCCESS) {
                ret = HAP_FAIL;
            }

        } else {
            // no other characteristics are settable
            ESP_LOGE(TAG, "invalid characteristic set, requested UUID: %s", hap_char_get_type_uuid(write->hc));
            *(write->status) = HAP_STATUS_RES_ABSENT;
            ret = HAP_FAIL;

        }
    }

    return ret;
}

GarageDoorCurrentState map_gdo_to_homekit_state(gdo_door_state_t gdo_state) {
    switch (gdo_state) {
        case GDO_DOOR_STATE_OPEN:
            return CURR_OPEN;
        case GDO_DOOR_STATE_CLOSED:
            return CURR_CLOSED;
        case GDO_DOOR_STATE_OPENING:
            return CURR_OPENING;
        case GDO_DOOR_STATE_CLOSING:
            return CURR_CLOSING;
        case GDO_DOOR_STATE_STOPPED:
            return CURR_STOPPED;
        default:
            // unknown or unsupported states; return a default value
            return CURR_STOPPED;
    }
}

static void queue_homekit_uint(HomeKitNotifDest dest, uint8_t value, const char *name) {
    GDOEvent e;
    e.dest = dest;
    e.value.u = value;
    if (!gdo_notif_event_q || xQueueSend(gdo_notif_event_q, &e, 0) == errQUEUE_FULL) {
        ESP_LOGE(TAG, "could not queue homekit notif of %s", name);
    }
}

static void sync_homekit_from_gdo_status(void) {
    gdo_status_t status = {};
    esp_err_t err = gdo_get_status(&status);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "could not seed HomeKit state from GDO status: %s", esp_err_to_name(err));
        return;
    }

    queue_homekit_uint(HomeKitNotifDest::DoorCurrentState, map_gdo_to_homekit_state(status.door), "initial door current state");
    notify_homekit_target_door_state_change(status.door);
    notify_homekit_current_lock(status.lock);
    notify_homekit_target_lock(status.lock);
    notify_homekit_obstruction(status.obstruction);
    notify_homekit_light(status.light);
    notify_homekit_motion(status.motion);
    notify_homekit_battery(status.battery);
}

// this function is called when the current state of the door changes in the world (i.e. we wish to
// update the representation in homekit)
void notify_homekit_current_door_state_change(gdo_door_state_t door) {
    GDOEvent e;
    e.dest = HomeKitNotifDest::DoorCurrentState;
    e.value.u = map_gdo_to_homekit_state(door);
    if (!gdo_notif_event_q || xQueueSend(gdo_notif_event_q, &e, 0) == errQUEUE_FULL) {
        ESP_LOGE(TAG, "could not queue homekit notif of door current state");
    }
}

void notify_homekit_target_door_state_change(gdo_door_state_t door) {
    switch (door) {
    case GDO_DOOR_STATE_OPEN:
    case GDO_DOOR_STATE_OPENING:
        queue_homekit_uint(HomeKitNotifDest::DoorTargetState, HOMEKIT_CHARACTERISTIC_TARGET_DOOR_STATE_OPEN, "door target state");
        break;
    case GDO_DOOR_STATE_CLOSED:
    case GDO_DOOR_STATE_CLOSING:
        queue_homekit_uint(HomeKitNotifDest::DoorTargetState, HOMEKIT_CHARACTERISTIC_TARGET_DOOR_STATE_CLOSED, "door target state");
        break;
    default:
        break;
    }
}

// this function is called by HomeKit when the value of a characteristic changes (i.e. has been set
// by the user) for the light service. It effectuates the value of the characteristic.
static int light_svc_set(hap_write_data_t write_data[], int count, void *serv_priv, void *write_priv) {

    int i, ret = HAP_SUCCESS;
    hap_write_data_t *write;
    for (i = 0; i < count; i++) {
        write = &write_data[i];

        if (!strcmp(hap_char_get_type_uuid(write->hc), HAP_CHAR_UUID_ON)) {
            ESP_LOGI(TAG, "set light: %s", write->val.b ? "On" : "Off");
            char data[32];
            snprintf(data, sizeof(data), "{\"on\":%s}", write->val.b ? "true" : "false");
            app_events_log("homekit", "info", "light_command", "HomeKit light requested", data, false);
            esp_err_t err;
            if (write->val.b) {
                err = gdo_light_on();
            } else {
                err = gdo_light_off();
            }
            set_write_result(write, err, "set light");
            if (*(write->status) != HAP_STATUS_SUCCESS) {
                ret = HAP_FAIL;
            }

        } else {
            // no other characteristics are settable
            ESP_LOGE(TAG, "invalid characteristic set, requested UUID: %s", hap_char_get_type_uuid(write->hc));
            *(write->status) = HAP_STATUS_RES_ABSENT;
            ret = HAP_FAIL;

        }
    }

    return ret;
}

// this function is called when the current state of the obstruction sensor changes in the world
// (i.e. we wish to update the representation in homekit)
void notify_homekit_obstruction(gdo_obstruction_state_t obstructed) {
    GDOEvent e;
    e.dest = HomeKitNotifDest::Obstruction;
    e.value.b = (obstructed == GDO_OBSTRUCTION_STATE_CLEAR)
                  ? HOMEKIT_CHARACTERISTIC_OBSTRUCTION_SENSOR_CLEAR
                  : HOMEKIT_CHARACTERISTIC_OBSTRUCTION_SENSOR_OBSTRUCTED;
    if (!gdo_notif_event_q || xQueueSend(gdo_notif_event_q, &e, 0) == errQUEUE_FULL) {
        ESP_LOGE(TAG, "could not queue homekit notif of door obstructed");
    }
}

// this function is called when the current state of the lock changes in the world (i.e. we wish to
// update the representation in homekit)
void notify_homekit_current_lock(gdo_lock_state_t lock) {
    GDOEvent e;
    e.dest = HomeKitNotifDest::LockCurrentState;
    e.value.u = map_lock_current_to_homekit(lock);
    if (!gdo_notif_event_q || xQueueSend(gdo_notif_event_q, &e, 0) == errQUEUE_FULL) {
        ESP_LOGE(TAG, "could not queue homekit notif of lock state");
    }
}

void notify_homekit_target_lock(gdo_lock_state_t lock) {
    uint8_t target = 0;
    if (map_lock_target_to_homekit(lock, &target)) {
        queue_homekit_uint(HomeKitNotifDest::LockTargetState, target, "lock target state");
    }
}

// this function is called when the state of the light changes in the world (i.e. we wish to update
// the representation in homekit)
void notify_homekit_light(gdo_light_state_t light) {
    GDOEvent e;
    e.dest = HomeKitNotifDest::Light;
    e.value.b = (light == GDO_LIGHT_STATE_ON);
    if (!gdo_notif_event_q || xQueueSend(gdo_notif_event_q, &e, 0) == errQUEUE_FULL) {
        ESP_LOGE(TAG, "could not queue homekit notif of light state");
    }
}

// this function is called when the state of the motion sensor changes in the world (i.e. we wish to
// update the representation in homekit)
void notify_homekit_motion(gdo_motion_state_t motion) {
    GDOEvent e;
    e.dest = HomeKitNotifDest::Motion;
    e.value.b = (motion == GDO_MOTION_STATE_CLEAR)
        ? HOMEKIT_CHARACTERISTIC_MOTION_NOT_DETECTED
        : HOMEKIT_CHARACTERISTIC_MOTION_DETECTED;
    if (!gdo_notif_event_q || xQueueSend(gdo_notif_event_q, &e, 0) == errQUEUE_FULL) {
        ESP_LOGE(TAG, "could not queue homekit notif of motion");
    }
}

void notify_homekit_battery(gdo_battery_state_t battery) {
    GDOEvent e;
    e.dest = HomeKitNotifDest::Battery;
    e.value.u = (uint8_t)battery;
    if (!gdo_notif_event_q || xQueueSend(gdo_notif_event_q, &e, 0) == errQUEUE_FULL) {
        ESP_LOGE(TAG, "could not queue homekit notif of battery");
    }
}

void notify_homekit_wall_button(gdo_button_state_t button) {
    if (button != GDO_BUTTON_STATE_PRESSED) {
        return;
    }
    queue_homekit_uint(HomeKitNotifDest::WallButton, HAP_PROGRAMMABLE_SWITCH_SINGLE_PRESS, "wall button");
}

int homekit_paired_controller_count(void)
{
    if (!s_homekit_initialized) {
        return -1;
    }
    return hap_get_paired_controller_count();
}

char *homekit_setup_payload(void)
{
    if (!s_homekit_initialized || hap_get_paired_controller_count() != 0) {
        return NULL;
    }
    return esp_hap_get_setup_payload(const_cast<char*>(HOMEKIT_SETUP_CODE), const_cast<char*>(HOMEKIT_SETUP_ID),
                                     false, HAP_CID_GARAGE_DOOR_OPENER);
}
