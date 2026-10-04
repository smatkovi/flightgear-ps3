/* OpenAL for the PS3: the part of OpenAL 1.1 and ALUT 1.x that SimGear's
   sound manager uses, mixed on a thread of its own (the PPU's second
   hardware thread) into a 2-channel audio port at 48 kHz.

   Buffers hold 16-bit samples (8-bit and stereo are converted on loading).
   Sources: buffer, gain, pitch, looping, play/stop, position relative to
   the listener (inverse distance attenuation with reference and maximum
   distance, panning by the sideways offset). Cones and Doppler are ignored.
   The mixer thread must not touch stdio (newlib's is per thread here). */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <malloc.h>
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alut.h>
#include <audio/audio.h>
#include <sys/thread.h>
#include <lv2/thread.h>
#include <sys/mutex.h>
#include <sys/event_queue.h>

#define MAX_BUFFERS 1024
#define MAX_SOURCES 256
#define RATE        48000.0f

typedef struct {
    int used;
    short *data;            /* frames of `channels` samples */
    int frames, channels;
    float freq;
} Buffer;

typedef struct {
    int used;
    ALuint buffer;
    ALint state;            /* AL_INITIAL, AL_PLAYING, AL_STOPPED */
    int looping, relative;
    float gain, pitch, ref_dist, max_dist, rolloff;
    float pos[3];
    double cursor;          /* position in the buffer, in frames */
} Source;

static Buffer buffers[MAX_BUFFERS];
static Source sources[MAX_SOURCES];
static float listener_gain = 1.0f, listener_pos[3];
static ALenum last_error = AL_NO_ERROR;
static ALenum alut_error = ALUT_ERROR_NO_ERROR;

static sys_mutex_t lock;
static int running;
static u32 port;
static audioPortConfig config;
static sys_event_queue_t queue;
static sys_ipc_key_t queue_key;
static sys_ppu_thread_t mixer;
static unsigned long blocks_mixed;
static int sources_playing;
static float peak;

static void take(void) { if (running) sysMutexLock(lock, 0); }
static void give(void) { if (running) sysMutexUnlock(lock); }
static void set_error(ALenum e) { if (last_error == AL_NO_ERROR) last_error = e; }

static Buffer *buf_of(ALuint id) { return id && id < MAX_BUFFERS && buffers[id].used ? &buffers[id] : NULL; }
static Source *src_of(ALuint id) { return id && id < MAX_SOURCES && sources[id].used ? &sources[id] : NULL; }

/* ---------------------------------------------------------------- mixing */

/* Adds one source to a block of `n` stereo frames */
static void mix_source(Source *s, float *out, int n)
{
    Buffer *b = buf_of(s->buffer);
    float gain, pan = 0.0f, gl, gr, step;
    double dist;
    int i;

    if (!b || !b->frames) { s->state = AL_STOPPED; return; }
    gain = s->gain * listener_gain;
    {   /* inverse distance, clamped (OpenAL's default model) */
        float dx = s->pos[0], dy = s->pos[1], dz = s->pos[2];
        if (!s->relative) { dx -= listener_pos[0]; dy -= listener_pos[1]; dz -= listener_pos[2]; }
        dist = sqrt(dx * dx + dy * dy + dz * dz);
        if (dist > 0.001) pan = dx / dist;
        if (dist < s->ref_dist) dist = s->ref_dist;
        if (dist > s->max_dist) dist = s->max_dist;
        if (s->ref_dist > 0.0f)
            gain *= s->ref_dist / (s->ref_dist + s->rolloff * (dist - s->ref_dist));
    }
    if (gain < 0.0001f) {       /* inaudible: just move on */
        step = s->pitch * b->freq / RATE;
        s->cursor += step * n;
        if (s->cursor >= b->frames) {
            if (s->looping) s->cursor = fmod(s->cursor, (double)b->frames);
            else s->state = AL_STOPPED;
        }
        return;
    }
    gl = gain * sqrtf((1.0f - pan) * 0.5f);
    gr = gain * sqrtf((1.0f + pan) * 0.5f);
    step = s->pitch * b->freq / RATE;
    for (i = 0; i < n; i++) {
        int f = (int)s->cursor, f2 = f + 1;
        float t = (float)(s->cursor - f), l, r;
        if (f2 >= b->frames) f2 = s->looping ? 0 : f;
        if (b->channels == 2) {
            l = b->data[2 * f] + (b->data[2 * f2] - b->data[2 * f]) * t;
            r = b->data[2 * f + 1] + (b->data[2 * f2 + 1] - b->data[2 * f + 1]) * t;
        } else {
            l = r = b->data[f] + (b->data[f2] - b->data[f]) * t;
        }
        out[2 * i] += l * (1.0f / 32768.0f) * gl;
        out[2 * i + 1] += r * (1.0f / 32768.0f) * gr;
        s->cursor += step;
        if (s->cursor >= b->frames) {
            if (!s->looping) { s->state = AL_STOPPED; s->cursor = 0; return; }
            s->cursor -= b->frames;
        }
    }
}

static void mixer_main(void *arg)
{
    float *ring = (float *)(uintptr_t)config.audioDataStart;
    float mix[AUDIO_BLOCK_SAMPLES * 2];
    sys_event_t ev;
    (void)arg;
    while (running) {
        u64 block;
        float *dst;
        int i, k;
        sysEventQueueReceive(queue, &ev, 20 * 1000);
        if (!running) break;
        /* fill the block after the one being played */
        block = *(volatile u64 *)(uintptr_t)config.readIndex;
        dst = ring + config.channelCount * AUDIO_BLOCK_SAMPLES * ((block + 1) % config.numBlocks);
        memset(mix, 0, sizeof mix);
        sysMutexLock(lock, 0);
        for (k = 1, i = 0; k < MAX_SOURCES; k++)
            if (sources[k].used && sources[k].state == AL_PLAYING) {
                mix_source(&sources[k], mix, AUDIO_BLOCK_SAMPLES);
                i++;
            }
        sources_playing = i;
        sysMutexUnlock(lock);
        for (i = 0; i < AUDIO_BLOCK_SAMPLES * 2; i++) {      /* clip */
            float v = mix[i];
            dst[i] = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
            if (v > peak) peak = v;
            else if (-v > peak) peak = -v;
        }
        blocks_mixed++;
    }
    sysThreadExit(0);
}

/* For the log: is sound coming out? (peak since the last report) */
void al_ps3_report(char *buf, int n)
{
    snprintf(buf, n, "audio: %s, %lu blocks mixed, %d sources playing, peak %.3f",
             running ? "running" : "off", blocks_mixed, sources_playing, peak);
    peak = 0.0f;
}

/* ---------------------------------------------------------------- ALUT */

ALboolean alutInit(int *argc, char **argv)
{
    audioPortParam params;
    sys_mutex_attr_t attr;
    (void)argc; (void)argv;
    if (running) return AL_TRUE;
    if (audioInit() != 0) { alut_error = ALUT_ERROR_OPEN_DEVICE; return AL_FALSE; }
    memset(&params, 0, sizeof params);
    params.numChannels = AUDIO_PORT_2CH;
    params.numBlocks = AUDIO_BLOCK_8;
    params.attrib = 0;
    params.level = 1.0f;
    if (audioPortOpen(&params, &port) != 0 || audioGetPortConfig(port, &config) != 0) {
        audioQuit();
        alut_error = ALUT_ERROR_OPEN_DEVICE;
        return AL_FALSE;
    }
    memset((void *)(uintptr_t)config.audioDataStart, 0, config.portSize);
    if (audioCreateNotifyEventQueue(&queue, &queue_key) != 0 || audioSetNotifyEventQueue(queue_key) != 0) {
        audioPortClose(port);
        audioQuit();
        alut_error = ALUT_ERROR_OPEN_DEVICE;
        return AL_FALSE;
    }
    sysEventQueueDrain(queue);
    sysMutexAttrInitialize(attr);
    sysMutexCreate(&lock, &attr);
    running = 1;
    audioPortStart(port);
    /* higher priority than the main thread (1001), so it is never starved */
    if (sysThreadCreate(&mixer, mixer_main, NULL, 100, 0x10000, THREAD_JOINABLE, "ps3-audio-mixer") != 0) {
        running = 0;
        audioPortStop(port);
        audioPortClose(port);
        audioQuit();
        alut_error = ALUT_ERROR_OPEN_DEVICE;
        return AL_FALSE;
    }
    return AL_TRUE;
}

ALboolean alutExit(void)
{
    u64 rv;
    if (!running) return AL_TRUE;
    running = 0;
    sysThreadJoin(mixer, &rv);
    audioPortStop(port);
    audioRemoveNotifyEventQueue(queue_key);
    audioPortClose(port);
    audioQuit();
    sysMutexDestroy(lock);
    return AL_TRUE;
}

ALenum alutGetError(void) { ALenum e = alut_error; alut_error = ALUT_ERROR_NO_ERROR; return e; }
const char *alutGetErrorString(ALenum e)
{
    switch (e) {
    case ALUT_ERROR_NO_ERROR: return "no error";
    case ALUT_ERROR_OPEN_DEVICE: return "the PS3 audio output could not be opened";
    case ALUT_ERROR_IO_ERROR: return "cannot read the sound file";
    case ALUT_ERROR_UNSUPPORTED_FILE_TYPE: return "not a PCM WAV file";
    case ALUT_ERROR_OUT_OF_MEMORY: return "out of memory";
    default: return "sound error";
    }
}

static unsigned rd16(const unsigned char *p) { return p[0] | p[1] << 8; }
static unsigned rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }

/* A PCM WAV file in memory -> OpenAL format and data (little-endian
   16-bit samples are swapped to the PS3's byte order) */
static ALvoid *parse_wav(unsigned char *d, long len, ALenum *fmt, ALsizei *size, ALfloat *freq)
{
    long p = 12;
    unsigned ch = 0, bits = 0, rate = 0;
    if (len < 12 || memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) {
        alut_error = ALUT_ERROR_UNSUPPORTED_FILE_TYPE;
        return NULL;
    }
    while (p + 8 <= len) {
        unsigned clen = rd32(d + p + 4);
        if (!memcmp(d + p, "fmt ", 4) && clen >= 16 && p + 8 + 16 <= len) {
            if (rd16(d + p + 8) != 1) break;            /* not PCM */
            ch = rd16(d + p + 10);
            rate = rd32(d + p + 12);
            bits = rd16(d + p + 22);
        } else if (!memcmp(d + p, "data", 4) && ch && rate) {
            unsigned char *out;
            long n = clen;
            if (p + 8 + n > len) n = len - p - 8;
            if ((ch != 1 && ch != 2) || (bits != 8 && bits != 16)) break;
            out = (unsigned char *)malloc(n ? n : 1);
            if (!out) { alut_error = ALUT_ERROR_OUT_OF_MEMORY; return NULL; }
            memcpy(out, d + p + 8, n);
            if (bits == 16) {
                long i;
                for (i = 0; i + 1 < n; i += 2) { unsigned char t = out[i]; out[i] = out[i + 1]; out[i + 1] = t; }
            }
            *fmt = ch == 1 ? (bits == 8 ? AL_FORMAT_MONO8 : AL_FORMAT_MONO16)
                           : (bits == 8 ? AL_FORMAT_STEREO8 : AL_FORMAT_STEREO16);
            *size = (ALsizei)n;
            *freq = (ALfloat)rate;
            return out;
        }
        p += 8 + clen + (clen & 1);
    }
    alut_error = ALUT_ERROR_UNSUPPORTED_FILE_TYPE;
    return NULL;
}

ALvoid *alutLoadMemoryFromFile(const char *f, ALenum *fmt, ALsizei *size, ALfloat *freq)
{
    FILE *fp = fopen(f, "rb");
    unsigned char *d;
    long len;
    ALvoid *res;
    if (!fp) { alut_error = ALUT_ERROR_IO_ERROR; return NULL; }
    fseek(fp, 0, SEEK_END);
    len = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    d = (unsigned char *)malloc(len > 0 ? len : 1);
    if (!d || fread(d, 1, len, fp) != (size_t)len) {
        fclose(fp);
        free(d);
        alut_error = ALUT_ERROR_IO_ERROR;
        return NULL;
    }
    fclose(fp);
    res = parse_wav(d, len, fmt, size, freq);
    free(d);
    return res;
}

/* A file that cannot be read gives a silent buffer, not AL_NONE: some
   instruments (MK VIII) stop FlightGear when a sample cannot be loaded. */
ALuint alutCreateBufferFromFile(const char *f)
{
    static const unsigned char silence[16] = { 128, 128, 128, 128, 128, 128, 128, 128,
                                               128, 128, 128, 128, 128, 128, 128, 128 };
    ALenum fmt;
    ALsizei size;
    ALfloat freq;
    ALuint b = 0;
    ALvoid *data = alutLoadMemoryFromFile(f, &fmt, &size, &freq);
    alGenBuffers(1, &b);
    if (!b) { free(data); return AL_NONE; }
    if (data) alBufferData(b, fmt, data, size, (ALsizei)freq);
    else alBufferData(b, AL_FORMAT_MONO8, silence, sizeof silence, 22050);
    free(data);
    return b;
}

/* ---------------------------------------------------------------- AL */

ALenum alGetError(void) { ALenum e = last_error; last_error = AL_NO_ERROR; return e; }

void alGenBuffers(ALsizei n, ALuint *ids)
{
    int i, k = 1;
    take();
    for (i = 0; i < n; i++) {
        while (k < MAX_BUFFERS && buffers[k].used) k++;
        if (k == MAX_BUFFERS) { ids[i] = 0; set_error(AL_OUT_OF_MEMORY); continue; }
        memset(&buffers[k], 0, sizeof(Buffer));
        buffers[k].used = 1;
        buffers[k].freq = 22050.0f;
        ids[i] = k;
    }
    give();
}

void alDeleteBuffers(ALsizei n, const ALuint *ids)
{
    int i;
    take();
    for (i = 0; i < n; i++) {
        Buffer *b = buf_of(ids[i]);
        if (!b) continue;
        free(b->data);
        memset(b, 0, sizeof(Buffer));
    }
    give();
}

void alBufferData(ALuint id, ALenum fmt, const ALvoid *data, ALsizei size, ALsizei freq)
{
    Buffer *b;
    short *d;
    int ch = (fmt == AL_FORMAT_STEREO8 || fmt == AL_FORMAT_STEREO16) ? 2 : 1;
    int bits = (fmt == AL_FORMAT_MONO8 || fmt == AL_FORMAT_STEREO8) ? 8 : 16;
    int frames = size / (ch * bits / 8), i;
    const unsigned char *s8 = (const unsigned char *)data;
    const short *s16 = (const short *)data;

    d = (short *)malloc((size_t)(frames > 0 ? frames : 1) * ch * sizeof(short));
    if (!d) { set_error(AL_OUT_OF_MEMORY); return; }
    for (i = 0; i < frames * ch; i++)
        d[i] = bits == 8 ? (short)((s8[i] - 128) << 8) : s16[i];
    take();
    b = buf_of(id);
    if (!b) { give(); free(d); set_error(AL_INVALID_NAME); return; }
    free(b->data);
    b->data = d;
    b->frames = frames;
    b->channels = ch;
    b->freq = freq > 0 ? (float)freq : 22050.0f;
    give();
}

void alGenSources(ALsizei n, ALuint *ids)
{
    int i, k = 1;
    take();
    for (i = 0; i < n; i++) {
        while (k < MAX_SOURCES && sources[k].used) k++;
        if (k == MAX_SOURCES) { ids[i] = 0; set_error(AL_OUT_OF_MEMORY); continue; }
        memset(&sources[k], 0, sizeof(Source));
        sources[k].used = 1;
        sources[k].state = AL_INITIAL;
        sources[k].gain = 1.0f;
        sources[k].pitch = 1.0f;
        sources[k].ref_dist = 1.0f;
        sources[k].max_dist = 1e30f;
        sources[k].rolloff = 1.0f;
        ids[i] = k;
    }
    give();
}

void alDeleteSources(ALsizei n, const ALuint *ids)
{
    int i;
    take();
    for (i = 0; i < n; i++) {
        Source *s = src_of(ids[i]);
        if (s) memset(s, 0, sizeof(Source));
    }
    give();
}

void alSourcef(ALuint id, ALenum p, ALfloat v)
{
    Source *s;
    take();
    s = src_of(id);
    if (!s) { give(); set_error(AL_INVALID_NAME); return; }
    switch (p) {
    case AL_GAIN:               s->gain = v < 0 ? 0 : v; break;
    case AL_PITCH:              s->pitch = v > 0 ? v : 0.0001f; break;
    case AL_REFERENCE_DISTANCE: s->ref_dist = v; break;
    case AL_MAX_DISTANCE:       s->max_dist = v; break;
    case AL_ROLLOFF_FACTOR:     s->rolloff = v; break;
    default: break;             /* cones, min/max gain: not used here */
    }
    give();
}

void alSourcefv(ALuint id, ALenum p, const ALfloat *v)
{
    Source *s;
    take();
    s = src_of(id);
    if (!s) { give(); set_error(AL_INVALID_NAME); return; }
    if (p == AL_POSITION) { s->pos[0] = v[0]; s->pos[1] = v[1]; s->pos[2] = v[2]; }
    give();                     /* velocity, direction: no Doppler or cones */
}

void alSourcei(ALuint id, ALenum p, ALint v)
{
    Source *s;
    take();
    s = src_of(id);
    if (!s) { give(); set_error(AL_INVALID_NAME); return; }
    switch (p) {
    case AL_BUFFER:          s->buffer = (ALuint)v; s->cursor = 0; break;
    case AL_LOOPING:         s->looping = v != 0; break;
    case AL_SOURCE_RELATIVE: s->relative = v != 0; break;
    default: break;
    }
    give();
}

void alGetSourcei(ALuint id, ALenum p, ALint *v)
{
    Source *s;
    take();
    s = src_of(id);
    if (!s) { give(); set_error(AL_INVALID_NAME); *v = AL_STOPPED; return; }
    switch (p) {
    case AL_SOURCE_STATE: *v = s->state; break;
    case AL_BUFFER:       *v = (ALint)s->buffer; break;
    case AL_LOOPING:      *v = s->looping; break;
    default:              *v = 0; break;
    }
    give();
}

void alSourcePlay(ALuint id)
{
    Source *s;
    take();
    s = src_of(id);
    if (s) {
        if (s->state != AL_PLAYING) s->cursor = 0;
        s->state = AL_PLAYING;
    }
    give();
}

void alSourceStop(ALuint id)
{
    Source *s;
    take();
    s = src_of(id);
    if (s) { s->state = AL_STOPPED; s->cursor = 0; }
    give();
}

void alListenerf(ALenum p, ALfloat v)
{
    if (p == AL_GAIN) listener_gain = v < 0 ? 0 : v;
}

void alListenerfv(ALenum p, const ALfloat *v)
{
    if (p == AL_POSITION) { take(); listener_pos[0] = v[0]; listener_pos[1] = v[1]; listener_pos[2] = v[2]; give(); }
}

void alDopplerFactor(ALfloat v) { (void)v; }
void alDopplerVelocity(ALfloat v) { (void)v; }
ALCcontext *alcGetCurrentContext(void) { return 0; }
void alcSuspendContext(ALCcontext *c) { (void)c; }
void alcProcessContext(ALCcontext *c) { (void)c; }
