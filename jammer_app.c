#include "jammer_app.h"
#include <furi_hal_region.h>
#include <furi.h>
#include <gui/gui.h>
#include <subghz/devices/devices.h>
#include <furi/core/log.h>
#include <furi_hal.h>
#include <toolbox/level_duration.h>
#include "helpers/radio_device_loader.h"

#define TAG "JammerApp"
#define SUBGHZ_FREQUENCY_MIN 300000000
#define SUBGHZ_FREQUENCY_MAX 928000000
#define MESSAGE_MAX_LEN 1024
#define JAMMER_PACKET_MAX_LEN 60
#define JAMMER_PACKET_TIMEOUT_TICKS 200
#define JAMMER_OOK_BIT_DURATION_US 264
#define JAMMER_2FSK_BIT_DURATION_US 208
#define JAMMER_TX_FLAG_STOP (1U << 0)

static const FuriHalRegionBand unlocked_region_bands[] = {
    {.start = 299999755, .end = 348000000, .power_limit = 20, .duty_cycle = 50},
    {.start = 386999938, .end = 464000000, .power_limit = 20, .duty_cycle = 50},
    {.start = 778999847, .end = 928000000, .power_limit = 20, .duty_cycle = 50},
};

typedef struct {
    uint32_t min;
    uint32_t max;
} FrequencyBand;

typedef struct {
    const uint8_t* data;
    size_t size;
    size_t byte_index;
    uint8_t bit_index;
    uint32_t bit_duration_us;
} JammerAsyncPattern;

typedef enum {
    JammerPacketResultSent,
    JammerPacketResultStopped,
    JammerPacketResultSetTxFailed,
    JammerPacketResultStartTimeout,
    JammerPacketResultCompletionTimeout,
} JammerPacketResult;

static const FrequencyBand valid_frequency_bands[] = {
    {300000000, 348000000},
    {387000000, 464000000},
    {779000000, 928000000},
};

#define NUM_FREQUENCY_BANDS (sizeof(valid_frequency_bands) / sizeof(valid_frequency_bands[0]))

static const char* jamming_modes[] = {
    "OOK 650kHz",
    "2FSK 2.38kHz",
    "2FSK 47.6kHz",
    "MSK 99.97Kb/s",
    "GFSK 9.99Kb/s",
    "Square Wave",
    "White Noise",
    "Bruteforce 0xFF",
    "Burst Mode",
};

static const char* jammer_ui_states[] = {
    "IDLE",
    "START",
    "TX",
    "STOP",
    "ERROR",
};

static const char* jammer_ui_errors[] = {
    "",
    "EXT NOT FOUND",
    "EXT BEGIN FAILED",
    "INT NOT FOUND",
    "THREAD ALLOC FAILED",
    "TUNE BLOCKED",
    "SET TX FAILED",
    "ASYNC START FAILED",
    "PACKET START TIMEOUT",
    "PACKET END TIMEOUT",
    "INVALID PRESET",
    "TX WAIT FAILED",
};

static const char* jammer_radio_device_labels[] = {
    "---",
    "EXT",
    "INT",
};

_Static_assert(COUNT_OF(jamming_modes) == JammerModeCount, "Jammer mode labels mismatch");
_Static_assert(COUNT_OF(jammer_ui_states) == JammerUiStateCount, "Jammer state labels mismatch");
_Static_assert(COUNT_OF(jammer_ui_errors) == JammerUiErrorCount, "Jammer error labels mismatch");
_Static_assert(
    COUNT_OF(jammer_radio_device_labels) == JammerRadioDeviceCount,
    "Jammer radio device labels mismatch");

static JammerUiError
    jammer_init_subghz(JammerApp* app, JammerRadioDevice radio_device_type);
static void jammer_show_internal_warning(JammerApp* app, JammerUiError reason);
static void jammer_accept_internal(JammerApp* app);
static void jammer_retry_external(JammerApp* app);
static void jammer_start_after_radio_init(JammerApp* app);
static void jammer_release_radio(JammerApp* app);
static void jammer_start_tx(JammerApp* app);
static void jammer_stop_tx(JammerApp* app);
static void jammer_toggle_tx(JammerApp* app);
static void jammer_switch_mode(JammerApp* app);
static bool jammer_load_preset(JammerApp* app, JammerMode mode);
static void jammer_update_view(JammerApp* app);
static void jammer_set_ui_state(JammerApp* app, JammerUiState state);
static JammerUiState jammer_get_ui_state(JammerApp* app);
static JammerUiScreen jammer_get_ui_screen(JammerApp* app);
static JammerRadioDevice jammer_get_requested_radio_device(JammerApp* app);
static JammerUiError jammer_get_missing_device_error(JammerApp* app);
static bool jammer_get_tx_elapsed_seconds(JammerApp* app, uint32_t* elapsed_seconds);
static void jammer_set_ui_error(JammerApp* app, JammerUiError error);
static void jammer_clear_ui_error(JammerApp* app);
static bool jammer_override_region(JammerApp* app);
static void jammer_restore_region(JammerApp* app);
static void jammer_adjust_frequency(JammerApp* app, bool up);
static uint32_t adjust_frequency_to_valid(uint32_t frequency, bool up);
static bool is_frequency_valid(uint32_t frequency);
static int32_t jammer_tx_thread(void* context);
static LevelDuration jammer_async_pattern_yield(void* context);
static bool jammer_tx_stop_requested(void);
static bool jammer_tx_wait_for_stop(uint32_t timeout);
static JammerPacketResult jammer_send_packet(JammerApp* app, const uint8_t* data, uint8_t size);
static void jammer_draw_callback(Canvas* canvas, void* context);
static void jammer_input_callback(InputEvent* input_event, void* context);

int32_t jammer_app(void* p) {
    UNUSED(p);

    FURI_LOG_I(TAG, "Starting JammerApp");

    JammerApp* app = jammer_app_alloc();
    if(!app) {
        FURI_LOG_E(TAG, "Failed to allocate JammerApp");
        return -1;
    }

    jammer_set_ui_state(app, JammerUiStateStarting);
    const JammerUiError init_error =
        jammer_init_subghz(app, JammerRadioDeviceExternal);
    if(init_error == JammerUiErrorExternalNotFound ||
       init_error == JammerUiErrorExternalBeginFailed) {
        jammer_show_internal_warning(app, init_error);
    } else if(init_error != JammerUiErrorNone) {
        jammer_set_ui_error(app, init_error);
    } else {
        jammer_start_after_radio_init(app);
    }

    FURI_LOG_I(TAG, "Entering main loop");

    InputEvent event;
    const uint32_t timer_tick_frequency = furi_kernel_get_tick_frequency();
    uint32_t timer_poll_interval = timer_tick_frequency / 4U;
    if(timer_poll_interval == 0) timer_poll_interval = 1;
    uint32_t last_timer_poll_tick = furi_get_tick();
    uint32_t last_tx_elapsed_seconds = UINT32_MAX;

    while(app->running) {
        if(furi_message_queue_get(app->event_queue, &event, 10) == FuriStatusOk) {
            if(jammer_get_ui_screen(app) == JammerUiScreenInternalWarning) {
                if(event.type == InputTypeLong && event.key == InputKeyOk) {
                    FURI_LOG_I(TAG, "Retrying external CC1101 initialization");
                    jammer_retry_external(app);
                } else if(event.type == InputTypeShort) {
                    if(event.key == InputKeyOk) {
                        FURI_LOG_W(TAG, "Internal CC1101 use confirmed");
                        jammer_accept_internal(app);
                    } else if(event.key == InputKeyBack) {
                        FURI_LOG_I(TAG, "Internal CC1101 warning declined");
                        app->running = false;
                    }
                }
                continue;
            }

            if(event.type == InputTypeLong && event.key == InputKeyOk) {
                FURI_LOG_I(TAG, "OK button held");
                jammer_toggle_tx(app);
            } else if(event.type == InputTypeShort) {
                switch(event.key) {
                    case InputKeyOk:
                        FURI_LOG_I(TAG, "OK button pressed");
                        jammer_switch_mode(app);
                        jammer_update_view(app);
                        break;
                    case InputKeyBack:
                        FURI_LOG_I(TAG, "Back button pressed");
                        app->running = false;
                        break;
                    case InputKeyRight:
                        FURI_LOG_I(TAG, "Right button pressed");
                        furi_check(
                            furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
                        if(app->cursor_position < 4) {
                            app->cursor_position++;
                        }
                        furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);
                        jammer_update_view(app);
                        break;
                    case InputKeyLeft:
                        FURI_LOG_I(TAG, "Left button pressed");
                        furi_check(
                            furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
                        if(app->cursor_position > 0) {
                            app->cursor_position--;
                        }
                        furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);
                        jammer_update_view(app);
                        break;
                    case InputKeyUp:
                        FURI_LOG_I(TAG, "Up button pressed");
                        jammer_adjust_frequency(app, true);
                        jammer_update_view(app);
                        break;
                    case InputKeyDown:
                        FURI_LOG_I(TAG, "Down button pressed");
                        jammer_adjust_frequency(app, false);
                        jammer_update_view(app);
                        break;
                    default:
                        break;
                }
            }
        }

        const uint32_t current_tick = furi_get_tick();
        if((current_tick - last_timer_poll_tick) >= timer_poll_interval) {
            last_timer_poll_tick = current_tick;

            uint32_t tx_elapsed_seconds;
            if(jammer_get_tx_elapsed_seconds(app, &tx_elapsed_seconds)) {
                if(last_tx_elapsed_seconds == UINT32_MAX ||
                   tx_elapsed_seconds < last_tx_elapsed_seconds) {
                    last_tx_elapsed_seconds = tx_elapsed_seconds;
                } else if(tx_elapsed_seconds != last_tx_elapsed_seconds) {
                    last_tx_elapsed_seconds = tx_elapsed_seconds;
                    jammer_update_view(app);
                }
            } else {
                last_tx_elapsed_seconds = UINT32_MAX;
            }
        }
    }

    FURI_LOG_I(TAG, "Exiting JammerApp main loop");

    jammer_app_free(app);

    FURI_LOG_I(TAG, "JammerApp exited");

    return 0;
}

JammerApp* jammer_app_alloc(void) {
    JammerApp* app = calloc(1, sizeof(JammerApp));
    if(!app) {
        return NULL;
    }

    app->frequency = 315000000;
    app->cursor_position = 0;
    app->running = true;
    app->tx_requested = false;
    app->jamming_mode = JammerModeOok650Async;
    app->radio_device_type = JammerRadioDeviceNone;
    app->requested_radio_device_type = JammerRadioDeviceExternal;
    app->ui_state = JammerUiStateIdle;
    app->ui_error = JammerUiErrorNone;
    app->ui_screen = JammerUiScreenMain;
    app->internal_warning = JammerInternalWarningExternalNotFound;
    app->tx_started_tick = 0;

    if(!jammer_override_region(app)) {
        free(app);
        return NULL;
    }

    app->ui_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->view_port = view_port_alloc();
    app->event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    if(!app->ui_mutex || !app->view_port || !app->event_queue) {
        if(app->event_queue) furi_message_queue_free(app->event_queue);
        if(app->view_port) view_port_free(app->view_port);
        if(app->ui_mutex) furi_mutex_free(app->ui_mutex);
        jammer_restore_region(app);
        free(app);
        return NULL;
    }

    app->gui = furi_record_open(RECORD_GUI);

    view_port_draw_callback_set(app->view_port, jammer_draw_callback, app);
    view_port_input_callback_set(app->view_port, jammer_input_callback, app);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    subghz_devices_init();

    furi_hal_power_suppress_charge_enter();

    return app;
}

void jammer_app_free(JammerApp* app) {
    furi_assert(app);

#ifdef FURI_DEBUG
    FURI_LOG_D(TAG, "Enter jammer_app_free");
#endif

    app->tx_requested = false;
    jammer_stop_tx(app);

#ifdef FURI_DEBUG
    FURI_LOG_D(TAG, "Ending radio device");
#endif
    jammer_release_radio(app);
#ifdef FURI_DEBUG
    FURI_LOG_D(TAG, "Radio device ended");
#endif

#ifdef FURI_DEBUG
    FURI_LOG_D(TAG, "Calling subghz_devices_deinit");
#endif
    subghz_devices_deinit();
#ifdef FURI_DEBUG
    FURI_LOG_D(TAG, "subghz_devices_deinit completed");
#endif

    jammer_restore_region(app);

#ifdef FURI_DEBUG
    FURI_LOG_D(TAG, "Exiting power suppression mode");
#endif
    furi_hal_power_suppress_charge_exit();

#ifdef FURI_DEBUG
    FURI_LOG_D(TAG, "Removing view port from GUI");
#endif
    gui_remove_view_port(app->gui, app->view_port);
#ifdef FURI_DEBUG
    FURI_LOG_D(TAG, "gui_remove_view_port completed");
#endif

    view_port_free(app->view_port);
#ifdef FURI_DEBUG
    FURI_LOG_D(TAG, "view_port freed");
#endif

#ifdef FURI_DEBUG
    FURI_LOG_D(TAG, "Closing GUI record");
#endif
    furi_record_close(RECORD_GUI);
#ifdef FURI_DEBUG
    FURI_LOG_D(TAG, "GUI record closed");
#endif

#ifdef FURI_DEBUG
    FURI_LOG_D(TAG, "Freeing event queue");
#endif
    furi_message_queue_free(app->event_queue);
#ifdef FURI_DEBUG
    FURI_LOG_D(TAG, "Event queue freed");
#endif

    furi_mutex_free(app->ui_mutex);

    free(app);
#ifdef FURI_DEBUG
    FURI_LOG_D(TAG, "Exit jammer_app_free");
#endif
}

static JammerUiError
    jammer_init_subghz(JammerApp* app, JammerRadioDevice radio_device_type) {
#ifdef FURI_DEBUG
    FURI_LOG_D(TAG, "Enter jammer_init_subghz");
#endif
    furi_assert(!app->device);
    furi_assert(
        radio_device_type == JammerRadioDeviceExternal ||
        radio_device_type == JammerRadioDeviceInternal);

    furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
    app->requested_radio_device_type = radio_device_type;
    furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);

    RadioDeviceLoaderStatus loader_status;
    if(radio_device_type == JammerRadioDeviceExternal) {
        app->device = radio_device_loader_set_external(&loader_status);
    } else {
        app->device = radio_device_loader_set_internal(&loader_status);
    }

    if(!app->device) {
        if(loader_status == RadioDeviceLoaderStatusExternalBeginFailed) {
            FURI_LOG_E(TAG, "External CC1101 was found but initialization failed.");
            return JammerUiErrorExternalBeginFailed;
        } else if(loader_status == RadioDeviceLoaderStatusInternalNotFound) {
            FURI_LOG_E(TAG, "Internal CC1101 was not found.");
            return JammerUiErrorInternalNotFound;
        }

        FURI_LOG_E(TAG, "External CC1101 was not found.");
        return JammerUiErrorExternalNotFound;
    }

    furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
    app->radio_device_type = radio_device_type;
    furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);

    subghz_devices_reset(app->device);
    subghz_devices_idle(app->device);

    FURI_LOG_I(TAG, "Initialized device %s", app->device->name);

    furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
    const JammerMode mode = app->jamming_mode;
    furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);

    if(!jammer_load_preset(app, mode)) {
        return JammerUiErrorInvalidPreset;
    }
#ifdef FURI_DEBUG
    FURI_LOG_D(TAG, "Exit jammer_init_subghz");
#endif
    return JammerUiErrorNone;
}

static void jammer_show_internal_warning(JammerApp* app, JammerUiError reason) {
    furi_assert(
        reason == JammerUiErrorExternalNotFound ||
        reason == JammerUiErrorExternalBeginFailed);

    app->tx_requested = false;
    furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
    app->internal_warning = reason == JammerUiErrorExternalBeginFailed ?
                                JammerInternalWarningExternalBeginFailed :
                                JammerInternalWarningExternalNotFound;
    app->ui_screen = JammerUiScreenInternalWarning;
    app->ui_state = JammerUiStateIdle;
    app->ui_error = JammerUiErrorNone;
    app->tx_started_tick = 0;
    furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);
    jammer_update_view(app);
}

static void jammer_start_after_radio_init(JammerApp* app) {
    furi_assert(app->device);

    app->tx_requested = true;
    furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
    app->ui_screen = JammerUiScreenMain;
    app->ui_error = JammerUiErrorNone;
    furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);
    jammer_start_tx(app);
}

static void jammer_accept_internal(JammerApp* app) {
    furi_assert(!app->device);

    const JammerUiError init_error =
        jammer_init_subghz(app, JammerRadioDeviceInternal);
    if(init_error == JammerUiErrorNone) {
        jammer_start_after_radio_init(app);
    } else {
        app->tx_requested = false;
        jammer_set_ui_error(app, init_error);
    }
}

static void jammer_retry_external(JammerApp* app) {
    furi_assert(!app->device);

    const JammerUiError init_error =
        jammer_init_subghz(app, JammerRadioDeviceExternal);
    if(init_error == JammerUiErrorNone) {
        jammer_start_after_radio_init(app);
    } else if(
        init_error == JammerUiErrorExternalNotFound ||
        init_error == JammerUiErrorExternalBeginFailed) {
        jammer_show_internal_warning(app, init_error);
    } else {
        app->tx_requested = false;
        jammer_set_ui_error(app, init_error);
    }
}

static void jammer_release_radio(JammerApp* app) {
    if(!app->device) {
        return;
    }

    furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
    const JammerRadioDevice radio_device_type = app->radio_device_type;
    furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);

#ifdef FURI_DEBUG
    const char* device_name = app->device->name ? app->device->name : "Unknown";
    FURI_LOG_D(TAG, "Device name: %s", device_name);
#endif

    if(radio_device_type == JammerRadioDeviceExternal) {
        radio_device_loader_end_external(app->device);
    } else {
        subghz_devices_idle(app->device);
        subghz_devices_sleep(app->device);
    }

    app->device = NULL;
    furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
    app->radio_device_type = JammerRadioDeviceNone;
    furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);
}

static void jammer_draw_callback(Canvas* canvas, void* context) {
    JammerApp* app = (JammerApp*)context;
    uint32_t frequency;
    uint8_t cursor_position;
    JammerMode mode;
    JammerUiState state;
    JammerUiError error;
    JammerUiScreen screen;
    JammerInternalWarning internal_warning;
    JammerRadioDevice radio_device_type;
    uint32_t tx_started_tick;

    furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
    frequency = app->frequency;
    cursor_position = app->cursor_position;
    mode = app->jamming_mode;
    state = app->ui_state;
    error = app->ui_error;
    screen = app->ui_screen;
    internal_warning = app->internal_warning;
    radio_device_type = app->radio_device_type;
    tx_started_tick = app->tx_started_tick;
    furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);

    canvas_clear(canvas);

    if(screen == JammerUiScreenInternalWarning) {
        const char* warning_title =
            internal_warning == JammerInternalWarningExternalBeginFailed ?
                "EXT INIT FAILED" :
                "EXT NOT FOUND";

        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 1, AlignCenter, AlignTop, warning_title);
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 13, AlignCenter, AlignTop, "LONG TX MAY DAMAGE");
        canvas_draw_str_aligned(canvas, 64, 23, AlignCenter, AlignTop, "INTERNAL RADIO");
        canvas_draw_str_aligned(canvas, 64, 35, AlignCenter, AlignTop, "OK: USE INTERNAL");
        canvas_draw_str_aligned(canvas, 64, 45, AlignCenter, AlignTop, "HOLD OK: RETRY EXT");
        canvas_draw_str_aligned(canvas, 64, 55, AlignCenter, AlignTop, "BACK: EXIT");
        return;
    }

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(
        canvas,
        64,
        1,
        AlignCenter,
        AlignTop,
        jammer_radio_device_labels[radio_device_type]);
    canvas_draw_str_aligned(canvas, 126, 1, AlignRight, AlignTop, jammer_ui_states[state]);

    if(state == JammerUiStateTransmitting) {
        const uint32_t elapsed_seconds =
            (furi_get_tick() - tx_started_tick) / furi_kernel_get_tick_frequency();
        const uint32_t elapsed_hours = elapsed_seconds / 3600U;
        const uint32_t elapsed_minutes = (elapsed_seconds % 3600U) / 60U;
        const uint32_t elapsed_remaining_seconds = elapsed_seconds % 60U;
        char timer_str[20];

        if(elapsed_hours > 0) {
            snprintf(
                timer_str,
                sizeof(timer_str),
                "%lu:%02lu:%02lu",
                elapsed_hours,
                elapsed_minutes,
                elapsed_remaining_seconds);
        } else {
            snprintf(
                timer_str,
                sizeof(timer_str),
                "%02lu:%02lu",
                elapsed_minutes,
                elapsed_remaining_seconds);
        }

        canvas_draw_str_aligned(canvas, 2, 1, AlignLeft, AlignTop, timer_str);
    }

    char freq_str[20];
    snprintf(
        freq_str,
        sizeof(freq_str),
        "%3lu.%02lu",
        frequency / 1000000,
        (frequency % 1000000) / 10000);

    int total_width = strlen(freq_str) * 12;
    int start_x = (128 - total_width) / 2;
    int digit_position = 0;

    for(size_t i = 0; i < strlen(freq_str); i++) {
        bool highlight = (digit_position == cursor_position);

        if(freq_str[i] != '.') {
            canvas_set_font(canvas, highlight ? FontBigNumbers : FontPrimary);
            char temp[2] = {freq_str[i], '\0'};
            canvas_draw_str_aligned(canvas, start_x + (i * 12), 10, AlignCenter, AlignTop, temp);
            digit_position++;
        } else {
            canvas_set_font(canvas, FontPrimary);
            char temp[2] = {freq_str[i], '\0'};
            canvas_draw_str_aligned(canvas, start_x + (i * 12), 10, AlignCenter, AlignTop, temp);
        }
    }

    canvas_set_font(canvas, FontSecondary);
    if(state == JammerUiStateError) {
        canvas_draw_str_aligned(
            canvas, 64, 34, AlignCenter, AlignTop, jammer_ui_errors[error]);
        canvas_draw_str_aligned(canvas, 64, 44, AlignCenter, AlignTop, "HOLD OK: RETRY");
    } else if(state == JammerUiStateTransmitting || state == JammerUiStateStarting) {
        canvas_draw_str_aligned(canvas, 64, 44, AlignCenter, AlignTop, "HOLD OK: PAUSE");
    } else if(state == JammerUiStateStopping) {
        canvas_draw_str_aligned(canvas, 64, 44, AlignCenter, AlignTop, "STOPPING...");
    } else {
        canvas_draw_str_aligned(canvas, 64, 44, AlignCenter, AlignTop, "HOLD OK: START");
    }

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 55, AlignCenter, AlignTop, jamming_modes[mode]);
}

static void jammer_input_callback(InputEvent* input_event, void* context) {
    JammerApp* app = (JammerApp*)context;
    const bool is_short_press = input_event->type == InputTypeShort;
    const bool is_tx_toggle =
        input_event->type == InputTypeLong && input_event->key == InputKeyOk;

    if(is_short_press || is_tx_toggle) {
        furi_message_queue_put(app->event_queue, input_event, 0);
    }
}

static void jammer_adjust_frequency(JammerApp* app, bool up) {
#ifdef FURI_DEBUG
    FURI_LOG_D(TAG, "Enter jammer_adjust_frequency");
#endif
    furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
    uint32_t frequency = app->frequency;
    const uint8_t cursor_position = app->cursor_position;
    furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);

    uint32_t step;

    switch(cursor_position) {
        case 0: step = 100000000; break;
        case 1: step = 10000000; break;
        case 2: step = 1000000; break;
        case 3: step = 100000; break;
        case 4: step = 10000; break;
        default: return;
    }

    frequency = up ? frequency + step : frequency - step;

    if(frequency > SUBGHZ_FREQUENCY_MAX) {
        frequency = SUBGHZ_FREQUENCY_MIN;
    } else if(frequency < SUBGHZ_FREQUENCY_MIN) {
        frequency = SUBGHZ_FREQUENCY_MAX;
    }

    frequency = adjust_frequency_to_valid(frequency, up);
    const bool restart_tx = app->tx_requested;

    if(app->tx_thread) {
        jammer_stop_tx(app);
    }

    furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
    app->frequency = frequency;
    furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);

    FURI_LOG_I(TAG, "Frequency adjusted to %lu Hz", frequency);

    if(!app->device) {
        FURI_LOG_E(TAG, "Cannot adjust frequency: selected CC1101 is unavailable");
        jammer_set_ui_error(app, jammer_get_missing_device_error(app));
    } else if(restart_tx) {
        jammer_clear_ui_error(app);
        jammer_start_tx(app);
        FURI_LOG_I(TAG, "Restarted jammer worker with new frequency %lu Hz", frequency);
    } else {
        jammer_clear_ui_error(app);
        jammer_set_ui_state(app, JammerUiStateIdle);
    }
#ifdef FURI_DEBUG
    FURI_LOG_D(TAG, "Exit jammer_adjust_frequency");
#endif
}

static void jammer_switch_mode(JammerApp* app) {
#ifdef FURI_DEBUG
    FURI_LOG_D(TAG, "Enter jammer_switch_mode");
#endif
    const bool restart_tx = app->tx_requested;

    if(app->tx_thread) {
        jammer_stop_tx(app);
    }

    furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
    app->jamming_mode = (JammerMode)((app->jamming_mode + 1) % JammerModeCount);
    const JammerMode mode = app->jamming_mode;
    furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);

    if(!app->device) {
        FURI_LOG_E(TAG, "Cannot switch mode: selected CC1101 is unavailable");
        jammer_set_ui_error(app, jammer_get_missing_device_error(app));
        return;
    }

    subghz_devices_reset(app->device);
    subghz_devices_idle(app->device);

    if(!jammer_load_preset(app, mode)) {
        jammer_set_ui_error(app, JammerUiErrorInvalidPreset);
        return;
    }

    jammer_clear_ui_error(app);
    if(restart_tx) {
        jammer_start_tx(app);
    } else {
        jammer_set_ui_state(app, JammerUiStateIdle);
    }
#ifdef FURI_DEBUG
    FURI_LOG_D(TAG, "Exit jammer_switch_mode");
#endif
}

static bool jammer_load_preset(JammerApp* app, JammerMode mode) {
    switch(mode) {
        case JammerModeOok650Async:
            subghz_devices_load_preset(app->device, FuriHalSubGhzPresetOok650Async, NULL);
            break;
        case JammerMode2FSKDev238Async:
            subghz_devices_load_preset(app->device, FuriHalSubGhzPreset2FSKDev238Async, NULL);
            break;
        case JammerMode2FSKDev476Async:
            subghz_devices_load_preset(app->device, FuriHalSubGhzPreset2FSKDev476Async, NULL);
            break;
        case JammerModeMSK99_97KbAsync:
            subghz_devices_load_preset(app->device, FuriHalSubGhzPresetMSK99_97KbAsync, NULL);
            break;
        case JammerModeGFSK9_99KbAsync:
            subghz_devices_load_preset(app->device, FuriHalSubGhzPresetGFSK9_99KbAsync, NULL);
            break;
        case JammerModeSquareWave:
        case JammerModeWhiteNoise:
        case JammerModeBruteforce:
        case JammerModeBurst:
            subghz_devices_load_preset(app->device, FuriHalSubGhzPresetOok650Async, NULL);
            break;
        default:
            return false;
    }

    return true;
}

static void jammer_toggle_tx(JammerApp* app) {
    const JammerUiState state = jammer_get_ui_state(app);

    if(state == JammerUiStateStopping) {
        return;
    }

    if(state == JammerUiStateError) {
        if(app->tx_thread) {
            jammer_stop_tx(app);
        }

        if(!app->device) {
            const JammerRadioDevice radio_device_type =
                jammer_get_requested_radio_device(app);
            const JammerUiError init_error =
                jammer_init_subghz(app, radio_device_type);
            if(init_error != JammerUiErrorNone) {
                app->tx_requested = false;
                if(init_error == JammerUiErrorExternalNotFound ||
                   init_error == JammerUiErrorExternalBeginFailed) {
                    jammer_show_internal_warning(app, init_error);
                } else {
                    jammer_set_ui_error(app, init_error);
                }
                return;
            }
        }

        app->tx_requested = true;
        jammer_clear_ui_error(app);
        jammer_start_tx(app);
        return;
    }

    if(app->tx_requested || app->tx_thread) {
        app->tx_requested = false;
        jammer_stop_tx(app);
        jammer_clear_ui_error(app);
        return;
    }

    if(!app->device) {
        const JammerRadioDevice radio_device_type =
            jammer_get_requested_radio_device(app);
        const JammerUiError init_error =
            jammer_init_subghz(app, radio_device_type);
        if(init_error != JammerUiErrorNone) {
            if(init_error == JammerUiErrorExternalNotFound ||
               init_error == JammerUiErrorExternalBeginFailed) {
                jammer_show_internal_warning(app, init_error);
            } else {
                jammer_set_ui_error(app, init_error);
            }
            return;
        }
    }

    app->tx_requested = true;
    jammer_clear_ui_error(app);
    jammer_start_tx(app);
}

static void jammer_start_tx(JammerApp* app) {
#ifdef FURI_DEBUG
    FURI_LOG_D(TAG, "Enter jammer_start_tx");
#endif

    if(!app->device) {
        jammer_set_ui_error(app, jammer_get_missing_device_error(app));
        return;
    }

    if(app->tx_thread) {
        jammer_stop_tx(app);
    }

    jammer_set_ui_state(app, JammerUiStateStarting);
    app->tx_thread = furi_thread_alloc();
    if(!app->tx_thread) {
        FURI_LOG_E(TAG, "Failed to allocate jammer TX thread");
        jammer_set_ui_error(app, JammerUiErrorThreadAllocFailed);
        return;
    }

    furi_thread_set_name(app->tx_thread, "Jammer TX");
    furi_thread_set_stack_size(app->tx_thread, 4096);
    furi_thread_set_context(app->tx_thread, app);
    furi_thread_set_callback(app->tx_thread, jammer_tx_thread);
    furi_thread_start(app->tx_thread);
#ifdef FURI_DEBUG
    FURI_LOG_D(TAG, "Exit jammer_start_tx");
#endif
}

static void jammer_stop_tx(JammerApp* app) {
    if(!app->tx_thread) {
        jammer_set_ui_state(app, JammerUiStateIdle);
        return;
    }

    const FuriThreadState thread_state = furi_thread_get_state(app->tx_thread);
    if(thread_state != FuriThreadStateStopped && thread_state != FuriThreadStateStopping) {
        const uint32_t flags =
            furi_thread_flags_set(furi_thread_get_id(app->tx_thread), JAMMER_TX_FLAG_STOP);
        furi_check((flags & FuriFlagError) == 0);
    }

    jammer_set_ui_state(app, JammerUiStateStopping);

    furi_thread_join(app->tx_thread);
    furi_thread_free(app->tx_thread);
    app->tx_thread = NULL;
    jammer_set_ui_state(app, JammerUiStateIdle);
}

static LevelDuration jammer_async_pattern_yield(void* context) {
    JammerAsyncPattern* pattern = context;
    const uint8_t byte = pattern->data[pattern->byte_index];
    const bool level = (byte >> (7U - pattern->bit_index)) & 1U;

    pattern->bit_index++;
    if(pattern->bit_index == 8U) {
        pattern->bit_index = 0;
        pattern->byte_index++;
        if(pattern->byte_index == pattern->size) {
            pattern->byte_index = 0;
        }
    }

    return level_duration_make(level, pattern->bit_duration_us);
}

static bool jammer_tx_stop_requested(void) {
    const uint32_t flags = furi_thread_flags_get();
    return (flags & FuriFlagError) == 0 && (flags & JAMMER_TX_FLAG_STOP) != 0;
}

static bool jammer_tx_wait_for_stop(uint32_t timeout) {
    const uint32_t flags =
        furi_thread_flags_wait(JAMMER_TX_FLAG_STOP, FuriFlagWaitAny, timeout);
    return (flags & FuriFlagError) == 0 && (flags & JAMMER_TX_FLAG_STOP) != 0;
}

static JammerPacketResult
    jammer_send_packet(JammerApp* app, const uint8_t* data, uint8_t size) {
    if(jammer_tx_stop_requested()) {
        return JammerPacketResultStopped;
    }

    const GpioPin* data_gpio = subghz_devices_get_data_gpio(app->device);
    subghz_devices_idle(app->device);
    subghz_devices_write_packet(app->device, data, size);

    if(!subghz_devices_set_tx(app->device)) {
        return JammerPacketResultSetTxFailed;
    }

    uint16_t timeout = JAMMER_PACKET_TIMEOUT_TICKS;
    while(!furi_hal_gpio_read(data_gpio) && timeout > 0) {
        if(jammer_tx_stop_requested()) {
            subghz_devices_idle(app->device);
            return JammerPacketResultStopped;
        }
        furi_delay_tick(1);
        timeout--;
    }
    if(timeout == 0) {
        FURI_LOG_W(TAG, "Packet TX start timeout");
        subghz_devices_idle(app->device);
        return JammerPacketResultStartTimeout;
    }

    timeout = JAMMER_PACKET_TIMEOUT_TICKS;
    while(furi_hal_gpio_read(data_gpio) && timeout > 0) {
        if(jammer_tx_stop_requested()) {
            subghz_devices_idle(app->device);
            return JammerPacketResultStopped;
        }
        furi_delay_tick(1);
        timeout--;
    }

    subghz_devices_idle(app->device);
    if(timeout == 0) {
        FURI_LOG_W(TAG, "Packet TX completion timeout");
        return JammerPacketResultCompletionTimeout;
    }

    return JammerPacketResultSent;
}

static int32_t jammer_tx_thread(void* context) {
    JammerApp* app = context;
    uint8_t jam_data[MESSAGE_MAX_LEN];
    JammerUiError tx_error = JammerUiErrorNone;

    furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
    const JammerMode mode = app->jamming_mode;
    const uint32_t frequency = app->frequency;
    furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);

    const char* device_name = (app->device && app->device->name) ? app->device->name : "Unknown";
    FURI_LOG_I(TAG, "TX Thread started with mode %d on device %s", mode, device_name);

    switch(mode) {
        case JammerModeOok650Async:
            memset(jam_data, 0xFF, sizeof(jam_data));
            break;
        case JammerMode2FSKDev238Async:
        case JammerMode2FSKDev476Async:
            for(size_t i = 0; i < sizeof(jam_data); i++) {
                jam_data[i] = (i % 2 == 0) ? 0xAA : 0x55;
            }
            break;
        case JammerModeMSK99_97KbAsync:
        case JammerModeGFSK9_99KbAsync:
        case JammerModeWhiteNoise:
            for(size_t i = 0; i < sizeof(jam_data); i++) {
                jam_data[i] = rand() % 256;
            }
            break;
        case JammerModeSquareWave:
            for(size_t i = 0; i < sizeof(jam_data); i++) {
                jam_data[i] = (i % 2 == 0) ? 0xFF : 0x00;
            }
            break;
        case JammerModeBruteforce:
            memset(jam_data, 0xFF, sizeof(jam_data));
            break;
        case JammerModeBurst:
            for(size_t i = 0; i < sizeof(jam_data); i++) {
                jam_data[i] = (i % 10 == 0) ? 0xFF : 0x00;
            }
            break;
        default:
            tx_error = JammerUiErrorInvalidPreset;
            break;
    }

    if(tx_error == JammerUiErrorNone) {
        if(subghz_devices_check_tx(app->device, frequency) != SubGhzTxAllowed) {
            tx_error = JammerUiErrorTuneBlocked;
            FURI_LOG_E(TAG, "TX is blocked at %lu Hz", frequency);
        }
    }

    if(tx_error == JammerUiErrorNone) {
        const uint32_t actual_frequency = subghz_devices_set_frequency(app->device, frequency);
        FURI_LOG_I(TAG, "Radio tuned to %lu Hz", actual_frequency);
    }

    const bool stop_before_tx = jammer_tx_stop_requested();

    if(tx_error == JammerUiErrorNone && !stop_before_tx &&
       (mode == JammerModeOok650Async || mode == JammerModeBruteforce)) {
        const GpioPin* data_gpio = subghz_devices_get_data_gpio(app->device);
        furi_hal_gpio_write(data_gpio, true);
        furi_hal_gpio_init(
            data_gpio, GpioModeOutputPushPull, GpioPullNo, GpioSpeedVeryHigh);

        if(subghz_devices_set_tx(app->device)) {
            if(!jammer_tx_stop_requested()) {
                jammer_set_ui_state(app, JammerUiStateTransmitting);
                if(!jammer_tx_wait_for_stop(FuriWaitForever)) {
                    tx_error = JammerUiErrorTxWaitFailed;
                }
            }
            subghz_devices_idle(app->device);
        } else {
            tx_error = JammerUiErrorSetTxFailed;
        }

        furi_hal_gpio_write(data_gpio, false);
        furi_hal_gpio_init(data_gpio, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
    } else if(
        tx_error == JammerUiErrorNone && !stop_before_tx &&
        (mode == JammerMode2FSKDev238Async || mode == JammerMode2FSKDev476Async ||
         mode == JammerModeSquareWave || mode == JammerModeWhiteNoise ||
         mode == JammerModeBurst)) {
        const bool is_2fsk =
            mode == JammerMode2FSKDev238Async || mode == JammerMode2FSKDev476Async;
        JammerAsyncPattern pattern = {
            .data = jam_data,
            .size = sizeof(jam_data),
            .byte_index = 0,
            .bit_index = 0,
            .bit_duration_us =
                is_2fsk ? JAMMER_2FSK_BIT_DURATION_US : JAMMER_OOK_BIT_DURATION_US,
        };

        if(subghz_devices_start_async_tx(app->device, jammer_async_pattern_yield, &pattern)) {
            if(!jammer_tx_stop_requested()) {
                jammer_set_ui_state(app, JammerUiStateTransmitting);
                if(!jammer_tx_wait_for_stop(FuriWaitForever)) {
                    tx_error = JammerUiErrorTxWaitFailed;
                }
            }
            subghz_devices_stop_async_tx(app->device);
        } else {
            tx_error = JammerUiErrorAsyncStartFailed;
        }
    } else if(
        tx_error == JammerUiErrorNone && !stop_before_tx &&
        (mode == JammerModeMSK99_97KbAsync || mode == JammerModeGFSK9_99KbAsync)) {
        const GpioPin* data_gpio = subghz_devices_get_data_gpio(app->device);
        furi_hal_gpio_init(data_gpio, GpioModeInput, GpioPullNo, GpioSpeedLow);

        uint8_t packet[JAMMER_PACKET_MAX_LEN];
        size_t data_index = 0;
        bool first_packet = true;
        while(!jammer_tx_stop_requested()) {
            for(size_t i = 0; i < sizeof(packet); i++) {
                packet[i] = jam_data[data_index];
                data_index = (data_index + 1) % sizeof(jam_data);
            }

            const JammerPacketResult packet_result =
                jammer_send_packet(app, packet, (uint8_t)sizeof(packet));
            if(packet_result == JammerPacketResultStopped) {
                break;
            } else if(packet_result == JammerPacketResultSetTxFailed) {
                tx_error = JammerUiErrorSetTxFailed;
                break;
            } else if(packet_result == JammerPacketResultStartTimeout) {
                tx_error = JammerUiErrorPacketStartTimeout;
                break;
            } else if(packet_result == JammerPacketResultCompletionTimeout) {
                tx_error = JammerUiErrorPacketEndTimeout;
                break;
            }

            if(first_packet && !jammer_tx_stop_requested()) {
                jammer_set_ui_state(app, JammerUiStateTransmitting);
                first_packet = false;
            }
            if(jammer_tx_wait_for_stop(10)) {
                break;
            }
        }

        furi_hal_gpio_init(data_gpio, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
    }

    if(tx_error != JammerUiErrorNone) {
        jammer_set_ui_error(app, tx_error);
        FURI_LOG_E(TAG, "TX worker failed in mode %d with error %d", mode, tx_error);
    }

    FURI_LOG_I(TAG, "TX Thread exiting");
    return 0;
}

static void jammer_update_view(JammerApp* app) {
    view_port_update(app->view_port);
}

static void jammer_set_ui_state(JammerApp* app, JammerUiState state) {
    furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
    if(state == JammerUiStateTransmitting && app->ui_state != JammerUiStateTransmitting) {
        app->tx_started_tick = furi_get_tick();
    }
    app->ui_state = state;
    furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);
    jammer_update_view(app);
}

static JammerUiState jammer_get_ui_state(JammerApp* app) {
    furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
    const JammerUiState state = app->ui_state;
    furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);
    return state;
}

static JammerUiScreen jammer_get_ui_screen(JammerApp* app) {
    furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
    const JammerUiScreen screen = app->ui_screen;
    furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);
    return screen;
}

static JammerRadioDevice jammer_get_requested_radio_device(JammerApp* app) {
    furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
    const JammerRadioDevice radio_device_type = app->requested_radio_device_type;
    furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);
    return radio_device_type;
}

static JammerUiError jammer_get_missing_device_error(JammerApp* app) {
    return jammer_get_requested_radio_device(app) == JammerRadioDeviceInternal ?
               JammerUiErrorInternalNotFound :
               JammerUiErrorExternalNotFound;
}

static bool jammer_get_tx_elapsed_seconds(JammerApp* app, uint32_t* elapsed_seconds) {
    furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
    const bool is_transmitting = app->ui_state == JammerUiStateTransmitting;
    const uint32_t tx_started_tick = app->tx_started_tick;
    furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);

    if(!is_transmitting) {
        return false;
    }

    *elapsed_seconds =
        (furi_get_tick() - tx_started_tick) / furi_kernel_get_tick_frequency();
    return true;
}

static void jammer_set_ui_error(JammerApp* app, JammerUiError error) {
    furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
    app->ui_error = error;
    app->ui_state = JammerUiStateError;
    app->ui_screen = JammerUiScreenMain;
    furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);
    jammer_update_view(app);
}

static void jammer_clear_ui_error(JammerApp* app) {
    furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
    app->ui_error = JammerUiErrorNone;
    furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);
}

static size_t jammer_region_size(uint16_t bands_count) {
    return sizeof(FuriHalRegion) + (sizeof(FuriHalRegionBand) * bands_count);
}

static bool jammer_override_region(JammerApp* app) {
    const FuriHalRegion* current_region = furi_hal_region_get();
    if(current_region) {
        const size_t current_region_size = jammer_region_size(current_region->bands_count);
        app->saved_region = malloc(current_region_size);
        if(!app->saved_region) {
            FURI_LOG_E(TAG, "Failed to save current region");
            return false;
        }
        memcpy(app->saved_region, current_region, current_region_size);
    }

    const uint16_t bands_count = (uint16_t)COUNT_OF(unlocked_region_bands);
    const size_t unlocked_region_size = jammer_region_size(bands_count);
    FuriHalRegion* unlocked_region = malloc(unlocked_region_size);
    if(!unlocked_region) {
        FURI_LOG_E(TAG, "Failed to allocate unlocked region");
        free(app->saved_region);
        app->saved_region = NULL;
        return false;
    }

    memset(unlocked_region, 0, unlocked_region_size);
    memcpy(unlocked_region->country_code, "FTW", sizeof(unlocked_region->country_code));
    unlocked_region->bands_count = bands_count;
    memcpy(unlocked_region->bands, unlocked_region_bands, sizeof(unlocked_region_bands));

    furi_hal_region_set(unlocked_region);
    app->region_overridden = true;
    return true;
}

static void jammer_restore_region(JammerApp* app) {
    if(!app->region_overridden) {
        return;
    }

    if(app->saved_region) {
        furi_hal_region_set(app->saved_region);
        app->saved_region = NULL;
    } else {
        FURI_LOG_W(TAG, "Previous region was not provisioned; unlocked region remains HAL-owned");
    }

    app->region_overridden = false;
}

static uint32_t adjust_frequency_to_valid(uint32_t frequency, bool up) {
    if(is_frequency_valid(frequency)) {
        return frequency;
    } else {
        if(up) {
            for(size_t i = 0; i < NUM_FREQUENCY_BANDS; i++) {
                if(frequency < valid_frequency_bands[i].min) {
                    return valid_frequency_bands[i].min;
                }
            }
            return valid_frequency_bands[0].min;
        } else {
            for(int i = NUM_FREQUENCY_BANDS - 1; i >= 0; i--) {
                if(frequency > valid_frequency_bands[i].max) {
                    return valid_frequency_bands[i].max;
                }
            }
            return valid_frequency_bands[NUM_FREQUENCY_BANDS - 1].max;
        }
    }
}

static bool is_frequency_valid(uint32_t frequency) {
    for(size_t i = 0; i < NUM_FREQUENCY_BANDS; i++) {
        if(frequency >= valid_frequency_bands[i].min && frequency <= valid_frequency_bands[i].max) {
            return true;
        }
    }
    return false;
}
