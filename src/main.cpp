extern "C" {
#include "../include/ffmpeg_utils.h"
#include "../include/recorder/audio_codec.h"
#include "../include/recorder/video_codec.h"
#include "../include/recorder/codec_select.h"
#include "../include/recorder/windowing.h"
#include "../include/recorder/capture_source.h"
#include "../include/recorder/capture_setup.h"
#include "../include/recorder/audio_input.h"
#include "../include/recorder/audio_capture.h"
#include "../include/recorder/muxer.h"
#include "../include/recorder/replay_save.h"
#include "../include/recorder/error.h"
#include "../include/capture/nvfbc.h"
#include "../include/capture/xcomposite.h"
#include "../include/capture/ximage.h"
#include "../include/capture/kms.h"
#include "../include/capture/v4l2.h"
#ifdef GSR_PORTAL
#include "../include/capture/portal.h"
#include "../include/dbus.h"
#endif
#ifdef GSR_APP_AUDIO
#include "../include/pipewire_audio.h"
#endif
#include "../include/encoder/encoder.h"
#include "../include/encoder/video/nvenc.h"
#include "../include/encoder/video/vaapi.h"
#include "../include/encoder/video/vulkan.h"
#include "../include/encoder/video/software.h"
#include "../include/codec_query/nvenc.h"
#include "../include/codec_query/vaapi.h"
#include "../include/codec_query/vulkan.h"
#include "../include/window/x11.h"
#include "../include/window/wayland.h"
#include "../include/egl.h"
#include "../include/utils.h"
#include "../include/damage.h"
#include "../include/color_conversion.h"
#include "../include/image_writer.h"
#include "../include/args_parser.h"
#include "../include/plugins.h"
#include "../kms/client/kms_client.h"
}

#include "../include/log.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <optional>
#include <thread>
#include <mutex>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>
#include <inttypes.h>
#include <libgen.h>
#include <malloc.h>

#include "../include/sound.h"

extern "C" {
#include <libavutil/pixfmt.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
}

#include <future>

#ifndef GSR_VERSION
#define GSR_VERSION "unknown"
#endif

// TODO: If options are not supported then they are returned (allocated) in the options. This should be free'd.

// TODO: Remove LIBAVUTIL_VERSION_MAJOR checks in the future when ubuntu, pop os LTS etc update ffmpeg to >= 5.0

static const int VIDEO_STREAM_INDEX = 0;

/* AV1/VP9 use the 0-255 qindex quantizer scale and VP8 uses 0-127. Unlike h264 qp the quantizer step size
   is not exponential in qindex, so the mapping to an equivalent h264 qp is not linear in the quantizer range.
   qindex = h264 qp * 4 matches the qindex = crf * 4 scale used by libvpx/libaom, where crf is roughly equivalent to h264 qp. */
static sig_atomic_t running = 1;
static sig_atomic_t toggle_pause = 0;
static sig_atomic_t toggle_replay_recording = 0;
static sig_atomic_t save_replay_seconds = 0;

static void stop_handler(int) {
    running = 0;
}

static void toggle_pause_handler(int) {
    toggle_pause = 1;
}

static void toggle_replay_recording_handler(int) {
    toggle_replay_recording = 1;
}

static void save_replay_handler(int) {
    save_replay_seconds = GSR_SAVE_REPLAY_SECONDS_FULL;
}

static void save_replay_10_seconds_handler(int) {
    save_replay_seconds = 10;
}

static void save_replay_30_seconds_handler(int) {
    save_replay_seconds = 30;
}

static void save_replay_1_minute_handler(int) {
    save_replay_seconds = 60;
}

static void save_replay_5_minutes_handler(int) {
    save_replay_seconds = 60*5;
}

static void save_replay_10_minutes_handler(int) {
    save_replay_seconds = 60*10;
}

static void save_replay_30_minutes_handler(int) {
    save_replay_seconds = 60*30;
}

static void run_recording_saved_script_async(const char *script_file, const char *video_file, const char *type) {
    char script_file_full[PATH_MAX];
    script_file_full[0] = '\0';
    if(!realpath(script_file, script_file_full)) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "script file not found: %s", script_file);
        return;
    }

    const char *args[7];
    const bool inside_flatpak = getenv("FLATPAK_ID") != NULL;

    if(inside_flatpak) {
        args[0] = "flatpak-spawn";
        args[1] = "--host";
        args[2] = "--";
        args[3] = script_file_full;
        args[4] = video_file;
        args[5] = type;
        args[6] = NULL;
    } else {
        args[0] = script_file_full;
        args[1] = video_file;
        args[2] = type;
        args[3] = NULL;
    }

    pid_t pid = fork();
    if(pid == -1) {
        perror(script_file_full);
        return;
    } else if(pid == 0) { // child
        setsid();
        signal(SIGHUP, SIG_IGN);

        pid_t second_child = fork();
        if(second_child == 0) { // child
            execvp(args[0], (char* const*)args);
            perror(script_file_full);
            _exit(127);
        } else if(second_child != -1) { // parent
            _exit(0);
        }
    } else { // parent
        waitpid(pid, NULL, 0);
    }
}

// TODO: Cleanup
static void list_system_info(bool wayland) {
    printf("display_server|%s\n", wayland ? "wayland" : "x11");
    bool supports_app_audio = false;
#ifdef GSR_APP_AUDIO
    supports_app_audio = pulseaudio_server_is_pipewire();
    if(supports_app_audio) {
        gsr_pipewire_audio audio;
        if(gsr_pipewire_audio_init(&audio))
            gsr_pipewire_audio_deinit(&audio);
        else
            supports_app_audio = false;
    }
#endif
    printf("supports_app_audio|%s\n", supports_app_audio ? "yes" : "no");
}

static void list_gpu_info(gsr_egl *egl) {
    switch(egl->gpu_info.vendor) {
        case GSR_GPU_VENDOR_AMD:
            printf("vendor|amd\n");
            break;
        case GSR_GPU_VENDOR_INTEL:
            printf("vendor|intel\n");
            break;
        case GSR_GPU_VENDOR_NVIDIA:
            printf("vendor|nvidia\n");
            break;
        case GSR_GPU_VENDOR_BROADCOM:
            printf("vendor|broadcom\n");
            break;
    }
    printf("card_path|%s\n", egl->card_path);
}

static void list_supported_video_codecs(gsr_egl *egl, bool wayland) {
    // Dont clean it up on purpose to increase shutdown speed
    gsr_supported_video_codecs supported_video_codecs;
    get_supported_video_codecs(egl, GSR_VIDEO_CODEC_H264, false, false, &supported_video_codecs);

    gsr_supported_video_codecs supported_video_codecs_vulkan;
    get_supported_video_codecs(egl, GSR_VIDEO_CODEC_H264_VULKAN, false, false, &supported_video_codecs_vulkan);

    set_supported_video_codecs_ffmpeg(&supported_video_codecs, &supported_video_codecs_vulkan, egl->gpu_info.vendor);

    if(supported_video_codecs.h264.supported)
        puts("h264");
    if(avcodec_find_encoder_by_name("libx264"))
        puts("h264_software");
    if(supported_video_codecs.hevc.supported)
        puts("hevc");
    if(supported_video_codecs.hevc_hdr.supported && wayland)
        puts("hevc_hdr");
    if(supported_video_codecs.hevc_10bit.supported)
        puts("hevc_10bit");
    if(supported_video_codecs.av1.supported)
        puts("av1");
    if(supported_video_codecs.av1_hdr.supported && wayland)
        puts("av1_hdr");
    if(supported_video_codecs.av1_10bit.supported)
        puts("av1_10bit");
    if(supported_video_codecs.vp8.supported)
        puts("vp8");
    if(supported_video_codecs.vp9.supported)
        puts("vp9");
    if(supported_video_codecs_vulkan.h264.supported)
       puts("h264_vulkan");
    if(supported_video_codecs_vulkan.hevc.supported)
       puts("hevc_vulkan");
    if(supported_video_codecs_vulkan.hevc_hdr.supported && wayland)
        puts("hevc_hdr_vulkan");
    if(supported_video_codecs_vulkan.hevc_10bit.supported)
        puts("hevc_10bit_vulkan");
    if(supported_video_codecs_vulkan.av1.supported)
        puts("av1_vulkan");
    if(supported_video_codecs_vulkan.av1_hdr.supported && wayland)
        puts("av1_hdr_vulkan");
    if(supported_video_codecs_vulkan.av1_10bit.supported)
        puts("av1_10bit_vulkan");
}

typedef struct {
    const gsr_window *window;
    int num_monitors;
} capture_options_callback;

static void output_monitor_info(const gsr_monitor *monitor, void *userdata) {
    capture_options_callback *options = (capture_options_callback*)userdata;
    if(gsr_window_get_display_server(options->window) == GSR_DISPLAY_SERVER_WAYLAND) {
        vec2i monitor_size = monitor->size;
        gsr_monitor_rotation monitor_rotation = GSR_MONITOR_ROT_0;
        vec2i monitor_position = {0, 0};
        drm_monitor_get_display_server_data(options->window, monitor, &monitor_rotation, &monitor_position);
        if(monitor_rotation == GSR_MONITOR_ROT_90 || monitor_rotation == GSR_MONITOR_ROT_270)
            std::swap(monitor_size.x, monitor_size.y);
        printf("%.*s|%dx%d\n", monitor->name_len, monitor->name, monitor_size.x, monitor_size.y);
    } else {
        printf("%.*s|%dx%d\n", monitor->name_len, monitor->name, monitor->size.x, monitor->size.y);
    }
    ++options->num_monitors;
}

static void camera_query_callback(const char *path, const gsr_capture_v4l2_supported_setup *setup, void *userdata) {
    (void)userdata;
    printf("%s|%ux%u@%uhz|%s\n", path, setup->resolution.width, setup->resolution.height, gsr_capture_v4l2_framerate_to_number(setup->framerate), gsr_capture_v4l2_pixfmt_to_string(setup->pixfmt));
}

// Returns the number of monitors found
static int list_monitors(const gsr_window *window, const char *card_path) {
    capture_options_callback options;
    options.window = window;
    options.num_monitors = 0;

    const bool is_x11 = gsr_window_get_display_server(window) == GSR_DISPLAY_SERVER_X11;
    const gsr_connection_type connection_type = is_x11 ? GSR_CONNECTION_X11 : GSR_CONNECTION_DRM;
    for_each_active_monitor_output(window, card_path, connection_type, output_monitor_info, &options);

    return options.num_monitors;
}

static void list_supported_capture_options(const gsr_window *window, const char *card_path, bool do_list_monitors) {
    const bool wayland = gsr_window_get_display_server(window) == GSR_DISPLAY_SERVER_WAYLAND;
    if(!wayland) {
        puts("window");
        puts("focused");
    }

    int num_monitors = 0;
    if(do_list_monitors)
        num_monitors = list_monitors(window, card_path);

    if(num_monitors > 0)
        puts("region");

    gsr_capture_v4l2_list_devices(camera_query_callback, NULL);

#ifdef GSR_PORTAL
    // Desktop portal capture on x11 doesn't seem to be hardware accelerated
    if(!wayland)
        return;

    gsr_dbus dbus;
    if(!gsr_dbus_init(&dbus, NULL))
        return;

    char *session_handle = NULL;
    if(gsr_dbus_screencast_create_session(&dbus, &session_handle) == 0)
        puts("portal");

    gsr_dbus_deinit(&dbus);
#endif
}

static void version_command(void *userdata) {
    (void)userdata;
    puts(GSR_VERSION);
    fflush(stdout);
    _exit(0);
}

static void info_command(void *userdata) {
    (void)userdata;
    gsr_windowing windowing;
    const gsr_windowing_params windowing_params = {false, false, false};
    if(gsr_windowing_init(&windowing, &windowing_params) != GSR_ERROR_OK)
        _exit(1);
    if(gsr_windowing_load_egl(&windowing, &windowing_params) != GSR_ERROR_OK)
        _exit(22);
    const bool wayland = gsr_windowing_is_wayland(&windowing);

    av_log_set_level(AV_LOG_FATAL);

    puts("section=system_info");
    list_system_info(wayland);
    if(windowing.egl.gpu_info.is_steam_deck)
        puts("is_steam_deck|yes");
    else
        puts("is_steam_deck|no");
    printf("gsr_version|%s\n", GSR_VERSION);
    puts("section=gpu_info");
    list_gpu_info(&windowing.egl);
    puts("section=video_codecs");
    list_supported_video_codecs(&windowing.egl, wayland);
    puts("section=image_formats");
    puts("jpeg");
    puts("png");
    puts("section=capture_options");
    list_supported_capture_options(windowing.window, windowing.egl.card_path, windowing.card_path_found);

    fflush(stdout);

    // Not needed as this will just slow down shutdown
    //gsr_egl_unload(&egl);
    //gsr_window_destroy(&window);
    //if(dpy)
    //    XCloseDisplay(dpy);

    _exit(0);
}

static void list_audio_devices_command(void *userdata) {
    (void)userdata;
    gsr_audio_devices audio_devices;
    get_pulseaudio_inputs(&audio_devices);

    if(audio_devices.default_output[0] != '\0')
        puts("default_output|Default output");

    if(audio_devices.default_input[0] != '\0')
        puts("default_input|Default input");

    for(size_t i = 0; i < audio_devices.num_items; ++i) {
        printf("%s|%s\n", audio_devices.items[i].name, audio_devices.items[i].description);
    }

    gsr_audio_devices_deinit(&audio_devices);
    fflush(stdout);
    _exit(0);
}

static bool app_audio_query_callback(const char *app_name, void*) {
    puts(app_name);
    return true;
}

static void list_application_audio_command(void *userdata) {
    (void)userdata;
#ifdef GSR_APP_AUDIO
    if(pulseaudio_server_is_pipewire()) {
        gsr_pipewire_audio audio;
        if(gsr_pipewire_audio_init(&audio)) {
            gsr_pipewire_audio_for_each_app(&audio, app_audio_query_callback, NULL);
            gsr_pipewire_audio_deinit(&audio);
        }
    }
#endif

    fflush(stdout);
    _exit(0);
}

static void list_v4l2_devices(void *userdata) {
    (void)userdata;
    gsr_capture_v4l2_list_devices(camera_query_callback, NULL);

    fflush(stdout);
    _exit(0);
}

// |card_path| can be NULL. If not NULL then |vendor| has to be valid
static void list_capture_options_command(const char *card_path, void *userdata) {
    (void)userdata;
    gsr_windowing windowing;
    const gsr_windowing_params windowing_params = {false, false, false};
    if(gsr_windowing_init(&windowing, &windowing_params) != GSR_ERROR_OK)
        _exit(1);
    if(!card_path && gsr_windowing_load_egl(&windowing, &windowing_params) != GSR_ERROR_OK)
        _exit(22);

    if(card_path)
        list_supported_capture_options(windowing.window, card_path, true);
    else
        list_supported_capture_options(windowing.window, windowing.egl.card_path, windowing.card_path_found);

    fflush(stdout);

    // Not needed as this will just slow down shutdown
    //gsr_egl_unload(&egl);
    //gsr_window_destroy(&window);
    //if(dpy)
    //    XCloseDisplay(dpy);

    _exit(0);
}

static void list_monitors_command(void *userdata) {
    (void)userdata;
    gsr_windowing windowing;
    const gsr_windowing_params windowing_params = {false, false, false};
    if(gsr_windowing_init(&windowing, &windowing_params) != GSR_ERROR_OK)
        _exit(1);
    if(gsr_windowing_load_egl(&windowing, &windowing_params) != GSR_ERROR_OK)
        _exit(22);

    if(windowing.card_path_found)
        list_monitors(windowing.window, windowing.egl.card_path);

    fflush(stdout);

    // Not needed as this will just slow down shutdown
    //gsr_egl_unload(&egl);
    //gsr_window_destroy(&window);
    //if(dpy)
    //    XCloseDisplay(dpy);

    _exit(0);
}

static gsr_capture_deps capture_deps;

static gsr_color_range image_format_to_color_range(gsr_image_format image_format, int image_quality) {
    switch(image_format) {
        case GSR_IMAGE_FORMAT_JPEG: return image_quality >= JPEG_YUV444_QUALITY_THRESHOLD ? GSR_COLOR_RANGE_FULL : GSR_COLOR_RANGE_LIMITED;
        case GSR_IMAGE_FORMAT_PNG:  return GSR_COLOR_RANGE_FULL;
    }
    assert(false);
    return GSR_COLOR_RANGE_FULL;
}

static int video_quality_to_image_quality_value(gsr_video_quality video_quality) {
    switch(video_quality) {
        case GSR_VIDEO_QUALITY_MEDIUM:
            return 75;
        case GSR_VIDEO_QUALITY_HIGH:
            return 85;
        case GSR_VIDEO_QUALITY_VERY_HIGH:
            return JPEG_YUV444_QUALITY_THRESHOLD; // Quality above 90 makes the jpeg image encoder (stb_image_writer) use yuv444 instead of yuv420, which greatly improves small colored text quality on dark background
        case GSR_VIDEO_QUALITY_ULTRA:
            return 97;
    }
    assert(false);
    return 90;
}

static void load_plugins(gsr_plugins *plugins, args_parser &arg_parser, gsr_egl *egl, vec2i video_size) {
    const Arg *plugin_arg = args_parser_get_arg(&arg_parser, "-p");
    assert(plugin_arg);

    if(plugin_arg->num_values > 0) {
        const gsr_color_depth color_depth = video_codec_to_bit_depth(arg_parser.settings.video_codec);
        assert(color_depth == GSR_COLOR_DEPTH_8_BITS || color_depth == GSR_COLOR_DEPTH_10_BITS);

        const gsr_plugin_init_params plugin_init_params = {
            (unsigned int)video_size.x,
            (unsigned int)video_size.y,
            (unsigned int)arg_parser.settings.fps,
            color_depth == GSR_COLOR_DEPTH_8_BITS ? GSR_PLUGIN_COLOR_DEPTH_8_BITS : GSR_PLUGIN_COLOR_DEPTH_10_BITS,
            egl->context_type == GSR_GL_CONTEXT_TYPE_GLX ? GSR_PLUGIN_GRAPHICS_API_GLX : GSR_PLUGIN_GRAPHICS_API_EGL_ES,
        };

        if(!gsr_plugins_init(plugins, plugin_init_params, egl))
            _exit(1);

        for(int i = 0; i < plugin_arg->num_values; ++i) {
            if(!gsr_plugins_load_plugin(plugins, plugin_arg->values[i]))
                _exit(1);
        }
    }
}

// TODO: 10-bit and hdr.
static void capture_image_to_file(args_parser &arg_parser, gsr_egl *egl, gsr_window *window, gsr_image_format image_format, gsr_capture_sources *capture_sources) {
    const int image_quality = video_quality_to_image_quality_value(arg_parser.settings.video_quality);
    const gsr_color_range color_range = image_format_to_color_range(image_format, image_quality);
    arg_parser.settings.fps = 60; // We want to capture an image as soon as possible

    vec2i video_size = {0, 0};
    gsr_video_sources video_sources_data;
    const int video_sources_result = gsr_video_sources_create(&video_sources_data, &arg_parser.settings, egl, &capture_deps, true, capture_sources, &video_size);
    if(video_sources_result != GSR_ERROR_OK)
        _exit(gsr_error_to_exit_code(video_sources_result));
    gsr_video_sources *video_sources = &video_sources_data;
    gsr_video_sources_update_with_real_video_size(video_sources, video_size);

    const Arg *plugin_arg = args_parser_get_arg(&arg_parser, "-p");
    assert(plugin_arg);

    gsr_plugins plugins;
    memset(&plugins, 0, sizeof(plugins));

    if(plugin_arg->num_values > 0)
        load_plugins(&plugins, arg_parser, egl, video_size);

    gsr_image_writer image_writer;
    if(!gsr_image_writer_init_opengl(&image_writer, egl, video_size.x, video_size.y)) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "capture_image_to_file: gsr_image_write_gl_init failed");
        _exit(1);
    }

    gsr_color_conversion_params color_conversion_params;
    memset(&color_conversion_params, 0, sizeof(color_conversion_params));
    color_conversion_params.color_range = color_range;
    color_conversion_params.egl = egl;
    color_conversion_params.load_external_image_shader = gsr_video_sources_uses_external_image(video_sources);

    color_conversion_params.destination_textures[0] = image_writer.texture;
    color_conversion_params.destination_textures_size[0] = video_size;
    color_conversion_params.num_destination_textures = 1;
    color_conversion_params.destination_color = GSR_DESTINATION_COLOR_RGB;

    gsr_color_conversion color_conversion;
    if(gsr_color_conversion_init(&color_conversion, &color_conversion_params) != 0) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "capture_image_to_file: failed to create color conversion");
        _exit(1);
    }

    gsr_color_conversion_clear(&color_conversion);

    gsr_color_conversion *output_color_conversion = plugins.num_plugins > 0 ? &plugins.color_conversion : &color_conversion;

    bool should_stop_error = false;
    egl->glClear(0);

    while(running) {
        while(gsr_window_process_event(window)) {
            if(capture_deps.x11_cursor_display && arg_parser.settings.record_cursor)
                gsr_cursor_on_event(&capture_deps.x11_cursor, gsr_window_get_event_data(window));

            for(size_t video_source_index = 0; video_source_index < video_sources->num_items; ++video_source_index) {
        gsr_video_source &video_source = video_sources->items[video_source_index];
                gsr_capture_on_event(video_source.capture, egl);
            }
        }

        if(capture_deps.x11_cursor_display && arg_parser.settings.record_cursor)
            gsr_cursor_tick(&capture_deps.x11_cursor, DefaultRootWindow(capture_deps.x11_cursor_display));

        gsr_capture_deps_cleanup_kms_fds(&capture_deps);

        gsr_capture_deps_update_kms(&capture_deps);

        should_stop_error = false;
        for(size_t video_source_index = 0; video_source_index < video_sources->num_items; ++video_source_index) {
        gsr_video_source &video_source = video_sources->items[video_source_index];
            gsr_capture_tick(video_source.capture);
            if(gsr_capture_should_stop(video_source.capture, &should_stop_error)) {
                running = 0;
                break;
            }
        }

        for(size_t video_source_index = 0; video_source_index < video_sources->num_items; ++video_source_index) {
        gsr_video_source &video_source = video_sources->items[video_source_index];
            if(video_source.capture->pre_capture)
                video_source.capture->pre_capture(video_source.capture, &video_source.metadata, output_color_conversion);
        }

        if(output_color_conversion->schedule_clear) {
            output_color_conversion->schedule_clear = false;
            gsr_color_conversion_clear(output_color_conversion);
        }

        bool all_sources_captured = true;
        for(size_t video_source_index = 0; video_source_index < video_sources->num_items; ++video_source_index) {
        gsr_video_source &video_source = video_sources->items[video_source_index];
            // It can fail, for example when capturing portal and the target is a monitor that hasn't been updated.
            // This can also happen for example if the system suspends and the monitor to capture's framebuffer is gone, or if the target window disappeared.
            if(gsr_capture_capture(video_source.capture, &video_source.metadata, output_color_conversion) != 0)
                all_sources_captured = false;
        }

        gsr_capture_deps_cleanup_kms_fds(&capture_deps);

        if(all_sources_captured)
            break;

        if(running)
            usleep(30 * 1000); // 30 ms
    }

    if(plugins.num_plugins > 0) {
        gsr_plugins_draw(&plugins);
        gsr_color_conversion_draw(&color_conversion, plugins.texture,
            {0, 0}, video_size,
            {0, 0}, video_size,
            video_size, GSR_ROT_0, GSR_FLIP_NONE, GSR_SOURCE_COLOR_RGB, false);
    }

    gsr_egl_swap_buffers(egl);

    if(!should_stop_error) {
        if(!gsr_image_writer_write_to_file(&image_writer, arg_parser.settings.filename, image_format, image_quality)) {
            gsr_log(GSR_LOG_LEVEL_ERROR, "capture_image_to_file: failed to write opengl texture to image output file %s", arg_parser.settings.filename);
            _exit(1);
        }

        if(arg_parser.settings.recording_saved_script)
            run_recording_saved_script_async(arg_parser.settings.recording_saved_script, arg_parser.settings.filename, "screenshot");
    }

    gsr_plugins_deinit(&plugins);
    gsr_image_writer_deinit(&image_writer);
    gsr_video_sources_deinit(video_sources);
    _exit(should_stop_error ? 3 : 0);
}

// Manually check if the audio inputs we give exist. This is only needed for pipewire, not pulseaudio.
// Pipewire instead DEFAULTS TO THE DEFAULT AUDIO INPUT. THAT'S RETARDED.
// OH, YOU MISSPELLED THE AUDIO INPUT? FUCK YOU
// Should use amix if more than 1 audio device and 0 application audio, merged
/* Returns -1 if none is available */
#ifdef GSR_APP_AUDIO
#endif

static bool get_image_format_from_filename(const char *filename, gsr_image_format *image_format) {
    if(gsr_string_ends_with(filename, ".jpg") || gsr_string_ends_with(filename, ".jpeg")) {
        *image_format = GSR_IMAGE_FORMAT_JPEG;
        return true;
    } else if(gsr_string_ends_with(filename, ".png")) {
        *image_format = GSR_IMAGE_FORMAT_PNG;
        return true;
    } else {
        return false;
    }
}

static void set_display_server_environment_variables() {
    // Some users dont have properly setup environments (no display manager that does systemctl --user import-environment DISPLAY WAYLAND_DISPLAY)
    const char *display = getenv("DISPLAY");
    if(!display) {
        display = ":0";
        setenv("DISPLAY", display, true);
    }

    const char *wayland_display = getenv("WAYLAND_DISPLAY");
    if(!wayland_display) {
        wayland_display = "wayland-0";
        setenv("WAYLAND_DISPLAY", wayland_display, true);
    }
}

static void validate_args_with_capture_sources(args_parser &arg_parser, const gsr_capture_sources *capture_sources) {
    const Arg *output_resolution_arg = args_parser_get_arg(&arg_parser, "-s");
    assert(output_resolution_arg);

    const Arg *region_arg = args_parser_get_arg(&arg_parser, "-region");
    assert(region_arg);

    if(gsr_capture_sources_has_type(capture_sources, GSR_CAPTURE_SOURCE_TYPE_FOCUSED_WINDOW) && output_resolution_arg->num_values == 0) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "option -s is required when using '-w focused' option");
        args_parser_print_usage();
        _exit(1);
    }

    const bool is_capturing_region = gsr_capture_sources_has_type(capture_sources, GSR_CAPTURE_SOURCE_TYPE_REGION);
    if(region_arg->num_values == 0) {
        if(is_capturing_region && !gsr_capture_sources_has_region_set(capture_sources)) {
            gsr_log(GSR_LOG_LEVEL_ERROR, "option -region is required when '-w region' is used");
            args_parser_print_usage();
            _exit(1);
        }
    } else {
        if(is_capturing_region) {
            gsr_log(GSR_LOG_LEVEL_WARNING, "option -region is deprecated, use -w with region directly instead, for example: -w %s", region_arg->values[0]);
        } else {
            gsr_log(GSR_LOG_LEVEL_ERROR, "option -region can only be used when option '-w region' is used");
            args_parser_print_usage();
            _exit(1);
        }
    }

    if(!arg_parser.settings.restore_portal_session && gsr_capture_sources_has_type(capture_sources, GSR_CAPTURE_SOURCE_TYPE_PORTAL))
        gsr_log(GSR_LOG_LEVEL_INFO, "option '-w portal' was used without '-restore-portal-session yes'. The previous screencast session will be ignored");
}

static void install_cuda_no_stable_perf_limit() {
    if(access("/proc/driver/nvidia/version", F_OK) != 0)
        return;

    const char *home = getenv("HOME");
    if(!home) {
        gsr_log(GSR_LOG_LEVEL_WARNING, "install_cuda_no_stable_perf_limit: $HOME not set");
        return;
    }

    char nv_profiles_path[4096];
    snprintf(nv_profiles_path, sizeof(nv_profiles_path), "%s/.nv/nvidia-application-profiles-rc.d", home);

    if(create_directory_recursive(nv_profiles_path) != 0) {
        gsr_log(GSR_LOG_LEVEL_WARNING, "install_cuda_no_stable_perf_limit: failed to create directory: %s", nv_profiles_path);
        return;
    }

    snprintf(nv_profiles_path, sizeof(nv_profiles_path), "%s/.nv/nvidia-application-profiles-rc.d/10-gsr-cuda-no-stable-perf-limit", home);

    FILE *f = fopen(nv_profiles_path, "wb");
    if(!f) {
        gsr_log(GSR_LOG_LEVEL_WARNING, "install_cuda_no_stable_perf_limit: failed to create file: %s", nv_profiles_path);
        return;
    }

    const char *profile_data =
        "{\n"
        "    \"profiles\": [\n"
        "        {\n"
        "            \"name\": \"CudaNoStablePerfLimit\",\n"
        "            \"settings\": [\"0x166c5e\", 0]\n"
        "        }\n"
        "    ],\n"
        "    \"rules\": [\n"
        "        { \"pattern\": \"gpu-screen-recorder\", \"profile\": \"CudaNoStablePerfLimit\" }\n"
        "    ]\n"
        "}\n";

    fwrite(profile_data, 1, strlen(profile_data), f);
    fclose(f);
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "C"); // Sigh... stupid C
#ifdef __GLIBC__
    mallopt(M_MMAP_THRESHOLD, 65536);
#endif

    signal(SIGINT, stop_handler);
    signal(SIGTERM, stop_handler);
    signal(SIGUSR1, save_replay_handler);
    signal(SIGUSR2, toggle_pause_handler);
    signal(SIGRTMIN, toggle_replay_recording_handler);
    signal(SIGRTMIN+1, save_replay_10_seconds_handler);
    signal(SIGRTMIN+2, save_replay_30_seconds_handler);
    signal(SIGRTMIN+3, save_replay_1_minute_handler);
    signal(SIGRTMIN+4, save_replay_5_minutes_handler);
    signal(SIGRTMIN+5, save_replay_10_minutes_handler);
    signal(SIGRTMIN+6, save_replay_30_minutes_handler);

    set_display_server_environment_variables();
    install_cuda_no_stable_perf_limit();

    // Linux nvidia driver 580.105.08 added the environment variable CUDA_DISABLE_PERF_BOOST to disable the p2 power level issue,
    // where running cuda (which includes nvenc) causes the gpu to be forcefully set to p2 power level which on many nvidia gpus
    // decreases gpu performance in games. On my GTX 1080 it decreased game performance by 10% for absolutely no reason.
    setenv("CUDA_DISABLE_PERF_BOOST", "1", true);
    // Stop nvidia driver from buffering frames
    setenv("__GL_MaxFramesAllowed", "1", true);
    // If this is set to 1 then cuGraphicsGLRegisterImage will fail for egl context with error: invalid OpenGL or DirectX context,
    // so we overwrite it
    setenv("__GL_THREADED_OPTIMIZATIONS", "0", true);
    // Some people set this to nvidia (for nvdec) or vdpau (for nvidia vdpau), which breaks gpu screen recorder since
    // nvidia doesn't support vaapi and nvidia-vaapi-driver doesn't support encoding yet.
    // Let vaapi find the right vaapi driver instead of forcing a specific one.
    unsetenv("LIBVA_DRIVER_NAME");
    // Some people set this to force all applications to vsync on nvidia, but this makes eglSwapBuffers never return.
    unsetenv("__GL_SYNC_TO_VBLANK");
    // Same as above, but for amd/intel
    unsetenv("vblank_mode");

    if(geteuid() == 0) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "don't run gpu-screen-recorder as the root user");
        _exit(1);
    }

    args_handlers arg_handlers;
    arg_handlers.version = version_command;
    arg_handlers.info = info_command;
    arg_handlers.list_audio_devices = list_audio_devices_command;
    arg_handlers.list_application_audio = list_application_audio_command;
    arg_handlers.list_v4l2_devices = list_v4l2_devices;
    arg_handlers.list_capture_options = list_capture_options_command;
    arg_handlers.list_monitors = list_monitors_command;

    args_parser arg_parser;
    if(!args_parser_parse(&arg_parser, argc, argv, &arg_handlers, NULL))
        _exit(1);

    if(!arg_parser.settings.low_power) {
        // Forces low latency encoding mode. Use this environment variable until vaapi supports setting this as a parameter.
        // The downside of this is that it always uses maximum power, which is not ideal for replay mode that runs on system startup.
        // This option was added in mesa 24.1.4, released in july 17, 2024.
        // Seems like the performance issue is not in encoding, but rendering the frame.
        // Some frames end up taking 10 times longer. Seems to be an issue with amd gpu power management when letting the application sleep on the cpu side?
        setenv("AMD_DEBUG", "lowlatencyenc", true);
    }

    gsr_capture_sources capture_sources_data;
    const int parse_capture_sources_result = gsr_capture_sources_parse(&capture_sources_data, arg_parser.settings.capture_source, arg_parser.settings.region_position, arg_parser.settings.region_size);
    if(parse_capture_sources_result != GSR_ERROR_OK)
        _exit(gsr_error_to_exit_code(parse_capture_sources_result));

    gsr_capture_sources *capture_sources = &capture_sources_data;
    if(capture_sources->num_items == 0) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "option -w can't be empty. You need to capture video from at least one source");
        args_parser_print_usage();
        _exit(1);
    }
    validate_args_with_capture_sources(arg_parser, capture_sources);

    //av_log_set_level(AV_LOG_TRACE);

    const Arg *audio_input_arg = args_parser_get_arg(&arg_parser, "-a");
    assert(audio_input_arg);

    gsr_audio_devices audio_devices;
    memset(&audio_devices, 0, sizeof(audio_devices));
    if(audio_input_arg->num_values > 0)
        get_pulseaudio_inputs(&audio_devices);

    gsr_audio_input_tracks requested_audio_inputs;
    const int parse_audio_inputs_result = gsr_audio_input_tracks_parse(&requested_audio_inputs, audio_input_arg->values, audio_input_arg->num_values, &audio_devices);
    gsr_audio_devices_deinit(&audio_devices);
    if(parse_audio_inputs_result != GSR_ERROR_OK)
        _exit(gsr_error_to_exit_code(parse_audio_inputs_result));

    const bool uses_app_audio = gsr_audio_input_tracks_has_app_audio(&requested_audio_inputs);
    gsr_app_audio_names app_audio_names;
    memset(&app_audio_names, 0, sizeof(app_audio_names));
#ifdef GSR_APP_AUDIO
    gsr_pipewire_audio pipewire_audio;
    memset(&pipewire_audio, 0, sizeof(pipewire_audio));
    if(uses_app_audio) {
        if(!pulseaudio_server_is_pipewire()) {
            gsr_log(GSR_LOG_LEVEL_ERROR, "your sound server is not PipeWire. Application audio is only available when running PipeWire audio server");
            _exit(2);
        }

        if(!gsr_pipewire_audio_init(&pipewire_audio)) {
            gsr_log(GSR_LOG_LEVEL_ERROR, "failed to setup PipeWire audio for application audio capture");
            _exit(2);
        }

        gsr_pipewire_audio_for_each_app(&pipewire_audio, [](const char *app_name, void *userdata) {
            gsr_app_audio_names *app_audio_names = (gsr_app_audio_names*)userdata;
            gsr_app_audio_names_add(app_audio_names, app_name);
            return true;
        }, &app_audio_names);
    }
#else
    if(uses_app_audio) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "application audio can't be recorded because GPU Screen Recorder is built without application audio support (-Dapp_audio option)");
        _exit(2);
    }
#endif

    const int validate_app_audio_result = gsr_audio_input_tracks_validate_app_audio(&requested_audio_inputs, &app_audio_names);
    gsr_app_audio_names_deinit(&app_audio_names);
    if(validate_app_audio_result != GSR_ERROR_OK)
        _exit(gsr_error_to_exit_code(validate_app_audio_result));

    gsr_windowing windowing;
    gsr_windowing_params windowing_params;
    windowing_params.monitor_capture = gsr_capture_sources_has_monitor_or_region(capture_sources);
    windowing_params.gl_debug = arg_parser.settings.gl_debug;
    windowing_params.listen_to_x11_events = true;
    if(gsr_windowing_init(&windowing, &windowing_params) != GSR_ERROR_OK)
        _exit(1);

    Display *dpy = windowing.display;
    gsr_window *window = windowing.window;

    if(gsr_capture_sources_has_type(capture_sources, GSR_CAPTURE_SOURCE_TYPE_PORTAL)) {
        if(gsr_windowing_is_using_prime_run()) {
            gsr_log(GSR_LOG_LEVEL_WARNING, "use of prime-run with -w portal option is currently not supported. Disabling prime-run");
            gsr_windowing_disable_prime_run();
        }

        if(video_codec_is_hdr(arg_parser.settings.video_codec)) {
            gsr_log(GSR_LOG_LEVEL_WARNING, "portal capture option doesn't support hdr yet (PipeWire doesn't support hdr), the video will be tonemapped from hdr to sdr");
            arg_parser.settings.video_codec = hdr_video_codec_to_sdr_video_codec(arg_parser.settings.video_codec);
        }
    }

    if(gsr_windowing_load_egl(&windowing, &windowing_params) != GSR_ERROR_OK)
        _exit(1);

    gsr_egl &egl = windowing.egl;

    gsr_shader_enable_debug_output(arg_parser.settings.gl_debug);
#ifndef NDEBUG
    gsr_shader_enable_debug_output(true);
#endif

    if(!args_parser_validate_with_gl_info(&arg_parser, &egl))
        _exit(1);

    if(!windowing.card_path_found) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "no /dev/dri/cardX device found. Make sure that you have at least one monitor connected or record a single window instead on X11 or record with the -w portal option");
        _exit(2);
    }

    gsr_capture_deps_init_cursor(&capture_deps, &egl, arg_parser.settings.record_cursor);

    // if(wayland && arg_parser.capture_source_type == GSR_CAPTURE_SOURCE_TYPE_MONITOR) {
    //     fprintf(stderr, "gsr warning: it's not possible to sync video to recorded monitor exactly on wayland when recording a monitor."
    //         " If you experience stutter in the video then record with portal capture option instead (-w portal) or use X11 instead\n");
    // }

    gsr_image_format image_format;
    if(get_image_format_from_filename(arg_parser.settings.filename, &image_format)) {
        if(audio_input_arg->num_values > 0) {
            gsr_log(GSR_LOG_LEVEL_ERROR, "can't record audio (-a) when taking a screenshot");
            _exit(1);
        }

        capture_image_to_file(arg_parser, &egl, window, image_format, capture_sources);
        _exit(0);
    }

    AVFormatContext *av_format_context;
    // The output format is automatically guessed by the file extension
    avformat_alloc_output_context2(&av_format_context, nullptr, arg_parser.settings.container_format, arg_parser.settings.filename);
    if (!av_format_context) {
        if(arg_parser.settings.container_format) {
            gsr_log(GSR_LOG_LEVEL_ERROR, "Container format '%s' (argument -c) is not valid", arg_parser.settings.container_format);
        } else {
            gsr_log(GSR_LOG_LEVEL_ERROR, "Failed to deduce container format from file extension. Use the '-c' option to specify container format");
            args_parser_print_usage();
            _exit(1);
        }
        _exit(1);
    }

    set_format_context_options(av_format_context);

    const AVOutputFormat *output_format = av_format_context->oformat;

    std::string file_extension = output_format->extensions ? output_format->extensions : "";
    {
        size_t comma_index = file_extension.find(',');
        if(comma_index != std::string::npos)
            file_extension = file_extension.substr(0, comma_index);
    }

    if(file_extension.empty())
        file_extension = arg_parser.settings.container_format ? arg_parser.settings.container_format : "";

    const bool force_no_audio_offset = arg_parser.settings.is_livestream || arg_parser.settings.is_output_piped || (file_extension != "mp4" && file_extension != "mkv" && file_extension != "webm");
    const double target_fps = 1.0 / (double)arg_parser.settings.fps;

    const bool uses_amix = gsr_audio_input_tracks_should_use_amix(&requested_audio_inputs);
    arg_parser.settings.audio_codec = select_audio_codec_with_fallback(arg_parser.settings.audio_codec, file_extension.c_str(), uses_amix);

    vec2i video_size = {0, 0};
    gsr_video_sources video_sources_data;
    const int video_sources_result = gsr_video_sources_create(&video_sources_data, &arg_parser.settings, &egl, &capture_deps, false, capture_sources, &video_size);
    if(video_sources_result != GSR_ERROR_OK)
        _exit(gsr_error_to_exit_code(video_sources_result));

    gsr_video_sources *video_sources = &video_sources_data;

    // (Some?) livestreaming services require at least one audio track to work.
    // If not audio is provided then create one silent audio track.
    if(arg_parser.settings.is_livestream && requested_audio_inputs.num_items == 0) {
        gsr_log(GSR_LOG_LEVEL_INFO, "live streaming but no audio track was added. Adding a silent audio track");
        gsr_merged_audio_inputs silent_audio_track;
        memset(&silent_audio_track, 0, sizeof(silent_audio_track));
        gsr_audio_input silent_audio_input;
        memset(&silent_audio_input, 0, sizeof(silent_audio_input));
        if(!gsr_merged_audio_inputs_add(&silent_audio_track, &silent_audio_input) || !gsr_audio_input_tracks_add(&requested_audio_inputs, &silent_audio_track))
            _exit(1);
    }

    AVStream *video_stream = nullptr;

    if(arg_parser.settings.video_encoder == GSR_VIDEO_ENCODER_HW_CPU && arg_parser.settings.video_codec != (gsr_video_codec)GSR_VIDEO_CODEC_AUTO && arg_parser.settings.video_codec != GSR_VIDEO_CODEC_H264) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "-encoder cpu was specified but a codec other than h264 was specified. -encoder cpu supports only h264 at the moment");
        _exit(1);
    }

    bool low_power = false;
    const AVCodec *video_codec_f = nullptr;
    const int select_video_codec_result = select_video_codec_with_fallback(video_size, &arg_parser.settings, file_extension.c_str(), &egl, &low_power, &video_codec_f);
    if(select_video_codec_result != GSR_ERROR_OK)
        _exit(gsr_error_to_exit_code(select_video_codec_result));

    const enum AVPixelFormat video_pix_fmt = get_pixel_format(arg_parser.settings.video_codec, egl.gpu_info.vendor, arg_parser.settings.video_encoder == GSR_VIDEO_ENCODER_HW_CPU);
    AVCodecContext *video_codec_context = create_video_codec_context(video_pix_fmt, video_codec_f, &egl, &arg_parser.settings, video_size.x, video_size.y);
    if(!arg_parser.settings.is_replaying)
        video_stream = create_stream(av_format_context, video_codec_context);

    AVFrame *video_frame = av_frame_alloc();
    if(!video_frame) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "Failed to allocate video frame");
        _exit(1);
    }
    video_frame->format = video_codec_context->pix_fmt;
    video_frame->width = video_size.x;
    video_frame->height = video_size.y;
    video_frame->color_range = video_codec_context->color_range;
    video_frame->color_primaries = video_codec_context->color_primaries;
    video_frame->color_trc = video_codec_context->color_trc;
    video_frame->colorspace = video_codec_context->colorspace;
    video_frame->chroma_location = video_codec_context->chroma_sample_location;

    const size_t estimated_replay_buffer_packets = calculate_estimated_replay_buffer_packets(arg_parser.settings.replay_buffer_size_secs, arg_parser.settings.fps, arg_parser.settings.audio_codec, &requested_audio_inputs);
    gsr_recording_clock *recording_clock = gsr_recording_clock_create();
    if(!recording_clock)
        _exit(1);

    gsr_encoder encoder;
    if(!gsr_encoder_init(&encoder, arg_parser.settings.replay_storage, estimated_replay_buffer_packets, arg_parser.settings.replay_buffer_size_secs, arg_parser.settings.filename)) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "failed to create encoder");
        _exit(1);
    }

    gsr_video_encoder *video_encoder = create_video_encoder(&egl, &arg_parser.settings);
    if(!video_encoder) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "failed to create video encoder");
        _exit(1);
    }

    if(!gsr_video_encoder_start(video_encoder, video_codec_context, video_frame)) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "failed to start video encoder");
        _exit(1);
    }

    video_size.x = video_codec_context->width;
    video_size.y = video_codec_context->height;
    gsr_video_sources_update_with_real_video_size(video_sources, video_size);

    const Arg *plugin_arg = args_parser_get_arg(&arg_parser, "-p");
    assert(plugin_arg);

    gsr_plugins plugins;
    memset(&plugins, 0, sizeof(plugins));

    if(plugin_arg->num_values > 0)
        load_plugins(&plugins, arg_parser, &egl, video_size);

    gsr_color_conversion_params color_conversion_params;
    memset(&color_conversion_params, 0, sizeof(color_conversion_params));
    color_conversion_params.color_range = arg_parser.settings.color_range;
    color_conversion_params.egl = &egl;
    color_conversion_params.load_external_image_shader = gsr_video_sources_uses_external_image(video_sources);
    gsr_video_encoder_get_textures(video_encoder, color_conversion_params.destination_textures, color_conversion_params.destination_textures_size, &color_conversion_params.num_destination_textures, &color_conversion_params.destination_color);

    gsr_color_conversion color_conversion;
    if(gsr_color_conversion_init(&color_conversion, &color_conversion_params) != 0) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "main: failed to create color conversion");
        _exit(1);
    }

    gsr_color_conversion_clear(&color_conversion);

    gsr_color_conversion *output_color_conversion = plugins.num_plugins > 0 ? &plugins.color_conversion : &color_conversion;

    if(arg_parser.settings.video_encoder == GSR_VIDEO_ENCODER_HW_CPU) {
        if(!open_video_software(video_codec_context, &arg_parser.settings))
            _exit(1);
    } else {
        if(!open_video_hardware(video_codec_context, low_power, &egl, &arg_parser.settings))
            _exit(1);
    }

    if(video_stream) {
        avcodec_parameters_from_context(video_stream->codecpar, video_codec_context);
        const size_t video_destination_id = gsr_encoder_add_recording_destination(&encoder, video_codec_context, av_format_context, video_stream, 0);
        if(arg_parser.settings.write_first_frame_ts && video_destination_id != (size_t)-1) {
            std::string ts_filepath = std::string(arg_parser.settings.filename) + ".ts";
            gsr_encoder_set_recording_destination_first_frame_ts_filepath(&encoder, video_destination_id, ts_filepath.c_str());
        }
    }

    gsr_audio_capture audio_capture;
    if(gsr_audio_capture_init(&audio_capture, &encoder, recording_clock, &running) != GSR_ERROR_OK)
        _exit(1);

    int audio_max_frame_size = 1024;
    int audio_stream_index = VIDEO_STREAM_INDEX + 1;
    for(size_t audio_track_index = 0; audio_track_index < requested_audio_inputs.num_items; ++audio_track_index) {
        const gsr_merged_audio_inputs &merged_audio_inputs = requested_audio_inputs.items[audio_track_index];
        const bool use_amix = gsr_audio_inputs_should_use_amix(&merged_audio_inputs);
        AVCodecContext *audio_codec_context = create_audio_codec_context(arg_parser.settings.fps, arg_parser.settings.audio_codec, use_amix, arg_parser.settings.audio_bitrate);
        if(!audio_codec_context)
            _exit(1);

        AVStream *audio_stream = nullptr;
        if(!arg_parser.settings.is_replaying) {
            audio_stream = create_stream(av_format_context, audio_codec_context);
            if(gsr_encoder_add_recording_destination(&encoder, audio_codec_context, av_format_context, audio_stream, 0) == (size_t)-1)
                gsr_log(GSR_LOG_LEVEL_ERROR, "added too many audio sources");
        }

        if(audio_stream && merged_audio_inputs.track_name[0] != '\0' && !arg_parser.settings.exclude_metadata)
            av_dict_set(&audio_stream->metadata, "title", merged_audio_inputs.track_name, 0);

        if(!open_audio(audio_codec_context, arg_parser.settings.ffmpeg_audio_opts))
            _exit(1);
        if(audio_stream)
            avcodec_parameters_from_context(audio_stream->codecpar, audio_codec_context);

        #if LIBAVCODEC_VERSION_MAJOR < 60
        const int num_channels = audio_codec_context->channels;
        #else
        const int num_channels = audio_codec_context->ch_layout.nb_channels;
        #endif

        //audio_frame->sample_rate = audio_codec_context->sample_rate;

        AVFilterContext *src_filter_ctx[GSR_MAX_AUDIO_SOURCES_PER_TRACK];
        AVFilterGraph *graph = nullptr;
        AVFilterContext *sink = nullptr;
        if(use_amix) {
            if(merged_audio_inputs.num_items > GSR_MAX_AUDIO_SOURCES_PER_TRACK) {
                gsr_log(GSR_LOG_LEVEL_ERROR, "too many audio sources for one audio track, the maximum is %d", GSR_MAX_AUDIO_SOURCES_PER_TRACK);
                _exit(1);
            }

            if(gsr_audio_init_filter_graph(audio_codec_context, &graph, &sink, src_filter_ctx, merged_audio_inputs.num_items) < 0) {
                gsr_log(GSR_LOG_LEVEL_ERROR, "failed to create audio filter");
                _exit(1);
            }
        }

        // TODO: Cleanup above

        const double audio_fps = (double)audio_codec_context->sample_rate / (double)audio_codec_context->frame_size;
        const double timeout_sec = 1000.0 / audio_fps / 1000.0;

        const double audio_startup_time_seconds = force_no_audio_offset ? 0 : audio_codec_get_desired_delay(arg_parser.settings.audio_codec, arg_parser.settings.fps);// * ((double)audio_codec_context->frame_size / 1024.0);
        const double num_audio_frames_shift = audio_startup_time_seconds / timeout_sec;

        gsr_audio_track audio_track;
        memset(&audio_track, 0, sizeof(audio_track));
        snprintf(audio_track.name, sizeof(audio_track.name), "%s", merged_audio_inputs.track_name);
        audio_track.codec_context = audio_codec_context;
        audio_track.graph = graph;
        audio_track.sink = sink;
        audio_track.stream_index = audio_stream_index;
        audio_track.pts = -audio_codec_context->frame_size * num_audio_frames_shift;

        int audio_track_result = GSR_ERROR_OK;
        if(gsr_audio_inputs_has_app_audio(&merged_audio_inputs)) {
            assert(!use_amix);
#ifdef GSR_APP_AUDIO
            audio_track_result = gsr_audio_track_init_application_input(&audio_track, &merged_audio_inputs, audio_codec_context, num_channels, num_audio_frames_shift, &pipewire_audio);
#endif
        } else {
            audio_track_result = gsr_audio_track_init_device_inputs(&audio_track, &merged_audio_inputs, audio_codec_context, num_channels, num_audio_frames_shift, src_filter_ctx, use_amix);
        }

        if(audio_track_result != GSR_ERROR_OK)
            _exit(gsr_error_to_exit_code(audio_track_result));

        if(!gsr_audio_capture_add_track(&audio_capture, &audio_track))
            _exit(1);
        ++audio_stream_index;

        if(audio_codec_context->frame_size > audio_max_frame_size)
            audio_max_frame_size = audio_codec_context->frame_size;
    }

    //av_dump_format(av_format_context, 0, filename, 1);

    if(!arg_parser.settings.is_replaying && !(output_format->flags & AVFMT_NOFILE)) {
        const int ret = avio_open(&av_format_context->pb, arg_parser.settings.filename, AVIO_FLAG_WRITE);
        if(ret < 0) {
            gsr_log(GSR_LOG_LEVEL_ERROR, "Could not open '%s': %s", arg_parser.settings.filename, gsr_av_error_to_string(ret));
            _exit(1);
        }
    }

    if(!arg_parser.settings.is_replaying)
        av_write_header(av_format_context, arg_parser.settings.ffmpeg_opts);

    double fps_start_time = clock_get_monotonic_seconds();
    //double frame_timer_start = fps_start_time;
    int fps_counter = 0;
    int damage_fps_counter = 0;

    bool paused = false;
    bool replay_recording = false;
    gsr_recording_output replay_recording_output;
    memset(&replay_recording_output, 0, sizeof(replay_recording_output));
    gsr_replay_save replay_save;
    gsr_replay_save_init(&replay_save);
    std::vector<size_t> replay_recording_items;
    std::string replay_recording_filepath;
    bool force_iframe_frame = false; // Only needed for video since audio frames are always iframes

    gsr_recording_clock_start(recording_clock);
    const double record_start_time = gsr_recording_clock_get_start_time(recording_clock);

    if(gsr_audio_capture_start(&audio_capture, audio_max_frame_size, uses_amix) != GSR_ERROR_OK)
        _exit(1);

    // Set update_fps to 24 to test if duplicate/delayed frames cause video/audio desync or too fast/slow video.
    //const double update_fps = fps + 190;
    bool should_stop_error = false;

    int64_t video_pts_counter = 0;
    int64_t video_prev_pts = 0;

    bool hdr_metadata_set = false;
    const bool hdr = video_codec_is_hdr(arg_parser.settings.video_codec);

    bool use_damage_tracking = false;
    gsr_damage damage;
    memset(&damage, 0, sizeof(damage));
    if(arg_parser.settings.framerate_mode == GSR_FRAMERATE_MODE_CONTENT && gsr_capture_sources_has_damage_tracked_target(capture_sources)) {
        if(gsr_window_get_display_server(window) == GSR_DISPLAY_SERVER_X11) {
            gsr_damage_init(&damage, &egl, &capture_deps.x11_cursor, arg_parser.settings.record_cursor);
            use_damage_tracking = true;

            for(size_t i = 0; i < capture_sources->num_items; ++i) {
                const gsr_capture_source *capture_source = &capture_sources->items[i];
                switch(capture_source->type) {
                    case GSR_CAPTURE_SOURCE_TYPE_WINDOW:
                        gsr_damage_start_tracking_window(&damage, capture_source->window_id);
                        break;
                    case GSR_CAPTURE_SOURCE_TYPE_MONITOR:
                    case GSR_CAPTURE_SOURCE_TYPE_REGION:
                        // TODO: When capturing a region only track damage in that region
                        gsr_damage_start_tracking_monitor(&damage, capture_source->name);
                        break;
                    default:
                        break;
                }
            }
        } else if(gsr_capture_sources_has_monitor_or_region(capture_sources)) {
            gsr_log(GSR_LOG_LEVEL_WARNING, "\"-fm content\" has no effect on Wayland when recording a monitor. Either record a monitor on X11 or capture with desktop portal instead (-w portal)");
        }
    }

    while(running) {
        while(gsr_window_process_event(window)) {
            if(capture_deps.x11_cursor_display && arg_parser.settings.record_cursor)
                gsr_cursor_on_event(&capture_deps.x11_cursor, gsr_window_get_event_data(window));

            gsr_damage_on_event(&damage, gsr_window_get_event_data(window));
            for(size_t video_source_index = 0; video_source_index < video_sources->num_items; ++video_source_index) {
        gsr_video_source &video_source = video_sources->items[video_source_index];
                gsr_capture_on_event(video_source.capture, &egl);
            }
        }

        if(capture_deps.x11_cursor_display && arg_parser.settings.record_cursor)
            gsr_cursor_tick(&capture_deps.x11_cursor, DefaultRootWindow(capture_deps.x11_cursor_display));

        gsr_damage_tick(&damage);

        should_stop_error = false;
        bool damaged = false;

        if(use_damage_tracking)
            damaged = gsr_damage_is_damaged(&damage);

        for(size_t video_source_index = 0; video_source_index < video_sources->num_items; ++video_source_index) {
        gsr_video_source &video_source = video_sources->items[video_source_index];
            gsr_capture_tick(video_source.capture);

            if(gsr_capture_should_stop(video_source.capture, &should_stop_error)) {
                running = 0;
                break;
            }

            if(video_source.capture_source->type == GSR_CAPTURE_SOURCE_TYPE_FOCUSED_WINDOW) {
                assert(video_source.capture->get_window_id);
                const Window damage_target_window = video_source.capture->get_window_id(video_source.capture);

                if((int64_t)damage_target_window != video_source.capture_source->window_id) {
                    gsr_damage_stop_tracking_window(&damage, video_source.capture_source->window_id);
                    if(damage_target_window != 0)
                        gsr_damage_start_tracking_window(&damage, damage_target_window);
                }

                video_source.capture_source->window_id = damage_target_window;
            }

            if(video_source.capture->is_damaged)
                damaged |= video_source.capture->is_damaged(video_source.capture);
            else if(!use_damage_tracking)
                damaged = true;
        }

        damaged |= gsr_plugins_is_damaged(&plugins);

        // TODO: Readd wayland sync warning when removing this
        if(arg_parser.settings.framerate_mode != GSR_FRAMERATE_MODE_CONTENT)
            damaged = true;

        if(damaged)
            ++damage_fps_counter;

        ++fps_counter;
        const double time_now = clock_get_monotonic_seconds();
        //const double frame_timer_elapsed = time_now - frame_timer_start;
        const double elapsed = time_now - fps_start_time;
        if (elapsed >= 1.0) {
            if(arg_parser.settings.verbose) {
                fprintf(stderr, "update fps: %d, damage fps: %d\n", fps_counter, damage_fps_counter);
            }
            fps_start_time = time_now;
            fps_counter = 0;
            damage_fps_counter = 0;
        }

        const double this_video_frame_time = gsr_recording_clock_get_time(recording_clock);
        const int64_t expected_frames = std::floor((this_video_frame_time - record_start_time) / target_fps);
        const int64_t num_missed_frames = expected_frames - video_pts_counter;

        if(damaged && num_missed_frames >= 1 && !paused) {
            // TODO: Dont do this if no damage?
            egl.glClear(0);

            gsr_damage_clear(&damage);
            gsr_plugins_clear_damage(&plugins);
            gsr_capture_deps_cleanup_kms_fds(&capture_deps);

            gsr_capture_deps_update_kms(&capture_deps);

            bool capture_has_synchronous_task = false;
            for(size_t video_source_index = 0; video_source_index < video_sources->num_items; ++video_source_index) {
        gsr_video_source &video_source = video_sources->items[video_source_index];
                if(video_source.capture->clear_damage)
                    video_source.capture->clear_damage(video_source.capture);

                if(video_source.capture->capture_has_synchronous_task) {
                    capture_has_synchronous_task = video_source.capture->capture_has_synchronous_task(video_source.capture);
                    if(capture_has_synchronous_task) {
                        paused = true;
                        gsr_recording_clock_set_paused(recording_clock, true);
                    }
                }
            }

            for(size_t video_source_index = 0; video_source_index < video_sources->num_items; ++video_source_index) {
        gsr_video_source &video_source = video_sources->items[video_source_index];
                if(video_source.capture->pre_capture)
                    video_source.capture->pre_capture(video_source.capture, &video_source.metadata, output_color_conversion);
            }

            if(output_color_conversion->schedule_clear) {
                output_color_conversion->schedule_clear = false;
                gsr_color_conversion_clear(output_color_conversion);
            }

            for(size_t video_source_index = 0; video_source_index < video_sources->num_items; ++video_source_index) {
        gsr_video_source &video_source = video_sources->items[video_source_index];
                gsr_capture_capture(video_source.capture, &video_source.metadata, output_color_conversion);
            }

            gsr_capture_deps_cleanup_kms_fds(&capture_deps);

            if(plugins.num_plugins > 0) {
                gsr_plugins_draw(&plugins);
                gsr_color_conversion_draw(&color_conversion, plugins.texture,
                    {0, 0}, video_size,
                    {0, 0}, video_size,
                    video_size, GSR_ROT_0, GSR_FLIP_NONE, GSR_SOURCE_COLOR_RGB, false);
            }

            if(capture_has_synchronous_task) {
                paused = false;
                gsr_recording_clock_set_paused(recording_clock, false);
            }

            gsr_egl_swap_buffers(&egl);
            gsr_video_encoder_copy_textures_to_frame(video_encoder, video_frame, output_color_conversion);

            for(size_t video_source_index = 0; video_source_index < video_sources->num_items; ++video_source_index) {
        gsr_video_source &video_source = video_sources->items[video_source_index];
                if(hdr && !hdr_metadata_set && !arg_parser.settings.is_replaying && add_hdr_metadata_to_video_stream(video_source.capture, video_stream))
                    hdr_metadata_set = true;
            }

            // TODO: Check if duplicate frame can be saved just by writing it with a different pts instead of sending it again
            const int num_frames_to_encode = arg_parser.settings.framerate_mode == GSR_FRAMERATE_MODE_CONSTANT ? num_missed_frames : 1;
            for(int i = 0; i < num_frames_to_encode; ++i) {
                if(arg_parser.settings.framerate_mode == GSR_FRAMERATE_MODE_CONSTANT) {
                    video_frame->pts = video_pts_counter + i;
                } else {
                    video_frame->pts = (this_video_frame_time - record_start_time) * (double)AV_TIME_BASE;
                    const bool same_pts = video_frame->pts == video_prev_pts;
                    video_prev_pts = video_frame->pts;
                    if(same_pts)
                        continue;
                }

                if(force_iframe_frame) {
                    video_frame->pict_type = AV_PICTURE_TYPE_I;
                }

                int ret = avcodec_send_frame(video_codec_context, video_frame);
                if(ret == 0) {
                    // TODO: Move to separate thread because this could write to network (for example when livestreaming)
                    gsr_encoder_receive_packets(&encoder, video_codec_context, video_frame->pts, VIDEO_STREAM_INDEX);
                } else {
                    gsr_log(GSR_LOG_LEVEL_ERROR, "avcodec_send_frame failed, error: %s", gsr_av_error_to_string(ret));
                }

                if(force_iframe_frame) {
                    force_iframe_frame = false;
                    video_frame->pict_type = AV_PICTURE_TYPE_NONE;
                }
            }

            video_pts_counter += num_missed_frames;
        }

        if(toggle_pause == 1 && !arg_parser.settings.is_replaying) {
            paused = !paused;
            gsr_recording_clock_set_paused(recording_clock, paused);
            gsr_log(GSR_LOG_LEVEL_INFO, paused ? "Paused" : "Unpaused");
            toggle_pause = 0;
        }

        if(toggle_replay_recording && !arg_parser.settings.replay_recording_directory) {
            toggle_replay_recording = 0;
            printf("gsr error: Unable to start recording since the -ro option was not specified\n");
            fflush(stdout);
        }

        if(toggle_replay_recording && arg_parser.settings.replay_recording_directory) {
            toggle_replay_recording = 0;
            const bool new_replay_recording_state = !replay_recording;
            if(new_replay_recording_state) {
                gsr_audio_capture_lock_filter(&audio_capture);
                replay_recording_items.clear();
                char replay_recording_filepath_buf[PATH_MAX];
                const bool filepath_created = gsr_create_new_recording_filepath_from_timestamp(replay_recording_filepath_buf, sizeof(replay_recording_filepath_buf), arg_parser.settings.replay_recording_directory, "Video", file_extension.c_str(), arg_parser.settings.date_folders);
                replay_recording_filepath = filepath_created ? replay_recording_filepath_buf : "";
                if(filepath_created && gsr_recording_output_start(&replay_recording_output, replay_recording_filepath.c_str(), &arg_parser.settings, video_codec_context, &audio_capture, hdr, video_sources)) {
                    const size_t video_recording_destination_id = gsr_encoder_add_recording_destination(&encoder, video_codec_context, replay_recording_output.av_format_context, replay_recording_output.video_stream, video_frame->pts);
                    if(arg_parser.settings.write_first_frame_ts && video_recording_destination_id != (size_t)-1) {
                        std::string ts_filepath = replay_recording_filepath + ".ts";
                        gsr_encoder_set_recording_destination_first_frame_ts_filepath(&encoder, video_recording_destination_id, ts_filepath.c_str());
                    }

                    if(video_recording_destination_id != (size_t)-1)
                        replay_recording_items.push_back(video_recording_destination_id);

                    for(size_t i = 0; i < replay_recording_output.num_audio_streams; ++i) {
                        const gsr_recording_audio_stream *audio_stream = &replay_recording_output.audio_streams[i];
                        const size_t audio_recording_destination_id = gsr_encoder_add_recording_destination(&encoder, audio_stream->audio_track->codec_context, replay_recording_output.av_format_context, audio_stream->stream, audio_stream->audio_track->pts);
                        if(audio_recording_destination_id != (size_t)-1)
                            replay_recording_items.push_back(audio_recording_destination_id);
                    }

                    replay_recording = true;
                    force_iframe_frame = true;
                    gsr_log(GSR_LOG_LEVEL_INFO, "Started recording");
                } else {
                    printf("gsr error: Failed to start recording\n");
                    fflush(stdout);
                }
                gsr_audio_capture_unlock_filter(&audio_capture);
            } else if(replay_recording_output.av_format_context) {
                for(size_t id : replay_recording_items) {
                    gsr_encoder_remove_recording_destination(&encoder, id);
                }
                replay_recording_items.clear();

                if(gsr_recording_output_stop(&replay_recording_output)) {
                    gsr_log(GSR_LOG_LEVEL_INFO, "Stopped recording");
                    puts(replay_recording_filepath.c_str());
                    fflush(stdout);
                    if(arg_parser.settings.recording_saved_script)
                        run_recording_saved_script_async(arg_parser.settings.recording_saved_script, replay_recording_filepath.c_str(), "regular");
                } else {
                    printf("gsr error: Failed to save recording\n");
                    fflush(stdout);
                }

                replay_recording = false;
                replay_recording_filepath.clear();
            }
        }

        bool replay_save_result = false;
        const char *replay_save_output_filepath = nullptr;
        if(gsr_replay_save_poll(&replay_save, &replay_save_result, &replay_save_output_filepath)) {
            if(replay_save_output_filepath[0] == '\0' || !replay_save_result) {
                printf("gsr error: Failed to save replay\n");
                fflush(stdout);
            } else {
                puts(replay_save_output_filepath);
                fflush(stdout);
                if(arg_parser.settings.recording_saved_script)
                    run_recording_saved_script_async(arg_parser.settings.recording_saved_script, replay_save_output_filepath, "replay");
            }
        }

        if(save_replay_seconds != 0 && !gsr_replay_save_is_running(&replay_save) && arg_parser.settings.is_replaying) {
            int current_save_replay_seconds = save_replay_seconds;
            if(current_save_replay_seconds > 0)
                current_save_replay_seconds += arg_parser.settings.keyint;

            save_replay_seconds = 0;
            const bool replay_start_result = gsr_replay_save_start(&replay_save, video_codec_context, VIDEO_STREAM_INDEX, &audio_capture, &encoder, &arg_parser.settings, file_extension.c_str(), hdr, video_sources, current_save_replay_seconds);
            if(!replay_start_result) {
                printf("gsr error: Failed to save replay\n");
                fflush(stdout);
            }

            if(arg_parser.settings.restart_replay_on_save && current_save_replay_seconds == GSR_SAVE_REPLAY_SECONDS_FULL) {
                pthread_mutex_lock(&encoder.replay_mutex);
                gsr_replay_buffer_clear(encoder.replay_buffer);
                pthread_mutex_unlock(&encoder.replay_mutex);
            }
        }

        const double time_at_frame_end = gsr_recording_clock_get_time(recording_clock);
        const double time_elapsed_total = time_at_frame_end - record_start_time;
        const int64_t frames_elapsed = std::floor(time_elapsed_total / target_fps);
        const double time_at_next_frame = (frames_elapsed + 1) * target_fps;
        double time_to_next_frame = time_at_next_frame - time_elapsed_total;
        if(time_to_next_frame > target_fps)
            time_to_next_frame = target_fps;
        const int64_t end_num_missed_frames = frames_elapsed - video_pts_counter;

        if(time_to_next_frame > 0.0 && end_num_missed_frames <= 0)
            av_usleep(time_to_next_frame * 1000.0 * 1000.0);
        else {
            if(paused)
                av_usleep(20.0 * 1000.0); // 20 milliseconds
            else if(arg_parser.settings.framerate_mode == GSR_FRAMERATE_MODE_CONTENT)
                av_usleep(2.8 * 1000.0); // 2.8 milliseconds
        }
    }

    running = 0;

    bool final_replay_save_result = false;
    const char *final_replay_save_output_filepath = nullptr;
    if(gsr_replay_save_join(&replay_save, &final_replay_save_result, &final_replay_save_output_filepath)) {
        if(final_replay_save_output_filepath[0] == '\0') {
            // TODO: Output failed to save
        } else {
            puts(final_replay_save_output_filepath);
            fflush(stdout);
            if(arg_parser.settings.recording_saved_script)
                run_recording_saved_script_async(arg_parser.settings.recording_saved_script, final_replay_save_output_filepath, "replay");
        }
    }

    gsr_plugins_deinit(&plugins);

    if(replay_recording_output.av_format_context) {
        for(size_t id : replay_recording_items) {
            gsr_encoder_remove_recording_destination(&encoder, id);
        }
        replay_recording_items.clear();

        if(gsr_recording_output_stop(&replay_recording_output)) {
            gsr_log(GSR_LOG_LEVEL_INFO, "Stopped recording");
            puts(replay_recording_filepath.c_str());
            fflush(stdout);
            if(arg_parser.settings.recording_saved_script)
                run_recording_saved_script_async(arg_parser.settings.recording_saved_script, replay_recording_filepath.c_str(), "regular");
        } else {
            printf("gsr error: Failed to save recording\n");
            fflush(stdout);
        }
    }

    gsr_audio_capture_join_threads(&audio_capture);

    // TODO: Replace this with start_recording_create_steams
    if(!arg_parser.settings.is_replaying && av_write_trailer(av_format_context) != 0) {
        //fprintf(stderr, "Failed to write trailer\n");
    }

    if(!arg_parser.settings.is_replaying && !(output_format->flags & AVFMT_NOFILE)) {
        avio_close(av_format_context->pb);
        avformat_free_context(av_format_context);
    }

    gsr_damage_deinit(&damage);
    gsr_color_conversion_deinit(&color_conversion);
    gsr_video_encoder_destroy(video_encoder, video_codec_context);
    gsr_encoder_deinit(&encoder);
    gsr_video_sources_deinit(video_sources);
#ifdef GSR_APP_AUDIO
    gsr_pipewire_audio_deinit(&pipewire_audio);
#endif
    gsr_capture_deps_deinit(&capture_deps);

    if(!arg_parser.settings.is_replaying && arg_parser.settings.recording_saved_script)
        run_recording_saved_script_async(arg_parser.settings.recording_saved_script, arg_parser.settings.filename, "regular");

    if(dpy) {
        // TODO: This causes a crash, why? maybe some other library dlclose xlib and that also happened to unload this???
        //XCloseDisplay(dpy);
    }

    //gsr_egl_unload(&egl);
    //gsr_window_destroy(&window);

    //av_frame_free(&video_frame);
    args_parser_deinit(&arg_parser);
    // We do an _exit here because cuda uses at_exit to do _something_ that causes the program to freeze,
    // but only on some nvidia driver versions on some gpus (RTX?), and _exit exits the program without calling
    // the at_exit registered functions.
    // Cuda (cuvid library in this case) seems to be waiting for a thread that never finishes execution.
    // Maybe this happens because we dont clean up all ffmpeg resources?
    // TODO: Investigate this.
    _exit(should_stop_error ? 3 : 0);
}
