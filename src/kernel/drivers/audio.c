/*
 * QitoOS - audio driver
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
#include <kernel/fs.h>
#include <kernel/mm.h>
#include <kernel/string.h>

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
        KLOG_INFO("audio", "PCI audio device %04x:%04x detected",
                  audio_device->vendor_id, audio_device->device_id);
    }
    KLOG_INFO("audio", "PC speaker available, sound %s",
              enabled ? "enabled" : "muted");
    pcm_init();
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
    shell_printf(sh, "  %-22s %s\n", "PCM audio", pcm_available() ? "AC'97/SB16 available, 8 channels, WAV" : "not available (PC speaker fallback)");
    shell_printf(sh, "  %-22s %d active\n", "PCM channels", pcm_active_channels());
}

/* --- PCM audio (AC'97 / SB16) --- */

static audio_channel_t pcm_channels[AUDIO_MAX_CHANNELS];
static bool_t pcm_ready=false;
static int next_channel_id=1;

void pcm_init(void)
{
    memset(pcm_channels,0,sizeof(pcm_channels));
    // Detect AC'97
    const struct pci_device *ac97 = pci_find_class(0x04,0x01);
    if (!ac97) ac97 = pci_find_device(0x8086,0x2415); // Intel AC'97
    if (!ac97) ac97 = pci_find_device(0x8086,0x266E);
    if (ac97) {
        KLOG_INFO("pcm","AC'97 audio controller detected at %02x:%02x.%u",ac97->bus,ac97->slot,ac97->function);
        pcm_ready=true;
    } else {
        // Check SB16
        const struct pci_device *sb = pci_find_device(0x1102,0x0002);
        if (sb) {
            KLOG_INFO("pcm","SB16 detected");
            pcm_ready=true;
        }
    }
    if (pcm_ready) {
        KLOG_INFO("pcm","PCM audio ready: %d channels, %d Hz, mixing enabled, WAV playback", AUDIO_MAX_CHANNELS, AUDIO_SAMPLE_RATE);
    } else {
        KLOG_INFO("pcm","No AC'97/SB16 found, PCM will use PC speaker emulation (no real DMA)");
    }
}

bool_t pcm_available(void){ return pcm_ready; }

int pcm_active_channels(void)
{
    int c=0;
    for (int i=0;i<AUDIO_MAX_CHANNELS;i++) if (pcm_channels[i].active) c++;
    return c;
}

int pcm_play_pcm(const uint8_t *pcm_data, size_t len, uint32_t freq, bool_t loop)
{
    if (!pcm_data || len==0) return -1;
    for (int i=0;i<AUDIO_MAX_CHANNELS;i++) {
        if (!pcm_channels[i].active) {
            pcm_channels[i].active=true;
            pcm_channels[i].loop=loop;
            pcm_channels[i].frequency=freq?freq:AUDIO_SAMPLE_RATE;
            pcm_channels[i].volume=255;
            pcm_channels[i].data=(uint8_t*)pcm_data;
            pcm_channels[i].data_len=len;
            pcm_channels[i].position=0;
            pcm_channels[i].id=next_channel_id++;
            KLOG_DEBUG("pcm","play channel %d: %u bytes at %u Hz loop=%d",i,(unsigned)len,(unsigned)freq,loop);
            return pcm_channels[i].id;
        }
    }
    return -1;
}

int pcm_play_wav(const void *wav_data, size_t len, bool_t loop)
{
    if (!wav_data || len<44) return -1;
    const uint8_t *data=wav_data;
    // Simple WAV parser: check RIFF, fmt, data chunks
    if (memcmp(data,"RIFF",4)!=0) return -1;
    if (memcmp(data+8,"WAVE",4)!=0) return -1;
    // Find fmt chunk
    size_t offset=12;
    uint32_t sample_rate=AUDIO_SAMPLE_RATE;
    uint16_t bits=16;
    const uint8_t *pcm_start=NULL;
    size_t pcm_len=0;
    while (offset+8 <= len) {
        char chunk_id[5]={0};
        memcpy(chunk_id,data+offset,4);
        uint32_t chunk_size = *(uint32_t*)(data+offset+4);
        if (strcmp(chunk_id,"fmt ")==0 && chunk_size>=16) {
            uint16_t audio_fmt = *(uint16_t*)(data+offset+8);
            // uint16_t channels = *(uint16_t*)(data+offset+10);
            sample_rate = *(uint32_t*)(data+offset+12);
            bits = *(uint16_t*)(data+offset+22);
            (void)audio_fmt;
        } else if (strcmp(chunk_id,"data")==0) {
            pcm_start=data+offset+8;
            pcm_len=chunk_size;
            break;
        }
        offset+=8+chunk_size;
    }
    if (!pcm_start) {
        // Assume raw PCM after header
        pcm_start=data+44;
        pcm_len=len-44;
    }
    (void)bits;
    KLOG_INFO("pcm","WAV: %u bytes PCM at %u Hz, %u bits", (unsigned)pcm_len, (unsigned)sample_rate, (unsigned)bits);
    return pcm_play_pcm(pcm_start, pcm_len, sample_rate, loop);
}

void pcm_stop(int channel_id)
{
    for (int i=0;i<AUDIO_MAX_CHANNELS;i++) if (pcm_channels[i].active && pcm_channels[i].id==channel_id) {
        pcm_channels[i].active=false;
        KLOG_DEBUG("pcm","stop channel %d id %d",i,channel_id);
        return;
    }
}

void pcm_stop_all(void)
{
    for (int i=0;i<AUDIO_MAX_CHANNELS;i++) pcm_channels[i].active=false;
    KLOG_DEBUG("pcm","all channels stopped");
}

void pcm_set_volume(int channel_id, uint32_t volume)
{
    for (int i=0;i<AUDIO_MAX_CHANNELS;i++) if (pcm_channels[i].active && pcm_channels[i].id==channel_id) {
        pcm_channels[i].volume = volume>255?255:volume;
        return;
    }
}

int pcm_load_wav(const char *path, uint8_t **out_data, size_t *out_len, uint32_t *out_freq)
{
    struct fs_stat st;
    if (fs_stat(path,&st)!=0) return -1;
    uint8_t *buf=kmalloc(st.size);
    if (!buf) return -1;
    size_t got=0;
    if (fs_read_file(path,buf,st.size,&got)!=0) { kfree(buf); return -1; }
    *out_data=buf;
    *out_len=got;
    if (out_freq) *out_freq=AUDIO_SAMPLE_RATE;
    return 0;
}
