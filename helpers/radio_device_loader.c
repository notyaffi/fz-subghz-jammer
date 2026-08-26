#include "radio_device_loader.h"

#include <applications/drivers/subghz/cc1101_ext/cc1101_ext_interconnect.h>
#include <lib/subghz/devices/cc1101_int/cc1101_int_interconnect.h>

static bool radio_device_loader_otg_was_enabled = false;
static bool radio_device_loader_otg_enabled_by_app = false;
static bool radio_device_loader_external_active = false;

static void radio_device_loader_power_on() {
    uint8_t attempts = 0;
    while(!furi_hal_power_is_otg_enabled() && attempts++ < 5) {
        furi_hal_power_enable_otg();
        //CC1101 power-up time
        furi_delay_ms(10);
    }
}

static void radio_device_loader_power_off() {
    if(furi_hal_power_is_otg_enabled()) furi_hal_power_disable_otg();
}

static void radio_device_loader_release_owned_otg() {
    if(radio_device_loader_otg_enabled_by_app) {
        radio_device_loader_power_off();
    }

    radio_device_loader_otg_was_enabled = false;
    radio_device_loader_otg_enabled_by_app = false;
}

static bool radio_device_loader_is_connect_external(void) {
    bool is_connect = false;
    bool is_otg_enabled = furi_hal_power_is_otg_enabled();

    if(!is_otg_enabled) {
        radio_device_loader_power_on();
    }

    const SubGhzDevice* device = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_EXT_NAME);
    if(device) {
        is_connect = subghz_devices_is_connect(device);
    }

    if(!is_otg_enabled) {
        radio_device_loader_power_off();
    }
    return is_connect;
}

const SubGhzDevice* radio_device_loader_set_external(RadioDeviceLoaderStatus* status) {
    furi_assert(status);
    furi_assert(!radio_device_loader_external_active);
    furi_assert(!radio_device_loader_otg_enabled_by_app);

    const SubGhzDevice* radio_device = NULL;
    *status = RadioDeviceLoaderStatusExternalNotFound;
    radio_device_loader_otg_was_enabled = furi_hal_power_is_otg_enabled();

    if(radio_device_loader_is_connect_external()) {
        radio_device_loader_power_on();
        radio_device_loader_otg_enabled_by_app =
            !radio_device_loader_otg_was_enabled && furi_hal_power_is_otg_enabled();
        radio_device = subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_EXT_NAME);
        if(radio_device && subghz_devices_begin(radio_device)) {
            radio_device_loader_external_active = true;
            *status = RadioDeviceLoaderStatusOk;
            FURI_LOG_D("radio_device_loader", "External CC1101 initialized.");
        } else {
            *status = RadioDeviceLoaderStatusExternalBeginFailed;
            FURI_LOG_E("radio_device_loader", "Failed to initialize external CC1101.");
            if(radio_device) {
                subghz_devices_end(radio_device);
                radio_device = NULL;
            }
            radio_device_loader_release_owned_otg();
        }
    } else {
        radio_device_loader_otg_was_enabled = false;
        FURI_LOG_W("radio_device_loader", "External CC1101 not found.");
    }

    return radio_device;
}

const SubGhzDevice* radio_device_loader_set_internal(RadioDeviceLoaderStatus* status) {
    furi_assert(status);

    const SubGhzDevice* radio_device =
        subghz_devices_get_by_name(SUBGHZ_DEVICE_CC1101_INT_NAME);
    if(radio_device) {
        *status = RadioDeviceLoaderStatusOk;
        FURI_LOG_D("radio_device_loader", "Internal CC1101 selected.");
    } else {
        *status = RadioDeviceLoaderStatusInternalNotFound;
        FURI_LOG_E("radio_device_loader", "Internal CC1101 not found.");
    }

    return radio_device;
}

void radio_device_loader_end_external(const SubGhzDevice* radio_device) {
    furi_assert(radio_device);
    furi_assert(radio_device_loader_external_active);

    radio_device_loader_release_owned_otg();
    subghz_devices_end(radio_device);
    radio_device_loader_external_active = false;
}
