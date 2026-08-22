#pragma once

#include <lib/subghz/devices/devices.h>

const SubGhzDevice* radio_device_loader_set(void);
void radio_device_loader_end(const SubGhzDevice* radio_device);
