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
    JammerUiErrorTxFailed,
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
    FuriThread* tx_thread;
    FuriMutex* ui_mutex;
    JammerMode jamming_mode;
    JammerUiState ui_state;
    JammerUiError ui_error;
    FuriHalRegion* saved_region;
    bool region_overridden;
} JammerApp;

JammerApp* jammer_app_alloc(void);
void jammer_app_free(JammerApp* app);
int32_t jammer_app(void* p);
