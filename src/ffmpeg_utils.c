#include "../include/ffmpeg_utils.h"

#include <string.h>
#include <libavutil/error.h>

static _Thread_local char av_error_buffer[AV_ERROR_MAX_STRING_SIZE];

const char* gsr_av_error_to_string(int err) {
    if(av_strerror(err, av_error_buffer, sizeof(av_error_buffer)) < 0)
        strcpy(av_error_buffer, "Unknown error");
    return av_error_buffer;
}
