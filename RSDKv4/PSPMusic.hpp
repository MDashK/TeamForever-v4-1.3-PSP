// Music as ATRAC3 on the psp (included by Audio.cpp)
//
// tools/build_psp_data.py stores every track in Data.psp as ATRAC3, under the track's .ogg path. The PSP's Media Engine
// decodes it (sceAtrac) while it streams from the pack through a small buffer: no Vorbis decoding on the main CPU (~5ms
// a frame) and no whole track in RAM. A track that isn't ATRAC3 (no Data.psp: Data.rsdk + mods) plays as Vorbis as before.
//
// Every sceAtrac call and every read of the track is the music loader thread's (MusicLoadThread, which wakes up every few
// ms for this): it decodes ahead into a ring, and the mixer (SDL's audio thread) only copies from the ring. Should the
// Media Engine stop answering (seen on Sonic Mania PSP), only the music stops.

#include <pspatrac3.h>
#include <psputility.h>
#include <pspiofilemgr.h>
#include <pspthreadman.h>
#include <unistd.h>

#define PSP_AT3_BUFFER (64 * 1024) // ~4s at 132kbps
#define PSP_AT3_READ   (16 * 1024) // the most read at once
#define PSP_AT3_RING   (16384)     // int16s (a power of two): 8192 stereo samples, ~186ms

static byte pspAt3Buffer[PSP_AT3_BUFFER] __attribute__((aligned(64)));
static Sint16 pspAt3Pcm[2048] __attribute__((aligned(64))); // one decoded frame: 1024 stereo samples
static Sint16 pspAt3Ring[PSP_AT3_RING] __attribute__((aligned(64)));
static volatile uint pspAt3RingRead, pspAt3RingWrite; // int16s taken/added so far

static int pspAt3ID         = -1;
static SceUID pspAt3File    = -1;
static int pspAt3Base       = 0; // where the track starts in the pack
static bool pspAt3Modules   = false;
static bool pspAvcodecLoaded = false, pspAtracLoaded = false; // (loaded by us: PSPMusicUnloadModules)
static bool pspAt3Streaming = false; // larger than the buffer: topped up from the pack as it plays
static bool pspAt3Ended     = false; // the decoder gave its last frame
static bool pspAt3Loop      = false;
static uint pspAt3LoopPoint = 0;
static int pspAt3Fails      = 0;
static char pspAt3Dir[0x100];

// sleep mode closes the file and powers the Media Engine down (its decoder is lost): the track is opened again where
// it had got to (main.cpp counts the wake-ups)
extern volatile int pspResumeCount;
static int pspAt3Resume = 0;        // the wake-up the open track was set up after
static char pspAt3Track[0x40];      // the open track, to set it up again
static int pspAt3ResumeTries = 0;   // services left to try that (the Memory Stick takes a moment to come back)
static uint pspAt3ResumeSample = 0;

static volatile uint pspAt3NextSample = 0; // the decoder's position (in samples)
static volatile bool pspAt3Done       = false; // ended for good (not looping): the mixer stops once the ring is empty
volatile bool pspMusicAt3             = false; // the playing track is this one (the mixer reads the ring)
volatile bool pspMusicCloseRequest    = false; // StopMusic: the loader thread lets the decoder go

static void PSPMusicInit()
{
    if (!getcwd(pspAt3Dir, sizeof(pspAt3Dir)))
        pspAt3Dir[0] = 0;
    int avcodec = sceUtilityLoadModule(PSP_MODULE_AV_AVCODEC);
    int atrac   = sceUtilityLoadModule(PSP_MODULE_AV_ATRAC3PLUS);
    // 0x80020139: already loaded
    pspAt3Modules = (avcodec >= 0 || avcodec == (int)0x80020139) && (atrac >= 0 || atrac == (int)0x80020139);
    pspAvcodecLoaded = avcodec >= 0;
    pspAtracLoaded   = atrac >= 0;
    PSP_BOOTLOG("music: ATRAC3 modules %08X %08X", avcodec, atrac);
}

// On exit: the modules go again (the XMB drops them anyway; under PSPLink, loading them again in the next run blocked)
static void PSPMusicUnloadModules()
{
    if (pspAtracLoaded)
        sceUtilityUnloadModule(PSP_MODULE_AV_ATRAC3PLUS);
    if (pspAvcodecLoaded)
        sceUtilityUnloadModule(PSP_MODULE_AV_AVCODEC);
    pspAtracLoaded = pspAvcodecLoaded = pspAt3Modules = false;
}

static void PSPMusicClose()
{
    pspMusicAt3 = false;
    if (pspAt3ID >= 0)
        sceAtracReleaseAtracID(pspAt3ID);
    if (pspAt3File >= 0)
        sceIoClose(pspAt3File);
    pspAt3ID   = -1;
    pspAt3File = -1;
}

// Plays on from sample (reading in whatever the decoder needs from there when streaming).
static bool PSPAt3Seek(uint sample)
{
    pspAt3Ended      = false;
    pspAt3NextSample = sample;
    if (!pspAt3Streaming)
        return sceAtracResetPlayPosition(pspAt3ID, sample, 0, 0) >= 0;

    PspBufferInfo info;
    if (sceAtracGetBufferInfoForReseting(pspAt3ID, sample, &info) < 0)
        return false;
    int first = 0, second = 0;
    if (info.uiWritableByteFirstBuf) {
        sceIoLseek32(pspAt3File, pspAt3Base + info.uiReadPositionFirstBuf, PSP_SEEK_SET);
        first = sceIoRead(pspAt3File, info.pucWritePositionFirstBuf, info.uiWritableByteFirstBuf);
    }
    if (info.uiWritableByteSecondBuf) {
        sceIoLseek32(pspAt3File, pspAt3Base + info.uiReadPositionSecondBuf, PSP_SEEK_SET);
        second = sceIoRead(pspAt3File, info.pucWritePositionSecondBuf, info.uiWritableByteSecondBuf);
    }
    return first >= 0 && second >= 0 && sceAtracResetPlayPosition(pspAt3ID, sample, first, second) >= 0;
}

// Decodes ahead into the ring while it has room for a frame, looping at the track's loop point. Loader thread only.
static void PSPAt3Decode()
{
    if (pspAt3ID < 0 || pspAt3Done)
        return;
    while ((int)(pspAt3RingWrite - pspAt3RingRead) <= PSP_AT3_RING - 2048) {
        if (pspAt3Ended) {
            if (pspAt3Loop && PSPAt3Seek(pspAt3LoopPoint))
                continue;
            pspAt3Done = true;
            break;
        }
        int n = 0, end = 0, remain = 0;
        int r = sceAtracDecodeData(pspAt3ID, (u16 *)pspAt3Pcm, &n, &end, &remain);
        if (r < 0) {
            if (r == (int)PSP_ATRAC_ERROR_ALLDATA_WAS_DECODED) {
                pspAt3Ended = true;
                continue;
            }
            if (++pspAt3Fails == 50)
                PrintLog("music: ATRAC3 decodes failing (%08X)", r);
            break; // starved (the Memory Stick fell behind): silence until the buffer catches up
        }
        pspAt3Fails = 0;
        for (int i = 0; i < n * 2; ++i) pspAt3Ring[(pspAt3RingWrite + i) & (PSP_AT3_RING - 1)] = pspAt3Pcm[i];
        pspAt3RingWrite += n * 2; // (after the samples: the mixer reads up to here)
        pspAt3NextSample += n;
        if (end)
            pspAt3Ended = true;
        if (!n && !pspAt3Ended)
            break;
    }
}

// Loader thread: opens the track if the pack has it as ATRAC3, and decodes its start. False: not ATRAC3 (play the OGG).
static bool PSPMusicOpen(const char *fileName, bool loop, uint loopPoint, bool resume, double resumePosition)
{
    PSPMusicClose();
    if (!pspAt3Modules)
        return false;

    char lower[0x100];
    StringLowerCase(lower, fileName);
    int index = CheckFileInfo(lower);
    if (index < 0)
        return false;
    RSDKFileInfo *entry = &rsdkContainer.files[index];
    if (entry->encrypted)
        return false;

    // the pack by its whole path: the loader thread may have no current directory (after sleep mode)
    char path[0x200];
    const char *packName = rsdkContainer.packNames[entry->packID];
    if (strchr(packName, ':') || !pspAt3Dir[0])
        sprintf(path, "%s", packName);
    else
        sprintf(path, "%s/%s", pspAt3Dir, packName);

    SceUID file = sceIoOpen(path, PSP_O_RDONLY, 0);
    if (file < 0)
        return false;
    int size = entry->filesize;
    sceIoLseek32(file, entry->offset, PSP_SEEK_SET);
    int bytes = sceIoRead(file, pspAt3Buffer, size < PSP_AT3_BUFFER ? size : PSP_AT3_BUFFER);
    if (bytes < 12 || memcmp(pspAt3Buffer, "RIFF", 4)) { // an OGG: not ours
        sceIoClose(file);
        return false;
    }
    int id = sceAtracSetDataAndGetID(pspAt3Buffer, size <= PSP_AT3_BUFFER ? size : PSP_AT3_BUFFER);
    if (id < 0) {
        PrintLog("music: '%s' rejected by the ATRAC3 decoder (%08X)", fileName, id);
        sceIoClose(file);
        return false;
    }

    pspAt3Resume = pspResumeCount;
    StrCopy(pspAt3Track, fileName);
    pspAt3ID        = id;
    pspAt3File      = file;
    pspAt3Base      = entry->offset;
    pspAt3Streaming = size > PSP_AT3_BUFFER;
    if (!pspAt3Streaming) {
        sceIoClose(file);
        pspAt3File = -1;
    }
    sceAtracSetLoopNum(id, 0); // the loop point is ours (PSPAt3Decode)
    pspAt3Loop       = loop;
    pspAt3LoopPoint  = loopPoint;
    pspAt3Ended      = false;
    pspAt3Done       = false;
    pspAt3Fails      = 0;
    pspAt3NextSample = 0;
    pspAt3RingRead   = pspAt3RingWrite; // (emptied without zeroing: the mixer may be reading it)

    if (resume) {
        int endSample = 0, loopStart = 0, loopEnd = 0;
        if (sceAtracGetSoundSample(id, &endSample, &loopStart, &loopEnd) >= 0 && endSample > 0)
            PSPAt3Seek((uint)fmod(resumePosition, (double)endSample));
    }
    PSPAt3Decode();
    return true;
}

// Loader thread, every few ms: lets the decoder go when the music stopped, tops its buffer up from the pack, decodes ahead.
static void PSPMusicService()
{
    if (pspMusicCloseRequest) {
        pspMusicCloseRequest = false;
        pspAt3ResumeTries    = 0;
        if (!pspMusicAt3)
            PSPMusicClose();
    }

    // back from sleep mode: the decoder and the file are gone, set the track up again where it was
    if (pspAt3ID >= 0 && pspAt3Resume != pspResumeCount) {
        pspAt3ResumeSample = pspAt3NextSample;
        pspAt3ResumeTries  = 300; // (~2.5s of tries)
        bool wasPlaying    = pspMusicAt3;
        PSPMusicClose();
        pspAt3Done  = false;
        pspMusicAt3 = wasPlaying; // (the mixer plays silence from the empty ring meanwhile, not the OGG path)
        if (!wasPlaying)
            pspAt3ResumeTries = 0;
        PrintLog("music: back from sleep, setting '%s' up again at %u", pspAt3Track, pspAt3ResumeSample);
    }
    if (pspAt3ResumeTries > 0 && pspAt3ID < 0) {
        if ((pspAt3ResumeTries-- & 7) == 0 && PSPMusicOpen(pspAt3Track, pspAt3Loop, pspAt3LoopPoint, true, pspAt3ResumeSample)) {
            pspAt3ResumeTries = 0;
            LockAudioDevice();
            if (musicStatus == MUSIC_PLAYING || musicStatus == MUSIC_PAUSED)
                pspMusicAt3 = true;
            UnlockAudioDevice();
        }
    }
    if (pspAt3ID < 0)
        return;

    if (pspAt3Streaming) {
        u8 *writePos   = NULL;
        u32 available  = 0;
        u32 readOffset = 0;
        if (sceAtracGetStreamDataInfo(pspAt3ID, &writePos, &available, &readOffset) >= 0 && available > 0) {
            int read = -1;
            if (pspAt3File >= 0 && sceIoLseek32(pspAt3File, pspAt3Base + readOffset, PSP_SEEK_SET) >= 0)
                read = sceIoRead(pspAt3File, writePos, available < PSP_AT3_READ ? available : PSP_AT3_READ);
            if (read > 0)
                sceAtracAddStreamData(pspAt3ID, read);
        }
    }
    PSPAt3Decode();
}

// Mixer (audio thread, holding the audio lock): the next samples of the track, from the ring.
static void PSPMusicMix(Sint32 *stream, size_t bytes_wanted)
{
    Sint16 buffer[MIX_BUFFER_SAMPLES];
    size_t wanted = bytes_wanted / sizeof(Sint16);
    if (wanted > MIX_BUFFER_SAMPLES)
        wanted = MIX_BUFFER_SAMPLES;

    size_t filled = 0;
    while (filled < wanted && (int)(pspAt3RingWrite - pspAt3RingRead) > 0) {
        buffer[filled++] = pspAt3Ring[pspAt3RingRead & (PSP_AT3_RING - 1)];
        pspAt3RingRead++;
    }
    if (filled)
        ProcessAudioMixing(stream, buffer, (int)filled, (bgmVolume * masterVolume) / MAX_VOLUME, 0);

    if (filled < wanted && pspAt3Done) { // the end of a track that doesn't loop
        pspMusicAt3 = false;
        musicStatus = MUSIC_STOPPED;
        trackID     = -1;
    }
    musicPosition = (int)(pspAt3NextSample - (uint)(pspAt3RingWrite - pspAt3RingRead) / 2);
}
