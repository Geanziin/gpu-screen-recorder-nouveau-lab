#ifndef GSR_CODEC_QUERY_NVENC_H
#define GSR_CODEC_QUERY_NVENC_H

#include "codec_query.h"

bool gsr_get_supported_video_codecs_nvenc(gsr_supported_video_codecs *video_codecs, const char *card_path, bool cleanup);

#endif /* GSR_CODEC_QUERY_NVENC_H */
