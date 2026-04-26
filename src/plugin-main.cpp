#include <obs-module.h>
#include <obs-frontend-api.h>
#include <QMainWindow>
#include "StudioPlayerDock.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-studio-player", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
    return "Studio Player - local video playback, queue, and embed controls for OBS scenes and recordings";
}

static StudioPlayerDock *g_dock = nullptr;
static constexpr const char *DOCK_ID = "obs-studio-player";

static void on_frontend_event(enum obs_frontend_event event, void *)
{
    // Wait until OBS has fully loaded before adding the dock — avoids crashes
    // if the Qt main window isn't ready yet.
    if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING)
        return;

    auto *mainWindow = static_cast<QMainWindow *>(obs_frontend_get_main_window());
    g_dock = new StudioPlayerDock(mainWindow);
    obs_frontend_add_custom_qdock(DOCK_ID, g_dock);
}

bool obs_module_load(void)
{
    obs_frontend_add_event_callback(on_frontend_event, nullptr);
    return true;
}

void obs_module_unload(void)
{
    // g_dock is parented to the OBS main window — Qt owns its lifetime.
    g_dock = nullptr;
}
