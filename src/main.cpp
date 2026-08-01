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
#include "../include/recorder/screenshot.h"
#include "../include/cli/commands.h"
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

// TODO: Cleanup
// Returns the number of monitors found
// |card_path| can be NULL. If not NULL then |vendor| has to be valid
static gsr_capture_deps capture_deps;

// TODO: 10-bit and hdr.
// Manually check if the audio inputs we give exist. This is only needed for pipewire, not pulseaudio.
// Pipewire instead DEFAULTS TO THE DEFAULT AUDIO INPUT. THAT'S RETARDED.
// OH, YOU MISSPELLED THE AUDIO INPUT? FUCK YOU
// Should use amix if more than 1 audio device and 0 application audio, merged
/* Returns -1 if none is available */
#ifdef GSR_APP_AUDIO
#endif

static void screenshot_saved_callback(const char *filepath, void *userdata) {
    const char *recording_saved_script = (const char*)userdata;
    if(recording_saved_script)
        run_recording_saved_script_async(recording_saved_script, filepath, "screenshot");
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
    int command_exit_code = 0;
    switch(args_parser_parse(&arg_parser, argc, argv, &arg_handlers, NULL, &command_exit_code)) {
        case ARGS_PARSE_RESULT_ERROR:
            _exit(1);
        case ARGS_PARSE_RESULT_COMMAND_HANDLED:
            _exit(command_exit_code);
        case ARGS_PARSE_RESULT_OK:
            break;
    }

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

        const Arg *screenshot_plugin_arg = args_parser_get_arg(&arg_parser, "-p");
        assert(screenshot_plugin_arg);

        arg_parser.settings.fps = 60; // We want to capture an image as soon as possible

        gsr_screenshot_params screenshot_params;
        memset(&screenshot_params, 0, sizeof(screenshot_params));
        screenshot_params.settings = &arg_parser.settings;
        screenshot_params.egl = &egl;
        screenshot_params.window = window;
        screenshot_params.capture_deps = &capture_deps;
        screenshot_params.capture_sources = capture_sources;
        screenshot_params.image_format = image_format;
        screenshot_params.plugin_filepaths = screenshot_plugin_arg->values;
        screenshot_params.num_plugin_filepaths = screenshot_plugin_arg->num_values;
        screenshot_params.running = &running;
        screenshot_params.screenshot_saved = screenshot_saved_callback;
        screenshot_params.userdata = (void*)arg_parser.settings.recording_saved_script;

        const int screenshot_result = gsr_screenshot_take(&screenshot_params);
        _exit(gsr_error_to_exit_code(screenshot_result));
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

    if(gsr_load_plugins(&plugins, plugin_arg->values, plugin_arg->num_values, &arg_parser.settings, &egl, video_size) != GSR_ERROR_OK)
        _exit(1);

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
