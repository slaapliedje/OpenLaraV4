// Arne (SAGA audio, Apollo V4) for the game's sound effects: each playing effect gets a hardware
// channel that plays the sample from fast RAM by DMA, with its pitch in the period register and its
// left/right volume in the volume register. The software mixer (sound.h) then has only the music and
// movie soundtracks left to mix. Facts from vampire-probes arneprobe (vampire-v4-dev skill, arne.md):
//   $DFF400 + n*$10: +0 pointer (L), +4 length in counts of 2 frames (L, never 0), +8 volume L<<8|R (W),
//   +A control (W: bit 0 16-bit, bit 1 one-shot, bit 2 stereo), +C period (W, rate = 3546895 / period,
//   keep >= 59, never 0). DMACON2 $DFF296 (write) / $DFF202 (read): bit n-4 = channel n, bit 15 set.
//   A one-shot clears its DMA bit at the end. Length and pointer latch at the next block; the period
//   applies at once. A stopped channel restarted within ~4 sample periods resumes its old block: a
//   channel stopped in one mixer pass is not used again in the same pass (passes are 23 ms apart).
//   AHI's V4 driver plays on channel 7: never used here, nor anything busy when the game starts.
// Samples are 16-bit signed big-endian mono copies of the level's sounds, made once per sound.
#ifndef H_ARNE
#define H_ARNE

#include <exec/memory.h>
#include <proto/exec.h>

namespace Arne {

    #define ARNE_REG_W(a)  (*(volatile uint16*)(a))
    #define ARNE_REG_L(a)  (*(volatile uint32*)(a))
    #define ARNE_CH(n)     (0xDFF400 + (n) * 0x10)
    #define ARNE_DMACON2   0xDFF296
    #define ARNE_DMACON2R  0xDFF202
    #define ARNE_INTREQ2   0xDFF29C
    #define ARNE_PAL_CLOCK 3546895

    bool     enabled = false;            // set by the platform once the live sound task runs
    uint16   usable  = 0;                // bit n: channel n may be used
    uint16   inUse   = 0;                // bit n: a sample plays on channel n
    uint16   cooling = 0;                // bit n: stopped since the current mixer pass began
    uint16   blocked = 0;                // bit n: stopped since the previous pass began: not reused in this one
    uint16   touched = 0;                // bit n: channels this program has ever started (stopped at exit)

    // the platform calls this when the sound task is up (AHI holds its channel by then)
    void init() {
        uint16 busy = ARNE_REG_W(ARNE_DMACON2R);
        usable = 0;
        for (int n = 4; n <= 15; n++)
            if (n != 7 && !(busy & (1 << (n - 4)))) usable |= 1 << n;
        inUse = cooling = blocked = touched = 0;
        enabled = usable != 0;
    }

    inline void stop(int n) {
        ARNE_REG_W(ARNE_DMACON2) = 1 << (n - 4);
        ARNE_REG_W(ARNE_CH(n) + 8) = 0;
        ARNE_REG_W(ARNE_INTREQ2) = 1 << (n - 4);
        inUse &= ~(1 << n);
        cooling |= 1 << n;
    }

    // a free channel that was not stopped in this pass, or -1
    inline int alloc() {
        uint16 free = usable & ~inUse & ~cooling & ~blocked;
        for (int n = 15; n >= 4; n--)
            if (free & (1 << n)) { inUse |= 1 << n; return n; }
        return -1;
    }

    inline uint16 period(int freq, float pitch) {
        float hz = float(freq) * (pitch > 0.0f ? pitch : 1.0f);
        int32 p = hz > 1.0f ? int32(float(ARNE_PAL_CLOCK) / hz + 0.5f) : 65535;
        return uint16(p < 59 ? 59 : (p > 65535 ? 65535 : p));
    }

    inline uint16 volume(float l, float r) {
        int32 vl = int32(l * 255.0f + 0.5f), vr = int32(r * 255.0f + 0.5f);
        vl = vl < 0 ? 0 : (vl > 255 ? 255 : vl);
        vr = vr < 0 ? 0 : (vr > 255 ? 255 : vr);
        return uint16((vl << 8) | vr);
    }

    // data: 16-bit mono big-endian, 8-byte aligned; frames even
    inline void start(int n, const int16 *data, uint32 frames, bool loop, uint16 vol, uint16 per) {
        ARNE_REG_W(ARNE_DMACON2) = 1 << (n - 4);
        ARNE_REG_L(ARNE_CH(n))      = uint32(data);
        ARNE_REG_L(ARNE_CH(n) + 4)  = frames / 2;
        ARNE_REG_W(ARNE_CH(n) + 8)  = vol;
        ARNE_REG_W(ARNE_CH(n) + 10) = loop ? 0x0001 : 0x0003;
        ARNE_REG_W(ARNE_CH(n) + 12) = per;
        ARNE_REG_W(ARNE_INTREQ2)    = 1 << (n - 4);
        ARNE_REG_W(ARNE_DMACON2)    = 0x8000 | (1 << (n - 4));
        touched |= 1 << n;
    }

    inline void setVolume(int n, uint16 vol) { ARNE_REG_W(ARNE_CH(n) + 8) = vol; }
    inline void setPeriod(int n, uint16 per) { ARNE_REG_W(ARNE_CH(n) + 12) = per; }
    inline bool playing(int n) { return (ARNE_REG_W(ARNE_DMACON2R) & (1 << (n - 4))) != 0; }

    // --- converted samples, one per sound of the level (keyed by its data in the level) ---
    struct Buf {
        const uint8 *src;                // the sound's first data byte in the level
        void        *mem;                // AllocVec block
        int16       *data;               // 8-byte aligned
        uint32       frames;             // even
        int          freq;
    };
    #define ARNE_BUFS 512
    Buf bufs[ARNE_BUFS];
    int bufCount = 0;

    // src: PCM bytes (bits 16 = little-endian signed, 8 = unsigned, -8 = signed), mono
    const Buf* get(const uint8 *src, int bytes, int bits, int freq) {
        for (int i = 0; i < bufCount; i++)
            if (bufs[i].src == src && bufs[i].freq == freq) return &bufs[i];
        if (bufCount >= ARNE_BUFS) return NULL;
        uint32 frames = bits == 16 ? uint32(bytes / 2) : uint32(bytes);
        if (frames < 2) return NULL;
        uint32 alloc = (frames + 1) & ~1U;
        void *mem = AllocVec(alloc * 2 + 16, MEMF_FAST | MEMF_CLEAR);
        if (!mem) mem = AllocVec(alloc * 2 + 16, MEMF_ANY | MEMF_CLEAR);
        if (!mem) return NULL;
        int16 *d = (int16*)((uint32(mem) + 7) & ~7U);
        for (uint32 i = 0; i < frames; i++) {
            int32 v;
            if (bits == 16)     v = int16(src[i * 2] | (src[i * 2 + 1] << 8));
            else if (bits == 8) v = int32(src[i]) * 257 - 32768;
            else                v = (int32(int8(src[i])) + 128) * 257 - 32768;
            d[i] = int16(v);                     // the 68k stores big-endian, as Arne reads
        }
        Buf &b = bufs[bufCount++];
        b.src = src; b.mem = mem; b.data = d; b.frames = alloc; b.freq = freq;
        return &b;
    }

    // all channels stopped first (the caller's job): free the converted sounds
    void flush() {
        for (int i = 0; i < bufCount; i++) FreeVec(bufs[i].mem);
        bufCount = 0;
    }

    void shutdown() {
        for (int n = 4; n <= 15; n++) if (touched & (1 << n)) stop(n);
        // let the last DMA reads finish before the memory goes (dos.library's Delay clashes with the
        // engine's Input namespace here): a few thousand register reads, well over 4 sample periods
        for (int i = 0; i < 20000; i++) (void)ARNE_REG_W(ARNE_DMACON2R);
        flush();
        enabled = false;
    }
}

#endif
