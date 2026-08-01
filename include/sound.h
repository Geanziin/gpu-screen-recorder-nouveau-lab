/*
    Copyright (C) 2020 dec05eba

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#ifndef GSR_SOUND_H
#define GSR_SOUND_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *handle;
    unsigned int frames;
} SoundDevice;

typedef struct {
    char name[256];
    char description[256];
} gsr_audio_device;

typedef struct {
    char default_output[256];
    char default_input[256];
    gsr_audio_device *items;
    size_t num_items;
    size_t capacity_items;
} gsr_audio_devices;

typedef enum {
    GSR_AUDIO_FORMAT_S16,
    GSR_AUDIO_FORMAT_S32,
    GSR_AUDIO_FORMAT_F32
} gsr_audio_format;

/*
    Get a sound device by name, returning the device into the |device| parameter.
    |device_name| can be a device name or "default_output", "default_input" or "" to not connect to any device (used for app audio for example).
    If the device name is "default_output" or "default_input" then it will automatically switch which
    device is records from when the default output/input is changed in the system audio settings.
    Returns 0 on success, or a negative value on failure.
*/
int sound_device_get_by_name(SoundDevice *device, const char *node_name, const char *device_name, const char *description, unsigned int num_channels, unsigned int period_frame_size, gsr_audio_format audio_format);

void sound_device_close(SoundDevice *device);

/*
    Discards the audio that has been captured so far.
    Call this before the first call to sound_device_read_next_chunk to not get audio that was captured before that point,
    since the sound device can be created a while before audio capture starts.
*/
void sound_device_flush(SoundDevice *device);

/*
    Returns the next chunk of audio into @buffer.
    Returns the number of frames read, or a negative value on failure.
*/
int sound_device_read_next_chunk(SoundDevice *device, void **buffer, double timeout_sec, double *latency_seconds);

void get_pulseaudio_inputs(gsr_audio_devices *audio_devices);
void gsr_audio_devices_deinit(gsr_audio_devices *self);
bool pulseaudio_server_is_pipewire(void);

#ifdef __cplusplus
}
#endif

#endif /* GSR_SOUND_H */
