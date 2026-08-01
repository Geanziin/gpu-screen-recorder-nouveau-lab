extern "C" {
#include "../include/ffmpeg_utils.h"
#include "../include/recorder/audio_codec.h"
#include "../include/recorder/video_codec.h"
#include "../include/recorder/codec_select.h"
#include "../include/recorder/windowing.h"
#include "../include/recorder/capture_source.h"
#include "../include/recorder/capture_setup.h"
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

enum class AudioInputType {
    DEVICE,
    APPLICATION
};

struct AudioInput {
    std::string name;
    AudioInputType type = AudioInputType::DEVICE;
    bool inverted = false;
};

struct MergedAudioInputs {
    std::optional<std::string> custom_name;
    std::string track_name;
    std::vector<AudioInput> audio_inputs;
};

// TODO: If options are not supported then they are returned (allocated) in the options. This should be free'd.

// TODO: Remove LIBAVUTIL_VERSION_MAJOR checks in the future when ubuntu, pop os LTS etc update ffmpeg to >= 5.0

static const int VIDEO_STREAM_INDEX = 0;

/* AV1/VP9 use the 0-255 qindex quantizer scale and VP8 uses 0-127. Unlike h264 qp the quantizer step size
   is not exponential in qindex, so the mapping to an equivalent h264 qp is not linear in the quantizer range.
   qindex = h264 qp * 4 matches the qindex = crf * 4 scale used by libvpx/libaom, where crf is roughly equivalent to h264 qp. */
static const int save_replay_seconds_full = -1;

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
    save_replay_seconds = save_replay_seconds_full;
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

static AVStream* create_stream(AVFormatContext *av_format_context, AVCodecContext *codec_context) {
    AVStream *stream = avformat_new_stream(av_format_context, nullptr);
    if (!stream) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "Could not allocate stream");
        _exit(1);
    }
    stream->id = av_format_context->nb_streams - 1;
    stream->time_base = codec_context->time_base;
    stream->avg_frame_rate = codec_context->framerate;
    //stream->r_frame_rate = codec_context->framerate;
    return stream;
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

struct AudioDeviceData {
    SoundDevice sound_device;
    AudioInput audio_input;
    AVFilterContext *src_filter_ctx = nullptr;
    AVFrame *frame = nullptr;
    std::thread thread; // TODO: Instead of having a thread for each track, have one thread for all threads and read the data with non-blocking read
};

// TODO: Cleanup
struct AudioTrack {
    std::string name;
    AVCodecContext *codec_context = nullptr;

    std::vector<AudioDeviceData> audio_devices;
    AVFilterGraph *graph = nullptr;
    AVFilterContext *sink = nullptr;
    int stream_index = 0;
    int64_t pts = 0;
};

static bool add_hdr_metadata_to_video_stream(gsr_capture *cap, AVStream *video_stream) {
    size_t light_metadata_size = 0;
    size_t mastering_display_metadata_size = 0;
    AVContentLightMetadata *light_metadata = av_content_light_metadata_alloc(&light_metadata_size);
    #if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(59, 37, 100)
    AVMasteringDisplayMetadata *mastering_display_metadata = av_mastering_display_metadata_alloc();
    mastering_display_metadata_size = sizeof(*mastering_display_metadata);
    #else
    AVMasteringDisplayMetadata *mastering_display_metadata = av_mastering_display_metadata_alloc_size(&mastering_display_metadata_size);
    #endif

    if(!light_metadata || !mastering_display_metadata) {
        if(light_metadata)
            av_freep(&light_metadata);

        if(mastering_display_metadata)
            av_freep(&mastering_display_metadata);

        return false;
    }

    if(!gsr_capture_set_hdr_metadata(cap, mastering_display_metadata, light_metadata)) {
        av_freep(&light_metadata);
        av_freep(&mastering_display_metadata);
        return false;
    }

    // TODO: More error checking

    #if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(60, 31, 102)
    const bool content_light_level_added = av_stream_add_side_data(video_stream, AV_PKT_DATA_CONTENT_LIGHT_LEVEL, (uint8_t*)light_metadata, light_metadata_size) == 0;
    #else
    const bool content_light_level_added = av_packet_side_data_add(&video_stream->codecpar->coded_side_data, &video_stream->codecpar->nb_coded_side_data, AV_PKT_DATA_CONTENT_LIGHT_LEVEL, light_metadata, light_metadata_size, 0) != NULL;
    #endif

    #if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(60, 31, 102)
    const bool mastering_display_metadata_added = av_stream_add_side_data(video_stream, AV_PKT_DATA_MASTERING_DISPLAY_METADATA, (uint8_t*)mastering_display_metadata, mastering_display_metadata_size) == 0;
    #else
    const bool mastering_display_metadata_added = av_packet_side_data_add(&video_stream->codecpar->coded_side_data, &video_stream->codecpar->nb_coded_side_data, AV_PKT_DATA_MASTERING_DISPLAY_METADATA, mastering_display_metadata, mastering_display_metadata_size, 0) != NULL;
    #endif

    if(!content_light_level_added)
        av_freep(&light_metadata);

    if(!mastering_display_metadata_added)
        av_freep(&mastering_display_metadata);

    // Return true even on failure because we dont want to retry adding hdr metadata on failure
    return true;
}

static void set_format_context_options(AVFormatContext *av_format_context) {
    if(LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(62, 6, 101)) {
        av_opt_set(av_format_context->priv_data, "use_editlist", "1", 0);
        const AVOption *opt = av_opt_find(av_format_context->priv_data, "movflags", NULL, 0, 0);
        if (opt && opt->unit) {
            const AVOption *flag = av_opt_find(av_format_context->priv_data, "hybrid_fragmented", opt->unit, 0, 0);
            if (flag)
                av_opt_set(av_format_context->priv_data, "movflags", "+hybrid_fragmented", 0);
        }
    } else {
        const AVOutputFormat *output_format = av_format_context->oformat;
        const char *file_extension = output_format->extensions ? output_format->extensions : "";
        if(strcmp(file_extension, "mp4") != 0 && strcmp(file_extension, "mov") != 0)
            return;

        gsr_log(GSR_LOG_LEVEL_WARNING, "your FFmpeg version is known to be buggy (it doesn't have working hybrid_fragmented movflags). If you experience stutter in the mp4 file then update your FFmpeg to at least version 8 or record to a mkv file");
    }
}

struct RecordingStartAudio {
    const AudioTrack *audio_track;
    AVStream *stream;
};

struct RecordingStartResult {
    AVFormatContext *av_format_context = nullptr;
    AVStream *video_stream = nullptr;
    std::vector<RecordingStartAudio> audio_inputs;
};

static RecordingStartResult start_recording_create_streams(const char *filename, const args_parser &arg_parser, AVCodecContext *video_codec_context, const std::vector<AudioTrack> &audio_tracks, bool hdr, gsr_video_sources *video_sources) {
    AVFormatContext *av_format_context;
    avformat_alloc_output_context2(&av_format_context, nullptr, arg_parser.settings.container_format, filename);
    set_format_context_options(av_format_context);

    AVStream *video_stream = create_stream(av_format_context, video_codec_context);
    avcodec_parameters_from_context(video_stream->codecpar, video_codec_context);

    RecordingStartResult result;
    result.audio_inputs.reserve(audio_tracks.size());

    for(const AudioTrack &audio_track : audio_tracks) {
        AVStream *audio_stream = create_stream(av_format_context, audio_track.codec_context);
        if(!audio_track.name.empty() && !arg_parser.settings.exclude_metadata)
            av_dict_set(&audio_stream->metadata, "title", audio_track.name.c_str(), 0);
        avcodec_parameters_from_context(audio_stream->codecpar, audio_track.codec_context);
        result.audio_inputs.push_back({&audio_track, audio_stream});
    }

    const int open_ret = avio_open(&av_format_context->pb, filename, AVIO_FLAG_WRITE);
    if(open_ret < 0) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "start_recording_create_streams: could not open '%s': %s", filename, gsr_av_error_to_string(open_ret));
        return result;
    }

    AVDictionary *options = nullptr;
    av_dict_set(&options, "strict", "experimental", 0);

    if(arg_parser.settings.ffmpeg_opts)
        av_dict_parse_string(&options, arg_parser.settings.ffmpeg_opts, "=", ";", 0);

    const int header_write_ret = avformat_write_header(av_format_context, &options);
    av_dict_free(&options);
    if(header_write_ret < 0) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "start_recording_create_streams: error occurred when writing header to output file: %s", gsr_av_error_to_string(header_write_ret));
        avio_close(av_format_context->pb);
        avformat_free_context(av_format_context);
        return result;
    }

    for(size_t video_source_index = 0; video_source_index < video_sources->num_items; ++video_source_index) {
        gsr_video_source &video_source = video_sources->items[video_source_index];
        if(hdr && add_hdr_metadata_to_video_stream(video_source.capture, video_stream))
            break;
    }

    result.av_format_context = av_format_context;
    result.video_stream = video_stream;
    return result;
}

static bool stop_recording_close_streams(AVFormatContext *av_format_context) {
    bool trailer_written = true;
    if(av_write_trailer(av_format_context) != 0) {
        //fprintf(stderr, "gsr error: end: failed to write trailer\n");
        //trailer_written = false;
    }

    const bool closed = avio_close(av_format_context->pb) == 0;
    avformat_free_context(av_format_context);
    return trailer_written && closed;
}

static std::future<bool> save_replay_thread;
static std::string save_replay_output_filepath;

static std::string create_new_recording_filepath_from_timestamp(std::string directory, const char *filename_prefix, const std::string &file_extension, bool date_folders) {
    char date_str[128];
    std::string output_filepath;
    if(date_folders) {
        gsr_get_date_only_str(date_str, sizeof(date_str));
        std::string output_folder = directory + '/' + date_str;
        if(create_directory_recursive(&output_folder[0]) != 0)
            gsr_log(GSR_LOG_LEVEL_ERROR, "failed to create directory: %s", output_folder.c_str());
        gsr_get_time_only_str(date_str, sizeof(date_str));
        output_filepath = output_folder + "/" + filename_prefix + "_" + date_str + "." + file_extension;
    } else {
        if(create_directory_recursive(&directory[0]) != 0)
            gsr_log(GSR_LOG_LEVEL_ERROR, "failed to create directory: %s", directory.c_str());
        gsr_get_date_str(date_str, sizeof(date_str));
        output_filepath = directory + "/" + filename_prefix + "_" + date_str + "." + file_extension;
    }
    return output_filepath;
}

static RecordingStartAudio* get_recording_start_item_by_stream_index(RecordingStartResult &result, int stream_index) {
    for(auto &audio_input : result.audio_inputs) {
        if(audio_input.stream->index == stream_index)
            return &audio_input;
    }
    return nullptr;
}

struct AudioPtsOffset {
    int64_t pts_offset = 0;
    int stream_index = 0;
};

static bool save_replay_async(AVCodecContext *video_codec_context, int video_stream_index, const std::vector<AudioTrack> &audio_tracks, gsr_encoder *encoder, const args_parser &arg_parser, const std::string &file_extension, bool date_folders, bool hdr, gsr_video_sources *video_sources, int current_save_replay_seconds) {
    if(save_replay_thread.valid())
        return true;

    pthread_mutex_lock(&encoder->replay_mutex);
    gsr_replay_buffer *cloned_replay_buffer = gsr_replay_buffer_clone(encoder->replay_buffer);
    pthread_mutex_unlock(&encoder->replay_mutex);
    if(!cloned_replay_buffer) {
        // TODO: Return this error to mark the replay as failed
        gsr_log(GSR_LOG_LEVEL_ERROR, "failed to save replay: failed to clone replay buffer");
        return false;
    }

    const gsr_replay_buffer_iterator search_start_iterator = current_save_replay_seconds == save_replay_seconds_full ? gsr_replay_buffer_iterator{0, 0} : gsr_replay_buffer_find_packet_index_by_time_passed(cloned_replay_buffer, current_save_replay_seconds);
    const gsr_replay_buffer_iterator video_start_iterator = gsr_replay_buffer_find_keyframe(cloned_replay_buffer, search_start_iterator, video_stream_index, false);
    if(video_start_iterator.packet_index == (size_t)-1) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "failed to save replay: failed to find a video keyframe. perhaps replay was saved too fast, before anything has been recorded");
        pthread_mutex_lock(&encoder->replay_mutex);
        gsr_replay_buffer_destroy(cloned_replay_buffer);
        pthread_mutex_unlock(&encoder->replay_mutex);
        return true;
    }

    const int64_t video_pts_offset = gsr_replay_buffer_iterator_get_packet(cloned_replay_buffer, video_start_iterator)->pts;

    std::vector<AudioPtsOffset> audio_pts_offsets;
    audio_pts_offsets.reserve(audio_tracks.size());
    for(const AudioTrack &audio_track : audio_tracks) {
        const gsr_replay_buffer_iterator audio_start_iterator = gsr_replay_buffer_find_keyframe(cloned_replay_buffer, video_start_iterator, audio_track.stream_index, false);
        const int64_t audio_pts_offset = audio_start_iterator.packet_index == (size_t)-1 ? 0 : gsr_replay_buffer_iterator_get_packet(cloned_replay_buffer, audio_start_iterator)->pts;
        audio_pts_offsets.push_back(AudioPtsOffset{audio_pts_offset, audio_track.stream_index});
    }

    std::string output_filepath = create_new_recording_filepath_from_timestamp(arg_parser.settings.filename, "Replay", file_extension, date_folders);
    RecordingStartResult recording_start_result = start_recording_create_streams(output_filepath.c_str(), arg_parser, video_codec_context, audio_tracks, hdr, video_sources);
    if(!recording_start_result.av_format_context) {
        pthread_mutex_lock(&encoder->replay_mutex);
        gsr_replay_buffer_destroy(cloned_replay_buffer);
        pthread_mutex_unlock(&encoder->replay_mutex);
        return false;
    }

    save_replay_output_filepath = std::move(output_filepath);

    save_replay_thread = std::async(std::launch::async, [video_stream_index, recording_start_result, video_start_iterator, video_pts_offset, audio_pts_offsets{std::move(audio_pts_offsets)}, video_codec_context, cloned_replay_buffer, encoder]() mutable {
        bool success = true;
        gsr_replay_buffer_iterator replay_iterator = video_start_iterator;

        for(;;) {
            AVPacket *replay_packet = gsr_replay_buffer_iterator_get_packet(cloned_replay_buffer, replay_iterator);
            uint8_t *replay_packet_data = NULL;
            if(replay_packet) {
                pthread_mutex_lock(&encoder->replay_mutex);
                replay_packet_data = gsr_replay_buffer_iterator_get_packet_data(cloned_replay_buffer, replay_iterator);
                pthread_mutex_unlock(&encoder->replay_mutex);
            }

            if(!replay_packet) {
                gsr_log(GSR_LOG_LEVEL_ERROR, "save_replay_async: no replay packet");
                success = false;
                break;
            }

            if(!replay_packet->data && !replay_packet_data) {
                gsr_log(GSR_LOG_LEVEL_ERROR, "save_replay_async: no replay packet data");
                success = false;
                break;
            }

            // TODO: Check if successful
            AVPacket av_packet;
            memset(&av_packet, 0, sizeof(av_packet));
            //av_packet_from_data(av_packet, replay_packet->data, replay_packet->size);
            av_packet.data = replay_packet->data ? replay_packet->data : replay_packet_data;
            av_packet.size = replay_packet->size;
            av_packet.stream_index = replay_packet->stream_index;
            av_packet.pts = replay_packet->pts;
            av_packet.dts = replay_packet->pts;
            av_packet.flags = replay_packet->flags;
            //av_packet.duration = replay_packet->duration;

            AVStream *stream = recording_start_result.video_stream;
            AVCodecContext *codec_context = video_codec_context;

            if(av_packet.stream_index == video_stream_index) {
                av_packet.pts -= video_pts_offset;
                av_packet.dts -= video_pts_offset;
            } else {
                RecordingStartAudio *recording_start_audio = get_recording_start_item_by_stream_index(recording_start_result, av_packet.stream_index);
                if(!recording_start_audio) {
                    gsr_log(GSR_LOG_LEVEL_ERROR, "save_replay_async: failed to find audio stream by index: %d", av_packet.stream_index);
                    free(replay_packet_data);
                    continue;
                }

                const AudioTrack *audio_track = recording_start_audio->audio_track;
                stream = recording_start_audio->stream;
                codec_context = audio_track->codec_context;

                const AudioPtsOffset &audio_pts_offset = audio_pts_offsets[av_packet.stream_index - 1];
                assert(audio_pts_offset.stream_index == av_packet.stream_index);
                av_packet.pts -= audio_pts_offset.pts_offset;
                av_packet.dts -= audio_pts_offset.pts_offset;
            }

            //av_packet.stream_index = stream->index;
            av_packet_rescale_ts(&av_packet, codec_context->time_base, stream->time_base);

            const int ret = av_write_frame(recording_start_result.av_format_context, &av_packet);
            if(ret < 0)
                gsr_log(GSR_LOG_LEVEL_ERROR, "Failed to write frame index %d to muxer, reason: %s (%d)", av_packet.stream_index, gsr_av_error_to_string(ret), ret);

            free(replay_packet_data);

            //av_packet_free(&av_packet);
            if(!gsr_replay_buffer_iterator_next(cloned_replay_buffer, &replay_iterator))
                break;
        }

        stop_recording_close_streams(recording_start_result.av_format_context);

        pthread_mutex_lock(&encoder->replay_mutex);
        gsr_replay_buffer_destroy(cloned_replay_buffer);
        pthread_mutex_unlock(&encoder->replay_mutex);

        return success;
    });

    return true;
}

static const gsr_audio_device* get_audio_device_by_name(const gsr_audio_devices *audio_devices, const char *name) {
    for(size_t i = 0; i < audio_devices->num_items; ++i) {
        if(strcmp(audio_devices->items[i].name, name) == 0)
            return &audio_devices->items[i];
    }
    return nullptr;
}

static bool parse_audio_input_arg_callback(const char *sub, size_t size, void *userdata) {
    MergedAudioInputs *result = (MergedAudioInputs*)userdata;
    if(size == 0)
        return true;

    AudioInput audio_input;
    audio_input.name.assign(sub, size);

    if(gsr_string_starts_with(audio_input.name.data(), audio_input.name.size(), "name:")) {
        result->custom_name = audio_input.name.substr(5);
    } else if(gsr_string_starts_with(audio_input.name.data(), audio_input.name.size(), "app:")) {
        audio_input.name.erase(audio_input.name.begin(), audio_input.name.begin() + 4);
        audio_input.type = AudioInputType::APPLICATION;
        audio_input.inverted = false;
        result->audio_inputs.push_back(std::move(audio_input));
    } else if(gsr_string_starts_with(audio_input.name.data(), audio_input.name.size(), "app-inverse:")) {
        audio_input.name.erase(audio_input.name.begin(), audio_input.name.begin() + 12);
        audio_input.type = AudioInputType::APPLICATION;
        audio_input.inverted = true;
        result->audio_inputs.push_back(std::move(audio_input));
    } else if(gsr_string_starts_with(audio_input.name.data(), audio_input.name.size(), "device:")) {
        audio_input.name.erase(audio_input.name.begin(), audio_input.name.begin() + 7);
        audio_input.type = AudioInputType::DEVICE;
        result->audio_inputs.push_back(std::move(audio_input));
    } else {
        audio_input.type = AudioInputType::DEVICE;
        result->audio_inputs.push_back(std::move(audio_input));
    }

    return true;
}

static MergedAudioInputs parse_audio_input_arg(const char *str) {
    MergedAudioInputs result;
    result.track_name = str;
    gsr_string_split(str, '|', parse_audio_input_arg_callback, &result);
    return result;
}

static int init_filter_graph(AVCodecContext* audio_codec_context, AVFilterGraph** graph, AVFilterContext** sink, std::vector<AVFilterContext*>& src_filter_ctx, size_t num_sources) {
    char ch_layout[64];
    int err = 0;
    ch_layout[0] = '\0';

    // C89-style variable declaration to
    // avoid problems because of goto
    AVFilterGraph* filter_graph = nullptr;
    AVFilterContext* mix_ctx = nullptr;

    const AVFilter* mix_filter = nullptr;
    const AVFilter* abuffersink = nullptr;
    AVFilterContext* abuffersink_ctx = nullptr;
    char args[512] = { 0 };
#if LIBAVFILTER_VERSION_INT >= AV_VERSION_INT(7, 107, 100)
    bool normalize = false;
#endif

    filter_graph = avfilter_graph_alloc();
    if (!filter_graph) {
        fprintf(stderr, "Unable to create filter graph.\n");
        err = AVERROR(ENOMEM);
        goto fail;
    }

    for(size_t i = 0; i < num_sources; ++i) {
        const AVFilter *abuffer = avfilter_get_by_name("abuffer");
        if (!abuffer) {
            fprintf(stderr, "Could not find the abuffer filter.\n");
            err = AVERROR_FILTER_NOT_FOUND;
            goto fail;
        }

        AVFilterContext *abuffer_ctx = avfilter_graph_alloc_filter(filter_graph, abuffer, NULL);
        if (!abuffer_ctx) {
            fprintf(stderr, "Could not allocate the abuffer instance.\n");
            err = AVERROR(ENOMEM);
            goto fail;
        }

        #if LIBAVCODEC_VERSION_MAJOR < 60
        av_get_channel_layout_string(ch_layout, sizeof(ch_layout), 0, AV_CH_LAYOUT_STEREO);
        #else
        av_channel_layout_describe(&audio_codec_context->ch_layout, ch_layout, sizeof(ch_layout));
        #endif
        av_opt_set    (abuffer_ctx, "channel_layout", ch_layout,                                               AV_OPT_SEARCH_CHILDREN);
        av_opt_set    (abuffer_ctx, "sample_fmt",     av_get_sample_fmt_name(audio_codec_context->sample_fmt), AV_OPT_SEARCH_CHILDREN);
        av_opt_set_q  (abuffer_ctx, "time_base",      audio_codec_context->time_base,                          AV_OPT_SEARCH_CHILDREN);
        av_opt_set_int(abuffer_ctx, "sample_rate",    audio_codec_context->sample_rate,                        AV_OPT_SEARCH_CHILDREN);
        av_opt_set_int(abuffer_ctx, "bit_rate",       audio_codec_context->bit_rate,                           AV_OPT_SEARCH_CHILDREN);

        err = avfilter_init_str(abuffer_ctx, NULL);
        if (err < 0) {
            fprintf(stderr, "Could not initialize the abuffer filter.\n");
            goto fail;
        }

        src_filter_ctx.push_back(abuffer_ctx);
    }

    mix_filter = avfilter_get_by_name("amix");
    if (!mix_filter) {
        av_log(NULL, AV_LOG_ERROR, "Could not find the mix filter.\n");
        err = AVERROR_FILTER_NOT_FOUND;
        goto fail;
    }

#if LIBAVFILTER_VERSION_INT >= AV_VERSION_INT(7, 107, 100)
    snprintf(args, sizeof(args), "inputs=%d:normalize=%s", (int)num_sources, normalize ? "true" : "false");
#else
    snprintf(args, sizeof(args), "inputs=%d", (int)num_sources);
    gsr_log(GSR_LOG_LEVEL_WARNING, "your ffmpeg version doesn't support disabling normalizing of mixed audio. Volume might be lower than expected");
#endif

    err = avfilter_graph_create_filter(&mix_ctx, mix_filter, "amix", args, NULL, filter_graph);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create audio amix filter\n");
        goto fail;
    }

    abuffersink = avfilter_get_by_name("abuffersink");
    if (!abuffersink) {
        fprintf(stderr, "Could not find the abuffersink filter.\n");
        err = AVERROR_FILTER_NOT_FOUND;
        goto fail;
    }

    abuffersink_ctx = avfilter_graph_alloc_filter(filter_graph, abuffersink, "sink");
    if (!abuffersink_ctx) {
        fprintf(stderr, "Could not allocate the abuffersink instance.\n");
        err = AVERROR(ENOMEM);
        goto fail;
    }

    err = avfilter_init_str(abuffersink_ctx, NULL);
    if (err < 0) {
        fprintf(stderr, "Could not initialize the abuffersink instance.\n");
        goto fail;
    }

    err = 0;
    for(size_t i = 0; i < src_filter_ctx.size(); ++i) {
        AVFilterContext *src_ctx = src_filter_ctx[i];
        if (err >= 0)
            err = avfilter_link(src_ctx, 0, mix_ctx, i);
    }
    if (err >= 0)
        err = avfilter_link(mix_ctx, 0, abuffersink_ctx, 0);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error connecting filters\n");
        goto fail;
    }

    err = avfilter_graph_config(filter_graph, NULL);
    if (err < 0) {
        av_log(NULL, AV_LOG_ERROR, "Error configuring the filter graph\n");
        goto fail;
    }

    /* Make sure the sink always outputs frames with the exact amount of samples the audio encoder wants,
       otherwise the audio encoder rejects the frame and that piece of audio is lost */
    av_buffersink_set_frame_size(abuffersink_ctx, audio_codec_context->frame_size);

    *graph = filter_graph;
    *sink = abuffersink_ctx;

    return 0;

fail:
    avfilter_graph_free(&filter_graph);
    src_filter_ctx.clear();  // possibly unnecessary?
    return err;
}

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

static void match_app_audio_input_to_available_apps(const std::vector<AudioInput> &requested_audio_inputs, const std::vector<std::string> &app_audio_names) {
    for(const AudioInput &request_audio_input : requested_audio_inputs) {
        if(request_audio_input.type != AudioInputType::APPLICATION || request_audio_input.inverted)
            continue;

        bool match = false;
        for(const std::string &app_name : app_audio_names) {
            if(strcasecmp(app_name.c_str(), request_audio_input.name.c_str()) == 0) {
                match = true;
                break;
            }
        }

        if(!match) {
            gsr_log(GSR_LOG_LEVEL_WARNING, "no audio application with the name \"%s\" was found, expected one of the following:", request_audio_input.name.c_str());
            for(const std::string &app_name : app_audio_names) {
                fprintf(stderr, "  * %s\n", app_name.c_str());
            }
            fprintf(stderr, "  assuming this is intentional (if you are trying to record audio for applications that haven't started yet).\n");
        }
    }
}

struct AudioTrackDescription {
    std::optional<std::string> custom_name;
    std::vector<std::string> devices;
    std::vector<std::string> applications;
    bool app_inverse = false;

    std::string to_title() const {
        if(custom_name.has_value()){
            return custom_name.value();
        }
        std::string title;
        if(!devices.empty()) {
            title += "Devices: ";
            for(size_t i = 0; i < devices.size(); ++i) {
                if(i > 0)
                    title += ", ";
                title += devices[i];
            }
        }

        if(!applications.empty()) {
            if(!title.empty())
                title += ". ";

            if(app_inverse)
                title += "All applications except: ";
            else
                title += "Applications: ";

            for(size_t i = 0; i < applications.size(); ++i) {
                if(i > 0)
                    title += ", ";
                title += applications[i];
            }
        }
        return title;
    }
};

// Manually check if the audio inputs we give exist. This is only needed for pipewire, not pulseaudio.
// Pipewire instead DEFAULTS TO THE DEFAULT AUDIO INPUT. THAT'S RETARDED.
// OH, YOU MISSPELLED THE AUDIO INPUT? FUCK YOU
static std::vector<MergedAudioInputs> parse_audio_inputs(const gsr_audio_devices *audio_devices, const Arg *audio_input_arg) {
    std::vector<MergedAudioInputs> requested_audio_inputs;

    for(int i = 0; i < audio_input_arg->num_values; ++i) {
        const char *audio_input = audio_input_arg->values[i];
        if(!audio_input || audio_input[0] == '\0')
            continue;

        MergedAudioInputs merged_inputs = parse_audio_input_arg(audio_input);
        requested_audio_inputs.push_back(merged_inputs);
        AudioTrackDescription audio_track_description;
        if(merged_inputs.custom_name.has_value()){
            audio_track_description.custom_name = merged_inputs.custom_name;
        }

        for(AudioInput &request_audio_input : requested_audio_inputs.back().audio_inputs) {
            if(request_audio_input.type == AudioInputType::APPLICATION) {
                audio_track_description.applications.push_back(request_audio_input.name);
                audio_track_description.app_inverse = request_audio_input.inverted;
                continue;
            }

            bool match = false;

            if(request_audio_input.name == "default_output") {
                if(audio_devices->default_output[0] == '\0') {
                    gsr_log(GSR_LOG_LEVEL_ERROR, "-a default_output was specified but no default audio output is specified in the audio server");
                    _exit(2);
                }
                match = true;
                audio_track_description.devices.push_back("Default output");
            } else if(request_audio_input.name == "default_input") {
                if(audio_devices->default_input[0] == '\0') {
                    gsr_log(GSR_LOG_LEVEL_ERROR, "-a default_input was specified but no default audio input is specified in the audio server");
                    _exit(2);
                }
                match = true;
                audio_track_description.devices.push_back("Default input");
            } else {
                const gsr_audio_device *audio_device = get_audio_device_by_name(audio_devices, request_audio_input.name.c_str());
                if(audio_device) {
                    match = true;
                    audio_track_description.devices.push_back(audio_device->description);
                }
            }

            if(!match) {
                gsr_log(GSR_LOG_LEVEL_ERROR, "Audio device '%s' is not a valid audio device, expected one of:", request_audio_input.name.c_str());
                if(audio_devices->default_output[0] != '\0')
                    fprintf(stderr, "    default_output (Default output)\n");
                if(audio_devices->default_input[0] != '\0')
                    fprintf(stderr, "    default_input (Default input)\n");
                for(size_t j = 0; j < audio_devices->num_items; ++j) {
                    fprintf(stderr, "    %s (%s)\n", audio_devices->items[j].name, audio_devices->items[j].description);
                }
                _exit(50);
            }
        }

        requested_audio_inputs.back().track_name = audio_track_description.to_title();
    }

    return requested_audio_inputs;
}

static bool audio_inputs_has_app_audio(const std::vector<AudioInput> &audio_inputs) {
    for(const auto &audio_input : audio_inputs) {
        if(audio_input.type == AudioInputType::APPLICATION)
            return true;
    }
    return false;
}

static bool merged_audio_inputs_has_app_audio(const std::vector<MergedAudioInputs> &merged_audio_inputs) {
    for(const auto &merged_audio_input : merged_audio_inputs) {
        if(audio_inputs_has_app_audio(merged_audio_input.audio_inputs))
            return true;
    }
    return false;
}

// Should use amix if more than 1 audio device and 0 application audio, merged
static bool audio_inputs_should_use_amix(const std::vector<AudioInput> &audio_inputs) {
    int num_audio_devices = 0;
    int num_app_audio = 0;

    for(const auto &audio_input : audio_inputs) {
        if(audio_input.type == AudioInputType::DEVICE)
            ++num_audio_devices;
        else if(audio_input.type == AudioInputType::APPLICATION)
            ++num_app_audio;
    }

    return num_audio_devices > 1 && num_app_audio == 0;
}

static bool merged_audio_inputs_should_use_amix(const std::vector<MergedAudioInputs> &merged_audio_inputs) {
    for(const auto &merged_audio_input : merged_audio_inputs) {
        if(audio_inputs_should_use_amix(merged_audio_input.audio_inputs))
            return true;
    }
    return false;
}

static void validate_merged_audio_inputs_app_audio(const std::vector<MergedAudioInputs> &merged_audio_inputs, const std::vector<std::string> &app_audio_names) {
    for(const auto &merged_audio_input : merged_audio_inputs) {
        int num_app_audio = 0;
        int num_app_inverted_audio = 0;

        for(const auto &audio_input : merged_audio_input.audio_inputs) {
            if(audio_input.type == AudioInputType::APPLICATION) {
                if(audio_input.inverted)
                    ++num_app_inverted_audio;
                else
                    ++num_app_audio;
            }
        }

        match_app_audio_input_to_available_apps(merged_audio_input.audio_inputs, app_audio_names);

        if(num_app_audio > 0 && num_app_inverted_audio > 0) {
            gsr_log(GSR_LOG_LEVEL_ERROR, "argument -a was provided with both app: and app-inverse:, only one of them can be used for one audio track");
            _exit(2);
        }
    }
}

/* Returns -1 if none is available */
static std::vector<AudioDeviceData> create_device_audio_inputs(const std::vector<AudioInput> &audio_inputs, AVCodecContext *audio_codec_context, int num_channels, double num_audio_frames_shift, std::vector<AVFilterContext*> &src_filter_ctx, bool use_amix) {
    std::vector<AudioDeviceData> audio_track_audio_devices;
    for(size_t i = 0; i < audio_inputs.size(); ++i) {
        const auto &audio_input = audio_inputs[i];
        AVFilterContext *src_ctx = nullptr;
        if(use_amix)
            src_ctx = src_filter_ctx[i];

        AudioDeviceData audio_device;
        audio_device.audio_input = audio_input;
        audio_device.src_filter_ctx = src_ctx;

        if(audio_input.name.empty()) {
            audio_device.sound_device.handle = NULL;
            audio_device.sound_device.frames = 0;
        } else {
            const std::string description = "gsr-" + audio_input.name;
            if(sound_device_get_by_name(&audio_device.sound_device, description.c_str(), audio_input.name.c_str(), description.c_str(), num_channels, audio_codec_context->frame_size, audio_codec_context_get_audio_format(audio_codec_context)) != 0) {
                gsr_log(GSR_LOG_LEVEL_ERROR, "failed to get \"%s\" audio device", audio_input.name.c_str());
                _exit(1);
            }
        }

        audio_device.frame = create_audio_frame(audio_codec_context);
        if(!audio_device.frame)
            _exit(1);
        audio_device.frame->pts = -audio_codec_context->frame_size * num_audio_frames_shift;

        audio_track_audio_devices.push_back(std::move(audio_device));
    }
    return audio_track_audio_devices;
}

#ifdef GSR_APP_AUDIO
static AudioDeviceData create_application_audio_audio_input(const MergedAudioInputs &merged_audio_inputs, AVCodecContext *audio_codec_context, int num_channels, double num_audio_frames_shift, gsr_pipewire_audio *pipewire_audio) {
    AudioDeviceData audio_device;
    audio_device.frame = create_audio_frame(audio_codec_context);
    if(!audio_device.frame)
        _exit(1);
    audio_device.frame->pts = -audio_codec_context->frame_size * num_audio_frames_shift;

    char random_str[8];
    if(!generate_random_characters_standard_alphabet(random_str, sizeof(random_str))) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "failed to generate random string");
        _exit(1);
    }

    std::string combined_sink_name = "gsr-combined-";
    combined_sink_name.append(random_str, sizeof(random_str));
    combined_sink_name += ".monitor";

    if(sound_device_get_by_name(&audio_device.sound_device, combined_sink_name.c_str(), "", "gpu-screen-recorder", num_channels, audio_codec_context->frame_size, audio_codec_context_get_audio_format(audio_codec_context)) != 0) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "failed to setup audio recording to combined sink");
        _exit(1);
    }

    std::vector<const char*> audio_devices_sources;
    for(const auto &audio_input : merged_audio_inputs.audio_inputs) {
        if(audio_input.type == AudioInputType::DEVICE)
            audio_devices_sources.push_back(audio_input.name.c_str());
    }

    bool app_audio_inverted = false;
    std::vector<const char*> app_names;
    for(const auto &audio_input : merged_audio_inputs.audio_inputs) {
        if(audio_input.type == AudioInputType::APPLICATION) {
            app_names.push_back(audio_input.name.c_str());
            app_audio_inverted = audio_input.inverted;
        }
    }

    if(!audio_devices_sources.empty()) {
        if(!gsr_pipewire_audio_add_link_from_sources_to_stream(pipewire_audio, audio_devices_sources.data(), audio_devices_sources.size(), combined_sink_name.c_str())) {
            gsr_log(GSR_LOG_LEVEL_ERROR, "failed to add application audio link");
            _exit(1);
        }
    }

    if(app_audio_inverted) {
        if(!gsr_pipewire_audio_add_link_from_apps_to_stream_inverted(pipewire_audio, app_names.data(), app_names.size(), combined_sink_name.c_str())) {
            gsr_log(GSR_LOG_LEVEL_ERROR, "failed to add application audio link");
            _exit(1);
        }
    } else {
        if(!gsr_pipewire_audio_add_link_from_apps_to_stream(pipewire_audio, app_names.data(), app_names.size(), combined_sink_name.c_str())) {
            gsr_log(GSR_LOG_LEVEL_ERROR, "failed to add application audio link");
            _exit(1);
        }
    }

    return audio_device;
}
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

static void av_write_header(AVFormatContext *av_format_context, const char *ffmpeg_opts) {
    AVDictionary *options = nullptr;
    av_dict_set(&options, "strict", "experimental", 0);

    if(ffmpeg_opts)
        av_dict_parse_string(&options, ffmpeg_opts, "=", ";", 0);

    const int ret = avformat_write_header(av_format_context, &options);
    if(ret < 0)
        fprintf(stderr, "Error occurred when writing header to output file: %s\n", gsr_av_error_to_string(ret));

    av_dict_free(&options);
}

static size_t calculate_estimated_replay_buffer_packets(int64_t replay_buffer_size_secs, int fps, gsr_audio_codec audio_codec, const std::vector<MergedAudioInputs> &audio_inputs) {
    if(replay_buffer_size_secs == -1)
        return 0;

    int audio_fps = 0;
    if(!audio_inputs.empty())
        audio_fps = GSR_AUDIO_SAMPLE_RATE / audio_codec_get_frame_size(audio_codec);

    return replay_buffer_size_secs * (fps + audio_fps * audio_inputs.size());
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

    std::vector<MergedAudioInputs> requested_audio_inputs = parse_audio_inputs(&audio_devices, audio_input_arg);
    gsr_audio_devices_deinit(&audio_devices);

    const bool uses_app_audio = merged_audio_inputs_has_app_audio(requested_audio_inputs);
    std::vector<std::string> app_audio_names;
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
            std::vector<std::string> *app_audio_names = (std::vector<std::string>*)userdata;
            app_audio_names->push_back(app_name);
            return true;
        }, &app_audio_names);
    }
#else
    if(uses_app_audio) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "application audio can't be recorded because GPU Screen Recorder is built without application audio support (-Dapp_audio option)");
        _exit(2);
    }
#endif

    validate_merged_audio_inputs_app_audio(requested_audio_inputs, app_audio_names);

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

    const bool uses_amix = merged_audio_inputs_should_use_amix(requested_audio_inputs);
    arg_parser.settings.audio_codec = select_audio_codec_with_fallback(arg_parser.settings.audio_codec, file_extension.c_str(), uses_amix);

    vec2i video_size = {0, 0};
    gsr_video_sources video_sources_data;
    const int video_sources_result = gsr_video_sources_create(&video_sources_data, &arg_parser.settings, &egl, &capture_deps, false, capture_sources, &video_size);
    if(video_sources_result != GSR_ERROR_OK)
        _exit(gsr_error_to_exit_code(video_sources_result));

    gsr_video_sources *video_sources = &video_sources_data;

    // (Some?) livestreaming services require at least one audio track to work.
    // If not audio is provided then create one silent audio track.
    if(arg_parser.settings.is_livestream && requested_audio_inputs.empty()) {
        gsr_log(GSR_LOG_LEVEL_INFO, "live streaming but no audio track was added. Adding a silent audio track");
        MergedAudioInputs mai;
        mai.audio_inputs.push_back({""});
        requested_audio_inputs.push_back(std::move(mai));
    }

    AVStream *video_stream = nullptr;
    std::vector<AudioTrack> audio_tracks;

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

    const size_t estimated_replay_buffer_packets = calculate_estimated_replay_buffer_packets(arg_parser.settings.replay_buffer_size_secs, arg_parser.settings.fps, arg_parser.settings.audio_codec, requested_audio_inputs);
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

    int audio_max_frame_size = 1024;
    int audio_stream_index = VIDEO_STREAM_INDEX + 1;
    for(const MergedAudioInputs &merged_audio_inputs : requested_audio_inputs) {
        const bool use_amix = audio_inputs_should_use_amix(merged_audio_inputs.audio_inputs);
        AVCodecContext *audio_codec_context = create_audio_codec_context(arg_parser.settings.fps, arg_parser.settings.audio_codec, use_amix, arg_parser.settings.audio_bitrate);
        if(!audio_codec_context)
            _exit(1);

        AVStream *audio_stream = nullptr;
        if(!arg_parser.settings.is_replaying) {
            audio_stream = create_stream(av_format_context, audio_codec_context);
            if(gsr_encoder_add_recording_destination(&encoder, audio_codec_context, av_format_context, audio_stream, 0) == (size_t)-1)
                gsr_log(GSR_LOG_LEVEL_ERROR, "added too many audio sources");
        }

        if(audio_stream && !merged_audio_inputs.track_name.empty() && !arg_parser.settings.exclude_metadata)
            av_dict_set(&audio_stream->metadata, "title", merged_audio_inputs.track_name.c_str(), 0);

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

        std::vector<AVFilterContext*> src_filter_ctx;
        AVFilterGraph *graph = nullptr;
        AVFilterContext *sink = nullptr;
        if(use_amix) {
            int err = init_filter_graph(audio_codec_context, &graph, &sink, src_filter_ctx, merged_audio_inputs.audio_inputs.size());
            if(err < 0) {
                gsr_log(GSR_LOG_LEVEL_ERROR, "failed to create audio filter");
                _exit(1);
            }
        }

        // TODO: Cleanup above

        const double audio_fps = (double)audio_codec_context->sample_rate / (double)audio_codec_context->frame_size;
        const double timeout_sec = 1000.0 / audio_fps / 1000.0;

        const double audio_startup_time_seconds = force_no_audio_offset ? 0 : audio_codec_get_desired_delay(arg_parser.settings.audio_codec, arg_parser.settings.fps);// * ((double)audio_codec_context->frame_size / 1024.0);
        const double num_audio_frames_shift = audio_startup_time_seconds / timeout_sec;

        std::vector<AudioDeviceData> audio_track_audio_devices;
        if(audio_inputs_has_app_audio(merged_audio_inputs.audio_inputs)) {
            assert(!use_amix);
#ifdef GSR_APP_AUDIO
            audio_track_audio_devices.push_back(create_application_audio_audio_input(merged_audio_inputs, audio_codec_context, num_channels, num_audio_frames_shift, &pipewire_audio));
#endif
        } else {
            audio_track_audio_devices = create_device_audio_inputs(merged_audio_inputs.audio_inputs, audio_codec_context, num_channels, num_audio_frames_shift, src_filter_ctx, use_amix);
        }

        AudioTrack audio_track;
        audio_track.name = merged_audio_inputs.track_name;
        audio_track.codec_context = audio_codec_context;
        audio_track.audio_devices = std::move(audio_track_audio_devices);
        audio_track.graph = graph;
        audio_track.sink = sink;
        audio_track.stream_index = audio_stream_index;
        audio_track.pts = -audio_codec_context->frame_size * num_audio_frames_shift;
        audio_tracks.push_back(std::move(audio_track));
        ++audio_stream_index;

        audio_max_frame_size = std::max(audio_max_frame_size, audio_codec_context->frame_size);
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
    std::atomic<double> paused_time_offset(0.0);
    double paused_time_start = 0.0;
    bool replay_recording = false;
    RecordingStartResult replay_recording_start_result;
    std::vector<size_t> replay_recording_items;
    std::string replay_recording_filepath;
    bool force_iframe_frame = false; // Only needed for video since audio frames are always iframes

    std::mutex audio_filter_mutex;

    const double record_start_time = clock_get_monotonic_seconds();

    const size_t audio_buffer_size = audio_max_frame_size * 4 * 2; // max 4 bytes/sample, 2 channels
    uint8_t *empty_audio = (uint8_t*)malloc(audio_buffer_size);
    if(!empty_audio) {
        gsr_log(GSR_LOG_LEVEL_ERROR, "failed to create empty audio");
        _exit(1);
    }
    memset(empty_audio, 0, audio_buffer_size);

    for(AudioTrack &audio_track : audio_tracks) {
        for(AudioDeviceData &audio_device : audio_track.audio_devices) {
            audio_device.thread = std::thread([&]() mutable {
                const AVSampleFormat sound_device_sample_format = audio_format_to_sample_format(audio_codec_context_get_audio_format(audio_track.codec_context));
                // TODO: Always do conversion for now. This fixes issue with stuttering audio on pulseaudio with opus + multiple audio sources merged
                const bool needs_audio_conversion = true;//audio_track.codec_context->sample_fmt != sound_device_sample_format;
                SwrContext *swr = nullptr;
                if(needs_audio_conversion) {
                    swr = swr_alloc();
                    if(!swr) {
                        fprintf(stderr, "Failed to create SwrContext\n");
                        _exit(1);
                    }
                    #if LIBAVUTIL_VERSION_MAJOR <= 56
                    av_opt_set_channel_layout(swr, "in_channel_layout", AV_CH_LAYOUT_STEREO, 0);
                    av_opt_set_channel_layout(swr, "out_channel_layout", AV_CH_LAYOUT_STEREO, 0);
                    #elif LIBAVUTIL_VERSION_MAJOR >= 59
                    av_opt_set_chlayout(swr, "in_chlayout", &audio_track.codec_context->ch_layout, 0);
                    av_opt_set_chlayout(swr, "out_chlayout", &audio_track.codec_context->ch_layout, 0);
                    #else
                    av_opt_set_chlayout(swr, "in_channel_layout", &audio_track.codec_context->ch_layout, 0);
                    av_opt_set_chlayout(swr, "out_channel_layout", &audio_track.codec_context->ch_layout, 0);
                    #endif
                    av_opt_set_int(swr, "in_sample_rate", audio_track.codec_context->sample_rate, 0);
                    av_opt_set_int(swr, "out_sample_rate", audio_track.codec_context->sample_rate, 0);
                    av_opt_set_sample_fmt(swr, "in_sample_fmt", sound_device_sample_format, 0);
                    av_opt_set_sample_fmt(swr, "out_sample_fmt", audio_track.codec_context->sample_fmt, 0);
                    swr_init(swr);
                }

                const double audio_fps = (double)audio_track.codec_context->sample_rate / (double)audio_track.codec_context->frame_size;
                const int64_t timeout_ms = std::round(1000.0 / audio_fps);
                const double timeout_sec = 1000.0 / audio_fps / 1000.0;
                int64_t num_received_frames = 0;

                // The sound device is opened before the recording starts, so it can contain old audio from before the recording started.
                // Discard it so the recording doesn't start with old audio.
                if(audio_device.sound_device.handle)
                    sound_device_flush(&audio_device.sound_device);

                while(running) {
                    void *sound_buffer;
                    int sound_buffer_size = -1;
                    const double time_before_read_seconds = clock_get_monotonic_seconds();
                    if(audio_device.sound_device.handle) {
                        // TODO: use this instead of calculating time to read. But this can fluctuate and we dont want to go back in time,
                        // also it's 0.0 for some users???
                        double latency_seconds = 0.0;
                        sound_buffer_size = sound_device_read_next_chunk(&audio_device.sound_device, &sound_buffer, timeout_sec * 2.0, &latency_seconds);
                    }

                    const bool got_audio_data = sound_buffer_size >= 0;
                    //fprintf(stderr, "got audio data: %s\n", got_audio_data ? "yes" : "no");
                    //fprintf(stderr, "time to read: %f, %s, %f\n", time_to_read_seconds, got_audio_data ? "yes" : "no", timeout_sec);
                    const double this_audio_frame_time = clock_get_monotonic_seconds() - paused_time_offset;

                    if(paused) {
                        if(!audio_device.sound_device.handle)
                            av_usleep(timeout_ms * 1000);

                        continue;
                    }

                    int ret = av_frame_make_writable(audio_device.frame);
                    if (ret < 0) {
                        fprintf(stderr, "Failed to make audio frame writable\n");
                        break;
                    }

                    // TODO: Is this |received_audio_time| really correct?
                    const int64_t num_expected_frames = std::floor((this_audio_frame_time - record_start_time) / timeout_sec);
                    int64_t num_missing_frames = std::max((int64_t)0LL, num_expected_frames - num_received_frames);

                    if(got_audio_data)
                        num_missing_frames = std::max((int64_t)0LL, num_missing_frames - 1);

                    if(!audio_device.sound_device.handle)
                        num_missing_frames = std::max((int64_t)1, num_missing_frames);

                    // Fucking hell is there a better way to do this? I JUST WANT TO KEEP VIDEO AND AUDIO SYNCED HOLY FUCK I WANT TO KILL MYSELF NOW.
                    // THIS PIECE OF SHIT WANTS EMPTY FRAMES OTHERWISE VIDEO PLAYS TOO FAST TO KEEP UP WITH AUDIO OR THE AUDIO PLAYS TOO EARLY.
                    // BUT WE CANT USE DELAYS TO GIVE DUMMY DATA BECAUSE PULSEAUDIO MIGHT GIVE AUDIO A BIG DELAYED!!!
                    // This garbage is needed because we want to produce constant frame rate videos instead of variable frame rate
                    // videos because bad software such as video editing software and VLC do not support variable frame rate software,
                    // despite nvidia shadowplay and xbox game bar producing variable frame rate videos.
                    // So we have to make sure we produce frames at the same relative rate as the video.
                    if((num_missing_frames >= 1 && got_audio_data) || num_missing_frames >= 5 || !audio_device.sound_device.handle) {
                        // Fill the missing frames with silence. Duplicating the previous audio frame to fill the gap instead
                        // sounds like a stutter and it's especially noticeable at the start of the recording when the audio device
                        // hasn't started to deliver audio at a stable rate yet, which repeats the first audio frame multiple times.
                        if(needs_audio_conversion)
                            swr_convert(swr, &audio_device.frame->data[0], audio_track.codec_context->frame_size, (const uint8_t**)&empty_audio, audio_track.codec_context->frame_size);
                        else
                            audio_device.frame->data[0] = empty_audio;

                        // TODO: Check if duplicate frame can be saved just by writing it with a different pts instead of sending it again
                        std::lock_guard<std::mutex> lock(audio_filter_mutex);
                        for(int i = 0; i < num_missing_frames; ++i) {
                            if(audio_track.graph) {
                                // TODO: av_buffersrc_add_frame
                                if(av_buffersrc_write_frame(audio_device.src_filter_ctx, audio_device.frame) < 0) {
                                    gsr_log(GSR_LOG_LEVEL_ERROR, "failed to add audio frame to filter");
                                }
                            } else {
                                ret = avcodec_send_frame(audio_track.codec_context, audio_device.frame);
                                if(ret >= 0) {
                                    // TODO: Move to separate thread because this could write to network (for example when livestreaming)
                                    gsr_encoder_receive_packets(&encoder, audio_track.codec_context, audio_device.frame->pts, audio_track.stream_index);
                                } else {
                                    fprintf(stderr, "Failed to encode audio!\n");
                                }
                                audio_track.pts += audio_track.codec_context->frame_size;
                            }

                            audio_device.frame->pts += audio_track.codec_context->frame_size;
                            num_received_frames++;
                        }
                    }

                    if(!audio_device.sound_device.handle) {
                        av_usleep(timeout_ms * 1000);
                    } else if(got_audio_data) {
                        // The frame has to be made writable again if the frame was already sent to the audio filter above (when filling missing frames)
                        // because the audio filter only references the frame data instead of copying it. Without this the sent frames data would be
                        // overwritten with the audio data below, causing the audio to repeat instead of the missing frames being silent.
                        ret = av_frame_make_writable(audio_device.frame);
                        if (ret < 0) {
                            fprintf(stderr, "Failed to make audio frame writable\n");
                            break;
                        }

                        // TODO: Instead of converting audio, get float audio from alsa. Or does alsa do conversion internally to get this format?
                        if(needs_audio_conversion)
                            swr_convert(swr, &audio_device.frame->data[0], audio_track.codec_context->frame_size, (const uint8_t**)&sound_buffer, audio_track.codec_context->frame_size);
                        else
                            audio_device.frame->data[0] = (uint8_t*)sound_buffer;

                        std::lock_guard<std::mutex> lock(audio_filter_mutex);

                        if(audio_track.graph) {
                            // TODO: av_buffersrc_add_frame
                            if(av_buffersrc_write_frame(audio_device.src_filter_ctx, audio_device.frame) < 0) {
                                gsr_log(GSR_LOG_LEVEL_ERROR, "failed to add audio frame to filter");
                            }
                        } else {
                            ret = avcodec_send_frame(audio_track.codec_context, audio_device.frame);
                            if(ret >= 0) {
                                // TODO: Move to separate thread because this could write to network (for example when livestreaming)
                                gsr_encoder_receive_packets(&encoder, audio_track.codec_context, audio_device.frame->pts, audio_track.stream_index);
                            } else {
                                fprintf(stderr, "Failed to encode audio!\n");
                            }
                            audio_track.pts += audio_track.codec_context->frame_size;
                        }

                        audio_device.frame->pts += audio_track.codec_context->frame_size;
                        num_received_frames++;
                    } else {
                        // TODO: Maybe sleep for time_to_sleep_until_next_frame/4? for better latency
                        const double time_after_read_seconds = clock_get_monotonic_seconds();
                        const double time_to_read_seconds = time_after_read_seconds - time_before_read_seconds;
                        const double time_to_sleep_until_next_frame = timeout_sec - time_to_read_seconds;
                        if(time_to_sleep_until_next_frame > 0.0)
                            av_usleep(time_to_sleep_until_next_frame * 1000ULL * 1000ULL);
                    }
                }

                if(swr)
                    swr_free(&swr);
            });
        }
    }

    std::thread amix_thread;
    if(uses_amix) {
        amix_thread = std::thread([&]() {
            AVFrame *aframe = av_frame_alloc();
            while(running) {
                {
                    std::lock_guard<std::mutex> lock(audio_filter_mutex);
                    for(AudioTrack &audio_track : audio_tracks) {
                        if(!audio_track.sink)
                            continue;

                        int err = 0;
                        while ((err = av_buffersink_get_frame(audio_track.sink, aframe)) >= 0) {
                            aframe->pts = audio_track.pts;
                            err = avcodec_send_frame(audio_track.codec_context, aframe);
                            if(err >= 0){
                                // TODO: Move to separate thread because this could write to network (for example when livestreaming)
                                gsr_encoder_receive_packets(&encoder, audio_track.codec_context, aframe->pts, audio_track.stream_index);
                            } else {
                                fprintf(stderr, "Failed to encode audio!\n");
                            }
                            av_frame_unref(aframe);
                            audio_track.pts += audio_track.codec_context->frame_size;
                        }
                    }
                }
                av_usleep(5 * 1000); // 5 milliseconds
            }
            av_frame_free(&aframe);
        });
    }

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

        const double this_video_frame_time = clock_get_monotonic_seconds() - paused_time_offset;
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
                        paused_time_start = clock_get_monotonic_seconds();
                        paused = true;
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
                paused_time_offset = paused_time_offset + (clock_get_monotonic_seconds() - paused_time_start);
                paused = false;
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
            const bool new_paused_state = !paused;
            if(new_paused_state) {
                paused_time_start = clock_get_monotonic_seconds();
                fprintf(stderr, "Paused\n");
            } else {
                paused_time_offset = paused_time_offset + (clock_get_monotonic_seconds() - paused_time_start);
                fprintf(stderr, "Unpaused\n");
            }

            toggle_pause = 0;
            paused = !paused;
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
                std::lock_guard<std::mutex> lock(audio_filter_mutex);
                replay_recording_items.clear();
                replay_recording_filepath = create_new_recording_filepath_from_timestamp(arg_parser.settings.replay_recording_directory, "Video", file_extension, arg_parser.settings.date_folders);
                replay_recording_start_result = start_recording_create_streams(replay_recording_filepath.c_str(), arg_parser, video_codec_context, audio_tracks, hdr, video_sources);
                if(replay_recording_start_result.av_format_context) {
                    const size_t video_recording_destination_id = gsr_encoder_add_recording_destination(&encoder, video_codec_context, replay_recording_start_result.av_format_context, replay_recording_start_result.video_stream, video_frame->pts);
                    if(arg_parser.settings.write_first_frame_ts && video_recording_destination_id != (size_t)-1) {
                        std::string ts_filepath = replay_recording_filepath + ".ts";
                        gsr_encoder_set_recording_destination_first_frame_ts_filepath(&encoder, video_recording_destination_id, ts_filepath.c_str());
                    }

                    if(video_recording_destination_id != (size_t)-1)
                        replay_recording_items.push_back(video_recording_destination_id);

                    for(const auto &audio_input : replay_recording_start_result.audio_inputs) {
                        const size_t audio_recording_destination_id = gsr_encoder_add_recording_destination(&encoder, audio_input.audio_track->codec_context, replay_recording_start_result.av_format_context, audio_input.stream, audio_input.audio_track->pts);
                        if(audio_recording_destination_id != (size_t)-1)
                            replay_recording_items.push_back(audio_recording_destination_id);
                    }

                    replay_recording = true;
                    force_iframe_frame = true;
                    fprintf(stderr, "Started recording\n");
                } else {
                    printf("gsr error: Failed to start recording\n");
                    fflush(stdout);
                }
            } else if(replay_recording_start_result.av_format_context) {
                for(size_t id : replay_recording_items) {
                    gsr_encoder_remove_recording_destination(&encoder, id);
                }
                replay_recording_items.clear();

                if(stop_recording_close_streams(replay_recording_start_result.av_format_context)) {
                    fprintf(stderr, "Stopped recording\n");
                    puts(replay_recording_filepath.c_str());
                    fflush(stdout);
                    if(arg_parser.settings.recording_saved_script)
                        run_recording_saved_script_async(arg_parser.settings.recording_saved_script, replay_recording_filepath.c_str(), "regular");
                } else {
                    printf("gsr error: Failed to save recording\n");
                    fflush(stdout);
                }

                replay_recording_start_result = RecordingStartResult{};
                replay_recording = false;
                replay_recording_filepath.clear();
            }
        }

        if(save_replay_thread.valid() && save_replay_thread.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
            const bool replay_save_result = save_replay_thread.get();
            if(save_replay_output_filepath.empty() || !replay_save_result) {
                printf("gsr error: Failed to save replay\n");
                fflush(stdout);
            } else {
                puts(save_replay_output_filepath.c_str());
                fflush(stdout);
                if(arg_parser.settings.recording_saved_script)
                    run_recording_saved_script_async(arg_parser.settings.recording_saved_script, save_replay_output_filepath.c_str(), "replay");
            }
        }

        if(save_replay_seconds != 0 && !save_replay_thread.valid() && arg_parser.settings.is_replaying) {
            int current_save_replay_seconds = save_replay_seconds;
            if(current_save_replay_seconds > 0)
                current_save_replay_seconds += arg_parser.settings.keyint;

            save_replay_seconds = 0;
            save_replay_output_filepath.clear();
            const bool replay_start_result = save_replay_async(video_codec_context, VIDEO_STREAM_INDEX, audio_tracks, &encoder, arg_parser, file_extension, arg_parser.settings.date_folders, hdr, video_sources, current_save_replay_seconds);
            if(!replay_start_result) {
                printf("gsr error: Failed to save replay\n");
                fflush(stdout);
            }

            if(arg_parser.settings.restart_replay_on_save && current_save_replay_seconds == save_replay_seconds_full) {
                pthread_mutex_lock(&encoder.replay_mutex);
                gsr_replay_buffer_clear(encoder.replay_buffer);
                pthread_mutex_unlock(&encoder.replay_mutex);
            }
        }

        const double time_at_frame_end = clock_get_monotonic_seconds() - paused_time_offset;
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

    if(save_replay_thread.valid()) {
        save_replay_thread.get();
        if(save_replay_output_filepath.empty()) {
            // TODO: Output failed to save
        } else {
            puts(save_replay_output_filepath.c_str());
            fflush(stdout);
            if(arg_parser.settings.recording_saved_script)
                run_recording_saved_script_async(arg_parser.settings.recording_saved_script, save_replay_output_filepath.c_str(), "replay");
        }
    }

    gsr_plugins_deinit(&plugins);

    if(replay_recording_start_result.av_format_context) {
        for(size_t id : replay_recording_items) {
            gsr_encoder_remove_recording_destination(&encoder, id);
        }
        replay_recording_items.clear();

        if(stop_recording_close_streams(replay_recording_start_result.av_format_context)) {
            fprintf(stderr, "Stopped recording\n");
            puts(replay_recording_filepath.c_str());
            fflush(stdout);
            if(arg_parser.settings.recording_saved_script)
                run_recording_saved_script_async(arg_parser.settings.recording_saved_script, replay_recording_filepath.c_str(), "regular");
        } else {
            printf("gsr error: Failed to save recording\n");
            fflush(stdout);
        }
    }

    for(AudioTrack &audio_track : audio_tracks) {
        for(auto &audio_device : audio_track.audio_devices) {
            audio_device.thread.join();
            sound_device_close(&audio_device.sound_device);
        }
    }

    if(amix_thread.joinable())
        amix_thread.join();

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
    free(empty_audio);
    args_parser_deinit(&arg_parser);
    // We do an _exit here because cuda uses at_exit to do _something_ that causes the program to freeze,
    // but only on some nvidia driver versions on some gpus (RTX?), and _exit exits the program without calling
    // the at_exit registered functions.
    // Cuda (cuvid library in this case) seems to be waiting for a thread that never finishes execution.
    // Maybe this happens because we dont clean up all ffmpeg resources?
    // TODO: Investigate this.
    _exit(should_stop_error ? 3 : 0);
}
