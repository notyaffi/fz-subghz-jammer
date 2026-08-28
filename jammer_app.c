#include "jammer_app.h"
#include <assets_icons.h>
#include <furi_hal_region.h>
#include <furi.h>
#include <gui/elements.h>
#include <gui/gui.h>
#include <storage/storage.h>
#include <subghz/devices/devices.h>
#include <furi/core/log.h>
#include <furi_hal.h>
#include <toolbox/level_duration.h>
#include <toolbox/saved_struct.h>
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
#define JAMMER_SETTINGS_PATH APP_DATA_PATH("settings.bin")
#define JAMMER_SETTINGS_MAGIC 0x4A
#define JAMMER_SETTINGS_VERSION 1
#define JAMMER_DEFAULT_FREQUENCY 315000000U
#define JAMMER_DEFAULT_MODE JammerModeOok650Async
#define JAMMER_DEFAULT_AUTO_START true
#define JAMMER_CONFIG_ANIMATION_DURATION_MS 160U
#define JAMMER_CONFIG_ANIMATION_FRAME_MS 40U
#define JAMMER_CONFIG_SAVE_INDICATOR_MS 600U
#define JAMMER_CONFIG_ITEM_SLIDE_DISTANCE 3
#define JAMMER_CONFIG_VALUE_SLIDE_DISTANCE 8

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
    uint32_t frequency;
    const char* label;
} JammerFrequencyOption;

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

typedef enum {
    JammerActionIconOk,
    JammerActionIconLeft,
    JammerActionIconUpDown,
    JammerActionIconBack,
} JammerActionIcon;

static const FrequencyBand valid_frequency_bands[] = {
    {300000000, 348000000},
    {387000000, 464000000},
    {779000000, 928000000},
};

static const JammerFrequencyOption jammer_frequency_options[] = {
    {.frequency = 315000000U, .label = "315.00"},
    {.frequency = 318000000U, .label = "318.00"},
    {.frequency = 433920000U, .label = "433.92"},
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
    "IDLE",
    "TX",
    "TX",
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
_Static_assert(sizeof(JammerSettings) == 8U, "Jammer settings layout mismatch");

static JammerUiError
    jammer_init_subghz(JammerApp* app, JammerRadioDevice radio_device_type);
static void jammer_show_internal_warning(JammerApp* app, JammerUiError reason);
static void jammer_accept_internal(JammerApp* app);
static void jammer_retry_external(JammerApp* app);
static void jammer_complete_radio_init(JammerApp* app);
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
static JammerSettings jammer_settings_defaults(void);
static bool jammer_settings_are_valid(const JammerSettings* settings);
static void jammer_load_settings(JammerApp* app);
static bool jammer_save_settings(const JammerSettings* settings);
static bool jammer_can_open_config(JammerApp* app);
static void jammer_open_config(JammerApp* app);
static void jammer_close_config(JammerApp* app);
static void jammer_show_config_save_error(JammerApp* app);
static void jammer_handle_config_input(JammerApp* app, const InputEvent* event);
static void jammer_handle_authors_input(JammerApp* app, const InputEvent* event);
static void jammer_handle_config_save_error_input(JammerApp* app, const InputEvent* event);
static void jammer_config_change_frequency(JammerApp* app, bool next);
static void jammer_config_change_mode(JammerApp* app, bool next);
static void jammer_config_toggle_auto_start(JammerApp* app, bool next);
static void jammer_config_change_item(JammerApp* app, bool next);
static void jammer_config_timer_callback(void* context);
static void jammer_config_start_timer(JammerApp* app);
static void jammer_config_stop_timer(JammerApp* app);
static size_t jammer_frequency_option_index(uint32_t frequency);
static void jammer_draw_config(
    Canvas* canvas,
    const JammerSettings* settings,
    JammerConfigItem cursor,
    const JammerConfigUi* config_ui);
static void jammer_draw_authors(Canvas* canvas);
static void jammer_draw_config_save_error(Canvas* canvas);

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
        jammer_complete_radio_init(app);
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
            const JammerUiScreen screen = jammer_get_ui_screen(app);
            if(screen == JammerUiScreenInternalWarning) {
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

            if(screen == JammerUiScreenConfig) {
                jammer_handle_config_input(app, &event);
                continue;
            } else if(screen == JammerUiScreenAuthors) {
                jammer_handle_authors_input(app, &event);
                continue;
            } else if(screen == JammerUiScreenConfigSaveError) {
                jammer_handle_config_save_error_input(app, &event);
                continue;
            }

            if(event.type == InputTypeLong && event.key == InputKeyLeft) {
                FURI_LOG_I(TAG, "Left button held");
                jammer_open_config(app);
            } else if(event.type == InputTypeLong && event.key == InputKeyOk) {
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

    jammer_load_settings(app);

    app->frequency = app->settings.default_frequency;
    app->cursor_position = 0;
    app->running = true;
    app->tx_requested = false;
    app->jamming_mode = (JammerMode)app->settings.default_mode;
    app->config_cursor = JammerConfigItemFrequency;
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
    app->config_ui_timer =
        furi_timer_alloc(jammer_config_timer_callback, FuriTimerTypePeriodic, app);
    if(!app->ui_mutex || !app->view_port || !app->event_queue || !app->config_ui_timer) {
        if(app->config_ui_timer) furi_timer_free(app->config_ui_timer);
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

    jammer_config_stop_timer(app);
    furi_timer_free(app->config_ui_timer);

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

static void jammer_complete_radio_init(JammerApp* app) {
    furi_assert(app->device);

    furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
    app->ui_screen = JammerUiScreenMain;
    app->ui_error = JammerUiErrorNone;
    const bool auto_start = app->settings.auto_start != 0U;
    furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);

    app->tx_requested = auto_start;
    if(auto_start) {
        jammer_start_tx(app);
    } else {
        jammer_set_ui_state(app, JammerUiStateIdle);
    }
}

static void jammer_accept_internal(JammerApp* app) {
    furi_assert(!app->device);

    const JammerUiError init_error =
        jammer_init_subghz(app, JammerRadioDeviceInternal);
    if(init_error == JammerUiErrorNone) {
        jammer_complete_radio_init(app);
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
        jammer_complete_radio_init(app);
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

static JammerSettings jammer_settings_defaults(void) {
    const JammerSettings settings = {
        .default_frequency = JAMMER_DEFAULT_FREQUENCY,
        .default_mode = (uint8_t)JAMMER_DEFAULT_MODE,
        .auto_start = (uint8_t)JAMMER_DEFAULT_AUTO_START,
        .reserved = {0U, 0U},
    };
    return settings;
}

static size_t jammer_frequency_option_index(uint32_t frequency) {
    for(size_t i = 0; i < COUNT_OF(jammer_frequency_options); i++) {
        if(jammer_frequency_options[i].frequency == frequency) {
            return i;
        }
    }

    return 0U;
}

static bool jammer_settings_are_valid(const JammerSettings* settings) {
    bool frequency_is_valid = false;
    for(size_t i = 0; i < COUNT_OF(jammer_frequency_options); i++) {
        if(jammer_frequency_options[i].frequency == settings->default_frequency) {
            frequency_is_valid = true;
            break;
        }
    }

    return frequency_is_valid && settings->default_mode < JammerModeCount &&
           settings->auto_start <= 1U;
}

static void jammer_load_settings(JammerApp* app) {
    JammerSettings settings = jammer_settings_defaults();
    JammerSettings stored_settings;

    if(saved_struct_load(
           JAMMER_SETTINGS_PATH,
           &stored_settings,
           sizeof(stored_settings),
           JAMMER_SETTINGS_MAGIC,
           JAMMER_SETTINGS_VERSION) &&
       jammer_settings_are_valid(&stored_settings)) {
        settings = stored_settings;
        FURI_LOG_I(TAG, "Loaded saved settings");
    } else {
        FURI_LOG_I(TAG, "Using default settings");
    }

    app->settings = settings;
}

static bool jammer_save_settings(const JammerSettings* settings) {
    return saved_struct_save(
        JAMMER_SETTINGS_PATH,
        settings,
        sizeof(*settings),
        JAMMER_SETTINGS_MAGIC,
        JAMMER_SETTINGS_VERSION);
}

static bool jammer_can_open_config(JammerApp* app) {
    furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
    const bool ui_is_idle = app->ui_screen == JammerUiScreenMain &&
                            app->ui_state == JammerUiStateIdle;
    furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);

    return ui_is_idle && !app->tx_requested && app->tx_thread == NULL;
}

static void jammer_open_config(JammerApp* app) {
    if(!jammer_can_open_config(app)) {
        return;
    }

    furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
    app->config_cursor = JammerConfigItemFrequency;
    app->config_ui.animation = JammerConfigAnimationNone;
    app->config_ui.saved_visible = false;
    app->ui_screen = JammerUiScreenConfig;
    furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);
    jammer_config_start_timer(app);
    jammer_update_view(app);
}

static void jammer_close_config(JammerApp* app) {
    jammer_config_stop_timer(app);

    furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
    app->config_ui.animation = JammerConfigAnimationNone;
    app->config_ui.saved_visible = false;
    app->ui_screen = JammerUiScreenMain;
    furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);
    jammer_update_view(app);
}

static void jammer_config_start_timer(JammerApp* app) {
    furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
    const bool timer_active = app->config_ui.timer_active;
    furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);

    if(!timer_active) {
        furi_check(
            furi_timer_start(
                app->config_ui_timer, furi_ms_to_ticks(JAMMER_CONFIG_ANIMATION_FRAME_MS)) ==
            FuriStatusOk);
        furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
        app->config_ui.timer_active = true;
        furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);
    }
}

static void jammer_config_stop_timer(JammerApp* app) {
    furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
    const bool timer_active = app->config_ui.timer_active;
    app->config_ui.timer_active = false;
    furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);

    if(timer_active) {
        furi_check(furi_timer_stop(app->config_ui_timer) == FuriStatusOk);
    }
}

static void jammer_config_timer_callback(void* context) {
    JammerApp* app = context;
    const uint32_t current_tick = furi_get_tick();
    bool redraw = false;

    if(furi_mutex_acquire(app->ui_mutex, 0) != FuriStatusOk) {
        return;
    }
    if(app->ui_screen == JammerUiScreenConfig) {
        if(app->config_ui.animation != JammerConfigAnimationNone) {
            if((current_tick - app->config_ui.animation_started_tick) >=
               furi_ms_to_ticks(JAMMER_CONFIG_ANIMATION_DURATION_MS)) {
                app->config_ui.animation = JammerConfigAnimationNone;
            }
            redraw = true;
        }

        if(app->config_ui.saved_visible &&
           (current_tick - app->config_ui.saved_tick) >=
               furi_ms_to_ticks(JAMMER_CONFIG_SAVE_INDICATOR_MS)) {
            app->config_ui.saved_visible = false;
            redraw = true;
        }
    }
    furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);

    if(redraw) {
        jammer_update_view(app);
    }
}

static void jammer_config_begin_animation_locked(
    JammerApp* app,
    JammerConfigAnimation animation,
    bool next,
    JammerConfigItem previous_item,
    const JammerSettings* previous_settings) {
    app->config_ui.animation = animation;
    app->config_ui.direction = next ? 1 : -1;
    app->config_ui.animation_started_tick = furi_get_tick();
    app->config_ui.previous_item = previous_item;
    app->config_ui.previous_settings = *previous_settings;
}

static void jammer_config_mark_saved_locked(JammerApp* app) {
    app->config_ui.saved_tick = furi_get_tick();
    app->config_ui.saved_visible = true;
}

static void jammer_show_config_save_error(JammerApp* app) {
    FURI_LOG_E(TAG, "Failed to save settings");
    furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
    app->config_ui.animation = JammerConfigAnimationNone;
    app->config_ui.saved_visible = false;
    app->ui_screen = JammerUiScreenConfigSaveError;
    furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);
    jammer_update_view(app);
}

static void jammer_config_change_frequency(JammerApp* app, bool next) {
    furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
    const JammerSettings previous_settings = app->settings;
    furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);

    JammerSettings candidate = previous_settings;

    size_t index = jammer_frequency_option_index(candidate.default_frequency);
    if(next) {
        index = (index + 1U) % COUNT_OF(jammer_frequency_options);
    } else {
        index = (index + COUNT_OF(jammer_frequency_options) - 1U) %
                COUNT_OF(jammer_frequency_options);
    }
    candidate.default_frequency = jammer_frequency_options[index].frequency;

    if(!jammer_save_settings(&candidate)) {
        jammer_show_config_save_error(app);
        return;
    }

    furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
    app->settings = candidate;
    app->frequency = candidate.default_frequency;
    jammer_config_begin_animation_locked(
        app,
        JammerConfigAnimationValue,
        next,
        JammerConfigItemFrequency,
        &previous_settings);
    jammer_config_mark_saved_locked(app);
    furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);
    FURI_LOG_I(TAG, "Default frequency saved: %lu Hz", candidate.default_frequency);
    jammer_update_view(app);
}

static void jammer_config_change_mode(JammerApp* app, bool next) {
    furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
    const JammerSettings previous_settings = app->settings;
    const JammerMode current_mode = app->jamming_mode;
    furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);

    JammerSettings candidate = previous_settings;
    if(next) {
        candidate.default_mode = (candidate.default_mode + 1U) % JammerModeCount;
    } else {
        candidate.default_mode =
            (candidate.default_mode + JammerModeCount - 1U) % JammerModeCount;
    }

    if(!jammer_save_settings(&candidate)) {
        jammer_show_config_save_error(app);
        return;
    }

    furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
    app->settings = candidate;
    app->jamming_mode = (JammerMode)candidate.default_mode;
    jammer_config_begin_animation_locked(
        app,
        JammerConfigAnimationValue,
        next,
        JammerConfigItemMode,
        &previous_settings);
    jammer_config_mark_saved_locked(app);
    furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);

    const JammerMode selected_mode = (JammerMode)candidate.default_mode;
    if(current_mode != selected_mode && app->device) {
        subghz_devices_reset(app->device);
        subghz_devices_idle(app->device);
        if(!jammer_load_preset(app, selected_mode)) {
            jammer_config_stop_timer(app);
            jammer_set_ui_error(app, JammerUiErrorInvalidPreset);
            return;
        }
    }

    FURI_LOG_I(TAG, "Default mode saved: %s", jamming_modes[selected_mode]);
    jammer_update_view(app);
}

static void jammer_config_toggle_auto_start(JammerApp* app, bool next) {
    furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
    const JammerSettings previous_settings = app->settings;
    furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);

    JammerSettings candidate = previous_settings;
    candidate.auto_start = candidate.auto_start ? 0U : 1U;

    if(!jammer_save_settings(&candidate)) {
        jammer_show_config_save_error(app);
        return;
    }

    furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
    app->settings = candidate;
    jammer_config_begin_animation_locked(
        app,
        JammerConfigAnimationValue,
        next,
        JammerConfigItemAutoStart,
        &previous_settings);
    jammer_config_mark_saved_locked(app);
    furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);
    FURI_LOG_I(TAG, "Auto Start saved: %s", candidate.auto_start ? "ON" : "OFF");
    jammer_update_view(app);
}

static void jammer_config_change_item(JammerApp* app, bool next) {
    furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
    const JammerConfigItem previous_item = app->config_cursor;
    const JammerSettings previous_settings = app->settings;
    if(next) {
        app->config_cursor =
            (JammerConfigItem)((app->config_cursor + 1U) % JammerConfigItemCount);
    } else {
        app->config_cursor = (JammerConfigItem)(
            (app->config_cursor + JammerConfigItemCount - 1U) % JammerConfigItemCount);
    }
    jammer_config_begin_animation_locked(
        app,
        JammerConfigAnimationItem,
        next,
        previous_item,
        &previous_settings);
    furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);
    jammer_update_view(app);
}

static void jammer_handle_config_input(JammerApp* app, const InputEvent* event) {
    if(event->type != InputTypeShort) {
        return;
    }

    furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
    const JammerConfigItem cursor = app->config_cursor;
    furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);

    if(event->key == InputKeyUp || event->key == InputKeyDown) {
        jammer_config_change_item(app, event->key == InputKeyDown);
    } else if(event->key == InputKeyLeft || event->key == InputKeyRight) {
        const bool next = event->key == InputKeyRight;
        if(cursor == JammerConfigItemFrequency) {
            jammer_config_change_frequency(app, next);
        } else if(cursor == JammerConfigItemMode) {
            jammer_config_change_mode(app, next);
        } else if(cursor == JammerConfigItemAutoStart) {
            jammer_config_toggle_auto_start(app, next);
        }
    } else if(event->key == InputKeyOk) {
        if(cursor == JammerConfigItemAuthors) {
            furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
            app->config_ui.animation = JammerConfigAnimationNone;
            app->config_ui.saved_visible = false;
            app->ui_screen = JammerUiScreenAuthors;
            furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);
            jammer_update_view(app);
        }
    } else if(event->key == InputKeyBack) {
        jammer_close_config(app);
    }
}

static void jammer_handle_authors_input(JammerApp* app, const InputEvent* event) {
    if(event->type == InputTypeShort &&
       (event->key == InputKeyOk || event->key == InputKeyBack)) {
        furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
        app->ui_screen = JammerUiScreenConfig;
        furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);
        jammer_update_view(app);
    }
}

static void jammer_handle_config_save_error_input(
    JammerApp* app,
    const InputEvent* event) {
    if(event->type == InputTypeShort &&
       (event->key == InputKeyOk || event->key == InputKeyBack)) {
        furi_check(furi_mutex_acquire(app->ui_mutex, FuriWaitForever) == FuriStatusOk);
        app->ui_screen = JammerUiScreenConfig;
        furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);
        jammer_update_view(app);
    }
}

static const char* jammer_config_item_label(JammerConfigItem item) {
    switch(item) {
    case JammerConfigItemFrequency:
        return "DEFAULT FREQUENCY";
    case JammerConfigItemMode:
        return "DEFAULT MODE";
    case JammerConfigItemAutoStart:
        return "AUTO START";
    case JammerConfigItemAuthors:
        return "AUTHORS";
    default:
        return "";
    }
}

static void jammer_config_item_value(
    JammerConfigItem item,
    const JammerSettings* settings,
    char* value,
    size_t value_size) {
    switch(item) {
    case JammerConfigItemFrequency: {
        const size_t frequency_index =
            jammer_frequency_option_index(settings->default_frequency);
        snprintf(
            value,
            value_size,
            "%s MHz",
            jammer_frequency_options[frequency_index].label);
        break;
    }
    case JammerConfigItemMode:
        snprintf(value, value_size, "%s", jamming_modes[settings->default_mode]);
        break;
    case JammerConfigItemAutoStart:
        snprintf(value, value_size, "%s", settings->auto_start ? "ON" : "OFF");
        break;
    case JammerConfigItemAuthors:
        snprintf(value, value_size, "OK TO OPEN");
        break;
    default:
        value[0] = '\0';
        break;
    }
}

static uint8_t jammer_config_animation_progress(const JammerConfigUi* config_ui) {
    if(config_ui->animation == JammerConfigAnimationNone) {
        return 100U;
    }

    const uint32_t duration_ticks = furi_ms_to_ticks(JAMMER_CONFIG_ANIMATION_DURATION_MS);
    const uint32_t elapsed_ticks = furi_get_tick() - config_ui->animation_started_tick;
    if(duration_ticks == 0U || elapsed_ticks >= duration_ticks) {
        return 100U;
    }

    return (uint8_t)((elapsed_ticks * 100U) / duration_ticks);
}

static void jammer_draw_status_badge(
    Canvas* canvas,
    int32_t anchor_x,
    bool align_right,
    const char* text) {
    canvas_set_font(canvas, FontSecondary);
    const int32_t width = (int32_t)canvas_string_width(canvas, text) + 6;
    const int32_t x = align_right ? anchor_x - width : anchor_x;

    canvas_set_color(canvas, ColorBlack);
    canvas_draw_rbox(canvas, x, 0, (size_t)width, 10, 2);
    canvas_set_color(canvas, ColorWhite);
    canvas_draw_str_aligned(canvas, x + (width / 2), 1, AlignCenter, AlignTop, text);
    canvas_set_color(canvas, ColorBlack);
}

static void jammer_draw_back_icon(Canvas* canvas, int32_t x, int32_t y) {
    canvas_draw_line(canvas, x, y + 3, x + 8, y + 3);
    canvas_draw_line(canvas, x, y + 3, x + 3, y);
    canvas_draw_line(canvas, x, y + 3, x + 3, y + 6);
    canvas_draw_line(canvas, x + 8, y + 3, x + 8, y);
    canvas_draw_line(canvas, x + 8, y, x + 6, y);
}

static void jammer_draw_action_icon(
    Canvas* canvas,
    JammerActionIcon icon,
    int32_t x,
    int32_t y) {
    switch(icon) {
    case JammerActionIconOk:
        canvas_draw_icon(canvas, x, y, &I_ButtonCenter_7x7);
        break;
    case JammerActionIconLeft:
        canvas_draw_icon(canvas, x + 1, y, &I_ButtonLeft_4x7);
        break;
    case JammerActionIconUpDown:
        canvas_draw_icon(canvas, x, y - 1, &I_ButtonUp_7x4);
        canvas_draw_icon(canvas, x, y + 5, &I_ButtonDown_7x4);
        break;
    case JammerActionIconBack:
        jammer_draw_back_icon(canvas, x, y);
        break;
    }
}

static void jammer_draw_action_chip(
    Canvas* canvas,
    int32_t x,
    int32_t y,
    size_t width,
    const char* label,
    JammerActionIcon icon,
    bool hold_action) {
    const size_t height = 12U;
    const int32_t icon_x = x + 5;
    const int32_t icon_y = y + 2;
    const int32_t text_left = x + 16;
    const int32_t text_center = text_left + ((int32_t)width - 16) / 2;

    canvas_set_color(canvas, ColorBlack);
    elements_slightly_rounded_box(canvas, x + 1, y + 1, width, height);
    canvas_set_color(canvas, ColorWhite);
    elements_slightly_rounded_box(canvas, x, y, width, height);
    canvas_set_color(canvas, ColorBlack);
    elements_slightly_rounded_frame(canvas, x, y, width, height);

    jammer_draw_action_icon(canvas, icon, icon_x, icon_y);
    if(hold_action) {
        canvas_draw_line(canvas, icon_x + 1, y + 10, icon_x + 6, y + 10);
    }
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, text_center, y + 2, AlignCenter, AlignTop, label);
}

static void jammer_draw_config_footer(Canvas* canvas, bool authors_item) {
    jammer_draw_action_chip(
        canvas,
        8,
        51,
        52U,
        authors_item ? "OPEN" : "ITEM",
        authors_item ? JammerActionIconOk : JammerActionIconUpDown,
        false);
    jammer_draw_action_chip(canvas, 68, 51, 52U, "EXIT", JammerActionIconBack, false);
}

static void jammer_draw_frequency(
    Canvas* canvas,
    uint32_t frequency,
    uint8_t cursor_position) {
    char frequency_text[16];
    snprintf(
        frequency_text,
        sizeof(frequency_text),
        "%03lu.%02lu",
        frequency / 1000000U,
        (frequency % 1000000U) / 10000U);

    const int32_t digit_cell_width = 12;
    const int32_t dot_cell_width = 5;
    const int32_t frequency_width = (digit_cell_width * 5) + dot_cell_width;
    canvas_set_font(canvas, FontSecondary);
    const int32_t unit_width = (int32_t)canvas_string_width(canvas, "MHz");
    const int32_t start_x = (128 - frequency_width - unit_width - 3) / 2;

    int32_t cell_x = start_x;
    uint8_t digit_position = 0U;
    for(size_t i = 0; frequency_text[i] != '\0'; i++) {
        char glyph[2] = {frequency_text[i], '\0'};
        if(frequency_text[i] == '.') {
            canvas_set_font(canvas, FontPrimary);
            canvas_draw_str_aligned(
                canvas,
                cell_x + (dot_cell_width / 2),
                15,
                AlignCenter,
                AlignTop,
                glyph);
            cell_x += dot_cell_width;
        } else {
            const bool selected = digit_position == cursor_position;
            canvas_set_font(canvas, selected ? FontBigNumbers : FontPrimary);
            canvas_draw_str_aligned(
                canvas,
                cell_x + (digit_cell_width / 2),
                selected ? 11 : 15,
                AlignCenter,
                AlignTop,
                glyph);
            digit_position++;
            cell_x += digit_cell_width;
        }
    }

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(
        canvas, start_x + frequency_width + 3, 18, AlignLeft, AlignTop, "MHz");
}

static void jammer_draw_config_card_content(
    Canvas* canvas,
    JammerConfigItem item,
    const JammerSettings* settings,
    int32_t x_offset,
    int32_t y_offset,
    bool move_label) {
    char value[32];
    jammer_config_item_value(item, settings, value, sizeof(value));

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(
        canvas,
        64,
        15 + (move_label ? y_offset : 0),
        AlignCenter,
        AlignTop,
        jammer_config_item_label(item));
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(
        canvas, 64 + x_offset, 29 + y_offset, AlignCenter, AlignTop, value);
}

static void jammer_draw_config(
    Canvas* canvas,
    const JammerSettings* settings,
    JammerConfigItem cursor,
    const JammerConfigUi* config_ui) {
    const uint8_t progress = jammer_config_animation_progress(config_ui);
    JammerConfigItem displayed_item = cursor;
    const JammerSettings* displayed_settings = settings;
    int32_t x_offset = 0;
    int32_t y_offset = 0;
    bool move_label = false;

    if(config_ui->animation == JammerConfigAnimationItem) {
        move_label = true;
        if(progress < 50U) {
            displayed_item = config_ui->previous_item;
            displayed_settings = &config_ui->previous_settings;
            y_offset = -config_ui->direction * (int32_t)progress *
                       JAMMER_CONFIG_ITEM_SLIDE_DISTANCE / 50;
        } else {
            y_offset = config_ui->direction * (int32_t)(100U - progress) *
                       JAMMER_CONFIG_ITEM_SLIDE_DISTANCE / 50;
        }
    } else if(config_ui->animation == JammerConfigAnimationValue) {
        if(progress < 50U) {
            displayed_settings = &config_ui->previous_settings;
            x_offset = -config_ui->direction * (int32_t)progress *
                       JAMMER_CONFIG_VALUE_SLIDE_DISTANCE / 50;
        } else {
            x_offset = config_ui->direction * (int32_t)(100U - progress) *
                       JAMMER_CONFIG_VALUE_SLIDE_DISTANCE / 50;
        }
    }

    char page[8];
    snprintf(
        page,
        sizeof(page),
        "%u/%u",
        (unsigned)displayed_item + 1U,
        (unsigned)JammerConfigItemCount);

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 4, 0, AlignLeft, AlignTop, "CONFIG");

    if(config_ui->saved_visible) {
        canvas_set_color(canvas, ColorBlack);
        canvas_draw_rbox(canvas, 47, 0, 34, 10, 2);
        canvas_set_color(canvas, ColorWhite);
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 1, AlignCenter, AlignTop, "SAVED");
        canvas_set_color(canvas, ColorBlack);
    }

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 124, 2, AlignRight, AlignTop, page);

    canvas_draw_rframe(canvas, 3, 12, 122, 35, 3);
    jammer_draw_config_card_content(
        canvas, displayed_item, displayed_settings, x_offset, y_offset, move_label);

    if(displayed_item != JammerConfigItemAuthors) {
        canvas_draw_icon(canvas, 9, 31, &I_ButtonLeftSmall_3x5);
        canvas_draw_icon(canvas, 116, 31, &I_ButtonRightSmall_3x5);
    }

    jammer_draw_config_footer(canvas, displayed_item == JammerConfigItemAuthors);
}

static void jammer_draw_authors(Canvas* canvas) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 0, AlignCenter, AlignTop, "AUTHORS");
    canvas_draw_rframe(canvas, 6, 13, 116, 34, 3);
    canvas_draw_str_aligned(canvas, 64, 18, AlignCenter, AlignTop, "@notyaffi");
    canvas_draw_str_aligned(canvas, 64, 33, AlignCenter, AlignTop, "@RocketGod-git");
    jammer_draw_action_chip(canvas, 36, 51, 56U, "BACK", JammerActionIconBack, false);
}

static void jammer_draw_config_save_error(Canvas* canvas) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 0, AlignCenter, AlignTop, "CONFIG");
    canvas_draw_rframe(canvas, 6, 13, 116, 36, 3);
    canvas_draw_str_aligned(canvas, 64, 19, AlignCenter, AlignTop, "SAVE FAILED");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(canvas, 64, 34, AlignCenter, AlignTop, "SETTING NOT CHANGED");
    jammer_draw_action_chip(canvas, 8, 51, 52U, "RETURN", JammerActionIconOk, false);
    jammer_draw_action_chip(canvas, 68, 51, 52U, "RETURN", JammerActionIconBack, false);
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
    JammerSettings settings;
    JammerConfigItem config_cursor;
    JammerConfigUi config_ui;

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
    settings = app->settings;
    config_cursor = app->config_cursor;
    config_ui = app->config_ui;
    furi_check(furi_mutex_release(app->ui_mutex) == FuriStatusOk);

    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);

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
        jammer_draw_action_chip(canvas, 26, 34, 76U, "RETRY", JammerActionIconOk, true);
        jammer_draw_action_chip(canvas, 4, 51, 58U, "USE INT", JammerActionIconOk, false);
        jammer_draw_action_chip(canvas, 66, 51, 58U, "EXIT", JammerActionIconBack, false);
        return;
    }

    if(screen == JammerUiScreenConfig) {
        jammer_draw_config(canvas, &settings, config_cursor, &config_ui);
        return;
    } else if(screen == JammerUiScreenAuthors) {
        jammer_draw_authors(canvas);
        return;
    } else if(screen == JammerUiScreenConfigSaveError) {
        jammer_draw_config_save_error(canvas);
        return;
    }

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

        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(canvas, 64, 1, AlignCenter, AlignTop, timer_str);
    }

    jammer_draw_status_badge(
        canvas, 1, false, jammer_radio_device_labels[radio_device_type]);
    jammer_draw_status_badge(canvas, 127, true, jammer_ui_states[state]);
    jammer_draw_frequency(canvas, frequency, cursor_position);

    if(state == JammerUiStateError) {
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str_aligned(
            canvas, 64, 34, AlignCenter, AlignTop, jammer_ui_errors[error]);
        jammer_draw_action_chip(canvas, 36, 51, 56U, "RETRY", JammerActionIconOk, true);
    } else {
        canvas_set_font(canvas, FontPrimary);
        canvas_draw_str_aligned(canvas, 64, 33, AlignCenter, AlignTop, jamming_modes[mode]);

        if(state == JammerUiStateIdle) {
            jammer_draw_action_chip(canvas, 8, 51, 52U, "START", JammerActionIconOk, true);
            jammer_draw_action_chip(
                canvas, 68, 51, 52U, "CONFIG", JammerActionIconLeft, true);
        } else if(
            state == JammerUiStateTransmitting || state == JammerUiStateStarting) {
            jammer_draw_action_chip(canvas, 36, 51, 56U, "PAUSE", JammerActionIconOk, true);
        } else {
            jammer_draw_action_chip(canvas, 36, 51, 56U, "START", JammerActionIconOk, true);
        }
    }
}

static void jammer_input_callback(InputEvent* input_event, void* context) {
    JammerApp* app = (JammerApp*)context;
    const bool is_short_press = input_event->type == InputTypeShort;
    const bool is_long_action = input_event->type == InputTypeLong &&
                                (input_event->key == InputKeyOk ||
                                 input_event->key == InputKeyLeft);

    if(is_short_press || is_long_action) {
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
