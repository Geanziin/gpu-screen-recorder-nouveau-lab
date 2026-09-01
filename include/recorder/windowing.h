#ifndef GSR_RECORDER_WINDOWING_H
#define GSR_RECORDER_WINDOWING_H

#include <stdbool.h>
#include "../defs.h"
#include "../egl.h"

#include <X11/Xlib.h>

typedef struct {
    bool monitor_capture;
    bool gl_debug;
    bool listen_to_x11_events;
    /* The name of the monitor to capture, or NULL. Used to capture and encode on the gpu that the monitor is connected to */
    const char *monitor_to_capture;
} gsr_windowing_params;

typedef struct {
    Display *display;
    gsr_window *window;
    gsr_egl egl;
    bool egl_loaded;
    bool card_path_found;
} gsr_windowing;

/* Returns a |gsr_error| value. Connects to the display server and creates a window */
int gsr_windowing_init(gsr_windowing *self, const gsr_windowing_params *params);
/* Returns a |gsr_error| value. Loads opengl and finds the drm card to use */
int gsr_windowing_load_egl(gsr_windowing *self, const gsr_windowing_params *params);
void gsr_windowing_deinit(gsr_windowing *self);
bool gsr_windowing_is_wayland(const gsr_windowing *self);

bool monitor_capture_use_drm(const gsr_window *window, gsr_gpu_vendor vendor);

#endif /* GSR_RECORDER_WINDOWING_H */
