/* OpenAL for PS3: not there yet. alutInit() fails, which makes SGSoundMgr mark
   itself as not working. Loading a sample still succeeds with a silent buffer:
   some instruments (MK VIII) exit when a sample cannot be loaded. */
#include <stdlib.h>
#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alut.h>

static ALuint next_id = 1;

ALboolean alutInit(int *argc, char **argv) { (void)argc; (void)argv; return AL_FALSE; }
ALboolean alutExit(void) { return AL_TRUE; }
ALenum alutGetError(void) { return ALUT_ERROR_NO_ERROR; }
const char *alutGetErrorString(ALenum e) { (void)e; return "no audio on PS3 yet"; }
ALuint alutCreateBufferFromFile(const char *f) { (void)f; return next_id++; }
ALvoid *alutLoadMemoryFromFile(const char *f, ALenum *fmt, ALsizei *size, ALfloat *freq)
{
    unsigned char *silence = (unsigned char *)calloc(1, 16);
    (void)f;
    if (silence) { silence[0] = 128; }
    *fmt = AL_FORMAT_MONO8;
    *size = 16;
    *freq = 22050.0f;
    return silence;
}

ALenum alGetError(void) { return AL_NO_ERROR; }
void alGenBuffers(ALsizei n, ALuint *b) { int i; for (i = 0; i < n; i++) b[i] = next_id++; }
void alDeleteBuffers(ALsizei n, const ALuint *b) { (void)n; (void)b; }
void alBufferData(ALuint b, ALenum fmt, const ALvoid *d, ALsizei size, ALsizei freq)
{ (void)b; (void)fmt; (void)d; (void)size; (void)freq; }
void alGenSources(ALsizei n, ALuint *s) { int i; for (i = 0; i < n; i++) s[i] = next_id++; }
void alDeleteSources(ALsizei n, const ALuint *s) { (void)n; (void)s; }
void alSourcef(ALuint s, ALenum p, ALfloat v) { (void)s; (void)p; (void)v; }
void alSourcefv(ALuint s, ALenum p, const ALfloat *v) { (void)s; (void)p; (void)v; }
void alSourcei(ALuint s, ALenum p, ALint v) { (void)s; (void)p; (void)v; }
void alGetSourcei(ALuint s, ALenum p, ALint *v) { (void)s; (void)p; *v = AL_STOPPED; }
void alSourcePlay(ALuint s) { (void)s; }
void alSourceStop(ALuint s) { (void)s; }
void alListenerf(ALenum p, ALfloat v) { (void)p; (void)v; }
void alListenerfv(ALenum p, const ALfloat *v) { (void)p; (void)v; }
void alDopplerFactor(ALfloat v) { (void)v; }
void alDopplerVelocity(ALfloat v) { (void)v; }
ALCcontext *alcGetCurrentContext(void) { return 0; }
void alcSuspendContext(ALCcontext *c) { (void)c; }
void alcProcessContext(ALCcontext *c) { (void)c; }
