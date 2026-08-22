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

static FuriHalRegion unlockedRegion = {
    .country_code = "FTW",
    .bands_count = 3,
    .bands = {
        {.start = 299999755, .end = 348000000, .power_limit = 20, .duty_cycle = 50},
        {.start = 386999938, .end = 464000000, .power_limit = 20, .duty_cycle = 50},
        {.start = 778999847, .end = 928000000, .power_limit = 20, .duty_cycle = 50},
    },
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

_Static_assert(COUNT_OF(jamming_modes) == JammerModeCount, "Jammer mode labels mismatch");
_Static_assert(COUNT_OF(jammer_ui_states) == JammerUiStateCount, "Jammer state labels mismatch");

static void jammer_show_splash_screen(JammerApp* app);
static bool jammer_init_subghz(JammerApp* app);
static void jammer_start_tx(JammerApp* app);
static void jammer_stop_tx(JammerApp* app);
static void jammer_switch_mode(JammerApp* app);
static void jammer_update_view(JammerApp* app);
static void jammer_set_ui_state(JammerApp* app, JammerUiState state);
static void jammer_adjust_frequency(JammerApp* app, bool up);
static uint32_t adjust_frequency_to_valid(uint32_t frequency, bool up);
static bool is_frequency_valid(uint32_t frequency);
static int32_t jammer_tx_thread(void* context);
static LevelDuration jammer_async_pattern_yield(void* context);
static bool jammer_send_packet(JammerApp* app, const uint8_t* data, uint8_t size);
static void jammer_splash_screen_draw_callback(Canvas* canvas, void* context);
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

    jammer_show_splash_screen(app);

    jammer_set_ui_state(app, JammerUiStateStarting);
    if(!jammer_init_subghz(app)) {
        jammer_set_ui_state(app, JammerUiStateError);
        jammer_app_free(app);
        return -1;
    }

    jammer_start_tx(app);

    FURI_LOG_I(TAG, "Entering main loop");

    InputEvent event;
    while(app->running) {
        if(furi_message_queue_get(app->event_queue, &event, 10) == FuriStatusOk) {
            if(event.type == InputTypeShort) {
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
                        if(app->cursor_position < 4) {
                            app->cursor_position++;
                            jammer_update_view(app);
                        }
                        break;
                    case InputKeyLeft:
                        FURI_LOG_I(TAG, "Left button pressed");
                        if(app->cursor_position > 0) {
                            app->cursor_position--;
                            jammer_update_view(app);
                        }
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
    }

    FURI_LOG_I(TAG, "Exiting JammerApp main loop");

    jammer_app_free(app);

    FURI_LOG_I(TAG, "JammerApp exited");

    return 0;
}

JammerApp* jammer_app_alloc(void) {
    JammerApp* app = malloc(sizeof(JammerApp));
    if(!app) {
        return NULL;
    }

    app->view_port = view_port_alloc();
    app->event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));
    app->frequency = 315000000;
    app->cursor_position = 0;
    app->running = true;
    app->tx_running = false;
    app->jamming_mode = JammerModeOok650Async;
    app->ui_state = JammerUiStateIdle;
    app->gui = furi_record_open(RECORD_GUI);

    furi_hal_region_set(&unlockedRegion);

    view_port_draw_callback_set(app->view_port, jammer_draw_callback, app);
    view_port_input_callback_set(app->view_port, jammer_input_callback, app);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    app->tx_thread = NULL;
    app->device = NULL;

    subghz_devices_init();

    furi_hal_power_suppress_charge_enter();

    return app;
}

void jammer_app_free(JammerApp* app) {
    furi_assert(app);

#ifdef FURI_DEBUG
    FURI_LOG_D(TAG, "Enter jammer_app_free");
#endif

    jammer_stop_tx(app);

#ifdef FURI_DEBUG
    FURI_LOG_D(TAG, "Ending radio device");
#endif
    if(app->device) {
#ifdef FURI_DEBUG
        const char* device_name = app->device->name ? app->device->name : "Unknown";
        FURI_LOG_D(TAG, "Device name: %s", device_name);
#endif

        radio_device_loader_end(app->device);

        app->device = NULL;
#ifdef FURI_DEBUG
        FURI_LOG_D(TAG, "Radio device ended");
#endif
    }

#ifdef FURI_DEBUG
    FURI_LOG_D(TAG, "Calling subghz_devices_deinit");
#endif
    subghz_devices_deinit();
#ifdef FURI_DEBUG
    FURI_LOG_D(TAG, "subghz_devices_deinit completed");
#endif

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

    free(app);
#ifdef FURI_DEBUG
    FURI_LOG_D(TAG, "Exit jammer_app_free");
#endif
}

static bool jammer_init_subghz(JammerApp* app) {
#ifdef FURI_DEBUG
    FURI_LOG_D(TAG, "Enter jammer_init_subghz");
#endif
    app->device = radio_device_loader_set(NULL, SubGhzRadioDeviceTypeExternalCC1101);

    if(!app->device) {
        FURI_LOG_E(TAG, "External CC1101 is required but was not found.");
        return false;
    }

    subghz_devices_reset(app->device);
    subghz_devices_idle(app->device);

    FURI_LOG_I(TAG, "Initialized device %s", app->device->name);

    subghz_devices_load_preset(app->device, FuriHalSubGhzPresetOok650Async, NULL);
#ifdef FURI_DEBUG
    FURI_LOG_D(TAG, "Exit jammer_init_subghz");
#endif
    return true;
}

static void jammer_show_splash_screen(JammerApp* app) {
    view_port_draw_callback_set(app->view_port, jammer_splash_screen_draw_callback, app);
    view_port_update(app->view_port);
    furi_delay_ms(2000);
    view_port_draw_callback_set(app->view_port, jammer_draw_callback, app);
}

static void jammer_splash_screen_draw_callback(Canvas* canvas, void* context) {
    UNUSED(context);

    canvas_clear(canvas);

    for(int x = 0; x < 128; x += 8) {
        for(int y = 0; y < 64; y += 8) {
            canvas_draw_dot(canvas, x, y);
        }
    }

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 15, AlignCenter, AlignTop, "RF Jammer");
    canvas_draw_str_aligned(canvas, 64, 35, AlignCenter, AlignTop, "by RocketGod");
    canvas_draw_frame(canvas, 0, 0, 128, 64);
}

static void jammer_draw_callback(Canvas* canvas, void* context) {
    JammerApp* app = (JammerApp*)context;
    canvas_clear(canvas);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(
        canvas, 126, 1, AlignRight, AlignTop, jammer_ui_states[app->ui_state]);

    char freq_str[20];
    snprintf(freq_str, sizeof(freq_str), "%3lu.%02lu",
             app->frequency / 1000000,
             (app->frequency % 1000000) / 10000);

    int total_width = strlen(freq_str) * 12;
    int start_x = (128 - total_width) / 2;
    int digit_position = 0;

    for(size_t i = 0; i < strlen(freq_str); i++) {
        bool highlight = (digit_position == app->cursor_position);

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

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 55, AlignCenter, AlignTop, jamming_modes[app->jamming_mode]);
}

static void jammer_input_callback(InputEvent* input_event, void* context) {
    JammerApp* app = (JammerApp*)context;
    furi_message_queue_put(app->event_queue, input_event, FuriWaitForever);
}

static void jammer_adjust_frequency(JammerApp* app, bool up) {
#ifdef FURI_DEBUG
    FURI_LOG_D(TAG, "Enter jammer_adjust_frequency");
#endif
    uint32_t frequency = app->frequency;
    uint32_t step;

    switch(app->cursor_position) {
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
    app->frequency = frequency;

    FURI_LOG_I(TAG, "Frequency adjusted to %lu Hz", app->frequency);

    if(app->tx_thread) {
        jammer_stop_tx(app);
        if(app->device) {
            jammer_start_tx(app);
            FURI_LOG_I(TAG, "Restarted jammer worker with new frequency %lu Hz", app->frequency);
        } else {
            FURI_LOG_E(TAG, "Cannot adjust frequency: external CC1101 is unavailable");
            jammer_set_ui_state(app, JammerUiStateError);
        }
    }
#ifdef FURI_DEBUG
    FURI_LOG_D(TAG, "Exit jammer_adjust_frequency");
#endif
}

static void jammer_switch_mode(JammerApp* app) {
#ifdef FURI_DEBUG
    FURI_LOG_D(TAG, "Enter jammer_switch_mode");
#endif
    jammer_stop_tx(app);

    if(!app->device) {
        FURI_LOG_E(TAG, "Cannot switch mode: external CC1101 is unavailable");
        jammer_set_ui_state(app, JammerUiStateError);
        return;
    }

    subghz_devices_reset(app->device);
    subghz_devices_idle(app->device);

    app->jamming_mode = (JammerMode)((app->jamming_mode + 1) % JammerModeCount);

    switch(app->jamming_mode) {
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
            jammer_set_ui_state(app, JammerUiStateError);
            return;
    }

    jammer_start_tx(app);
#ifdef FURI_DEBUG
    FURI_LOG_D(TAG, "Exit jammer_switch_mode");
#endif
}

static void jammer_start_tx(JammerApp* app) {
#ifdef FURI_DEBUG
    FURI_LOG_D(TAG, "Enter jammer_start_tx");
#endif

    if(app->tx_thread) {
        jammer_stop_tx(app);
    }

    jammer_set_ui_state(app, JammerUiStateStarting);
    app->tx_thread = furi_thread_alloc();
    if(!app->tx_thread) {
        FURI_LOG_E(TAG, "Failed to allocate jammer TX thread");
        app->tx_running = false;
        jammer_set_ui_state(app, JammerUiStateError);
        return;
    }

    app->tx_running = true;
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
    const bool preserve_error = app->ui_state == JammerUiStateError;

    if(!preserve_error) {
        jammer_set_ui_state(app, JammerUiStateStopping);
    }

    app->tx_running = false;
    if(app->tx_thread) {
        furi_thread_join(app->tx_thread);
        furi_thread_free(app->tx_thread);
        app->tx_thread = NULL;
    }

    if(preserve_error || app->ui_state == JammerUiStateError) {
        jammer_set_ui_state(app, JammerUiStateError);
    } else {
        jammer_set_ui_state(app, JammerUiStateIdle);
    }
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

static bool jammer_send_packet(JammerApp* app, const uint8_t* data, uint8_t size) {
    const GpioPin* data_gpio = subghz_devices_get_data_gpio(app->device);
    subghz_devices_idle(app->device);
    subghz_devices_write_packet(app->device, data, size);

    if(!subghz_devices_set_tx(app->device)) {
        return false;
    }

    uint16_t timeout = JAMMER_PACKET_TIMEOUT_TICKS;
    while(!furi_hal_gpio_read(data_gpio) && timeout > 0) {
        furi_delay_tick(1);
        timeout--;
    }
    if(timeout == 0) {
        FURI_LOG_W(TAG, "Packet TX start timeout");
        subghz_devices_idle(app->device);
        return false;
    }

    timeout = JAMMER_PACKET_TIMEOUT_TICKS;
    while(furi_hal_gpio_read(data_gpio) && timeout > 0) {
        furi_delay_tick(1);
        timeout--;
    }

    subghz_devices_idle(app->device);
    if(timeout == 0) {
        FURI_LOG_W(TAG, "Packet TX completion timeout");
        return false;
    }

    return true;
}

static int32_t jammer_tx_thread(void* context) {
    JammerApp* app = context;
    uint8_t jam_data[MESSAGE_MAX_LEN];
    bool tx_ok = true;

    const char* device_name = (app->device && app->device->name) ? app->device->name : "Unknown";
    FURI_LOG_I(TAG, "TX Thread started with mode %d on device %s", app->jamming_mode, device_name);

    switch(app->jamming_mode) {
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
            tx_ok = false;
            break;
    }

    if(tx_ok) {
        const uint32_t actual_frequency =
            subghz_devices_set_frequency(app->device, app->frequency);
        FURI_LOG_I(TAG, "Radio tuned to %lu Hz", actual_frequency);
    }

    if(tx_ok && (app->jamming_mode == JammerModeOok650Async ||
                 app->jamming_mode == JammerModeBruteforce)) {
        const GpioPin* data_gpio = subghz_devices_get_data_gpio(app->device);
        furi_hal_gpio_write(data_gpio, true);
        furi_hal_gpio_init(
            data_gpio, GpioModeOutputPushPull, GpioPullNo, GpioSpeedVeryHigh);

        if(subghz_devices_set_tx(app->device)) {
            jammer_set_ui_state(app, JammerUiStateTransmitting);
            while(app->tx_running) {
                furi_delay_ms(10);
            }
            subghz_devices_idle(app->device);
        } else {
            tx_ok = false;
        }

        furi_hal_gpio_write(data_gpio, false);
        furi_hal_gpio_init(data_gpio, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
    } else if(
        tx_ok && (app->jamming_mode == JammerMode2FSKDev238Async ||
                  app->jamming_mode == JammerMode2FSKDev476Async ||
                  app->jamming_mode == JammerModeSquareWave ||
                  app->jamming_mode == JammerModeWhiteNoise ||
                  app->jamming_mode == JammerModeBurst)) {
        const bool is_2fsk = app->jamming_mode == JammerMode2FSKDev238Async ||
                            app->jamming_mode == JammerMode2FSKDev476Async;
        JammerAsyncPattern pattern = {
            .data = jam_data,
            .size = sizeof(jam_data),
            .byte_index = 0,
            .bit_index = 0,
            .bit_duration_us =
                is_2fsk ? JAMMER_2FSK_BIT_DURATION_US : JAMMER_OOK_BIT_DURATION_US,
        };

        if(subghz_devices_start_async_tx(app->device, jammer_async_pattern_yield, &pattern)) {
            jammer_set_ui_state(app, JammerUiStateTransmitting);
            while(app->tx_running) {
                furi_delay_ms(10);
            }
            subghz_devices_stop_async_tx(app->device);
        } else {
            tx_ok = false;
        }
    } else if(
        tx_ok && (app->jamming_mode == JammerModeMSK99_97KbAsync ||
                  app->jamming_mode == JammerModeGFSK9_99KbAsync)) {
        const GpioPin* data_gpio = subghz_devices_get_data_gpio(app->device);
        furi_hal_gpio_init(data_gpio, GpioModeInput, GpioPullNo, GpioSpeedLow);

        uint8_t packet[JAMMER_PACKET_MAX_LEN];
        size_t data_index = 0;
        bool first_packet = true;
        while(app->tx_running) {
            for(size_t i = 0; i < sizeof(packet); i++) {
                packet[i] = jam_data[data_index];
                data_index = (data_index + 1) % sizeof(jam_data);
            }

            if(!jammer_send_packet(app, packet, (uint8_t)sizeof(packet))) {
                tx_ok = false;
                break;
            }

            if(first_packet) {
                jammer_set_ui_state(app, JammerUiStateTransmitting);
                first_packet = false;
            }
            furi_delay_ms(10);
        }

        furi_hal_gpio_init(data_gpio, GpioModeAnalog, GpioPullNo, GpioSpeedLow);
    }

    if(!tx_ok) {
        app->tx_running = false;
        jammer_set_ui_state(app, JammerUiStateError);
        FURI_LOG_E(TAG, "TX worker failed in mode %d", app->jamming_mode);
    }

    FURI_LOG_I(TAG, "TX Thread exiting");
    return 0;
}

static void jammer_update_view(JammerApp* app) {
    view_port_update(app->view_port);
}

static void jammer_set_ui_state(JammerApp* app, JammerUiState state) {
    app->ui_state = state;
    jammer_update_view(app);
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
