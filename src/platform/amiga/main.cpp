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
static uint16* fbBuf[2];                // double buffering: SAGA shows one while we draw the other
static int fbBack = 0;
static ULONG sagaOld = 0;
#define SAGA_GFXADDR   ((volatile ULONG*)0xDFF1EC)
#define SAGA_GFXADDR_R ((volatile ULONG*)0xDFE1EC)

#define SCREEN_WIDTH  320
#define SCREEN_HEIGHT 240

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

void* osMutexInit()            { return (void*)1; }   // single-threaded build
void  osMutexFree(void*)       {}
void  osMutexLock(void*)       {}
void  osMutexUnlock(void*)     {}
void* osRWLockInit()           { return (void*)1; }
void  osRWLockFree(void*)      {}
void  osRWLockRead(void*)      {}
void  osRWUnlockRead(void*)    {}
void  osRWLockWrite(void*)     {}
void  osRWUnlockWrite(void*)   {}

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
    struct IntuiMessage* msg;
    while (window && (msg = (struct IntuiMessage*)GetMsg(window->UserPort))) {
        if (msg->Class == IDCMP_RAWKEY) {
            UWORD code = msg->Code & 0x7F;
            bool down = !(msg->Code & 0x80);
            if (code == 0x59 && down) quit = true;         // F10
        #ifdef OL_MAGGIE
            if (code == 0x56 && down) Mag::zEnabled = !Mag::zEnabled;   // F7: depth buffer on/off
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
    if (modeID == (ULONG)INVALID_ID) { printf("no 320x240x16 mode\n"); return false; }
    screen = OpenScreenTags(NULL, SA_DisplayID, modeID, SA_Depth, 16, SA_ShowTitle, FALSE, SA_Quiet, TRUE,
                            SA_Type, CUSTOMSCREEN, TAG_DONE);
    if (!screen) { printf("OpenScreen failed\n"); return false; }
    window = OpenWindowTags(NULL, WA_CustomScreen, (ULONG)screen, WA_Width, screen->Width, WA_Height, screen->Height,
                            WA_Flags, WFLG_ACTIVATE | WFLG_RMBTRAP | WFLG_BACKDROP | WFLG_BORDERLESS,
                            WA_IDCMP, IDCMP_RAWKEY, TAG_DONE);
    ULONG base = 0, bpr = 0;
    APTR lock = LockBitMapTags(screen->RastPort.BitMap, LBMI_BASEADDRESS, (ULONG)&base, LBMI_BYTESPERROW, (ULONG)&bpr, TAG_DONE);
    if (lock) UnLockBitMap(lock);
    printf("mode %08lx screen %dx%d bpr %lu base %08lx shown %08lx\n", modeID, screen->Width, screen->Height, bpr, base, *SAGA_GFXADDR_R);
    if (bpr != SCREEN_WIDTH * 2 || screen->Width != SCREEN_WIDTH) { printf("unexpected screen layout\n"); return false; }

    // two frame buffers of our own, shown by pointing SAGA at them (P96 screen buffers flicker on vampiregfx)
    ULONG size = (SCREEN_WIDTH * 2 * screen->Height + 63) & ~63UL;
    fbMem = (UBYTE*)AllocVec(size * 2 + 64, MEMF_ANY | MEMF_CLEAR);
    if (!fbMem) return false;
    fbBuf[0] = (uint16*)(((ULONG)fbMem + 63) & ~63UL);
    fbBuf[1] = (uint16*)((UBYTE*)fbBuf[0] + size);
    sagaOld = base;
    *SAGA_GFXADDR = (ULONG)fbBuf[1];
    fbBack = 0;
    fb = fbBuf[0];
    return true;
}

static void videoFree()
{
    if (screen && sagaOld) { *SAGA_GFXADDR = sagaOld; WaitTOF(); WaitTOF(); }
    if (window) CloseWindow(window);
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
    Game::init(level);
    printf("init %d ms\n", osGetTimeMS() - t0);

    GAPI::resize();
    GAPI::swColor = fb;

    int loopFrame = 0;
    if (headlessFrames && __argc > 4 && loadScript(__argv[4]))
        fakeTimeMS = 0;
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
            if (!headlessFrames) {       // show it, draw the next one into the other buffer
                *SAGA_GFXADDR = (ULONG)fb;
                fbBack ^= 1;
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
        }
        if (++frames == 100) {
            int all = osGetTimeMS() - tStart;
            printf("100 frames: %d ms/frame (%d.%d fps)  update %d.%d ms  render %d.%d ms\n",
                   all / 100, 100000 / (all ? all : 1), (1000000 / (all ? all : 1)) % 10,
                   tUpd / 100, (tUpd / 10) % 10, tRen / 100, (tRen / 10) % 10);
            frames = 0; tUpd = tRen = 0; tStart = osGetTimeMS();
        }
    }

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
