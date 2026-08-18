/*
 * Qira OS - audio driver
 *
 * Tone generation through the PC speaker, which is driven by channel 2 of the
 * 8253/8254 timer gated by port 0x61. PCI audio devices are detected and
 * reported but not yet driven.
 */

#include <kernel/audio.h>
#include <kernel/io.h>
#include <kernel/log.h>
#include <kernel/time.h>
#include <kernel/pci.h>
#include <kernel/shell.h>
#include <kernel/config.h>

#define PIT_CHANNEL2  0x42
#define PIT_COMMAND   0x43
#define SPEAKER_PORT  0x61
#define PIT_FREQUENCY 1193182u

static bool_t enabled = true;
static bool_t playing;
static const struct pci_device *audio_device;

void audio_tone(uint32_t frequency_hz)
{
    if (!enabled || frequency_hz < 20 || frequency_hz > 20000) {
        audio_silence();
        return;
    }

    uint32_t divisor = PIT_FREQUENCY / frequency_hz;
    if (divisor > 65535) {
        divisor = 65535;
    }

    /* Channel 2, lobyte/hibyte, square wave (mode 3). */
    outb(PIT_COMMAND, 0xB6);
    outb(PIT_CHANNEL2, (uint8_t)(divisor & 0xFF));
    outb(PIT_CHANNEL2, (uint8_t)(divisor >> 8));

    /* Connect the timer output to the speaker. */
    uint8_t control = inb(SPEAKER_PORT);
    outb(SPEAKER_PORT, (uint8_t)(control | 0x03));
    playing = true;
}

void audio_silence(void)
{
    uint8_t control = inb(SPEAKER_PORT);
    outb(SPEAKER_PORT, (uint8_t)(control & ~0x03));
    playing = false;
}

void audio_beep(uint32_t frequency_hz, uint32_t duration_ms)
{
    if (!enabled) {
        return;
    }
    audio_tone(frequency_hz);
    time_sleep_ms(duration_ms);
    audio_silence();
}

void audio_play(const struct audio_note *notes, int count)
{
    if (!enabled) {
        return;
    }
    for (int i = 0; i < count; i++) {
        if (notes[i].frequency_hz == 0) {
            audio_silence();
        } else {
            audio_tone(notes[i].frequency_hz);
        }
        time_sleep_ms(notes[i].duration_ms);
    }
    audio_silence();
}

void audio_startup_chime(void)
{
    /* A rising major triad: C5, E5, G5, C6. */
    static const struct audio_note chime[] = {
        {523, 90}, {659, 90}, {784, 90}, {1047, 160}, {0, 40},
    };
    audio_play(chime, (int)ARRAY_SIZE(chime));
}

void audio_notify(void)
{
    static const struct audio_note notes[] = {{880, 60}, {1175, 90}, {0, 20}};
    audio_play(notes, (int)ARRAY_SIZE(notes));
}

void audio_error_sound(void)
{
    static const struct audio_note notes[] = {{330, 120}, {247, 180}, {0, 20}};
    audio_play(notes, (int)ARRAY_SIZE(notes));
}

bool_t audio_enabled(void)
{
    return enabled;
}

void audio_set_enabled(bool_t value)
{
    enabled = value;
    if (!enabled) {
        audio_silence();
    }
}

void audio_init(void)
{
    audio_silence();
    enabled = config_get_bool("audio.enabled", true);

    /* Look for a PCI audio device (class 0x04). */
    audio_device = pci_find_class(0x04, 0x01);
    if (!audio_device) {
        audio_device = pci_find_class(0x04, 0x03);
    }

    if (audio_device) {
        KLOG_INFO("audio", "PCI audio device %04x:%04x detected (no driver yet)",
                  audio_device->vendor_id, audio_device->device_id);
    }
    KLOG_INFO("audio", "PC speaker available, sound %s",
              enabled ? "enabled" : "muted");
}

void audio_print_info(struct shell *sh)
{
    shell_printf(sh, "  %-22s %s\n", "PC speaker",
                 enabled ? "available" : "available (muted)");
    shell_printf(sh, "  %-22s %s\n", "Currently playing", playing ? "yes" : "no");

    if (audio_device) {
        shell_printf(sh, "  %-22s %04x:%04x %s (no driver)\n", "PCI audio",
                     audio_device->vendor_id, audio_device->device_id,
                     pci_vendor_name(audio_device->vendor_id));
    } else {
        shell_printf(sh, "  %-22s none detected\n", "PCI audio");
    }
}
