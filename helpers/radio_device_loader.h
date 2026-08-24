#pragma once

#include <lib/subghz/devices/devices.h>

typedef enum {
    RadioDeviceLoaderStatusOk,
    RadioDeviceLoaderStatusNotFound,
    RadioDeviceLoaderStatusBeginFailed,
} RadioDeviceLoaderStatus;

const SubGhzDevice* radio_device_loader_set(RadioDeviceLoaderStatus* status);
void radio_device_loader_end(const SubGhzDevice* radio_device);
