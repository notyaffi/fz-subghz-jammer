#pragma once

#include <lib/subghz/devices/devices.h>

typedef enum {
    RadioDeviceLoaderStatusOk,
    RadioDeviceLoaderStatusExternalNotFound,
    RadioDeviceLoaderStatusExternalBeginFailed,
    RadioDeviceLoaderStatusInternalNotFound,
} RadioDeviceLoaderStatus;

const SubGhzDevice* radio_device_loader_set_external(RadioDeviceLoaderStatus* status);
const SubGhzDevice* radio_device_loader_set_internal(RadioDeviceLoaderStatus* status);
void radio_device_loader_end_external(const SubGhzDevice* radio_device);
