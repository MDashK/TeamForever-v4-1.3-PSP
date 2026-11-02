#include "RetroEngine.hpp"

#if !RETRO_USE_ORIGINAL_CODE

#if RETRO_PLATFORM == RETRO_WIN
#include "Windows.h"
#endif

#if RETRO_PLATFORM == RETRO_VITA
#include <psp2/power.h>
#include <psp2/kernel/processmgr.h>

#ifndef VITA_HEAP_SIZE
#define VITA_HEAP_SIZE (160 * 1024 * 1024)
#endif
// newlib heap (malloc/new). SDL's textures live in separate gpu memory.
int _newlib_heap_size_user = VITA_HEAP_SIZE;
// the script compiler and scene loader keep some big buffers on the stack
unsigned int sceUserMainThreadStackSize = 2 * 1024 * 1024;
#define PROF_TIME() sceKernelGetProcessTimeWide()
#define PROF_ON engineDebugMode
#endif

#if RETRO_PLATFORM == RETRO_PSP
#include <pspkernel.h>
#define PROF_TIME() sceKernelGetSystemTimeWide()
// the profilers (this one, Drawing.cpp's per function one) only run under PSPLink (the data folder given as an argument):
// timing ~1000 draw calls a frame costs a playing PSP a few ms
bool pspProfiling = false;
#define PROF_ON pspProfiling
#endif

// frame profiler (vita & psp, EngineDebugMode only): average/max ms per section, written to log.txt every ~2 seconds
#if RETRO_PLATFORM == RETRO_VITA || RETRO_PLATFORM == RETRO_PSP
static SceUInt64 profStart[VPROF_COUNT];
static SceUInt64 profTotal[VPROF_COUNT];
static SceUInt64 profMax[VPROF_COUNT];
static SceUInt64 profFrameMax[VPROF_COUNT];
static SceUInt64 profWindowStart = 0;
static int profFrames            = 0;

void VitaProfileBegin(int section)
{
    if (PROF_ON)
        profStart[section] = PROF_TIME();
}
void VitaProfileEnd(int section)
{
    if (PROF_ON && profStart[section]) {
        SceUInt64 t = PROF_TIME() - profStart[section];
        profTotal[section] += t;
        profFrameMax[section] += t; // a section can run more than once per frame
        profStart[section] = 0;
    }
}
// Debug helper: if BASE_PATH/dumpframe.txt holds a frame number, the RNG is seeded with a constant and the raw
// framebuffer of that frame is written to BASE_PATH/frame.raw (used to check optimisations don't change the output)
static int dumpFrameID    = -2;
static int dumpFrameCount = 0;
static void VitaDumpFrame()
{
    if (dumpFrameID == -2) {
        dumpFrameID  = -1;
        FileIO *file = fOpen(BASE_PATH "dumpframe.txt", "rb");
        if (file) {
            char text[0x20] = {};
            fRead(text, 1, sizeof(text) - 1, file);
            fClose(file);
            dumpFrameID = atoi(text);
        }
    }
    if (dumpFrameID < 0)
        return;

    if (dumpFrameCount++ == dumpFrameID) {
        FileIO *file = fOpen(BASE_PATH "frame.raw", "wb");
        if (file) {
            for (int y = 0; y < SCREEN_YSIZE; ++y) fWrite(&Engine.frameBuffer[y * GFX_LINESIZE], sizeof(ushort), SCREEN_XSIZE, file);
            fClose(file);
        }
    }
}
bool VitaDumpFrameEnabled()
{
    FileIO *file = fOpen(BASE_PATH "dumpframe.txt", "rb");
    if (file)
        fClose(file);
    return file != NULL;
}

void VitaProfileFrame()
{
    VitaDumpFrame();
    if (!PROF_ON)
        return;

    for (int s = 0; s < VPROF_COUNT; ++s) {
        if (profFrameMax[s] > profMax[s])
            profMax[s] = profFrameMax[s];
        profFrameMax[s] = 0;
    }

    SceUInt64 now = PROF_TIME();
    if (!profWindowStart)
        profWindowStart = now;
    ++profFrames;
    if (now - profWindowStart >= 2000000) {
        // (short names: PrintLog formats into a 256 byte buffer)
        const char *names[] = { "native", "obj", "draw", "sort3d", "draw3d", "flip", "layers", "sprites", "audio", "music" };
        char buffer[0x200];
        // avg/max ms per frame; keep it short, PrintLog formats into a 256 byte buffer
        int pos = sprintf(buffer, "[prof] %.1ffps", profFrames * 1000000.0 / (now - profWindowStart));
        for (int s = 0; s < VPROF_COUNT; ++s)
            pos += sprintf(buffer + pos, " %s %.1f/%.1f", names[s], profTotal[s] / 1000.0 / profFrames, profMax[s] / 1000.0);
        PrintLog("%s", buffer);
#if RETRO_PLATFORM == RETRO_PSP
        extern void PSPSpriteProfReport(int frames); // (Drawing.cpp)
        PSPSpriteProfReport(profFrames);
#ifdef PSP_OPPROF
        extern void PSPOpProfReport(int frames);
        PSPOpProfReport(profFrames);
#endif
#endif

        memset(profTotal, 0, sizeof(profTotal));
        memset(profMax, 0, sizeof(profMax));
        profFrames      = 0;
        profWindowStart = now;
    }
}
#endif

#if RETRO_PLATFORM == RETRO_PSP
#include <pspkernel.h>
#include <psppower.h>
#include <unistd.h>

PSP_MODULE_INFO("RSDKv4", PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
// the script compiler and scene loader keep some big buffers on the stack
PSP_MAIN_THREAD_STACK_SIZE_KB(1024);
// malloc gets everything but this much, which is left for the other threads' stacks (SDL audio, music loader)
PSP_HEAP_THRESHOLD_SIZE_KB(2048);

// the HOME menu's "exit game"
static int PSPExitCallback(int arg1, int arg2, void *common)
{
    PSPLogFlush();
    sceKernelExitGame();
    return 0;
}

// sleep mode: counts the wake-ups, so whoever holds something sleep takes away (the music's file and its Media Engine
// decoder, PSPMusic.hpp) knows to set it up again
volatile int pspResumeCount = 0;
static int PSPPowerCallback(int unknown, int powerInfo, void *common)
{
    if (powerInfo & PSP_POWER_CB_RESUME_COMPLETE)
        ++pspResumeCount;
    return 0;
}

static int PSPCallbackThread(SceSize args, void *argp)
{
    int cbid = sceKernelCreateCallback("ExitCallback", PSPExitCallback, NULL);
    sceKernelRegisterExitCallback(cbid);
    int pcbid = sceKernelCreateCallback("PowerCallback", PSPPowerCallback, NULL);
    scePowerRegisterCallback(-1, pcbid);
    sceKernelSleepThreadCB();
    return 0;
}

static void PSPSetupCallbacks()
{
    int thid = sceKernelCreateThread("CallbackThread", PSPCallbackThread, 0x11, 0x1000, PSP_THREAD_ATTR_USER, 0);
    if (thid >= 0)
        sceKernelStartThread(thid, 0, 0);
}

// all the data paths are relative: make sure they start in the EBOOT's folder, and note where that was
// (boot.txt is written before settings.ini is read, so it works without EngineDebugMode)
static void PSPSetupWorkingDir(int argc, char *argv[])
{
    char dir[0x200] = "";
    if (argc > 0 && argv[0]) {
        StrCopy(dir, argv[0]);
        char *slash = strrchr(dir, '/');
        if (slash) {
            slash[1] = 0;
            chdir(dir);
        }
    }

    // run through PSPLink (the module from the PC, host0:) with the data folder as its argument, e.g.
    //   ./RSDKv4.prx ms0:/PSP/GAME/Sonic1Forever
    // the data stays on the Memory Stick, so loading is as it is for players, not over USB
    if (argc > 1 && argv[1] && !strncmp(argv[1], "ms0:", 4))
    {
        chdir(argv[1]);
        pspProfiling = true; // (a PSPLink run: the profilers on)
    }

    char cwd[0x200] = "";
    getcwd(cwd, sizeof(cwd));
    FILE *f = fopen("boot.txt", "w");
    if (f) {
        fprintf(f, "argv[0]: %s\ncwd: %s\nfree memory: %d KB (largest block %d KB)\n", argc > 0 && argv[0] ? argv[0] : "(none)", cwd,
                (int)(sceKernelTotalFreeMemSize() / 1024), (int)(sceKernelMaxFreeMemSize() / 1024));
        fclose(f);
    }
}
#endif

void parseArguments(int argc, char *argv[])
{
    for (int a = 0; a < argc; ++a) {
        const char *find = "";

        find = strstr(argv[a], "stage=");
        if (find) {
            int b = 0;
            int c = 6;
            while (find[c] && find[c] != ';') Engine.startSceneFolder[b++] = find[c++];
            Engine.startSceneFolder[b] = 0;
        }

        find = strstr(argv[a], "scene=");
        if (find) {
            int b = 0;
            int c = 6;
            while (find[c] && find[c] != ';') Engine.startSceneID[b++] = find[c++];
            Engine.startSceneID[b] = 0;
        }

        find = strstr(argv[a], "console=true");
        if (find) {
            engineDebugMode       = true;
            Engine.devMenu        = true;
            Engine.consoleEnabled = true;
#if RETRO_PLATFORM == RETRO_WIN
            AllocConsole();
            freopen_s((FILE **)stdin, "CONIN$", "w", stdin);
            freopen_s((FILE **)stdout, "CONOUT$", "w", stdout);
            freopen_s((FILE **)stderr, "CONOUT$", "w", stderr);
#endif
        }

        find = strstr(argv[a], "usingCWD=true");
        if (find) {
            usingCWD = true;
        }
    }
}
#endif

#ifdef NXLINK
#include <switch.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/errno.h>
#include <arpa/inet.h>
#include <unistd.h>

static int s_nxlinkSock = -1;

static void initNxLink()
{
    if (R_FAILED(socketInitializeDefault()))
        return;

    s_nxlinkSock = nxlinkStdio();
    if (s_nxlinkSock >= 0)
        printf("printf output now goes to nxlink server\n");
    else
        socketExit();
}
#endif

int main(int argc, char *argv[])
{
#ifdef NXLINK
    initNxLink();
#endif

#if RETRO_PLATFORM == RETRO_VITA
    // the whole frame is drawn in software, so run at full speed
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
#endif
#if RETRO_PLATFORM == RETRO_PSP
    PSPSetupCallbacks();
    PSPSetupWorkingDir(argc, argv);
    // FPU exceptions off for this thread (the game's): a float division by zero gives inf as on a PC, instead of an
    // exception that switches the PSP off (seen in S2A's special stage: DrawSpriteScaled at scale 0)
    {
        unsigned int fcr31;
        __asm__ volatile("cfc1 %0, $31" : "=r"(fcr31));
        fcr31 &= ~0x00000F80u; // (the enable bits: inexact, underflow, overflow, division by zero, invalid)
        __asm__ volatile("ctc1 %0, $31" : : "r"(fcr31));
    }
    // the whole frame is drawn in software, so run at full speed
    scePowerSetClockFrequency(333, 333, 166);
    SDL_SetMainReady();
#endif

#if !RETRO_USE_ORIGINAL_CODE
    parseArguments(argc, argv);
#endif

    SDL_SetHint(SDL_HINT_WINRT_HANDLE_BACK_BUTTON, "1");
    Engine.Init();
#if RETRO_PLATFORM == RETRO_VITA || RETRO_PLATFORM == RETRO_PSP
    if (VitaDumpFrameEnabled())
        srand(1); // deterministic run for frame comparisons
#endif
    Engine.Run();
#if RETRO_PLATFORM == RETRO_PSP
    extern void PSPAudioShutdown(); // (Audio.cpp)
    PSPAudioShutdown();
    PSPLogFlush();
#endif

#if RETRO_PLATFORM == RETRO_VITA
    sceKernelExitProcess(0);
#endif
#if RETRO_PLATFORM == RETRO_PSP
    sceKernelExitGame();
#endif

#if !RETRO_USE_ORIGINAL_CODE
    if (Engine.consoleEnabled) {
#if RETRO_PLATFORM == RETRO_WIN
        FreeConsole();
#endif //! RETRO_PLATFORM == RETRO_WIN
    }
#endif //! !RETRO_USE_ORIGINAL_CODE

#ifdef NXLINK
    socketExit();
#endif //! NXLINK

    return 0;
}

#if RETRO_PLATFORM == RETRO_UWP
int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) { return SDL_WinRTRunApp(main, NULL); }
#endif
