/*
 * QitoOS - audio
 *
 * Tone generation through PC speaker, plus PCM mixing (AC'97/SB16) for WAV playback
 */

#ifndef QITO_AUDIO_H
#define QITO_AUDIO_H

#include <kernel/types.h>

struct shell;

/* PC speaker notes */
struct audio_note {
    uint32_t frequency_hz;
    uint32_t duration_ms;
};

void audio_init(void);
void audio_tone(uint32_t frequency_hz);
void audio_silence(void);
void audio_beep(uint32_t frequency_hz, uint32_t duration_ms);
void audio_play(const struct audio_note *notes, int count);
void audio_startup_chime(void);
void audio_notify(void);
void audio_error_sound(void);
bool_t audio_enabled(void);
void audio_set_enabled(bool_t enabled);
void audio_print_info(struct shell *sh);

/* PCM audio (AC'97 or SB16) - new for Minecraft and games */
#define AUDIO_MAX_CHANNELS 8
#define AUDIO_SAMPLE_RATE 44100
#define AUDIO_BUFFER_SIZE 4096

typedef struct {
    bool_t active;
    bool_t loop;
    uint32_t frequency;
    uint32_t volume; /* 0-255 */
    uint8_t *data;
    size_t data_len;
    size_t position;
    int id;
} audio_channel_t;

void pcm_init(void);
int  pcm_play_wav(const void *wav_data, size_t len, bool_t loop);
int  pcm_play_pcm(const uint8_t *pcm_data, size_t len, uint32_t freq, bool_t loop);
void pcm_stop(int channel_id);
void pcm_stop_all(void);
void pcm_set_volume(int channel_id, uint32_t volume);
int  pcm_active_channels(void);
int  pcm_load_wav(const char *path, uint8_t **out_data, size_t *out_len, uint32_t *out_freq);
bool_t pcm_available(void);

#endif /* QITO_AUDIO_H */
