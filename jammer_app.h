#pragma once

#include <gui/gui.h>
#include <furi.h>
#include <furi_hal.h>
#include <lib/subghz/devices/devices.h>
#include <stdint.h>

typedef enum {
    JammerModeOok650Async,
    JammerMode2FSKDev238Async,
    JammerMode2FSKDev476Async,
    JammerModeMSK99_97KbAsync,
    JammerModeGFSK9_99KbAsync,
    JammerModeSquareWave,
    JammerModeWhiteNoise,
    JammerModeBruteforce,
    JammerModeBurst,
    JammerModeCount,
} JammerMode;

typedef enum {
    JammerRadioDeviceNone,
    JammerRadioDeviceExternal,
    JammerRadioDeviceInternal,
    JammerRadioDeviceCount,
} JammerRadioDevice;

typedef enum {
    JammerUiScreenMain,
    JammerUiScreenInternalWarning,
    JammerUiScreenCount,
} JammerUiScreen;

typedef enum {
    JammerInternalWarningExternalNotFound,
    JammerInternalWarningExternalBeginFailed,
    JammerInternalWarningCount,
} JammerInternalWarning;

typedef enum {
    JammerUiStateIdle,
    JammerUiStateStarting,
    JammerUiStateTransmitting,
    JammerUiStateStopping,
    JammerUiStateError,
    JammerUiStateCount,
} JammerUiState;

typedef enum {
    JammerUiErrorNone,
    JammerUiErrorExternalNotFound,
    JammerUiErrorExternalBeginFailed,
    JammerUiErrorInternalNotFound,
    JammerUiErrorThreadAllocFailed,
    JammerUiErrorTuneBlocked,
    JammerUiErrorSetTxFailed,
    JammerUiErrorAsyncStartFailed,
    JammerUiErrorPacketStartTimeout,
    JammerUiErrorPacketEndTimeout,
    JammerUiErrorInvalidPreset,
    JammerUiErrorTxWaitFailed,
    JammerUiErrorCount,
} JammerUiError;

typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriMessageQueue* event_queue;
    uint32_t frequency;
    uint8_t cursor_position;
    bool running;
    bool tx_requested;
    const SubGhzDevice* device;
    JammerRadioDevice radio_device_type;
    JammerRadioDevice requested_radio_device_type;
    FuriThread* tx_thread;
    FuriMutex* ui_mutex;
    JammerMode jamming_mode;
    JammerUiState ui_state;
    JammerUiError ui_error;
    JammerUiScreen ui_screen;
    JammerInternalWarning internal_warning;
    uint32_t tx_started_tick;
    FuriHalRegion* saved_region;
    bool region_overridden;
} JammerApp;

JammerApp* jammer_app_alloc(void);
void jammer_app_free(JammerApp* app);
int32_t jammer_app(void* p);
