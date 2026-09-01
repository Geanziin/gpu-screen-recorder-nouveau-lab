#include "../../include/recorder/windowing.h"
#include "../../include/recorder/error.h"
#include "../../include/window/x11.h"
#include "../../include/window/wayland.h"
#include "../../include/utils.h"
#include "../../include/log.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static int x11_error_handler(Display *display, XErrorEvent *event) {
    (void)display;
    (void)event;
    return 0;
}

static int x11_io_error_handler(Display *display) {
    (void)display;
    return 0;
}

static void xwayland_check_callback(const gsr_monitor *monitor, void *userdata) {
    bool *xwayland_found = (bool*)userdata;
    if(monitor->name_len >= 8 && strncmp(monitor->name, "XWAYLAND", 8) == 0)
        *xwayland_found = true;
    else if(memmem(monitor->name, monitor->name_len, "X11", 3))
        *xwayland_found = true;
}

static bool is_xwayland(Display *display) {
    int opcode, event, error;
    if(XQueryExtension(display, "XWAYLAND", &opcode, &event, &error))
        return true;

    bool xwayland_found = false;
    for_each_active_monitor_output_x11_not_cached(display, xwayland_check_callback, &xwayland_found);
    return xwayland_found;
}

static bool is_using_prime_run(void) {
    const char *prime_render_offload = getenv("__NV_PRIME_RENDER_OFFLOAD");
    return (prime_render_offload && strcmp(prime_render_offload, "1") == 0) || getenv("DRI_PRIME");
}

/* Prime run and similar options force rendering onto another gpu, but gpu screen recorder chooses
   which gpu to capture and encode on itself, depending on which gpu the monitor to capture is connected to */
static void disable_prime_run(void) {
    if(!is_using_prime_run())
        return;

    gsr_log(GSR_LOG_LEVEL_WARNING, "prime-run and similar options are not supported. GPU Screen Recorder automatically uses the gpu that the monitor to capture is connected to. Disabling prime-run");
    unsetenv("__NV_PRIME_RENDER_OFFLOAD");
    unsetenv("__NV_PRIME_RENDER_OFFLOAD_PROVIDER");
    unsetenv("__GLX_VENDOR_LIBRARY_NAME");
    unsetenv("__VK_LAYER_NV_optimus");
    unsetenv("DRI_PRIME");
}

static gsr_window* window_create(Display *display, bool wayland) {
    if(wayland)
        return gsr_window_wayland_create();
    else
        return gsr_window_x11_create(display);
}

bool monitor_capture_use_drm(const gsr_window *window, gsr_gpu_vendor vendor) {
    return gsr_window_get_display_server(window) == GSR_DISPLAY_SERVER_WAYLAND || vendor != GSR_GPU_VENDOR_NVIDIA;
}

int gsr_windowing_init(gsr_windowing *self, const gsr_windowing_params *params) {
    memset(self, 0, sizeof(*self));
    self->card_path_found = true;

    disable_prime_run();

    bool wayland = false;
    self->display = XOpenDisplay(NULL);
    if(self->display) {
        if(params->listen_to_x11_events)
            XSelectInput(self->display, DefaultRootWindow(self->display), PropertyChangeMask);
    } else {
        wayland = true;
        gsr_log(GSR_LOG_LEVEL_WARNING, "failed to connect to the X server. Assuming wayland is running without Xwayland");
    }

    XSetErrorHandler(x11_error_handler);
    XSetIOErrorHandler(x11_io_error_handler);

    if(!wayland)
        wayland = is_xwayland(self->display);

    self->window = window_create(self->display, wayland);
    if(!self->window) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "failed to create window");
        return GSR_ERROR_GENERIC;
    }

    return GSR_ERROR_OK;
}

/* On x11 every monitor is part of one framebuffer on the gpu that the display server runs on, so the monitor gpu is only used on wayland */
static bool get_monitor_to_capture_card_path(const gsr_window *window, const char *monitor_to_capture, char *card_path) {
    card_path[0] = '\0';
    if(gsr_window_get_display_server(window) != GSR_DISPLAY_SERVER_WAYLAND || !monitor_to_capture || monitor_to_capture[0] == '\0')
        return false;

    if(!gsr_get_monitor_card_path(window, monitor_to_capture, card_path)) {
        gsr_log(GSR_LOG_LEVEL_WARNING, "failed to find the gpu that the monitor \"%s\" is connected to, using the display server gpu instead", monitor_to_capture);
        return false;
    }

    return true;
}

int gsr_windowing_load_egl(gsr_windowing *self, const gsr_windowing_params *params) {
    char monitor_card_path[GSR_DRI_CARD_PATH_MAX_SIZE];
    const bool monitor_card_path_found = get_monitor_to_capture_card_path(self->window, params->monitor_to_capture, monitor_card_path);

    if(!gsr_egl_load(&self->egl, self->window, params->monitor_capture, params->gl_debug, monitor_card_path_found ? monitor_card_path : NULL)) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "failed to load opengl");
        return GSR_ERROR_OPENGL_LOAD_FAILED;
    }
    self->egl_loaded = true;

    self->egl.card_path[0] = '\0';
    if(monitor_card_path_found && gsr_egl_uses_card(&self->egl, monitor_card_path)) {
        snprintf(self->egl.card_path, sizeof(self->egl.card_path), "%s", monitor_card_path);
        gsr_log(GSR_LOG_LEVEL_INFO, "the monitor \"%s\" is connected to %s, capturing and encoding on that gpu", params->monitor_to_capture, monitor_card_path);
    } else if(monitor_capture_use_drm(self->window, self->egl.gpu_info.vendor)) {
        // TODO: Allow specifying another card, and in other places
        if(!gsr_get_valid_card_path(&self->egl, self->egl.card_path, params->monitor_capture))
            self->card_path_found = false;
    } else {
        gsr_get_valid_card_path(&self->egl, self->egl.card_path, false);
    }

    return GSR_ERROR_OK;
}

void gsr_windowing_deinit(gsr_windowing *self) {
    if(self->egl_loaded) {
        gsr_egl_unload(&self->egl);
        self->egl_loaded = false;
    }

    if(self->window) {
        gsr_window_destroy(self->window);
        self->window = NULL;
    }

    if(self->display) {
        /* TODO: XCloseDisplay causes a crash, why? maybe some other library dlclose xlib and that also happened to unload this??? */
        //XCloseDisplay(self->display);
        self->display = NULL;
    }
}

bool gsr_windowing_is_wayland(const gsr_windowing *self) {
    return gsr_window_get_display_server(self->window) == GSR_DISPLAY_SERVER_WAYLAND;
}
