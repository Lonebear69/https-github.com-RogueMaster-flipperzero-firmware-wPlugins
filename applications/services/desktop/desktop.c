#include <storage/storage.h>
#include <bt/bt_service/bt.h>
#include <assets_icons.h>
#include <gui/gui.h>
#include <gui/view_stack.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <furi.h>
#include <furi_hal.h>

#include "animations/animation_manager.h"
#include "desktop/scenes/desktop_scene.h"
#include "desktop/scenes/desktop_scene_i.h"
#include "desktop/views/desktop_view_locked.h"
#include "desktop/views/desktop_view_pin_input.h"
#include "desktop/views/desktop_view_pin_timeout.h"
#include "desktop_i.h"
#include "helpers/pin_lock.h"

#define TAG "Desktop"

static void desktop_auto_lock_arm(Desktop*);
static void desktop_auto_lock_inhibit(Desktop*);
static void desktop_start_auto_lock_timer(Desktop*);

static void desktop_loader_callback(const void* message, void* context) {
    furi_assert(context);
    Desktop* desktop = context;
    const LoaderEvent* event = message;

    if(event->type == LoaderEventTypeApplicationStarted) {
        view_dispatcher_send_custom_event(desktop->view_dispatcher, DesktopGlobalBeforeAppStarted);
    } else if(event->type == LoaderEventTypeApplicationStopped) {
        view_dispatcher_send_custom_event(desktop->view_dispatcher, DesktopGlobalAfterAppFinished);
    }
}

static void desktop_sdcard_icon_draw_callback(Canvas* canvas, void* context) {
    UNUSED(context);
    furi_assert(canvas);

    canvas_draw_icon(canvas, 0, 0, &I_SDcardMounted_11x8);
}

static void storage_Desktop_status_callback(const void* message, void* context) {
    furi_assert(context);
    Desktop* desktop = context;
    const StorageEvent* storage_event = message;

    if((storage_event->type == StorageEventTypeCardUnmount) ||
       (storage_event->type == StorageEventTypeCardMountError)) {
        view_port_enabled_set(desktop->sdcard_icon_viewport, false);
        view_port_enabled_set(desktop->sdcard_icon_slim_viewport, false);
    }

    if(storage_event->type == StorageEventTypeCardMount) {
        switch(desktop->settings.icon_style) {
        case ICON_STYLE_SLIM:
            view_port_enabled_set(desktop->sdcard_icon_slim_viewport, desktop->settings.sdcard);
            view_port_update(desktop->sdcard_icon_slim_viewport);
            break;
        case ICON_STYLE_STOCK:
            view_port_enabled_set(desktop->sdcard_icon_viewport, desktop->settings.sdcard);
            view_port_update(desktop->sdcard_icon_viewport);
            break;
        }
    }
}

static void desktop_bt_icon_draw_idle_callback(Canvas* canvas, void* context) {
    UNUSED(context);
    furi_assert(canvas);

    canvas_draw_icon(canvas, 0, 0, &I_Bluetooth_Idle_5x8);
}

static void desktop_bt_icon_draw_connected_callback(Canvas* canvas, void* context) {
    UNUSED(context);
    furi_assert(canvas);

    canvas_draw_icon(canvas, 0, 0, &I_Bluetooth_Connected_16x8);
}

static void desktop_bt_connection_status_update_icon(BtStatus status, void* context) {
    furi_assert(context);
    Desktop* desktop = context;

    if(status == BtStatusAdvertising) {
        switch(desktop->settings.icon_style) {
        case ICON_STYLE_SLIM:
            view_port_set_width(
                desktop->bt_icon_slim_viewport, icon_get_width(&I_Bluetooth_Idle_5x8));
            view_port_draw_callback_set(
                desktop->bt_icon_slim_viewport, desktop_bt_icon_draw_idle_callback, desktop);
            view_port_enabled_set(desktop->bt_icon_slim_viewport, desktop->settings.bt_icon);
            view_port_update(desktop->bt_icon_slim_viewport);
            break;
        case ICON_STYLE_STOCK:
            view_port_set_width(desktop->bt_icon_viewport, icon_get_width(&I_Bluetooth_Idle_5x8));
            view_port_draw_callback_set(
                desktop->bt_icon_viewport, desktop_bt_icon_draw_idle_callback, desktop);
            view_port_enabled_set(desktop->bt_icon_viewport, desktop->settings.bt_icon);
            view_port_update(desktop->bt_icon_viewport);
            break;
        }
    } else if(status == BtStatusConnected) {
        switch(desktop->settings.icon_style) {
        case ICON_STYLE_SLIM:
            view_port_set_width(
                desktop->bt_icon_slim_viewport, icon_get_width(&I_Bluetooth_Connected_16x8));
            view_port_draw_callback_set(
                desktop->bt_icon_slim_viewport, desktop_bt_icon_draw_connected_callback, desktop);
            view_port_enabled_set(desktop->bt_icon_slim_viewport, desktop->settings.bt_icon);
            view_port_update(desktop->bt_icon_slim_viewport);
            break;
        case ICON_STYLE_STOCK:
            view_port_set_width(
                desktop->bt_icon_viewport, icon_get_width(&I_Bluetooth_Connected_16x8));
            view_port_draw_callback_set(
                desktop->bt_icon_viewport, desktop_bt_icon_draw_connected_callback, desktop);
            view_port_enabled_set(desktop->bt_icon_viewport, desktop->settings.bt_icon);
            view_port_update(desktop->bt_icon_viewport);
            break;
        }
    } else {
        view_port_enabled_set(desktop->bt_icon_slim_viewport, false);
        view_port_enabled_set(desktop->bt_icon_viewport, false);
        view_port_update(desktop->bt_icon_viewport);
        view_port_update(desktop->bt_icon_slim_viewport);
    }
}

static void desktop_lock_icon_draw_callback(Canvas* canvas, void* context) {
    UNUSED(context);
    furi_assert(canvas);
    canvas_draw_icon(canvas, 0, 0, &I_Lock_7x8);
}

static void desktop_dummy_mode_icon_draw_callback(Canvas* canvas, void* context) {
    UNUSED(context);
    furi_assert(canvas);
    canvas_draw_icon(canvas, 0, 0, &I_GameMode_11x8);
}

static void desktop_topbar_icon_draw_callback(Canvas* canvas, void* context) {
    UNUSED(context);
    furi_assert(canvas);

    canvas_set_bitmap_mode(canvas, 1);
    canvas_draw_icon(canvas, 0, 0, &I_Background_128x11);
    canvas_set_bitmap_mode(canvas, 0);
}

static void desktop_stealth_mode_icon_draw_callback(Canvas* canvas, void* context) {
    UNUSED(context);
    furi_assert(canvas);
    canvas_draw_icon(canvas, 0, 0, &I_Muted_8x8);
}

static bool desktop_custom_event_callback(void* context, uint32_t event) {
    furi_assert(context);
    Desktop* desktop = (Desktop*)context;

    switch(event) {
    case DesktopGlobalBeforeAppStarted:
        animation_manager_unload_and_stall_animation(desktop->animation_manager);
        desktop_auto_lock_inhibit(desktop);
        return true;
    case DesktopGlobalAfterAppFinished:
        animation_manager_load_and_continue_animation(desktop->animation_manager);
        // TODO: Implement a message mechanism for loading settings and (optionally)
        // locking and unlocking
        DESKTOP_SETTINGS_LOAD(&desktop->settings);
        desktop_auto_lock_arm(desktop);
        return true;
    case DesktopGlobalAutoLock:
        if(!loader_is_locked(desktop->loader)) {
            if(desktop->settings.auto_lock_with_pin && desktop->settings.pin_code.length > 0) {
                desktop_pin_lock(&desktop->settings);
            }
            desktop_lock(desktop);
        }
        return true;
    }

    return scene_manager_handle_custom_event(desktop->scene_manager, event);
}

static bool desktop_back_event_callback(void* context) {
    furi_assert(context);
    Desktop* desktop = (Desktop*)context;
    return scene_manager_handle_back_event(desktop->scene_manager);
}

static void desktop_tick_event_callback(void* context) {
    furi_assert(context);
    Desktop* desktop = context;

    if(desktop->settings.bt_icon) {
        BtStatus status = bt_get_status(desktop->bt);
        desktop_bt_connection_status_update_icon(status, desktop);
    }

    scene_manager_handle_tick_event(desktop->scene_manager);
}

static void desktop_input_event_callback(const void* value, void* context) {
    furi_assert(value);
    furi_assert(context);
    const InputEvent* event = value;
    Desktop* desktop = context;
    if(event->type == InputTypePress) {
        desktop_start_auto_lock_timer(desktop);
    }
}

static void desktop_auto_lock_timer_callback(void* context) {
    furi_assert(context);
    Desktop* desktop = context;
    view_dispatcher_send_custom_event(desktop->view_dispatcher, DesktopGlobalAutoLock);
}

static void desktop_start_auto_lock_timer(Desktop* desktop) {
    furi_timer_start(
        desktop->auto_lock_timer, furi_ms_to_ticks(desktop->settings.auto_lock_delay_ms));
}

static void desktop_stop_auto_lock_timer(Desktop* desktop) {
    furi_timer_stop(desktop->auto_lock_timer);
}

static void desktop_auto_lock_arm(Desktop* desktop) {
    if(desktop->settings.auto_lock_delay_ms) {
        desktop->input_events_subscription = furi_pubsub_subscribe(
            desktop->input_events_pubsub, desktop_input_event_callback, desktop);
        desktop_start_auto_lock_timer(desktop);
    }
}

static void desktop_auto_lock_inhibit(Desktop* desktop) {
    desktop_stop_auto_lock_timer(desktop);
    if(desktop->input_events_subscription) {
        furi_pubsub_unsubscribe(desktop->input_events_pubsub, desktop->input_events_subscription);
        desktop->input_events_subscription = NULL;
    }
}

void desktop_lock(Desktop* desktop) {
    desktop_auto_lock_inhibit(desktop);
    scene_manager_set_scene_state(
        desktop->scene_manager, DesktopSceneLocked, SCENE_LOCKED_FIRST_ENTER);
    scene_manager_next_scene(desktop->scene_manager, DesktopSceneLocked);
    notification_message(desktop->notification, &sequence_display_backlight_off_delay_1000);
}

void desktop_unlock(Desktop* desktop) {
    view_port_enabled_set(desktop->lock_icon_viewport, false);
    view_port_enabled_set(desktop->lock_icon_slim_viewport, false);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_set_lockdown(gui, false);
    furi_record_close(RECORD_GUI);
    desktop_view_locked_unlock(desktop->locked_view);
    scene_manager_search_and_switch_to_previous_scene(desktop->scene_manager, DesktopSceneMain);
    desktop_auto_lock_arm(desktop);
}

void desktop_set_dummy_mode_state(Desktop* desktop, bool enabled) {
    desktop->in_transition = true;
    if(desktop->settings.dumbmode_icon) {
        switch(desktop->settings.icon_style) {
        case ICON_STYLE_SLIM:
            view_port_enabled_set(desktop->dummy_mode_icon_slim_viewport, enabled);
            break;
        case ICON_STYLE_STOCK:
            view_port_enabled_set(desktop->dummy_mode_icon_viewport, enabled);
            break;
        }
    }
    desktop_main_set_dummy_mode_state(desktop->main_view, enabled);
    animation_manager_set_dummy_mode_state(desktop->animation_manager, enabled);
    desktop->settings.dummy_mode = enabled;
    DESKTOP_SETTINGS_SAVE(&desktop->settings);
    desktop->in_transition = false;
}

void desktop_set_stealth_mode_state(Desktop* desktop, bool enabled) {
    desktop->in_transition = true;
    if(enabled) {
        furi_hal_rtc_set_flag(FuriHalRtcFlagStealthMode);
    } else {
        furi_hal_rtc_reset_flag(FuriHalRtcFlagStealthMode);
    }
    if(desktop->settings.stealth_icon) {
        switch(desktop->settings.icon_style) {
        case ICON_STYLE_SLIM:
            view_port_enabled_set(desktop->stealth_mode_icon_slim_viewport, enabled);
            break;
        case ICON_STYLE_STOCK:
            view_port_enabled_set(desktop->stealth_mode_icon_viewport, enabled);
            break;
        }
    }
    desktop->in_transition = false;
}

Desktop* desktop_alloc() {
    Desktop* desktop = malloc(sizeof(Desktop));

    desktop->animation_manager = animation_manager_alloc();
    desktop->gui = furi_record_open(RECORD_GUI);
    desktop->scene_thread = furi_thread_alloc();
    desktop->view_dispatcher = view_dispatcher_alloc();
    desktop->scene_manager = scene_manager_alloc(&desktop_scene_handlers, desktop);

    view_dispatcher_enable_queue(desktop->view_dispatcher);
    view_dispatcher_attach_to_gui(
        desktop->view_dispatcher, desktop->gui, ViewDispatcherTypeDesktop);
    view_dispatcher_set_tick_event_callback(
        desktop->view_dispatcher, desktop_tick_event_callback, 500);

    view_dispatcher_set_event_callback_context(desktop->view_dispatcher, desktop);
    view_dispatcher_set_custom_event_callback(
        desktop->view_dispatcher, desktop_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        desktop->view_dispatcher, desktop_back_event_callback);

    desktop->lock_menu = desktop_lock_menu_alloc();
    desktop->debug_view = desktop_debug_alloc();
    desktop->hw_mismatch_popup = popup_alloc();
    desktop->locked_view = desktop_view_locked_alloc();
    desktop->pin_input_view = desktop_view_pin_input_alloc();
    desktop->pin_timeout_view = desktop_view_pin_timeout_alloc();
    desktop->slideshow_view = desktop_view_slideshow_alloc();

    desktop->main_view_stack = view_stack_alloc();
    desktop->main_view = desktop_main_alloc();
    View* dolphin_view = animation_manager_get_animation_view(desktop->animation_manager);
    view_stack_add_view(desktop->main_view_stack, desktop_main_get_view(desktop->main_view));
    view_stack_add_view(desktop->main_view_stack, dolphin_view);
    view_stack_add_view(
        desktop->main_view_stack, desktop_view_locked_get_view(desktop->locked_view));

    /* locked view (as animation view) attends in 2 scenes: main & locked,
     * because it has to draw "Unlocked" label on main scene */
    desktop->locked_view_stack = view_stack_alloc();
    view_stack_add_view(desktop->locked_view_stack, dolphin_view);
    view_stack_add_view(
        desktop->locked_view_stack, desktop_view_locked_get_view(desktop->locked_view));

    view_dispatcher_add_view(
        desktop->view_dispatcher,
        DesktopViewIdMain,
        view_stack_get_view(desktop->main_view_stack));
    view_dispatcher_add_view(
        desktop->view_dispatcher,
        DesktopViewIdLocked,
        view_stack_get_view(desktop->locked_view_stack));
    view_dispatcher_add_view(
        desktop->view_dispatcher,
        DesktopViewIdLockMenu,
        desktop_lock_menu_get_view(desktop->lock_menu));
    view_dispatcher_add_view(
        desktop->view_dispatcher, DesktopViewIdDebug, desktop_debug_get_view(desktop->debug_view));
    view_dispatcher_add_view(
        desktop->view_dispatcher,
        DesktopViewIdHwMismatch,
        popup_get_view(desktop->hw_mismatch_popup));
    view_dispatcher_add_view(
        desktop->view_dispatcher,
        DesktopViewIdPinTimeout,
        desktop_view_pin_timeout_get_view(desktop->pin_timeout_view));
    view_dispatcher_add_view(
        desktop->view_dispatcher,
        DesktopViewIdPinInput,
        desktop_view_pin_input_get_view(desktop->pin_input_view));
    view_dispatcher_add_view(
        desktop->view_dispatcher,
        DesktopViewIdSlideshow,
        desktop_view_slideshow_get_view(desktop->slideshow_view));

    // Lock icon
    desktop->lock_icon_viewport = view_port_alloc();
    view_port_set_width(desktop->lock_icon_viewport, icon_get_width(&I_Lock_7x8));
    view_port_draw_callback_set(
        desktop->lock_icon_viewport, desktop_lock_icon_draw_callback, desktop);
    view_port_enabled_set(desktop->lock_icon_viewport, false);
    gui_add_view_port(desktop->gui, desktop->lock_icon_viewport, GuiLayerStatusBarLeft);

    // Lock icon - Slim
    desktop->lock_icon_slim_viewport = view_port_alloc();
    view_port_set_width(desktop->lock_icon_slim_viewport, icon_get_width(&I_Lock_7x8));
    view_port_draw_callback_set(
        desktop->lock_icon_slim_viewport, desktop_lock_icon_draw_callback, desktop);
    view_port_enabled_set(desktop->lock_icon_slim_viewport, false);
    gui_add_view_port(desktop->gui, desktop->lock_icon_slim_viewport, GuiLayerStatusBarLeftSlim);

    // Dummy mode icon
    desktop->dummy_mode_icon_viewport = view_port_alloc();
    view_port_set_width(desktop->dummy_mode_icon_viewport, icon_get_width(&I_GameMode_11x8));
    view_port_draw_callback_set(
        desktop->dummy_mode_icon_viewport, desktop_dummy_mode_icon_draw_callback, desktop);
    view_port_enabled_set(desktop->dummy_mode_icon_viewport, false);
    gui_add_view_port(desktop->gui, desktop->dummy_mode_icon_viewport, GuiLayerStatusBarLeft);

    // Dummy mode icon - Slim
    desktop->dummy_mode_icon_slim_viewport = view_port_alloc();
    view_port_set_width(desktop->dummy_mode_icon_slim_viewport, icon_get_width(&I_GameMode_11x8));
    view_port_draw_callback_set(
        desktop->dummy_mode_icon_slim_viewport, desktop_dummy_mode_icon_draw_callback, desktop);
    view_port_enabled_set(desktop->dummy_mode_icon_slim_viewport, false);
    gui_add_view_port(
        desktop->gui, desktop->dummy_mode_icon_slim_viewport, GuiLayerStatusBarLeftSlim);

    // SD card icon hack
    desktop->sdcard_icon_viewport = view_port_alloc();
    view_port_set_width(desktop->sdcard_icon_viewport, icon_get_width(&I_SDcardMounted_11x8));
    view_port_draw_callback_set(
        desktop->sdcard_icon_viewport, desktop_sdcard_icon_draw_callback, desktop);
    view_port_enabled_set(desktop->sdcard_icon_viewport, false);
    gui_add_view_port(desktop->gui, desktop->sdcard_icon_viewport, GuiLayerStatusBarLeft);

    // SD card icon hack - Slim
    desktop->sdcard_icon_slim_viewport = view_port_alloc();
    view_port_set_width(desktop->sdcard_icon_slim_viewport, icon_get_width(&I_SDcardMounted_11x8));
    view_port_draw_callback_set(
        desktop->sdcard_icon_slim_viewport, desktop_sdcard_icon_draw_callback, desktop);
    view_port_enabled_set(desktop->sdcard_icon_slim_viewport, false);
    gui_add_view_port(desktop->gui, desktop->sdcard_icon_slim_viewport, GuiLayerStatusBarLeftSlim);

    // BT icon hack
    desktop->bt_icon_viewport = view_port_alloc();
    view_port_set_width(desktop->bt_icon_viewport, icon_get_width(&I_Bluetooth_Idle_5x8));
    view_port_draw_callback_set(
        desktop->bt_icon_viewport, desktop_bt_icon_draw_idle_callback, desktop);
    view_port_enabled_set(desktop->bt_icon_viewport, false);
    gui_add_view_port(desktop->gui, desktop->bt_icon_viewport, GuiLayerStatusBarLeft);

    // BT icon hack - Slim
    desktop->bt_icon_slim_viewport = view_port_alloc();
    view_port_set_width(desktop->bt_icon_slim_viewport, icon_get_width(&I_Bluetooth_Idle_5x8));
    view_port_draw_callback_set(
        desktop->bt_icon_slim_viewport, desktop_bt_icon_draw_idle_callback, desktop);
    view_port_enabled_set(desktop->bt_icon_slim_viewport, false);
    gui_add_view_port(desktop->gui, desktop->bt_icon_slim_viewport, GuiLayerStatusBarLeftSlim);

    // Stealth mode icon
    desktop->stealth_mode_icon_viewport = view_port_alloc();
    view_port_set_width(desktop->stealth_mode_icon_viewport, icon_get_width(&I_Muted_8x8));
    view_port_draw_callback_set(
        desktop->stealth_mode_icon_viewport, desktop_stealth_mode_icon_draw_callback, desktop);
    view_port_enabled_set(desktop->stealth_mode_icon_viewport, false);
    gui_add_view_port(desktop->gui, desktop->stealth_mode_icon_viewport, GuiLayerStatusBarLeft);

    // Stealth mode Slim icon
    desktop->stealth_mode_icon_slim_viewport = view_port_alloc();
    view_port_set_width(desktop->stealth_mode_icon_slim_viewport, icon_get_width(&I_Muted_8x8));
    view_port_draw_callback_set(
        desktop->stealth_mode_icon_slim_viewport,
        desktop_stealth_mode_icon_draw_callback,
        desktop);
    view_port_enabled_set(desktop->stealth_mode_icon_slim_viewport, false);
    gui_add_view_port(
        desktop->gui, desktop->stealth_mode_icon_slim_viewport, GuiLayerStatusBarLeftSlim);

    // Top bar icon
    desktop->topbar_icon_viewport = view_port_alloc();
    view_port_set_width(desktop->topbar_icon_viewport, icon_get_width(&I_Background_128x11));
    view_port_draw_callback_set(
        desktop->topbar_icon_viewport, desktop_topbar_icon_draw_callback, desktop);
    view_port_enabled_set(desktop->topbar_icon_viewport, false);
    gui_add_view_port(desktop->gui, desktop->topbar_icon_viewport, GuiLayerStatusBarTop);

    // Special case: autostart application is already running
    desktop->loader = furi_record_open(RECORD_LOADER);
    if(loader_is_locked(desktop->loader) &&
       animation_manager_is_animation_loaded(desktop->animation_manager)) {
        animation_manager_unload_and_stall_animation(desktop->animation_manager);
    }

    desktop->notification = furi_record_open(RECORD_NOTIFICATION);
    desktop->app_start_stop_subscription = furi_pubsub_subscribe(
        loader_get_pubsub(desktop->loader), desktop_loader_callback, desktop);

    desktop->input_events_pubsub = furi_record_open(RECORD_INPUT_EVENTS);
    desktop->input_events_subscription = NULL;

    desktop->auto_lock_timer =
        furi_timer_alloc(desktop_auto_lock_timer_callback, FuriTimerTypeOnce, desktop);

    Storage* storage = furi_record_open(RECORD_STORAGE);
    desktop->storage_sub = furi_pubsub_subscribe(
        storage_get_pubsub(storage), storage_Desktop_status_callback, desktop);
    furi_record_close(RECORD_STORAGE);

    desktop->bt = furi_record_open(RECORD_BT);

    return desktop;
}

void desktop_free(Desktop* desktop) {
    furi_assert(desktop);

    furi_pubsub_unsubscribe(
        loader_get_pubsub(desktop->loader), desktop->app_start_stop_subscription);

    if(desktop->input_events_subscription) {
        furi_pubsub_unsubscribe(desktop->input_events_pubsub, desktop->input_events_subscription);
        desktop->input_events_subscription = NULL;
    }

    Storage* storage = furi_record_open(RECORD_STORAGE);
    furi_pubsub_unsubscribe(storage_get_pubsub(storage), desktop->storage_sub);
    furi_record_close(RECORD_STORAGE);
    desktop->storage_sub = NULL;

    furi_record_close(RECORD_BT);
    desktop->bt = NULL;

    desktop->loader = NULL;
    desktop->input_events_pubsub = NULL;
    furi_record_close(RECORD_LOADER);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_INPUT_EVENTS);

    //free the viewports
    free(desktop->lock_icon_viewport);
    free(desktop->dummy_mode_icon_viewport);
    free(desktop->topbar_icon_viewport);
    free(desktop->sdcard_icon_viewport);
    free(desktop->bt_icon_viewport);
    free(desktop->stealth_mode_icon_viewport);

    free(desktop->lock_icon_slim_viewport);
    free(desktop->dummy_mode_icon_slim_viewport);
    free(desktop->topbar_icon_slim_viewport);
    free(desktop->sdcard_icon_slim_viewport);
    free(desktop->bt_icon_slim_viewport);
    free(desktop->stealth_mode_icon_slim_viewport);

    view_dispatcher_remove_view(desktop->view_dispatcher, DesktopViewIdMain);
    view_dispatcher_remove_view(desktop->view_dispatcher, DesktopViewIdLockMenu);
    view_dispatcher_remove_view(desktop->view_dispatcher, DesktopViewIdLocked);
    view_dispatcher_remove_view(desktop->view_dispatcher, DesktopViewIdDebug);
    view_dispatcher_remove_view(desktop->view_dispatcher, DesktopViewIdHwMismatch);
    view_dispatcher_remove_view(desktop->view_dispatcher, DesktopViewIdPinInput);
    view_dispatcher_remove_view(desktop->view_dispatcher, DesktopViewIdPinTimeout);

    view_dispatcher_free(desktop->view_dispatcher);
    scene_manager_free(desktop->scene_manager);

    animation_manager_free(desktop->animation_manager);
    view_stack_free(desktop->main_view_stack);
    desktop_main_free(desktop->main_view);
    view_stack_free(desktop->locked_view_stack);
    desktop_view_locked_free(desktop->locked_view);
    desktop_lock_menu_free(desktop->lock_menu);
    desktop_view_locked_free(desktop->locked_view);
    desktop_debug_free(desktop->debug_view);
    popup_free(desktop->hw_mismatch_popup);
    desktop_view_pin_timeout_free(desktop->pin_timeout_view);

    furi_record_close(RECORD_GUI);
    desktop->gui = NULL;

    furi_thread_free(desktop->scene_thread);

    furi_record_close("menu");

    furi_timer_free(desktop->auto_lock_timer);

    free(desktop);
}

static bool desktop_check_file_flag(const char* flag_path) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    bool exists = storage_common_stat(storage, flag_path, NULL) == FSE_OK;
    furi_record_close(RECORD_STORAGE);

    return exists;
}

int32_t desktop_srv(void* p) {
    UNUSED(p);

    if(furi_hal_rtc_get_boot_mode() != FuriHalRtcBootModeNormal) {
        FURI_LOG_W("Desktop", "Desktop load skipped. Device is in special startup mode.");
    } else {
        if(furi_hal_rtc_is_flag_set(FuriHalRtcFlagResetPin)) {
            Storage* storage = furi_record_open(RECORD_STORAGE);
            storage_common_remove(storage, DESKTOP_SETTINGS_PATH);
            storage_common_remove(storage, DESKTOP_SETTINGS_OLD_PATH);
            furi_record_close(RECORD_STORAGE);
            furi_hal_rtc_reset_flag(FuriHalRtcFlagResetPin);
        }

        Desktop* desktop = desktop_alloc();

        bool loaded = DESKTOP_SETTINGS_LOAD(&desktop->settings);
        if(!loaded) {
            memset(&desktop->settings, 0, sizeof(desktop->settings));
            desktop->settings.displayBatteryPercentage = DISPLAY_BATTERY_BAR_PERCENT;
            desktop->settings.icon_style = ICON_STYLE_SLIM;
            desktop->settings.lock_icon = true;
            desktop->settings.bt_icon = true;
            desktop->settings.rpc_icon = true;
            desktop->settings.sdcard = true;
            desktop->settings.stealth_icon = true;
            desktop->settings.top_bar = false;
            desktop->settings.dummy_mode = false;
            desktop->settings.dumbmode_icon = true;
            DESKTOP_SETTINGS_SAVE(&desktop->settings);
        }

        view_port_enabled_set(desktop->topbar_icon_viewport, desktop->settings.top_bar);

        switch(desktop->settings.icon_style) {
        case ICON_STYLE_SLIM:
            view_port_enabled_set(desktop->sdcard_icon_slim_viewport, desktop->settings.sdcard);
            if(desktop->settings.dumbmode_icon) {
                view_port_enabled_set(
                    desktop->dummy_mode_icon_slim_viewport, desktop->settings.dummy_mode);
            }
            if(furi_hal_rtc_is_flag_set(FuriHalRtcFlagStealthMode)) {
                view_port_enabled_set(
                    desktop->stealth_mode_icon_slim_viewport, desktop->settings.stealth_icon);
            }
            break;
        case ICON_STYLE_STOCK:
            view_port_enabled_set(desktop->sdcard_icon_viewport, desktop->settings.sdcard);
            if(desktop->settings.dumbmode_icon) {
                view_port_enabled_set(
                    desktop->dummy_mode_icon_viewport, desktop->settings.dummy_mode);
            }
            if(furi_hal_rtc_is_flag_set(FuriHalRtcFlagStealthMode)) {
                view_port_enabled_set(
                    desktop->stealth_mode_icon_viewport, desktop->settings.stealth_icon);
            }
            break;
        }

        desktop_main_set_dummy_mode_state(desktop->main_view, desktop->settings.dummy_mode);
        animation_manager_set_dummy_mode_state(
            desktop->animation_manager, desktop->settings.dummy_mode);

        scene_manager_next_scene(desktop->scene_manager, DesktopSceneMain);

        desktop_pin_lock_init(&desktop->settings);

        if(!desktop_pin_lock_is_locked()) {
            if(!loader_is_locked(desktop->loader)) {
                desktop_auto_lock_arm(desktop);
            }
        } else {
            desktop_lock(desktop);
        }

        if(desktop_check_file_flag(SLIDESHOW_FS_PATH)) {
            scene_manager_next_scene(desktop->scene_manager, DesktopSceneSlideshow);
        }

        if(!furi_hal_version_do_i_belong_here()) {
            scene_manager_next_scene(desktop->scene_manager, DesktopSceneHwMismatch);
        }

        if(furi_hal_rtc_get_fault_data()) {
            scene_manager_next_scene(desktop->scene_manager, DesktopSceneFault);
        }

        view_dispatcher_run(desktop->view_dispatcher);
        desktop_free(desktop);
    }

    return 0;
}
