/*
 * Qira OS - audio
 *
 * Qira drives the PC speaker for system sounds and detects (but does not yet
 * drive) PCI sound cards. The speaker is genuinely useful: it gives the
 * desktop audible feedback and works on every PC and emulator.
 */
#ifndef QIRA_AUDIO_H
#define QIRA_AUDIO_H

#include <kernel/types.h>

struct shell;

void audio_init(void);

/* PC speaker tone generation. */
void audio_tone(uint32_t frequency_hz);
void audio_silence(void);
void audio_beep(uint32_t frequency_hz, uint32_t duration_ms);

/* Play a sequence of (frequency, duration) pairs. */
struct audio_note {
    uint32_t frequency_hz;   /* 0 for a rest */
    uint32_t duration_ms;
};
void audio_play(const struct audio_note *notes, int count);

/* System sounds used by the desktop. */
void audio_startup_chime(void);
void audio_notify(void);
void audio_error_sound(void);

bool_t audio_enabled(void);
void   audio_set_enabled(bool_t enabled);

void audio_print_info(struct shell *sh);

#endif /* QIRA_AUDIO_H */
