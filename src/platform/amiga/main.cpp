// OpenLara main engine on the Apollo Vampire V4 (68080 + SAGA), AmigaOS 3.x.
// Experiment: the engine's software renderer (GAPI_SW) draws RGB565 into our own buffer, and the
// SAGA display is pointed at it (P96 only opens the screen for the mode). No sound yet.
// Run from a Shell in the directory that holds DATA/:  OpenLara [DATA/LEVEL1.PHD]  (no level = title screen)
// Prints a frame-time profile (update / render ms) every 100 frames. F10 quits (or RAM:olquit); Esc is the game's inventory key.
// Headless test (no screen, e.g. in the emulator): OpenLara LEVEL FRAMES OUTPREFIX - writes level stats
// (compare with phdstats.py) and the last frame, see emu.sh.

#include "game.h"

#define Object AmiObject
#define Input AmiInput          // dos.library Input() vs the engine's Input namespace
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/intuition.h>
#include <proto/graphics.h>
#include <proto/lowlevel.h>
#include <intuition/intuition.h>
#include <intuition/intuitionbase.h>
#include <exec/semaphores.h>
#include <dos/dostags.h>
#include <cybergraphx/cybergraphics.h>
#include <proto/cybergraphics.h>
#undef Object
#undef Input

// The engine needs a big stack; this libnix ignores __stack, so main() swaps to its own (see below)
#define OL_STACK_SIZE (512 * 1024)
struct Library* CyberGfxBase = NULL;
struct Library* LowLevelBase = NULL;

static struct Screen* screen = NULL;
static struct Window* window = NULL;
static UBYTE* fbMem = NULL;
static uint16* fb = NULL;               // the buffer being drawn
// Triple buffering: SAGA takes a new display address at the next vertical blank, so the buffer we
// just replaced may still be on screen for up to a frame; clearing it right away showed as a black
// line running through the picture. With three, the buffer we draw into left the screen two flips ago.
#define FB_COUNT 3
static uint16* fbBuf[FB_COUNT];
static int fbBack = 0;
static int fbHidden[FB_COUNT];           // osGetTimeMS() when each buffer was replaced on screen
static ULONG sagaOld = 0;
#define SAGA_GFXADDR   ((volatile ULONG*)0xDFF1EC)
#define SAGA_GFXADDR_R ((volatile ULONG*)0xDFE1EC)

static int scrW = 320, scrH = 240;       // ENV:OLRES ("640x480"): any 16-bit RTG mode of exactly that size
#define SCREEN_WIDTH  scrW
#define SCREEN_HEIGHT scrH

// --- OS hooks the engine needs ---------------------------------------------------------------
static struct EClockVal eclock;
static ULONG timeAcc = 0;   // 16.16 seconds
static int fakeTimeMS = -1; // scripted headless runs: virtual time, 30 fps steps (deterministic)

int osGetTimeMS()
{
    if (fakeTimeMS >= 0) return fakeTimeMS;
    timeAcc += ElapsedTime(&eclock);
    return (int)(((uint64)timeAcc * 1000) >> 16);
}

// the sound task mixes in parallel with the game: the engine's locks are exec semaphores
static void* newSemaphore()
{
    struct SignalSemaphore *s = (struct SignalSemaphore*)AllocVec(sizeof(*s), MEMF_PUBLIC | MEMF_CLEAR);
    if (s) InitSemaphore(s);
    return s;
}
void* osMutexInit()            { return newSemaphore(); }
void  osMutexFree(void *m)     { FreeVec(m); }
void  osMutexLock(void *m)     { ObtainSemaphore((struct SignalSemaphore*)m); }
void  osMutexUnlock(void *m)   { ReleaseSemaphore((struct SignalSemaphore*)m); }
void* osRWLockInit()           { return newSemaphore(); }
void  osRWLockFree(void *m)    { FreeVec(m); }
void  osRWLockRead(void *m)    { ObtainSemaphoreShared((struct SignalSemaphore*)m); }
void  osRWUnlockRead(void *m)  { ReleaseSemaphore((struct SignalSemaphore*)m); }
void  osRWLockWrite(void *m)   { ObtainSemaphore((struct SignalSemaphore*)m); }
void  osRWUnlockWrite(void *m) { ReleaseSemaphore((struct SignalSemaphore*)m); }

// --- sound: AHI (on the V4 the ARNE driver = SAGA 16-bit audio) ---------------------------------
// A task of its own keeps two AHI write requests in flight: while one plays, Sound::fill() mixes the
// next (always the same frame count: the mixer sizes its scratch buffer on the first call).
// Interface of ahi.device V4 (declared here: the AHI SDK headers are not part of this tree).
struct OLAHIRequest {
    struct IOStdReq ahir_Std;
    UWORD   ahir_Version;
    UWORD   ahir_Pad1;
    ULONG   ahir_Private[2];
    ULONG   ahir_Type;
    ULONG   ahir_Frequency;
    LONG    ahir_Volume;                 // 16.16
    LONG    ahir_Position;               // 16.16, 0.5 = centre
    struct OLAHIRequest *ahir_Link;
};
#define OL_AHI_DEFAULT_UNIT 0
#define OL_AHIST_S16S       3

#define SND_FRAMES 1024                  // 23 ms at 44.1 kHz per buffer
#define SND_DUMP_FRAMES 1024             // headless dump: same size (the mixer keeps its first size)
static volatile bool sndQuit = false;
static volatile bool sndRunning = false;
static volatile ULONG sndMixTime = 0, sndMixCount = 0;   // Sound::fill time (EClock ticks) / calls, for the profile
static volatile int  sndOpenError = 1;   // set by the sound task: 0 = AHI open, else OpenDevice's error
static int sndUnit = OL_AHI_DEFAULT_UNIT; // ENV:OLAHIUNIT
static void sndStop();
static struct Task *sndMainTask = NULL;
static BYTE sndDoneSig = -1;

extern "C" void olSndTrapHandler();

static void sndTask()
{
    FindTask(NULL)->tc_TrapCode = (APTR)olSndTrapHandler;
    struct MsgPort *port = CreateMsgPort();
    OLAHIRequest *req[2] = { NULL, NULL };
    Sound::Frame *buf[2] = { NULL, NULL };
    bool open = false;
    if (port) {
        req[0] = (OLAHIRequest*)CreateIORequest(port, sizeof(OLAHIRequest));
        req[1] = (OLAHIRequest*)AllocVec(sizeof(OLAHIRequest), MEMF_PUBLIC);
        buf[0] = (Sound::Frame*)AllocVec(SND_FRAMES * sizeof(Sound::Frame), MEMF_PUBLIC | MEMF_CLEAR);
        buf[1] = (Sound::Frame*)AllocVec(SND_FRAMES * sizeof(Sound::Frame), MEMF_PUBLIC | MEMF_CLEAR);
        if (req[0] && req[1] && buf[0] && buf[1]) {
            req[0]->ahir_Version = 4;
            sndOpenError = OpenDevice((STRPTR)"ahi.device", sndUnit, (struct IORequest*)req[0], 0);
            open = sndOpenError == 0;
        }
    }
    if (!open && !sndOpenError) sndOpenError = -99;   // no memory / port
    if (open) {
        CopyMem(req[0], req[1], sizeof(OLAHIRequest));
        sndRunning = true;
        OLAHIRequest *link = NULL;
        int cur = 0;
        struct EClockVal ev;
        ElapsedTime(&ev);
        while (!sndQuit) {
            ElapsedTime(&ev);
            Sound::fill(buf[cur], SND_FRAMES);
            sndMixTime += ElapsedTime(&ev);      // 16.16 seconds
            sndMixCount++;
            OLAHIRequest *r = req[cur];
            r->ahir_Std.io_Message.mn_Node.ln_Pri = 0;
            r->ahir_Std.io_Command = CMD_WRITE;
            r->ahir_Std.io_Data    = buf[cur];
            r->ahir_Std.io_Length  = SND_FRAMES * sizeof(Sound::Frame);
            r->ahir_Std.io_Offset  = 0;
            r->ahir_Type      = OL_AHIST_S16S;
            r->ahir_Frequency = 44100;
            r->ahir_Volume    = 0x10000;
            r->ahir_Position  = 0x8000;
            r->ahir_Link      = link;
            SendIO((struct IORequest*)r);
            if (link) WaitIO((struct IORequest*)link);   // the previous buffer has finished playing
            link = r;
            cur ^= 1;
        }
        if (link) { AbortIO((struct IORequest*)link); WaitIO((struct IORequest*)link); }
        CloseDevice((struct IORequest*)req[0]);
    }
    if (req[1]) FreeVec(req[1]);
    if (req[0]) DeleteIORequest((struct IORequest*)req[0]);
    if (buf[0]) FreeVec(buf[0]);
    if (buf[1]) FreeVec(buf[1]);
    if (port) DeleteMsgPort(port);
    sndRunning = false;
    Forbid();                            // the main task may free our code once signalled
    Signal(sndMainTask, 1UL << sndDoneSig);
}

static bool sndStart()
{
    sndMainTask = FindTask(NULL);
    sndDoneSig = AllocSignal(-1);
    if (sndDoneSig < 0) return false;
    sndQuit = false;
    struct Process *p = CreateNewProcTags(NP_Entry, (ULONG)sndTask, NP_Name, (ULONG)"OpenLara sound",
                                          NP_Priority, 5, NP_StackSize, 128 * 1024, TAG_DONE);
    if (!p) { FreeSignal(sndDoneSig); sndDoneSig = -1; return false; }
    // wait until the task has opened AHI (or failed) so the result can be reported
    for (int i = 0; i < 100 && !sndRunning; i++) {
        if (SetSignal(0, 0) & (1UL << sndDoneSig)) break;   // the task ended: OpenDevice failed
        Delay(1);
    }
    if (!sndRunning) {
        printf("sound: AHI unit %d did not open (error %d)\n", sndUnit, sndOpenError);
        sndStop();
        return false;
    }
    printf("sound: AHI unit %d\n", sndUnit);
    return true;
}

static void sndStop()
{
    if (sndDoneSig < 0) return;
    sndQuit = true;
    Wait(1UL << sndDoneSig);
    FreeSignal(sndDoneSig);
    sndDoneSig = -1;
}

// Maggie backend (gapi/sw.h + maggie_sw.h with -DOL_MAGGIE): memory and cache calls
void* osMagAlloc(int size)     { return AllocVec(size, MEMF_ANY | MEMF_CLEAR); }
void  osMagFree(void *ptr)     { FreeVec(ptr); }
void  osMagCacheFlush()        { CacheClearU(); }

bool osJoyReady(int index)     { return index == 0; }
void osJoyVibrate(int index, float L, float R) {}

// --- input: keyboard (IDCMP raw keys) and the V4 USB pad ($DFF220, pad in DirectInput mode) ---
static InputKey rawToKey(UWORD code)
{
    switch (code) {
        case 0x4C: return ikUp;     case 0x4D: return ikDown;
        case 0x4F: return ikLeft;   case 0x4E: return ikRight;
        case 0x63: return ikCtrl;   case 0x64: case 0x65: return ikAlt;
        case 0x60: case 0x61: return ikShift;
        case 0x40: return ikSpace;  case 0x44: return ikEnter;
        case 0x45: return ikEscape; case 0x5F: return ikTab;
        case 0x46: return ikDel;    case 0x41: return ikBack;
        case 0x43: return ikEnter;  // numeric pad Enter
        default:   break;
    }
    // letters, digits, numeric pad and punctuation (US positions; the engine binds Look C, Duck Z,
    // Dash X, Roll A by default)
    static const char  qrow[] = "QWERTYUIOP", arow[] = "ASDFGHJKL", zrow[] = "ZXCVBNM";
    if (code >= 0x10 && code < 0x10 + 10) return InputKey(ikA + qrow[code - 0x10] - 'A');
    if (code >= 0x20 && code < 0x20 + 9)  return InputKey(ikA + arow[code - 0x20] - 'A');
    if (code >= 0x31 && code < 0x31 + 7)  return InputKey(ikA + zrow[code - 0x31] - 'A');
    if (code >= 0x01 && code <= 0x09)     return InputKey(ik1 + code - 0x01);
    switch (code) {
        case 0x0A: return ik0;
        case 0x0F: return ikN0;  case 0x1D: return ikN1;  case 0x1E: return ikN2;  case 0x1F: return ikN3;
        case 0x2D: return ikN4;  case 0x2E: return ikN5;  case 0x2F: return ikN6;
        case 0x3D: return ikN7;  case 0x3E: return ikN8;  case 0x3F: return ikN9;
        case 0x3C: return ikNDot; case 0x4A: return ikNSub; case 0x5E: return ikNAdd;
        case 0x5D: return ikNMul; case 0x5C: return ikNDiv;
        case 0x0B: return ikMinus; case 0x0C: return ikPlus;  case 0x0D: return ikBSlash;
        case 0x1A: return ikLSB;   case 0x1B: return ikRSB;   case 0x29: return ikColon;
        case 0x2A: return ikApos;  case 0x38: return ikComma; case 0x39: return ikDot;
        case 0x3A: return ikSlash; case 0x00: return ikTilda;
        default:   return ikNone;
    }
}

static bool quit = false;

// --- scripted input for headless runs: lines "FRAME KEY 1|0" (KEY: up down left right ctrl alt
// shift space enter esc tab del back) or "FRAME shot" (save the frame as OUTPREFIX-FRAME.raw).
// FRAME counts loop iterations from the start; time advances 1/30 s per iteration.
struct ScriptEvent { int frame; int key; int down; };   // key -1 = shot
static ScriptEvent script[512];
static int scriptCount = 0, scriptPos = 0;

static bool loadScript(const char *name)
{
    FILE *f = fopen(name, "r");
    if (!f) { printf("no script %s\n", name); return false; }
    static const struct { const char *name; InputKey key; } keys[] = {
        { "up", ikUp }, { "down", ikDown }, { "left", ikLeft }, { "right", ikRight }, { "ctrl", ikCtrl },
        { "alt", ikAlt }, { "shift", ikShift }, { "space", ikSpace }, { "enter", ikEnter },
        { "esc", ikEscape }, { "tab", ikTab }, { "del", ikDel }, { "back", ikBack },
    };
    char line[128], name2[32];
    while (scriptCount < 512 && fgets(line, sizeof(line), f)) {
        int frame, down = 1;
        if (line[0] == '#' || sscanf(line, "%d %31s %d", &frame, name2, &down) < 2) continue;
        ScriptEvent &e = script[scriptCount];
        e.frame = frame; e.down = down; e.key = -2;
        if (!strcmp(name2, "shot")) e.key = -1;
        for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
            if (!strcmp(name2, keys[i].name)) e.key = keys[i].key;
        if (e.key != -2) scriptCount++;
    }
    fclose(f);
    for (int i = 1; i < scriptCount; i++)           // by frame, keeping file order within a frame
        for (int j = i; j > 0 && script[j - 1].frame > script[j].frame; j--) {
            ScriptEvent t = script[j]; script[j] = script[j - 1]; script[j - 1] = t;
        }
    printf("script: %d events\n", scriptCount);
    return true;
}

static void saveFrame(const char *prefix, int frame, const uint16 *pixels);

static void pollInput()
{
    // The picture goes to SAGA directly, so it stays on show even when Intuition has brought
    // another screen to the front (a mouse click, Amiga-M): keys then go to that screen's window
    // (an AmigaShell behind the game). Take the front and the focus back while the game runs.
    if (window && ((struct IntuitionBase*)IntuitionBase)->ActiveWindow != window) {
        static int lastTry = -1000;
        int now = osGetTimeMS();
        if (now - lastTry > 500) {
            lastTry = now;
            ScreenToFront(screen);
            ActivateWindow(window);
        }
    }

    struct IntuiMessage* msg;
    while (window && (msg = (struct IntuiMessage*)GetMsg(window->UserPort))) {
        if (msg->Class == IDCMP_RAWKEY) {
            UWORD code = msg->Code & 0x7F;
            bool down = !(msg->Code & 0x80);
            if (code == 0x59 && down) quit = true;         // F10
        #ifdef OL_MAGGIE
            if (code == 0x56 && down) Mag::zEnabled = !Mag::zEnabled;   // F7: depth buffer on/off
            if (code == 0x55 && down) Mag::perspective = !Mag::perspective; // F6: perspective-correct texturing
            if (code == 0x57 && down) Mag::bilinear = !Mag::bilinear;       // F8: bilinear filtering
            if (code == 0x58 && down) Mag::mipmaps = !Mag::mipmaps;         // F9: mipmaps (hardware back end)
        #endif
            InputKey k = rawToKey(code);
            if (k != ikNone) Input::setDown(k, down);
        }
        ReplyMsg((struct Message*)msg);
    }

    static UWORD lastPad = 0;
    UWORD pad = *(volatile UWORD*)0xDFF220;
    if (!(pad & 1)) pad = 0;
    UWORD changed = pad ^ lastPad;
    if (changed) {
        static const struct { UWORD bit; JoyKey key; } map[] = {
            { 0x1000, jkUp }, { 0x2000, jkDown }, { 0x4000, jkLeft }, { 0x8000, jkRight },
            { 0x0002, jkA }, { 0x0004, jkB }, { 0x0008, jkX }, { 0x0010, jkY },
            { 0x0020, jkLB }, { 0x0040, jkRB }, { 0x0080, jkLT }, { 0x0100, jkRT },
            { 0x0400, jkStart },   // Select (0x0200) is the core's hotkey button: not used
        };
        for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
            if (changed & map[i].bit) Input::setJoyDown(0, map[i].key, (pad & map[i].bit) != 0);
        lastPad = pad;
    }
}

// --- display ----------------------------------------------------------------------------------
static bool videoInit()
{
    CyberGfxBase = OpenLibrary((STRPTR)"cybergraphics.library", 41);
    if (!CyberGfxBase) { printf("no cybergraphics.library\n"); return false; }
    ULONG modeID = BestCModeIDTags(CYBRBIDTG_NominalWidth, SCREEN_WIDTH, CYBRBIDTG_NominalHeight, SCREEN_HEIGHT,
                                   CYBRBIDTG_Depth, 16, TAG_DONE);
    if (modeID == (ULONG)INVALID_ID) { printf("no %dx%dx16 mode\n", SCREEN_WIDTH, SCREEN_HEIGHT); return false; }
    screen = OpenScreenTags(NULL, SA_DisplayID, modeID, SA_Depth, 16, SA_ShowTitle, FALSE, SA_Quiet, TRUE,
                            SA_Type, CUSTOMSCREEN, TAG_DONE);
    if (!screen) { printf("OpenScreen failed\n"); return false; }
    window = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)screen, WA_Width, screen->Width, WA_Height, screen->Height,
                            WA_Flags, WFLG_ACTIVATE | WFLG_RMBTRAP | WFLG_BACKDROP | WFLG_BORDERLESS,
                            WA_IDCMP, IDCMP_RAWKEY, TAG_DONE);
    // no mouse pointer over the game: an empty sprite image for our window (Intuition shows it
    // while the window is active; the system pointer is back when it closes)
    if (window) {
        static UWORD *blankPointer = NULL;
        if (!blankPointer) blankPointer = (UWORD*)AllocVec(6 * sizeof(UWORD), MEMF_CHIP | MEMF_CLEAR);
        if (blankPointer) SetPointer(window, blankPointer, 1, 16, 0, 0);
    }
    ULONG base = 0, bpr = 0;
    APTR lock = LockBitMapTags(screen->RastPort.BitMap, LBMI_BASEADDRESS, (ULONG)&base, LBMI_BYTESPERROW, (ULONG)&bpr, TAG_DONE);
    if (lock) UnLockBitMap(lock);
    printf("mode %08lx screen %dx%d bpr %lu base %08lx shown %08lx\n", modeID, screen->Width, screen->Height, bpr, base, *SAGA_GFXADDR_R);
    if (bpr != (ULONG)SCREEN_WIDTH * 2 || screen->Width != SCREEN_WIDTH || screen->Height != SCREEN_HEIGHT) {
        printf("no %dx%d mode: the nearest is %dx%d (ENV:OLRES)\n", SCREEN_WIDTH, SCREEN_HEIGHT, screen->Width, screen->Height);
        return false;
    }

    // two frame buffers of our own, shown by pointing SAGA at them (P96 screen buffers flicker on vampiregfx)
    ULONG size = (SCREEN_WIDTH * 2 * screen->Height + 63) & ~63UL;
    fbMem = (UBYTE*)AllocVec(size * FB_COUNT + 64, MEMF_ANY | MEMF_CLEAR);
    if (!fbMem) return false;
    for (int i = 0; i < FB_COUNT; i++) {
        fbBuf[i] = (uint16*)((((ULONG)fbMem + 63) & ~63UL) + i * size);
        fbHidden[i] = 0;
    }
    sagaOld = base;
    *SAGA_GFXADDR = (ULONG)fbBuf[FB_COUNT - 1];
    fbBack = 0;
    fb = fbBuf[0];
    return true;
}

static void videoFree()
{
    if (screen && sagaOld) { *SAGA_GFXADDR = sagaOld; WaitTOF(); WaitTOF(); }
    if (window) { ClearPointer(window); CloseWindow(window); }
    if (screen) CloseScreen(screen);
    if (fbMem) FreeVec(fbMem);
    if (CyberGfxBase) CloseLibrary(CyberGfxBase);
}


// Level statistics after loading, compared against tools/phdstats.py (reads the PHD on the PC):
// any byte-order mistake in the loader shows up as a mismatch.
static void dumpLevelStats(FILE *f)
{
    TR::Level &l = Game::level->level;
    fprintf(f, "rooms %d floors %ld meshData %ld meshOffsets %ld anims %ld states %ld nodes %ld frames %ld models %ld statics %ld\n",
            l.roomsCount, (long)l.floorsCount, (long)l.meshDataSize, (long)l.meshOffsetsCount, (long)l.animsCount,
            (long)l.statesCount, (long)l.nodesDataSize, (long)l.frameDataSize, (long)l.modelsCount, (long)l.staticMeshesCount);
    fprintf(f, "objTex %ld sprTex %ld cameras %ld soundSources %ld boxes %ld overlaps %ld entities %ld\n",
            (long)l.objectTexturesCount, (long)l.spriteTexturesCount, (long)l.camerasCount, (long)l.soundSourcesCount,
            (long)l.boxesCount, (long)l.overlapsCount, (long)l.entitiesBaseCount);
    uint32 a = 0, b = 0, c = 0;
    for (int i = 0; i < l.roomsCount; i++) {
        TR::Room &r = l.rooms[i];
        a += r.info.x + r.info.z * 3 + r.info.yBottom * 5 + r.info.yTop * 7;
        for (int j = 0; j < r.data.vCount; j++) b += r.data.vertices[j].pos.x + r.data.vertices[j].pos.y * 3 + r.data.vertices[j].pos.z * 5;
        for (int j = 0; j < r.portalsCount; j++) c += r.portals[j].roomIndex + r.portals[j].normal.y * 3 + r.portals[j].vertices[2].x * 5;
    }
    fprintf(f, "room0 %ld %ld %ld %ld v%d  roomsum %lu vertsum %lu portalsum %lu\n", (long)l.rooms[0].info.x, (long)l.rooms[0].info.z,
            (long)l.rooms[0].info.yBottom, (long)l.rooms[0].info.yTop, l.rooms[0].data.vCount, (unsigned long)a, (unsigned long)b, (unsigned long)c);
    a = b = c = 0;
    for (int i = 0; i < l.floorsCount; i++) a += l.floors[i].value * (i + 1);
    for (int i = 0; i < l.frameDataSize; i++) b += l.frameData[i] * (i + 1);
    for (int i = 0; i < l.nodesDataSize; i++) c += l.nodesData[i] * (i + 1);
    fprintf(f, "floorsum %lu framesum %lu nodesum %lu\n", (unsigned long)a, (unsigned long)b, (unsigned long)c);
    a = b = c = 0;
    for (int i = 0; i < l.animsCount; i++) a += l.anims[i].frameOffset + l.anims[i].state * 3 + l.anims[i].speed.value * 5 + l.anims[i].nextAnimation * 7;
    for (int i = 0; i < l.boxesCount; i++) b += l.boxes[i].minZ + l.boxes[i].maxX * 3 + l.boxes[i].floor * 5 + l.boxes[i].overlap.value * 7;
    for (int i = 0; i < l.entitiesBaseCount; i++) c += l.entities[i].type + l.entities[i].room * 3 + l.entities[i].flags.value * 5;
    fprintf(f, "animsum %lu boxsum %lu entsum %lu\n", (unsigned long)a, (unsigned long)b, (unsigned long)c);
    a = b = c = 0;
    for (int i = 0; i < l.objectTexturesCount; i++) a += l.objectTextures[i].tile + l.objectTextures[i].texCoord[2].x * 3 + l.objectTextures[i].texCoord[3].y * 5;
    for (int i = 0; i < l.spriteTexturesCount; i++) b += l.spriteTextures[i].tile + l.spriteTextures[i].l * 3 + l.spriteTextures[i].b * 5;
    for (int i = 0; i < l.staticMeshesCount; i++) c += l.staticMeshes[i].id + l.staticMeshes[i].mesh * 3 + l.staticMeshes[i].vbox.minX * 5 + l.staticMeshes[i].cbox.maxZ * 7;
    fprintf(f, "objtexsum %lu sprtexsum %lu staticsum %lu\n", (unsigned long)a, (unsigned long)b, (unsigned long)c);
    fprintf(f, "entity0 type %d room %d pos %ld %ld %ld rot %u flags %04x\n", l.entities[0].type, l.entities[0].room,
            (long)l.entities[0].x, (long)l.entities[0].y, (long)l.entities[0].z, l.entities[0].rotation.value, l.entities[0].flags.value);
}

static void saveFrame(const char *prefix, int frame, const uint16 *pixels)
{
    char name[256];
    sprintf(name, "%s-%04d.raw", prefix, frame);
    FILE *f = fopen(name, "wb");
    if (f) { fwrite(pixels, 2, SCREEN_WIDTH * SCREEN_HEIGHT, f); fclose(f); }
}

// --- crash report: the task's trap handler records the exception, PC and registers, then
// resumes in olCrashed() (user mode), which prints them, writes RAM:olcrash.txt, restores the
// display and exits instead of leaving a hung task. ./crashaddr.py OFFSET names the function
// (offsets are relative to main, see build*/map.txt).
extern "C" {
    ULONG olTrapNum, olTrapPC, olTrapRegs[16];
    void olCrashed();
    void olTrapHandler();
    void olSndCrashed();
    void olSndTrapHandler();
}
// the sound task has its own handler (same frame handling, resumes in olSndCrashed)
asm(".text\n.even\n.globl _olSndTrapHandler\n_olSndTrapHandler:\n"
    "  movem.l d0-d7/a0-a6,_olTrapRegs\n"
    "  move.l (sp)+,_olTrapNum\n"
    "  move.l usp,a0\n  move.l a0,_olTrapRegs+60\n"
    "  move.l 2(sp),_olTrapPC\n"
    "  move.l #_olSndCrashed,2(sp)\n"
    "  rte\n");
asm(".text\n.even\n.globl _olTrapHandler\n_olTrapHandler:\n"
    "  movem.l d0-d7/a0-a6,_olTrapRegs\n"
    "  move.l (sp)+,_olTrapNum\n"          // exec pushes the exception number
    "  move.l usp,a0\n  move.l a0,_olTrapRegs+60\n"
    "  move.l 2(sp),_olTrapPC\n"           // the frame: SR.w, PC.l, ...
    "  move.l #_olCrashed,2(sp)\n"
    "  rte\n");

static void videoFree();
static void sndStop();
int main();

static int crashReport(const char *who, char *buf)
{
    int n = sprintf(buf, "CRASH (%s): exception %lu at PC %08lx = main%+ld\n", who, olTrapNum, olTrapPC, (long)(olTrapPC - (ULONG)main));
    for (int i = 0; i < 16; i++)
        n += sprintf(buf + n, "%s%d=%08lx%s", i < 8 ? "d" : "a", i & 7, olTrapRegs[i], (i & 7) == 7 ? "\n" : " ");
    // RAM: for a quick look, PROGDIR: so it survives a reset
    static const char *files[] = { "RAM:olcrash.txt", "PROGDIR:olcrash.txt" };
    for (int i = 0; i < 2; i++) {
        BPTR f = Open((STRPTR)files[i], MODE_NEWFILE);
        if (f) { Write(f, buf, n); Close(f); }
    }
    return n;
}

static volatile bool sndCrashed = false;

void olSndCrashed()                      // the sound task crashed: report, then end the task
{
    char buf[512];
    crashReport("sound task", buf);
    sndCrashed = true;
    sndRunning = false;
    Forbid();
    Signal(sndMainTask, 1UL << sndDoneSig);
    Wait(0);                             // never returns: the task stays parked, the game runs on silently
}

void olCrashed()
{
    char buf[512];
    crashReport("game", buf);
    printf("%s", buf);
    sndStop();
    videoFree();
    Exit(20);
}

extern int    __argc;
extern char **__argv;

__attribute__((noinline)) static int olMain()
{
    setvbuf(stdout, NULL, _IONBF, 0);   // a hang or crash must not swallow the last log lines
    {
        struct Task *t = FindTask(NULL);
        int here;
        printf("stack %08lx..%08lx (%ld bytes), sp %08lx\n", (ULONG)t->tc_SPLower, (ULONG)t->tc_SPUpper,
               (long)((ULONG)t->tc_SPUpper - (ULONG)t->tc_SPLower), (ULONG)&here);
    }
    LowLevelBase = OpenLibrary((STRPTR)"lowlevel.library", 40);
    if (!LowLevelBase) { printf("no lowlevel.library\n"); return 20; }
    ElapsedTime(&eclock);

    // headless test (emulator, no RTG): OpenLara LEVEL FRAMES OUTPREFIX -> OUTPREFIX.txt (level stats,
    // frame times) and OUTPREFIX.raw (last frame, 320x240 RGB565 big-endian), then quits
    int headlessFrames = __argc > 3 ? atoi(__argv[2]) : 0;
    const char *outPrefix = __argc > 3 ? __argv[3] : NULL;
    FindTask(NULL)->tc_TrapCode = (APTR)olTrapHandler;
    // the game loop never waits: above priority 0 it starves the network stack, sshd and rsh
    // (Workbench/some shells start programs at 1), so run at 0 and share the CPU with them
    SetTaskPri(FindTask(NULL), 0);

    {
        char res[16];
        struct Process *me = (struct Process*)FindTask(NULL);
        APTR oldWin = me->pr_WindowPtr;
        me->pr_WindowPtr = (APTR)-1;     // no "insert volume ENV:" requester
        if (GetVar((STRPTR)"OLRES", (STRPTR)res, sizeof(res), 0) > 0) {
            int w = 0, h = 0;
            if (sscanf(res, "%dx%d", &w, &h) == 2 && w >= 160 && w <= 1280 && h >= 120 && h <= 1024) { scrW = w & ~1; scrH = h; }
        }
        me->pr_WindowPtr = oldWin;
    }
    if (headlessFrames) {
        fbMem = (UBYTE*)AllocVec(SCREEN_WIDTH * 2 * SCREEN_HEIGHT + 64, MEMF_ANY | MEMF_CLEAR);
        if (!fbMem) return 20;
        fb = (uint16*)(((ULONG)fbMem + 63) & ~63UL);
    } else if (!videoInit()) { videoFree(); CloseLibrary(LowLevelBase); return 20; }

    strcpy(cacheDir, "RAM:");
    saveDir[0] = 0;
    contentDir[0] = 0;

    Core::width  = SCREEN_WIDTH;
    Core::height = SCREEN_HEIGHT;
    Core::defLang = 0;

    const char* level = __argc > 1 ? __argv[1] : NULL;     // no level ("" or -): the normal game (title screen)
    if (level && (!level[0] || !strcmp(level, "-"))) level = NULL;
    printf("loading %s\n", level ? level : "title");
    int t0 = osGetTimeMS();
    // scripted runs: loading "takes" 7.7 s on the virtual clock whatever the machine (the GL reference
    // build does the same), so loop numbers and frames match between the V4, the emulator and the PC
    if (headlessFrames && __argc > 4) fakeTimeMS = 7700;
    Game::init(level);
    printf("init %d ms\n", osGetTimeMS() - t0);

    GAPI::resize();
    GAPI::swColor = fb;

    char envTmp[8];                      // ENV:OLNOSOUND set = no sound task
    struct Process *me = (struct Process*)FindTask(NULL);
    APTR oldWin = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;         // no "insert volume ENV:" requester where ENV: is missing
    bool noSound = GetVar((STRPTR)"OLNOSOUND", (STRPTR)envTmp, sizeof(envTmp), 0) >= 0;
    if (GetVar((STRPTR)"OLAHIUNIT", (STRPTR)envTmp, sizeof(envTmp), 0) >= 0) sndUnit = atoi(envTmp);
    // Reverb costs 15.6 ms of every 23.2 ms of sound on the 68080 (the mix alone 2.0 ms) - three
    // quarters of the machine: off unless ENV:OLREVERB is set, whatever the saved settings say.
    Core::settings.audio.reverb = GetVar((STRPTR)"OLREVERB", (STRPTR)envTmp, sizeof(envTmp), 0) >= 0;
    bool sound = !headlessFrames && !noSound && sndStart();
#ifdef OL_MAGGIE
#ifdef OL_PROFILE
    if (GetVar((STRPTR)"OLDBG", (STRPTR)envTmp, sizeof(envTmp), 0) >= 0) Mag::dbg = atoi(envTmp);
#endif
    if (GetVar((STRPTR)"OLNOZ", (STRPTR)envTmp, sizeof(envTmp), 0) >= 0) Mag::zEnabled = false;   // ENV:OLNOZ: no depth buffer
    if (GetVar((STRPTR)"OLNOPERSP", (STRPTR)envTmp, sizeof(envTmp), 0) >= 0) Mag::perspective = false; // ENV:OLNOPERSP
    if (GetVar((STRPTR)"OLNONCULL", (STRPTR)envTmp, sizeof(envTmp), 0) >= 0) Mag::normalCull = false;    // ENV:OLNONCULL
    if (GetVar((STRPTR)"OLNOCELLS", (STRPTR)envTmp, sizeof(envTmp), 0) >= 0) Mag::roomCells = false;     // ENV:OLNOCELLS
    if (GetVar((STRPTR)"OLPICK", (STRPTR)envTmp, sizeof(envTmp), 0) > 0) sscanf(envTmp, "%d,%d", &Mag::pickX, &Mag::pickY);
    // the texture error bound in pixels: the same size on screen at every resolution (1 px per 320
    // columns, so 2 at 640x480); ENV:OLPERSPERR overrides
    Mag::perspErr = SCREEN_WIDTH >= 640 ? float(SCREEN_WIDTH) / 320.0f : 1.0f;
#ifdef _GAPI_MAGGIE
    if (GetVar((STRPTR)"OLDRAWDIST", (STRPTR)envTmp, sizeof(envTmp), 0) > 0) { int d = atoi(envTmp); if (d >= 8 && d <= 45) GAPI::magDrawDist = float(d) * 1024.0f; }
#endif
    if (GetVar((STRPTR)"OLPERSPERR", (STRPTR)envTmp, sizeof(envTmp), 0) > 0) { int e = atoi(envTmp); if (e >= 1 && e <= 8) Mag::perspErr = float(e); }
    if (GetVar((STRPTR)"OLNOALPHA", (STRPTR)envTmp, sizeof(envTmp), 0) >= 0) Mag::cpuAlpha = false;   // ENV:OLNOALPHA
    if (GetVar((STRPTR)"OLNOMIP", (STRPTR)envTmp, sizeof(envTmp), 0) >= 0) Mag::mipmaps = false;         // ENV:OLNOMIP
    if (GetVar((STRPTR)"OLNOBILINEAR", (STRPTR)envTmp, sizeof(envTmp), 0) >= 0) Mag::bilinear = false; // ENV:OLNOBILINEAR
#endif
#ifdef SW_NEAR_CLIP
    if (GetVar((STRPTR)"OLNOCLIP", (STRPTR)envTmp, sizeof(envTmp), 0) >= 0) GAPI::swNearClip = false;  // ENV:OLNOCLIP
#endif
    me->pr_WindowPtr = oldWin;
    if (!sound) printf("sound: off\n");
    printf("volume: music %d sound %d of %d, reverb %d\n", Core::settings.audio.music, Core::settings.audio.sound, SND_MAX_VOLUME, Core::settings.audio.reverb ? 1 : 0);

    int loopFrame = 0;
    FILE *sndDump = NULL;                // OUTPREFIX.pcm: 44.1 kHz 16-bit stereo big-endian
    if (headlessFrames && __argc > 4 && loadScript(__argv[4])) {
        fakeTimeMS = 0;
        char name[256];
        sprintf(name, "%s.pcm", outPrefix);
        sndDump = fopen(name, "wb");
    }
    int frames = 0, tUpd = 0, tRen = 0, tStart = osGetTimeMS();
    int levelFrames = -1;
    FILE *out = NULL;
    if (outPrefix) {
        char name[256];
        sprintf(name, "%s.txt", outPrefix);
        out = fopen(name, "w");
    }
    while (!quit) {
        if (!headlessFrames) pollInput();
        bool shotNow = false;
        while (scriptPos < scriptCount && script[scriptPos].frame <= loopFrame) {
            ScriptEvent &e = script[scriptPos++];
            if (e.key == -1) shotNow = true;
            else Input::setDown(InputKey(e.key), e.down != 0);
            if (out && inventory)
                fprintf(out, "script %d key %d %d: inv active %d ring %d page %d choose %d title %d dt %d settings %d\n", loopFrame, e.key, e.down,
                        inventory->active, int(inventory->phaseRing * 100), int(inventory->phasePage * 100), int(inventory->phaseChoose * 100),
                        int(inventory->titleTimer * 100), int(Core::deltaTime * 1000), Core::settings.version);
        }
        if (fakeTimeMS >= 0) fakeTimeMS += 33;

        int a = osGetTimeMS();
        bool draw = Game::update();
        if (Core::isQuit) break;
        // a level the loader rejected leaves an empty level behind: rendering it crashes the machine
        if (Game::level && Game::level->level.roomsCount == 0) {
            printf("level failed to load (unsupported format?) - quitting\n");
            break;
        }
        int b = osGetTimeMS();
        if (draw) {
            Game::render();              // ends with GAPI::endFrame: Maggie has finished the frame
            if (!headlessFrames) {       // show it, draw the next one into the oldest buffer
                int shown = (fbBack + FB_COUNT - 1) % FB_COUNT;   // on screen until the next vblank
                *SAGA_GFXADDR = (ULONG)fb;
                fbHidden[shown] = osGetTimeMS();
                fbBack = (fbBack + 1) % FB_COUNT;
                if (osGetTimeMS() - fbHidden[fbBack] < 20) WaitTOF();   // very fast frames only
                fb = fbBuf[fbBack];
                GAPI::swColor = fb;
            }
        }
        int c = osGetTimeMS();
        tUpd += b - a; tRen += c - b;

        static int noLevelFrames = 0;
        if (headlessFrames && !Game::level && ++noLevelFrames > 500) {
            printf("headless: no level after 500 frames - quitting\n");
            break;
        }
        if (shotNow && outPrefix) saveFrame(outPrefix, loopFrame, fb);
        if (sndDump) {                   // scripted runs: the mixer's output, 1/30 s per loop
            static Sound::Frame frames[SND_DUMP_FRAMES];
            struct EClockVal ev;         // real time of the mix, for the report at the end
            ElapsedTime(&ev);
            Sound::fill(frames, SND_DUMP_FRAMES);
            sndMixTime += ElapsedTime(&ev);
            sndMixCount++;
            fwrite(frames, sizeof(frames), 1, sndDump);
        }
        if (out && fakeTimeMS >= 0 && (loopFrame % 30) == 0) {
            fprintf(out, "loop %d level %d\n", loopFrame, Game::level ? (int)Game::level->level.id : -1);
            fflush(out);
        }
        loopFrame++;
        if (headlessFrames && Game::level) {
            if (levelFrames < 0) {
                levelFrames = 0;
                if (out) { dumpLevelStats(out); fflush(out); }
            }
            if (out) { fprintf(out, "frame %d draw %d update %d ms render %d ms\n", levelFrames, draw ? 1 : 0, b - a, c - b); fflush(out); }
            if (++levelFrames >= headlessFrames) {
                char name[256];
                sprintf(name, "%s.raw", outPrefix);
                FILE *f = fopen(name, "wb");
                if (f) { fwrite(fb, 2, SCREEN_WIDTH * SCREEN_HEIGHT, f); fclose(f); }
                break;
            }
        }

        if (frames % 50 == 49) {          // remote quit (tools: RAM:olquit)
            BPTR l = Lock((STRPTR)"RAM:olquit", ACCESS_READ);
            if (l) { UnLock(l); DeleteFile((STRPTR)"RAM:olquit"); break; }
            // remote setting: RAM:olerr holding a number 1..8 sets the texture error bound (OLPERSPERR) live
            l = Lock((STRPTR)"RAM:olerr", ACCESS_READ);
            if (l) {
                UnLock(l);
                FILE *ef = fopen("RAM:olerr", "r");
                int e = 0;
                if (ef) { if (fscanf(ef, "%d", &e) != 1) e = 0; fclose(ef); }
                DeleteFile((STRPTR)"RAM:olerr");
                if (e >= 1 && e <= 8) { Mag::perspErr = float(e); printf("texture error bound now %d px\n", e); fflush(stdout); }
            }
            // remote screenshot: RAM:olshot -> RAM:olshot-NNNN.raw (the frame on screen, 320x240 RGB565)
            l = Lock((STRPTR)"RAM:olshot", ACCESS_READ);
            if (l) {
                UnLock(l);
                DeleteFile((STRPTR)"RAM:olshot");
                static int shotNum = 0;
                const uint16 *shown = headlessFrames ? fb : fbBuf[(fbBack + FB_COUNT - 1) % FB_COUNT];
                saveFrame("RAM:olshot", shotNum++, shown);
            }
        }
        if (++frames == 100) {
            int all = osGetTimeMS() - tStart;
            printf("100 frames: %d ms/frame (%d.%d fps)  update %d.%d ms  render %d.%d ms",
                   all / 100, 100000 / (all ? all : 1), (1000000 / (all ? all : 1)) % 10,
                   tUpd / 100, (tUpd / 10) % 10, tRen / 100, (tRen / 10) % 10);
            if (sndMixCount) {               // mixer cost per 1024-frame (23.2 ms) buffer
                ULONG us = (ULONG)(((uint64)sndMixTime * 1000000) >> 16) / sndMixCount;
                printf("  mix %lu.%lu ms / 23.2 ms buffer", us / 1000, (us / 100) % 10);
                sndMixTime = sndMixCount = 0;
            }
            printf("\n");
            frames = 0; tUpd = tRen = 0; tStart = osGetTimeMS();
        }
    }

#ifdef OL_PROFILE
    printf("ENC %u textures %u texels\n", Mag::encodes, Mag::encodeTexels);
    printf("LIGHT corners %u lightsChecked %u lightsReaching %u\n", Mag::cnt[10], Mag::cnt[8], Mag::cnt[9]);
    printf("CNT polys %u corners %u drawn %u spans %u pixels %u c5 %u cells %u skipped %u loops %d\n", Mag::cnt[0], Mag::cnt[1], Mag::cnt[2], Mag::cnt[3], Mag::cnt[4], Mag::cnt[5], Mag::cnt[6], Mag::cnt[7], loopFrame);
#endif
    if (sound) sndStop();
    if (sndDump && sndMixCount) {
        ULONG us = (ULONG)(((uint64)sndMixTime * 1000000) >> 16) / sndMixCount;
        printf("mixer: %lu.%lu ms per %d frames (23.2 ms of sound)\n", us / 1000, (us / 100) % 10, SND_DUMP_FRAMES);
    }
    if (sndDump) fclose(sndDump);
    if (out) fclose(out);
    Game::deinit();
    videoFree();
    CloseLibrary(LowLevelBase);
    return 0;
}

// Run olMain() on a 512 KB stack when the shell's is smaller (rsh / Execute give a few KB).
// Only statics are touched around StackSwap(): locals live on the stack being swapped out.
static struct StackSwapStruct olStack;
static UBYTE *olStackMem;
static int olResult;

int main()
{
    struct Task *task = FindTask(NULL);
    if ((ULONG)task->tc_SPUpper - (ULONG)task->tc_SPLower >= OL_STACK_SIZE)
        return olMain();
    olStackMem = (UBYTE*)AllocVec(OL_STACK_SIZE, MEMF_ANY);
    if (!olStackMem) { printf("no memory for a %d KB stack\n", OL_STACK_SIZE / 1024); return 20; }
    olStack.stk_Lower   = olStackMem;
    olStack.stk_Upper   = (ULONG)(olStackMem + OL_STACK_SIZE);
    olStack.stk_Pointer = (APTR)(olStackMem + OL_STACK_SIZE);
    StackSwap(&olStack);
    olResult = olMain();
    StackSwap(&olStack);
    FreeVec(olStackMem);
    return olResult;
}
