#ifndef H_SOUND
#define H_SOUND

#ifdef _OS_TNS
    #define NO_SOUND
#endif

#ifndef NO_SOUND
    #define DECODE_ADPCM
    #define DECODE_IMA
    #define DECODE_VAG
    #define DECODE_XA
    #define DECODE_OGG

    #if !defined(_OS_PSP) && !defined(_OS_WEB) && !defined(_OS_PSV) && !defined(_OS_3DS) && !defined(_OS_XBOX) && !defined(_OS_XB1)
        #define DECODE_MP3
    #endif
#endif

#include "utils.h"

#ifdef DECODE_MP3
    #include "libs/minimp3/minimp3.h"
#endif

#ifdef DECODE_OGG
    #ifdef USE_LIBVORBIS
        #include <tremor/ivorbisfile.h>
    #else
        #define STB_VORBIS_HEADER_ONLY
        #include "libs/stb_vorbis/stb_vorbis.c"
    #endif
#endif

#define SND_CHANNELS_MAX    128
#define SND_FADEOFF_DIST    (1024.0f * 8.0f)
#define SND_LOWPASS_FREQ    0.2f
#define SND_MAX_VOLUME      20
#define AIFF_HEAD           4096        // bytes read when an AIFF track starts (the header)
#define AIFF_PUMP           (32 * 1024) // bytes read per mixer pass after that (1.4 MB/s)
#define SND_PAN_FACTOR      0.7f
#define SND_FACING_FACTOR   0.3f

#ifdef OL_ARNE
#include "platform/amiga/arne.h"   // hardware channels for the sound effects
#endif
#ifdef OL_AMMX
extern "C" void magVolAMMX(short *frames, long groups, long factor);   // platform/amiga/asm/mixammx.s
#endif

namespace Sound {

    static const int8 SPU_POS[] = { 0, 60, 115,  98, 122 };
    static const int8 SPU_NEG[] = { 0,  0, -52, -55, -60 };

    static const int16 SPU_ZIG_ZAG[7][30] = {
        { 0, 0, 0, 0, 0, 0, -0x0002, +0x000A, -0x0022, +0x0041, -0x0054, +0x0034, +0x0009, -0x010A, +0x0400, -0x0A78, +0x234C, +0x6794, -0x1780, +0x0BCD, -0x0623, +0x0350, -0x016D, +0x006B, +0x000A, -0x0010, +0x0011, -0x0008, +0x0003, -0x0001 },
        { 0, 0, 0, 0, -0x0002, 0, +0x0003, -0x0013, +0x003C, -0x004B, +0x00A2, -0x00E3, +0x0132, -0x0043, -0x0267, +0x0C9D, +0x74BB, -0x11B4, +0x09B8, -0x05BF, +0x0372, -0x01A8, +0x00A6, -0x001B, +0x0005, +0x0006, -0x0008, +0x0003, -0x0001, 0 },
        { 0, 0, 0, -0x0001, +0x0003, -0x0002, -0x0005, +0x001F, -0x004A, +0x00B3, -0x0192, +0x02B1, -0x039E, +0x04F8, -0x05A6, +0x7939, -0x05A6, +0x04F8, -0x039E, +0x02B1, -0x0192, +0x00B3, -0x004A, +0x001F, -0x0005, -0x0002, +0x0003, -0x0001, 0, 0 },
        { 0, 0, -0x0001, +0x0003, -0x0008, +0x0006, +0x0005, -0x001B, +0x00A6, -0x01A8, +0x0372, -0x05BF, +0x09B8, -0x11B4, +0x74BB, +0x0C9D, -0x0267, -0x0043, +0x0132, -0x00E3, +0x00A2, -0x004B, +0x003C, -0x0013, +0x0003, 0, -0x0002, 0, 0, 0 },
        { 0, -0x0001, +0x0003, -0x0008, +0x0011, -0x0010, +0x000A, +0x006B, -0x016D, +0x0350, -0x0623, +0x0BCD, -0x1780, +0x6794, +0x234C, -0x0A78, +0x0400, -0x010A, +0x0009, +0x0034, -0x0054, +0x0041, -0x0022, +0x000A, -0x0001, 0, +0x0001, 0, 0, 0 },
        { 0, +0x0002, -0x0008, +0x0010, -0x0023, +0x002B, +0x001A, -0x00EB, +0x027B, -0x0548, +0x0AFA, -0x16FA, +0x53E0, +0x3C07, -0x1249, +0x080E, -0x0347, +0x015B, -0x0044, -0x0017, +0x0046, -0x0023, +0x0011, -0x0005, 0, 0, 0, 0, 0, 0 },
        { 0, -0x0005, +0x0011, -0x0023, +0x0046, -0x0017, -0x0044, +0x015B, -0x0347, +0x080E, -0x1249, +0x3C07, +0x53E0, -0x16FA, +0x0AFA, -0x0548, +0x027B, -0x00EB, +0x001A, +0x002B, -0x0023, +0x0010, -0x0008, +0x0002, 0, 0, 0, 0, 0, 0 },
    };

    struct Frame {
        int16 L, R;
    };

    struct FrameHI {
        int32 L, R;
    };

    struct Stats {
        int mixer;
        int reverb;
        int render[2];
        int ogg;
    } stats;

    namespace Filter {
        #define MAX_FDN       16
        #define MAX_DELAY     954

        #define DSP_SCALE_BIT 8
        #define DSP_SCALE     (1 << DSP_SCALE_BIT)

        static const int16 FDN[MAX_FDN] = { 281, 331, 373, 419, 461, 503, 547, 593, 641, 683, 727, 769, 811, 853, 907, 953 };

        struct Delay {
            int     index;
            int16   out[MAX_DELAY];

            void process(int16 &x, int16 delay) {
                index = (index + 1) % delay;
                int16 y = out[index];
                out[index] = x;
                x = y;
            }
        };

        struct Absorption {
            int16 out;

            void process(int16 &x, int32 gain, int32 damping) {
                x = out = (out * damping + ((x * gain * (DSP_SCALE - damping)) >> DSP_SCALE_BIT)) >> DSP_SCALE_BIT;
            }
        };

        struct LowPass {
            float buffer[2][4];

            LowPass() {
                memset(buffer, 0, sizeof(buffer));
            }

            inline void process(int32 &x, float *out, float freq) {
                out[0] += freq * (float(x) - out[0]);
                out[1] += freq * (out[0] - out[1]);
                out[2] += freq * (out[1] - out[2]);
                out[3] += freq * (out[2] - out[3]);
                x = int32(out[3]);
            }

            void process(FrameHI *frames, int count, float freq) {
                for (int i = 0; i < count; i++) {
                    process(frames[i].L, buffer[0], freq);
                    process(frames[i].R, buffer[1], freq);
                }
            }
        };

        struct Reverberation {
            Delay       df[MAX_FDN];
            Absorption  af[MAX_FDN];

            int32       output[MAX_FDN];
            int16       buffer[MAX_FDN];
            int16       panCoeff[MAX_FDN][2];
            int32       absCoeff[MAX_FDN][2];   // absorption gain & damping

            Reverberation() {
                for (int i = 0; i < MAX_FDN; i++) {
                    panCoeff[i][0] = (i % 2) ? -1 : 1;
                }

                for (int i = 0; i < MAX_FDN; i += 2) {
                    if ((i / 2) % 2)
                        panCoeff[i][1] = panCoeff[i + 1][1] = -1;
                    else
                        panCoeff[i][1] = panCoeff[i + 1][1] =  1;
                }

                clear();
            }

            void clear() {
            #ifdef OL_FASTMATH
                roomSizeValid = false;
            #endif
                memset(output, 0, sizeof(output));
                memset(df, 0, sizeof(df));
                memset(af, 0, sizeof(af));

                setRoomSize(vec3(1.0f));
            }

        #ifdef OL_FASTMATH
            // The camera sets the room size every frame (camera.h updateListener) and each call costs
            // eight powf: recompute only when the size changes, and not at all with reverb off (the
            // coefficients are marked stale and made on the first call after it is turned on).
            vec3 roomSize;
            bool roomSizeValid;
        #endif

            void setRoomSize(const vec3 &size) {
            #ifdef OL_FASTMATH
                if (!Core::settings.audio.reverb) { roomSizeValid = false; return; }
                if (roomSizeValid && size.x == roomSize.x && size.y == roomSize.y && size.z == roomSize.z) return;
                roomSize = size;
                roomSizeValid = true;
            #endif
                float S = (size.x * size.z + size.x * size.y + size.z * size.y) * 2;
                float V = size.x * size.y * size.z;
                float f = 0.1f; // absorption factor
                float rt60 = 0.161f * (V / (S * f));
                float k = -10.0f / (44100.0f * rt60);

                for (int i = 0; i < MAX_FDN; i++) {
                    float v = powf(10.0f, FDN[i] * k);
                    absCoeff[i][0] = int32(v * DSP_SCALE);
                    absCoeff[i][1] = int32((1.0f - (2.0f / (1.0f + powf(v, 1.0f - 1.0f / 0.15f)))) * DSP_SCALE);
                }
            };

            void process(FrameHI *frames, int count) {
                PROFILE_CPU_TIMING(stats.reverb);

                for (int i = 0; i < count; i++) {
                    FrameHI &frame = frames[i];
                    int32 in  = (frame.L + frame.R) / 2;
                    int32 out = 0;
                    int32 L   = 0;
                    int32 R   = 0;

                // apply delay & absorption filters
                    for (int j = 0; j < MAX_FDN; j++) {
                        int16 k = clamp(in + output[j], -0x7FFF, 0x7FFF);
                        df[j].process(k, FDN[j]);
                        af[j].process(k, absCoeff[j][0], absCoeff[j][1]);
                        buffer[j] = k;
                        out += k;
                    }
                    out = out * 2 / MAX_FDN;

                // apply pan
                    int16 buf = buffer[MAX_FDN - 1];
                    for (int j = 0; j < MAX_FDN; j++) {
                        output[j] = max(0, out - buf);
                        buf = buffer[j];
                        L += buf ^ panCoeff[j][0];
                        R += buf ^ panCoeff[j][1];
                    }

                    frame.L += L / MAX_FDN;
                    frame.R += R / MAX_FDN;
                }
            }
        };

        #undef MAX_FDN
        #undef MAX_DELAY
    };

    struct Decoder {
        Stream  *stream;
        int     channels, freq, offset;
        Frame   prevFrame;

        Decoder(Stream *stream, int channels, int freq) : stream(stream), channels(channels), freq(freq), offset(stream ? stream->pos : 0) {
            memset(&prevFrame, 0, sizeof(prevFrame));
        }

        virtual ~Decoder() { delete stream; }
        virtual int decode(Frame *frames, int count) { return 0; }
        // a mono PCM sound in memory (a level's sound effect), for hardware channels: its bytes and format
        virtual bool hwSource(const uint8 *&src, int &bytes, int &bits, int &rate) { return false; }
        // samples already as Arne plays them (16-bit big-endian, 8-byte aligned, in memory): the music
        virtual bool hwDirect(const int16 *&data, uint32 &frames, int &ch, int &rate) { return false; }
        virtual void replay() { stream->seek(offset - stream->pos); }
        // the sound task's turn to read more of a file it plays from memory (AIFF)
        virtual void pump() {}

        __attribute__((always_inline)) inline int resample(Sound::Frame *frames, Sound::Frame &frame) {
            if (freq == 44100) {
                frames[0] = frame;
                return 1;
            }

            int dL = int(frame.L) - int(prevFrame.L);
            int dR = int(frame.R) - int(prevFrame.R);
            switch (freq) {
                case 11025 :
                    if (channels == 2) {
                        frames[0].L = prevFrame.L + dL / 4;                   // 0.25 L
                        frames[0].R = prevFrame.R + dR / 4;                   // 0.25 R
                        frames[1].L = prevFrame.L + dL / 2;                   // 0.50 L
                        frames[1].R = prevFrame.R + dR / 2;                   // 0.50 R
                        frames[2].L = prevFrame.L + dL * 3 / 4;               // 0.75 L
                        frames[2].R = prevFrame.R + dR * 3 / 4;               // 0.75 R
                    } else {
                        frames[0].L = frames[0].R = prevFrame.L + dL / 4;     // 0.25 LR
                        frames[1].L = frames[1].R = prevFrame.L + dL / 2;     // 0.50 LR
                        frames[2].L = frames[2].R = prevFrame.L + dL * 3 / 4; // 0.75 LR
                    }
                    frames[3] = prevFrame = frame;                            // 1.00 LR
                    return 4;
                case 22050 :
                    if (channels == 2) {
                        frames[0].L = prevFrame.L + dL / 2;                   // 0.50 L
                        frames[0].R = prevFrame.R + dR / 2;                   // 0.50 R
                    } else
                        frames[0].L = frames[0].R = prevFrame.L + dL / 2;     // 0.50 LR
                    frames[1] = prevFrame = frame;                            // 1.00 LR
                    return 2;
                default    : // impossible
                    ASSERT(false);
                    int k = 44100 / freq;
                    for (int i = 0; i < k; i++) frames[i] = frame; // no lerp
                    return k;
            }        
        }
    };

    struct PCM : Decoder {
        int size, bits;

        PCM(Stream *stream, int channels, int freq, int size, int bits) : Decoder(stream, channels, freq), size(size), bits(bits) {}

        virtual bool hwSource(const uint8 *&src, int &bytes, int &bitsOut, int &rate) {
            if (!stream || !stream->data || channels != 1 || !(bits == 16 || bits == 8 || bits == -8)) return false;
            if (freq != 11025 && freq != 22050 && freq != 44100) return false;
            src = (const uint8*)stream->data + offset;
            bytes = size < stream->size - offset ? size : stream->size - offset;
            bitsOut = bits; rate = freq;
            return bytes > 4;
        }

        virtual int decode(Frame *frames, int count) {
            if (stream->pos - offset >= size) return 0;
            // A sample in memory (the level's sounds): the whole request in one call, straight from the
            // bytes. The per-sample path below costs a virtual call, a stream read and a byte swap for
            // every source sample. Same samples, same resampling, same end position.
            if (stream->data && (bits == 16 || bits == 8 || bits == -8) && channels >= 1 && channels <= 2) {
                const uint8 *src = (const uint8*)stream->data;
                int p = stream->pos, done = 0;
                const int end = offset + size, bytes = (bits == 16 ? 2 : 1) * channels;
                while (done < count && p - offset < size && p + bytes <= stream->size) {
                    Frame frame;
                    if (bits == 16) {
                        frame.L = int16(src[p] | (src[p + 1] << 8));
                        frame.R = channels == 2 ? int16(src[p + 2] | (src[p + 3] << 8)) : frame.L;
                    } else if (bits == 8) {
                        frame.L = int16(src[p] * 257 - 32768);
                        frame.R = channels == 2 ? int16(src[p + 1] * 257 - 32768) : frame.L;
                    } else {
                        frame.L = int16((int8(src[p]) + 128) * 257 - 32768);
                        frame.R = channels == 2 ? int16((int8(src[p + 1]) + 128) * 257 - 32768) : frame.L;
                    }
                    p += bytes;
                    done += resample(frames + done, frame);
                }
                (void)end;
                stream->pos = p;
                return done;
            }

            // ! in the original game series only 11025 and 22050 Hz single channel samples were used ! //

            Frame frame;
            if (bits == 16) {
                int16 value;
                if (channels == 2) {
                    frame.L = stream->read(value);
                    frame.R = stream->read(value);
                } else
                    frame.L = frame.R = stream->read(value);
            } else if (bits == 8 || bits == -8) {

                if (bits > 0) {
                    uint8 value;
                    if (channels == 2) {
                        frame.L = stream->read(value) * 257 - 32768;
                        frame.R = stream->read(value) * 257 - 32768;
                    } else
                        frame.L = frame.R = stream->read(value) * 257 - 32768;
                } else {
                    int8 value;
                    if (channels == 2) {
                        frame.L = (stream->read(value) + 128) * 257 - 32768;
                        frame.R = (stream->read(value) + 128) * 257 - 32768;
                    } else
                        frame.L = frame.R = (stream->read(value) + 128) * 257 - 32768;
                }

            } else {
                ASSERT(false);
                return 0;
            }

            return resample(frames, frame);
        }
    };

    // AIFF, 16-bit PCM (the music on the V4): the whole file is read into RAM when the track starts,
    // then played from memory - no decoding, and on a big-endian machine not even a byte swap.
    // 22050 / 11025 Hz are stretched to 44.1 kHz like the PCM sounds.
    struct AIFF : Decoder {
        uint8       *mem;                // the samples' block (owned)
        uint8       *data;               // first sample frame, 8-byte aligned
        uint32       bytes, loaded;      // sample bytes in all / read in so far
        int          frames, pos;

        // The file is read into memory by the sound task, a piece per mixer pass (pump), so starting a
        // track costs the main thread only its header: Arne plays from the start of the block while the
        // rest comes in, at many times the speed it plays.
        AIFF(Stream *stream, uint8 *mem, uint8 *data, uint32 bytes, uint32 loaded, int ch, int rate)
            : Decoder(stream, ch, rate), mem(mem), data(data), bytes(bytes), loaded(loaded), frames(int(bytes / uint32(ch * 2))), pos(0) {
            if (loaded >= bytes) done();
        }

        virtual ~AIFF() { delete[] mem; }

        void done() { delete stream; stream = NULL; }   // all in: the file is closed

        virtual void pump() {
            if (!stream || loaded >= bytes) return;
            uint32 n = bytes - loaded;
            if (n > AIFF_PUMP) n = AIFF_PUMP;
            stream->raw(data + loaded, int(n));
            loaded += n;
            if (loaded >= bytes) done();
        }

        virtual int decode(Frame *out, int count) {
            int done = 0;
            const int step = channels * 2;
            const int avail = int(loaded / uint32(step));
            const uint8 *p = data + pos * step;
            while (done < count && pos < avail) {
                Frame f;
                f.L = int16((p[0] << 8) | p[1]);
                f.R = channels == 2 ? int16((p[2] << 8) | p[3]) : f.L;
                p += step; pos++;
                done += resample(out + done, f);
            }
            if (done < count && pos < frames) {  // still coming in: silence, not the end
                memset(out + done, 0, sizeof(Frame) * (count - done));
                done = count;
            }
            return done;
        }

        virtual void replay() { pos = 0; }

        virtual bool hwDirect(const int16 *&d, uint32 &n, int &ch, int &rate) {
            if (frames < 2) return false;
            d = (const int16*)data; n = uint32(frames) & ~1U; ch = channels; rate = freq;
            return true;
        }

        // the header -> a decoder, or NULL if it is not 16-bit PCM at 44100 / 22050 / 11025 Hz; the
        // stream (positioned after its 4-byte "FORM") is the decoder's from then on
        static AIFF* open(Stream *stream) {
            if (stream->size < 54) return NULL;
            uint8 head[AIFF_HEAD];
            int headLen = stream->size < AIFF_HEAD ? stream->size : AIFF_HEAD;
            stream->setPos(0);
            stream->raw(head, headLen);
            #define AIFF_BE32(q) ((uint32(q[0]) << 24) | (uint32(q[1]) << 16) | (uint32(q[2]) << 8) | uint32(q[3]))
            int ch = 0, bits = 0, rate = 0;
            uint32 nFrames = 0, ssndOfs = 0, ssndLen = 0;
            if (memcmp(head, "FORM", 4) || memcmp(head + 8, "AIFF", 4)) return NULL;
            uint32 at = 12;                      // file offset of the chunk header
            while (at + 16 <= uint32(headLen)) {
                const uint8 *c = head + at, *body = c + 8;
                uint32 len = AIFF_BE32((c + 4));
                if (!memcmp(c, "COMM", 4) && len >= 18 && at + 8 + 18 <= uint32(headLen)) {
                    ch = (body[0] << 8) | body[1];
                    nFrames = AIFF_BE32((body + 2));
                    bits = (body[6] << 8) | body[7];
                    // sample rate: 80-bit extended float, exponent 16383-biased, explicit integer bit
                    int e = ((body[8] & 0x7F) << 8 | body[9]) - 16383;
                    uint32 m = AIFF_BE32((body + 10));
                    rate = (e >= 0 && e < 32) ? int(m >> (31 - e)) : 0;
                } else if (!memcmp(c, "SSND", 4) && len >= 8) {
                    uint32 offset = AIFF_BE32(body);
                    ssndOfs = at + 16 + offset;
                    ssndLen = len - 8 - offset;
                    break;                       // the samples run to the chunk's end
                }
                at += 8 + len + (len & 1);
            }
            #undef AIFF_BE32
            if (!ssndOfs || bits != 16 || ch < 1 || ch > 2 || (rate != 44100 && rate != 22050 && rate != 11025)) return NULL;
            if (ssndOfs + ssndLen > uint32(stream->size)) ssndLen = uint32(stream->size) > ssndOfs ? uint32(stream->size) - ssndOfs : 0;
            uint32 avail = ssndLen / uint32(ch * 2);
            if (nFrames > avail) nFrames = avail;
            nFrames &= ~1U;                      // whole Arne blocks
            if (nFrames < 2) return NULL;
            uint32 bytes = nFrames * uint32(ch * 2);
            uint8 *mem = new uint8[bytes + 8];
            uint8 *data = (uint8*)((uint32(mem) + 7) & ~7U);   // Arne's DMA wants 8-byte alignment
            uint32 have = 0;                     // samples already in the header read
            if (ssndOfs < uint32(headLen)) {
                have = uint32(headLen) - ssndOfs;
                if (have > bytes) have = bytes;
                memcpy(data, head + ssndOfs, have);
            }
            stream->setPos(int(ssndOfs + have));
            stream->buffering = false;           // straight into the block
            return new AIFF(stream, mem, data, bytes, have, ch, rate);
        }
    };

#ifdef DECODE_ADPCM
    struct ADPCM : Decoder // https://wiki.multimedia.cx/?title=Microsoft_ADPCM
    {
        int size, block;

        struct Channel
        {
            int16 c1, c2;
            int16 delta;
            int16 sample1;
            int16 sample2;

            int predicate(uint8 nibble)
            {
                static const int table[] = { 230, 230, 230, 230, 307, 409, 512, 614, 768, 614, 512, 409, 307, 230, 230, 230 };

                int8 ns = nibble;
                if (ns & 8) ns -= 16;

                int sample = (sample1 * c1 + sample2 * c2) / 256 + ns * delta;
                sample  = clamp(sample, -32768, 32767);
                sample2 = sample1;
                sample1 = sample;
                delta   = max(table[nibble] * delta / 256, 16);
                return sample;
            }
        } channel[2];

        ADPCM(Stream *stream, int channels, int freq, int size, int block) : Decoder(stream, channels, freq), size(size), block(block)
        {
            memset(channel, 0, sizeof(channel));
        }

        virtual int decode(Frame *frames, int count)
        {
            static const int coeff1[] = { 256, 512, 0, 192, 240, 460, 392 };
            static const int coeff2[] = { 0, -256, 0, 64, 0, -208, -232 };
            
            int seek = stream->pos - offset;
            if (seek >= size) return 0;

            if (seek % block == 0)
            {
                for (int i = 0; i < channels; i++)
                {
                    uint8 index;
                    stream->read(index);
                    channel[i].c1 = coeff1[index];
                    channel[i].c2 = coeff2[index];
                }
                for (int i = 0; i < channels; i++) stream->read(channel[i].delta);
                for (int i = 0; i < channels; i++) stream->read(channel[i].sample1);
                for (int i = 0; i < channels; i++) stream->read(channel[i].sample2);

                if (channels == 1)
                {
                    if (freq == 22050)
                    {
                        ASSERT(count >= 4);
                        frames[0].L = frames[0].R =
                        frames[1].L = frames[1].R = channel[0].sample2;
                        frames[2].L = frames[2].R =
                        frames[3].L = frames[3].R = channel[0].sample1;
                        return 4;
                    } else {
                        ASSERT(count >= 2);
                        frames[0].L = frames[0].R = channel[0].sample2;
                        frames[1].L = frames[1].R = channel[0].sample1;
                        return 2;
                    }
                } else {
                    ASSERT(freq == 44100);
                    ASSERT(count >= 2);
                    frames[0].L = channel[0].sample2;
                    frames[0].R = channel[1].sample2;
                    frames[1].L = channel[0].sample1;
                    frames[1].R = channel[1].sample1;
                    return 2;
                }
            } else {
                uint8 value;
                stream->read(value);
                uint8 n1 = value >> 4, n2 = value & 0xF;

                if (channels == 1)
                {
                    if (freq == 22050)
                    {
                        ASSERT(count >= 4);
                        frames[0].L = frames[0].R =
                        frames[1].L = frames[1].R = channel[0].predicate(n1);
                        frames[2].L = frames[2].R =
                        frames[3].L = frames[3].R = channel[0].predicate(n2);
                        return 4;
                    } else {
                        ASSERT(count >= 2);
                        frames[0].L = frames[0].R = channel[0].predicate(n1);
                        frames[1].L = frames[1].R = channel[0].predicate(n2);
                        return 2;
                    }
                } else {
                    ASSERT(freq == 44100);
                    frames[0].L = channel[0].predicate(n1);
                    frames[0].R = channel[1].predicate(n2);
                    return 1;
                }
            }
        }
    };
#endif

#ifdef DECODE_IMA
    struct IMA : Decoder { // https://wiki.multimedia.cx/?title=Microsoft_ADPCM
        struct State {
            int amp, idx;
        } state[2];

        int freq;

        IMA(Stream *stream, int channels, int freq) : Decoder(stream, channels, freq) { 
            memset(state, 0, sizeof(state)); 
        }

        int16 getSample(uint8 n, State &state) {
            static int indexLUT[] = {
                -1, -1, -1, -1, 2, 4, 6, 8,
            };

            static int stepLUT[] = {
                7,     8,     9,     10,    11,    12,    13,    14,
                16,    17,    19,    21,    23,    25,    28,    31,
                34,    37,    41,    45,    50,    55,    60,    66,
                73,    80,    88,    97,    107,   118,   130,   143,
                157,   173,   190,   209,   230,   253,   279,   307,
                337,   371,   408,   449,   494,   544,   598,   658,
                724,   796,   876,   963,   1060,  1166,  1282,  1411,
                1552,  1707,  1878,  2066,  2272,  2499,  2749,  3024,
                3327,  3660,  4026,  4428,  4871,  5358,  5894,  6484,
                7132,  7845,  8630,  9493,  10442, 11487, 12635, 13899,
                15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
                32767
            };

            int step = stepLUT[state.idx];
            int idx  = n & 7;

            state.idx = clamp(state.idx + indexLUT[idx], 0, 88);

            int diff = (2 * idx + 1) * step >> 3;

            if (n & 8) {
                state.amp -= diff;
                if (state.amp < -32768)
                    state.amp = -32768;
            } else {
                state.amp += diff;
                if (state.amp > 32767)
                    state.amp = 32767;
            }

            return state.amp;
        }

        virtual int decode(Frame *frames, int count) {
            uint8 n;
            stream->read(n);

            int a = getSample(n >> 4, state[0]);
            int b = getSample(n,      state[1 % channels]);

            Frame frame;
            if (channels == 2) {
                frame.L = a;
                frame.R = b;
                return resample(frames, frame);
            } else {
                frame.L = frame.R = a;
                int i = resample(frames, frame);
                frame.L = frame.R = b;
                return i + resample(frames + i, frame);
            }
        }
    };
#endif

#ifdef DECODE_VAG
    struct VAG : Decoder {
        uint8 pred, shift, flags;
        int s1, s2;
        Frame buffer[28 * 4];
        int bufferSize;

        VAG(Stream *stream) : Decoder(stream, 1, 11025), s1(0), s2(0), bufferSize(0) {
            memset(buffer, 0, sizeof(buffer));
        }

        void predicate(short value) {
            int s = (s1 * SPU_POS[pred] + s2 * SPU_NEG[pred]) >> 6;
            s = clamp((value >> shift) + s, -32768, 32767);
            s2 = s1;
            s1 = s;
        }

        void resample(Frame *frames, short value) {
            predicate(value);
            frames[0].L = frames[0].R = s2 + (s1 - s2) / 4;  // 0.25
            frames[1].L = frames[1].R = s2 + (s1 - s2) / 2;  // 0.50
            frames[2].L = frames[2].R = s1 - (s1 - s2) / 4;  // 0.75
            frames[3].L = frames[3].R = s1;                  // 1.00
        }

        int processBlock() {
            if (stream->pos >= stream->size)
                return 0;
            stream->read(pred);
            stream->read(flags);
            shift = pred & 0x0F;
            pred >>= 4;

            int i = 0;
            while (i < 14) {
                uint8 d;
                stream->read(d);
                resample(&buffer[i * 8 + 0], (d & 0x0F) << 12);
                resample(&buffer[i * 8 + 4], (d & 0xF0) <<  8);
                i++;
            }
            return i * 8;
        }

        virtual int decode(Frame *frames, int count) {
            int res = 0;

            while (res < count) {
                if (!bufferSize && !(bufferSize = processBlock())) // if no data in buffer - process next block
                    break;

                int length = min(bufferSize, count - res);
                memcpy(&frames[res], buffer, length * sizeof(Frame));
                res += length;

                if (bufferSize -= length) { // if data remained in buffer, move it to the beginning
                    for (int i = 0; i < bufferSize; i++) {
                        buffer[i] = buffer[sizeof(buffer) / sizeof(Frame) - bufferSize + i];
                    }
                    break;
                }
            }

            return res;
        }

        virtual void replay() {
            stream->setPos(0);
            s1 = s2 = 0;
        }
    };
#endif

#ifdef DECODE_XA
    // http://problemkaputt.de/psx-spx.htm#cdromxaaudioadpcmcompression
    struct XA : Decoder
    {
        typedef bool (Callback)(void* userData);

        struct Group
        {
            uint8 params[16];
            uint8 data[112];
        };

        Frame     buffer[18 * 112];
        Frame     prevFrames[2];
        Frame     lerpFrames[32];

        int32     pos;
        int32     lerpPos;
        int32     frameIndex;
        void*     userData;
        Callback* nextSectorCallback;

        XA(void* userData, Callback* nextSectorCallback) : Decoder(NULL, 1, 11025), pos(COUNT(buffer)), lerpPos(0), frameIndex(7), userData(userData), nextSectorCallback(nextSectorCallback)
        {
            memset(prevFrames, 0, sizeof(prevFrames));
            memset(lerpFrames, 0, sizeof(lerpFrames));
        }

        void decode28(Group &group, int block, int channel)
        {
            int16 *dst   = channel ? &buffer[pos].R  : &buffer[pos].L;
            int16 &old   = channel ? prevFrames[0].R : prevFrames[0].L; 
            int16 &older = channel ? prevFrames[1].R : prevFrames[1].L; 

            int shift  = 12 - (group.params[4 + block * 2 + channel] & 0x0F);
            int filter =      (group.params[4 + block * 2 + channel] & 0x30) >> 4;

            int f0 = SPU_POS[filter];
            int f1 = SPU_NEG[filter];

            for (int i = 0; i < 28; i++)
            {
                int t = (group.data[block + i * 4] >> (channel * 4)) & 0x0F;
                if (t & 8) t -= 16;
                int s = (t << shift) + ((old * f0 + older * f1 + 32) / 64);
                s = clamp(s, -32768, 32767);
                older  = old;
                old    = s;
                dst[0] = s;
                dst += 2; // skip second channel
            }
        }

        void processSector(void* data)
        {
            Group* groups = (Group*)data;

            pos = 0;

            for (int i = 0; i < 18; i++)
            {
                for (int j = 0; j < 4; j++)
                {
                    decode28(groups[i], j, 0);
                    decode28(groups[i], j, 1);
                    pos += 28;
                }
            }

            pos = 0;
        }

        void ZigZagOut(Frame &frame, int32 p, const int16 *LUT)
        {
            FrameHI sum;
            sum.L = sum.R = 0;

            for (int32 i = 1; i < 30; i++)
            {
                Frame &f = lerpFrames[(p - i) & 31];
                sum.L += f.L * LUT[i];
                sum.R += f.R * LUT[i];
            }

            frame.L = clamp(sum.L >> 15, -32767, 32767);
            frame.R = clamp(sum.R >> 15, -32767, 32767);
        }

        virtual int decode(Frame *frames, int count) {
        #if defined(_OS_PSV) || defined(_OS_PSP) // TODO crash
            memset(frames, 0, count * sizeof(Frame));
            return count;
        #endif
            int i = 0;

            while (i < count) {
                if (frameIndex == 7) {
                    frameIndex = 0;

                    if (pos >= COUNT(buffer)) {
                        if (!nextSectorCallback || !nextSectorCallback(userData)) {
                            break;
                        }
                    }

                    ASSERT(pos <= (COUNT(buffer) - 6));

                    lerpFrames[lerpPos++ & 31] = buffer[pos++];
                    lerpFrames[lerpPos++ & 31] = buffer[pos++];
                    lerpFrames[lerpPos++ & 31] = buffer[pos++];
                    lerpFrames[lerpPos++ & 31] = buffer[pos++];
                    lerpFrames[lerpPos++ & 31] = buffer[pos++];
                    lerpFrames[lerpPos++ & 31] = buffer[pos++];
                } else {
                    ZigZagOut(frames[i++], lerpPos, SPU_ZIG_ZAG[frameIndex++]);
                }
            }

            return i;
        }

        virtual void replay()
        {
            pos = COUNT(buffer);
            lerpPos = 0;
            frameIndex = 7;
        }
    };
#endif

#ifdef DECODE_MP3
    struct MP3 : Decoder {
        mp3_decoder_t   mp3;
        char    *buffer;
        int     size, pos;        

        MP3(Stream *stream, int channels) : Decoder(stream, channels, 0), size(stream->size), pos(0) {
            mp3 = mp3_create();
            buffer = new char[size]; // TODO: file streaming
            stream->raw(buffer, size);
        }

        virtual ~MP3() {
            delete[] buffer;
            mp3_done(mp3);
        }

        virtual int decode(Frame *frames, int count) {
            mp3_info_t info;
            int i = 0;
            char *ptr = (char*)frames;
            while (ptr < (char*)&frames[count]) {
                int res = mp3_decode(mp3, buffer + pos, size - pos, (short*)ptr, &info);
                if (res) {
                    pos += res;
                    ptr += info.audio_bytes;
                    i   += info.audio_bytes;
                } else
                    break;
            }
            return i;
        }

        virtual void replay() {
            mp3_done(mp3);
            mp3 = mp3_create();
            pos = 0;
        }
    };
#endif

#ifdef DECODE_OGG

#ifdef USE_LIBVORBIS
    struct OGG : Decoder {
        OggVorbis_File   vf;
        FILE            *memFile;
        uint8           *data;

        OGG(Stream *stream, int channels) : Decoder(stream, channels, 0) {
            char buf[255];
            strcpy(buf, contentDir);
            strcat(buf, stream->name);

            data = new uint8[stream->size];
            stream->raw(data, stream->size);

            memFile = fmemopen(data, stream->size, "rb");
            int err = ov_open(memFile, &vf, NULL, 0);
            ASSERT(err >= 0);
            vorbis_info *info = ov_info(&vf, -1);
            this->channels = info->channels;
            this->freq     = info->rate;
        }

        virtual ~OGG() {
            ov_clear(&vf);
            fclose(memFile);
            delete[] data;
        }

        virtual int decode(Frame *frames, int count) {
            PROFILE_CPU_TIMING(stats.ogg);
            int i = 0;
            int bytes = count * sizeof(Frame);
            while (i < bytes) {
                int bitstream;
                int res = ov_read(&vf, (char*)frames + i, bytes - i, &bitstream);
                if (!res) break;
                i += res;
            }
            return i / sizeof(Frame);
        }

        virtual void replay() {
            fseek(memFile, 0, SEEK_SET);
            ov_open(memFile, &vf, NULL, 0);
        }
    };
#else // stb_vorbis
    struct OGG : Decoder {
        stb_vorbis       *ogg;
        stb_vorbis_alloc alloc;
        uint8            *data;

        OGG(Stream *stream, int channels) : Decoder(stream, channels, 0) {
            char buf[255];
            strcpy(buf, contentDir);
            strcat(buf, stream->name);

            data = new uint8[stream->size];
            stream->raw(data, stream->size);

            alloc.alloc_buffer_length_in_bytes = 256 * 1024;
            alloc.alloc_buffer = new char[alloc.alloc_buffer_length_in_bytes];
            ogg = stb_vorbis_open_memory(data, stream->size, NULL, &alloc);
            ASSERT(ogg);
            stb_vorbis_info info = stb_vorbis_get_info(ogg);
            this->channels = info.channels;
            this->freq     = info.sample_rate;
        }

        virtual ~OGG() {
            stb_vorbis_close(ogg);
            delete[] alloc.alloc_buffer;
            delete[] data;
        }

        virtual int decode(Frame *frames, int count) {
            PROFILE_CPU_TIMING(stats.ogg);
            if (freq == 22050 || freq == 11025) {
                // lower-rate music (half the decoding per second of sound): decoded into the back of the
                // buffer, then stretched to 44.1 kHz in place, front to back, as the PCM sounds are
                const int k = 44100 / freq;
                int n = count / k;
                if (n < 1) n = 1;
                Frame *src = frames + count - n;
                int got = 0;
                while (got < n) {
                    int res = stb_vorbis_get_samples_short_interleaved(ogg, 2, (short*)(src + got), (n - got) * 2);
                    if (!res) break;
                    got += res;
                }
                int out = 0;
                for (int j = 0; j < got && out + k <= count; j++) {
                    Frame f = src[j];
                    out += resample(frames + out, f);
                }
                return out;
            }
            int i = 0;
            while (i < count) {
                int res = stb_vorbis_get_samples_short_interleaved(ogg, channels, (short*)frames + i, (count - i) * 2);
                if (!res) break;
                i += res;
            }
            return i;
        }

        virtual void replay() {
            stb_vorbis_seek_start(ogg);
        }
    };
#endif

#endif // DECODE_OGG

    Core::Mutex lock;

    struct Listener
    {
        mat4 matrix;
        bool underwater;
    };

    Listener listener[2];
    int      listenersCount;

    Listener& getListener(const vec3 &pos)
    {
        if (listenersCount == 1 || (listener[0].matrix.getPos() - pos).length2() < (listener[1].matrix.getPos() - pos).length2())
            return listener[0];
        return listener[1];
    }

    enum Flags {
        LOOP            = 1,
        PAN             = 2,
        UNIQUE          = 4,
        REPLAY          = 8,
        MUSIC           = 16,
        FLIPPED         = 32,
        UNFLIPPED       = 64,
    };

    bool flipped;

    struct Sample
    {
        const vec3 *uniquePtr;
        Decoder *decoder;
        vec3    pos;
        float   volume;
        float   volumeTarget;
        float   volumeDelta;
        float   pitch;
        int     flags;
        int     id;
        bool    isPlaying;
        bool    isPaused;
        bool    stopAfterFade;
    #ifdef OL_ARNE
        int     hwCh;                    // Arne channel playing it, or -1 (software mixing)
        bool    hwTried;                 // a channel was looked for when it started
        const Arne::Buf *hwBuf;
        int     hwRate;                  // its sample rate (for the period)
    #endif

        Sample(Decoder *decoder, float volume, float pitch, int flags, int id) : uniquePtr(NULL), decoder(decoder), volume(volume), volumeTarget(volume), volumeDelta(0.0f), pitch(pitch), flags(flags), id(id)
        {
        #ifdef OL_ARNE
            hwCh = -1; hwTried = false; hwBuf = NULL; hwRate = 0;
        #endif
            isPlaying = decoder != NULL;
            isPaused  = false;
            stopAfterFade = true;
        }

        Sample(Stream *stream, const vec3 *pos, float volume, float pitch, int flags, int id) : uniquePtr(pos), decoder(NULL), volume(volume), volumeTarget(volume), volumeDelta(0.0f), pitch(pitch), flags(flags), id(id)
        {
        #ifdef OL_ARNE
            hwCh = -1; hwTried = false; hwBuf = NULL; hwRate = 0;
        #endif
            this->pos = pos ? *pos : vec3(0.0f);

        #ifndef NO_SOUND
            uint32 fourcc;
            stream->read(fourcc);
            if (fourcc == FOURCC("RIFF")) // wav
            {
                struct {
                    uint16  format;
                    uint16  channels;
                    uint32  samplesPerSec;
                    uint32  bytesPerSec;
                    uint16  block;
                    uint16  sampleBits;
                } waveFmt = {};

                stream->seek(8);
                while (stream->pos < stream->size) {
                    uint32 type, size;
                    stream->read(type);
                    stream->read(size);
                    if (type == FOURCC("fmt ")) {
                        stream->raw(&waveFmt, sizeof(waveFmt));
                        LE_FIX(waveFmt.format);   LE_FIX(waveFmt.channels);
                        LE_FIX(waveFmt.samplesPerSec); LE_FIX(waveFmt.bytesPerSec);
                        LE_FIX(waveFmt.block);    LE_FIX(waveFmt.sampleBits);
                        stream->seek(size - sizeof(waveFmt));
                    } else if (type == FOURCC("data")) {
                        if (waveFmt.format == 1) decoder = new PCM(stream, waveFmt.channels, waveFmt.samplesPerSec, size, waveFmt.sampleBits);
                    #ifdef DECODE_ADPCM
                        if (waveFmt.format == 2) decoder = new ADPCM(stream, waveFmt.channels, waveFmt.samplesPerSec, size, waveFmt.block);
                    #endif
                        break;
                    } else {
                        stream->seek(size);
                    }
                }
            } else if (fourcc == FOURCC("FORM")) { // aiff, 16-bit PCM: read into memory, played from there
                decoder = AIFF::open(stream);
            } else if (fourcc == FOURCC("OggS")) { // ogg
                stream->seek(-4);
                #ifdef DECODE_OGG
                    decoder = new OGG(stream, 2);
                #endif 
            } else if (fourcc == FOURCC("ID3\3")) { // mp3
                #ifdef DECODE_MP3
                    decoder = new MP3(stream, 2);
                #endif
            } else if (fourcc == FOURCC("SEGA")) { // Sega Saturn PCM mono signed 8-bit 11025 Hz
                decoder = new PCM(stream, 1, 11025, stream->size, -8);
            } else { // vag
                stream->setPos(0);
                #ifdef DECODE_VAG
                    decoder = new VAG(stream);
                #endif
            }
        #endif

            if (!decoder)
            {
                delete stream;
            }

            isPlaying = decoder != NULL;
            isPaused  = false;
        }

        ~Sample()
        {
        #ifdef OL_ARNE
            if (hwCh >= 0) Arne::stop(hwCh);
        #endif
            delete decoder;
        }

        void setVolume(float value, float time)
        {
            if (value < 0.0f) {
                stopAfterFade = true;
                value = 0.0f;
            } else {
                stopAfterFade = false;
            }

            volumeTarget = value;
            volumeDelta  = volumeTarget - volume;

            if (time > 0.0f)
            {
                volumeDelta /= 44100.0f * time;
            }
        }

        vec2 getPan()
        {
            if (!(flags & PAN)) return vec2(1.0f);

            mat4  m = Sound::getListener(pos).matrix;
            vec3  v = pos - m.offset().xyz();
            vec3  n = v.normal();

            float dist   = max(0.0f, 1.0f - (v.length() / SND_FADEOFF_DIST));
            float pan    = m.right().xyz().dot(n);
            float facing = (0.5f - m.dir().xyz().dot(n) * 0.5f) * SND_FACING_FACTOR + (1.0f - SND_FACING_FACTOR);

            vec2  value(min(1.0f, 1.0f - pan),
                        min(1.0f, 1.0f + pan));

            return (value * SND_PAN_FACTOR + (1.0f - SND_PAN_FACTOR)) * facing * dist;
        }

        bool render(Frame *frames, int count)
        {
            if (!isPlaying) return false;

            if (isPaused)
            {
                memset(frames, 0, sizeof(Frame) * count);
                return true;
            }

        // decode
            int i = 0;
            while (i < count)
            {
                int ret = decoder->decode(&frames[i], count - i);

                if (ret == 0)
                {
                    if (!(flags & LOOP))
                    {
                        isPlaying = false;
                        break;
                    }
                    decoder->replay();
                }

                i += ret;
            }

        // apply volume
        #ifdef _OS_AMIGA
            // The curve below leaves a 0.3 volume sound (Lara's footsteps, already a quiet sample) at 5%
            // amplitude: inaudible on the V4 next to the music and effects. Linear keeps it present.
            #define VOL_CONV(x) (x);
        #else
            #define VOL_CONV(x) (1.0f - sqrtf(1.0f - x * x));
        #endif

            float m = ((flags & MUSIC) ? Core::settings.audio.music : Core::settings.audio.sound) / float(SND_MAX_VOLUME);
            float v = volume * m;
            vec2 pan = getPan();
            vec2 vol = pan * VOL_CONV(v);
            if (volumeDelta == 0.0f) {
            #ifdef OL_AMMX
                // constant volume on the 68080: AMMX PMUL88, four samples per instruction (8.8 volume)
                {
                    int32 vl8 = int32(vol.x * 256.0f + 0.5f), vr8 = int32(vol.y * 256.0f + 0.5f);
                    vl8 = vl8 < 0 ? 0 : (vl8 > 32767 ? 32767 : vl8);
                    vr8 = vr8 < 0 ? 0 : (vr8 > 32767 ? 32767 : vr8);
                    magVolAMMX((short*)frames, i >> 1, (vl8 << 16) | vr8);
                    if (i & 1) {
                        frames[i - 1].L = int16((int32(frames[i - 1].L) * vl8) >> 8);
                        frames[i - 1].R = int16((int32(frames[i - 1].R) * vr8) >> 8);
                    }
                }
            #else
                // constant volume (nearly always): 16.16 fixed point, truncated towards zero as the float
                // version's int() is; the float loop below costs a float compare per sample and an
                // int-float-int round trip per channel (68080: about 25 cycles a frame)
                const int32 vl = int32(vol.x * 65536.0f), vr = int32(vol.y * 65536.0f);
                for (int j = 0; j < i; j++) {
                    int32 pl = int32(frames[j].L) * vl, pr = int32(frames[j].R) * vr;
                    frames[j].L = int16((pl + ((pl >> 31) & 0xFFFF)) >> 16);
                    frames[j].R = int16((pr + ((pr >> 31) & 0xFFFF)) >> 16);
                }
            #endif
            } else
            for (int j = 0; j < i; j++)
            {
                if (volumeDelta != 0.0f) // increase / decrease channel volume
                {
                    volume += volumeDelta;

                    if ((volumeDelta < 0.0f && volume < volumeTarget) ||
                        (volumeDelta > 0.0f && volume > volumeTarget))
                    {
                        volume = volumeTarget;
                        volumeDelta = 0.0f;
                        if (stopAfterFade)
                        {
                            isPlaying = false;
                        }
                    }

                    v   = volume * m;
                    vol = pan * VOL_CONV(v);
                }

                frames[j].L = int(frames[j].L * vol.x);
                frames[j].R = int(frames[j].R * vol.y);
            }
            #undef VOL_CONV

            return true;
        }

        void stop()
        {
            isPlaying = false;
        }

        void replay()
        {
        #ifdef OL_ARNE
            if (hwCh >= 0) { Arne::stop(hwCh); hwCh = -1; }   // restarted on another channel by the next pass
            hwTried = false;
        #endif
            decoder->replay();
        }

        void pause()
        {
            isPaused = true;
        }

        void resume()
        {
            isPaused = false;
        }
    } *channels[SND_CHANNELS_MAX];
    int channelsCount;

    typedef void (Callback)(Sample *channel);
    Callback *callback;

    FrameHI *result;
    Frame   *buffer;

    // TODO: per listener
    Filter::Reverberation reverb;
    Filter::LowPass       lowPass;

    void init()
    {
        flipped = false;
        channelsCount = 0;
        callback = NULL;
        buffer = NULL;
        result = NULL;
    #ifdef DECODE_MP3
        mp3_decode_init();
    #endif
    }

    void deinit()
    {
        for (int i = 0; i < channelsCount; i++)
        {
            delete channels[i];
        }
    #ifdef DECODE_MP3
        mp3_decode_free();
    #endif
        delete[] buffer;
        delete[] result;
    }

    void renderChannels(FrameHI *result, int count, bool music)
    {
        PROFILE_CPU_TIMING(stats.render[music]);

        int bufSize = count + count / 2 + 4;
        if (!buffer) {
            buffer = new Frame[bufSize]; // + 50% for pitch
        }

        for (int i = 0; i < channelsCount; i++)
        {
            if (music != ((channels[i]->flags & MUSIC) != 0)) {
                continue;
            }
        #ifdef OL_ARNE
            if (channels[i]->hwCh >= 0) continue;    // playing on an Arne channel
        #endif

            if (channels[i]->flags & (FLIPPED | UNFLIPPED)) {
                if (!(channels[i]->flags & (flipped ? FLIPPED : UNFLIPPED))) {
                    continue;
                }

                vec3 d = channels[i]->pos - getListener(channels[i]->pos).matrix.getPos();
                if (fabsf(d.x) > SND_FADEOFF_DIST || fabsf(d.y) > SND_FADEOFF_DIST || fabsf(d.z) > SND_FADEOFF_DIST) {
                    continue;
                }
            }

            if ((channels[i]->flags & LOOP) && channels[i]->volume < EPS && channels[i]->volumeTarget < EPS) {
                continue;
            }

            memset(buffer, 0, sizeof(Frame) * bufSize);
            channels[i]->render(buffer, (int(count * channels[i]->pitch) + 3) / 4 * 4);

            if (channels[i]->pitch == 1.0f) { // no pitch

                for (int j = 0; j < count; j++)
                {
                    result[j].L += buffer[j].L;
                    result[j].R += buffer[j].R;
                }

            } else { // has pitch (interpolate values for smooth wave)
                // position in 16.16 fixed point: no float-to-int conversions per output frame
                uint32 t = 0;
                const uint32 dt = uint32(channels[i]->pitch * 65536.0f + 0.5f);

                for (int j = 0; j < count; j++, t += dt)
                {
                    int idxA = int(t >> 16);
                    int idxB = (j == (count - 1)) ? idxA : (idxA + 1);
                    int st = int((t >> (16 - DSP_SCALE_BIT)) & (DSP_SCALE - 1));
                    Frame &a = buffer[idxA];
                    Frame &b = buffer[idxB];

                    result[j].L += a.L + ((b.L - a.L) * st >> DSP_SCALE_BIT);
                    result[j].R += a.R + ((b.R - a.R) * st >> DSP_SCALE_BIT);
                }

            }
        }
    }

#ifdef OL_ARNE
    // the left/right volume Arne should play a sample at: as Sample::render works it out (volume x
    // the sound setting x pan, linear on the V4), 0 while paused
    uint16 arneVolume(Sample *s)
    {
        if (s->isPaused) return 0;
        const int setting = (s->flags & MUSIC) ? Core::settings.audio.music : Core::settings.audio.sound;
        float v = s->volume * (setting / float(SND_MAX_VOLUME));
        vec2 pan = s->getPan();
        return Arne::volume(pan.x * v, pan.y * v);
    }

    // Once per mixer pass (23 ms): effects that started get a channel if one is free (else they are
    // mixed in software as before), the others get their volume, pitch and fades; a one-shot that
    // ended stops its sample. count = the pass's frames at 44.1 kHz (fades are set per frame).
    void arneUpdate(int count)
    {
        // a channel stopped since the last pass began (here or from the main thread, maybe a moment
        // ago) waits for the next pass, 23 ms on: restarted sooner, Arne resumes its old sound
        Arne::blocked = Arne::cooling;
        Arne::cooling = 0;
        if (!Arne::enabled) return;
        for (int i = 0; i < channelsCount; i++)
        {
            Sample *s = channels[i];
            if (s->hwCh < 0) {
                if (s->hwTried || !s->isPlaying || !s->decoder) continue;
                s->hwTried = true;
                // music already in Arne's format (AIFF in memory): straight from its buffer, stereo
                const int16 *direct; uint32 dFrames; int dCh, dRate;
                if (s->decoder->hwDirect(direct, dFrames, dCh, dRate)) {
                    int n = Arne::alloc();
                    if (n < 0) continue;
                    s->hwCh = n; s->hwBuf = NULL; s->hwRate = dRate;
                    Arne::start(n, direct, dFrames, (s->flags & LOOP) != 0, arneVolume(s), Arne::period(dRate, s->pitch > 0.0f ? s->pitch : 1.0f), dCh == 2);
                    continue;
                }
                if (s->flags & MUSIC) continue;  // other music (Ogg, movie soundtracks): software
                const uint8 *src; int bytes, bits, rate;
                if (!s->decoder->hwSource(src, bytes, bits, rate)) continue;
                const Arne::Buf *b = Arne::get(src, bytes, bits, rate);
                if (!b) continue;
                int n = Arne::alloc();
                if (n < 0) continue;
                s->hwCh = n; s->hwBuf = b; s->hwRate = b->freq;
                Arne::start(n, b->data, b->frames, (s->flags & LOOP) != 0, arneVolume(s), Arne::period(b->freq, s->pitch));
                continue;
            }
            const int n = s->hwCh;
            if (s->isPlaying && !(s->flags & LOOP) && !Arne::playing(n))
                s->isPlaying = false;            // the one-shot has ended
            if (s->isPlaying && s->volumeDelta != 0.0f) {
                s->volume += s->volumeDelta * float(count);
                if ((s->volumeDelta < 0.0f && s->volume < s->volumeTarget) || (s->volumeDelta > 0.0f && s->volume > s->volumeTarget)) {
                    s->volume = s->volumeTarget;
                    s->volumeDelta = 0.0f;
                    if (s->stopAfterFade) s->isPlaying = false;
                }
            }
            if (!s->isPlaying) { Arne::stop(n); s->hwCh = -1; continue; }
            Arne::setVolume(n, arneVolume(s));
            Arne::setPeriod(n, Arne::period(s->hwRate, s->pitch > 0.0f ? s->pitch : 1.0f));
        }
    }
#endif

    void convFrames(FrameHI *from, Frame *to, int count)
    {
        for (int i = 0; i < count; i++)
        {
            to[i].L = clamp(from[i].L, -32767, 32767);
            to[i].R = clamp(from[i].R, -32767, 32767);
        }
    }

    void fill(Frame *frames, int count)
    {
        OS_LOCK(lock);
        PROFILE_CPU_TIMING(stats.mixer);
        for (int i = 0; i < channelsCount; i++)  // tracks still being read in: the next piece
            if (channels[i]->decoder) channels[i]->decoder->pump();
    #ifdef OL_ARNE
        arneUpdate(count);
    #endif

        if (!channelsCount) {
            if (result && (Core::settings.audio.music != 0 || Core::settings.audio.sound != 0)) {
                memset(result, 0, sizeof(FrameHI) * count);

                if (Core::settings.audio.reverb)
                {
                    reverb.process(result, count);
                }

                convFrames(result, frames, count);
            } else {
                memset(frames, 0, sizeof(frames[0]) * count);
            }
            return;
        }

        if (!result)
        {
            result = new FrameHI[count];
        }

        memset(result, 0, sizeof(FrameHI) * count);

        if (Core::settings.audio.sound != 0)
        {
            renderChannels(result, count, false);

            if (Core::settings.audio.reverb)
            {
                if (listener[0].underwater)
                {
                    lowPass.process(result, count, SND_LOWPASS_FREQ);
                }
                reverb.process(result, count);
            }
        }

        if (Core::settings.audio.music != 0)
        {
            renderChannels(result, count, true);
        }

        convFrames(result, frames, count);

        for (int i = 0; i < channelsCount; i++)
        {
            if (!channels[i]->isPlaying)
            {
                if (callback)
                {
                    callback(channels[i]);
                }

                delete channels[i];
                channels[i] = channels[--channelsCount];
                i--;
            }
        }
    }

    Stream *openCDAudioWAD(const char *name, int index = -1)
    {
        if (!Stream::existsContent(name))
        {
            return NULL;
        }

        Stream *stream = new Stream(name);
        if (stream->size)
        {
            struct Item {
                char name[260];
                int  size;
                int  offset;
            } entity;

            stream->seek(sizeof(entity) * index);
            stream->raw(&entity, sizeof(entity));
            LE_FIX(entity.size);
            LE_FIX(entity.offset);
            stream->setPos(entity.offset);
            return stream;
        }
        delete stream;
        return NULL;
    }

    Stream *openCDAudioMP3(const char *dat, const char *name, int index = -1)
    {
        if (!Stream::existsContent(dat) || !Stream::existsContent(name))
        {
            return NULL;
        }
        Stream *stream = new Stream(name);
        return stream;
    }

    Sample* getChannel(int id, const vec3 *pos)
    {
        for (int i = 0; i < channelsCount; i++)
        {
            if (channels[i]->id == id && channels[i]->uniquePtr == pos)
            {
                return channels[i];
            }
        }
        return NULL;
    }

    Sample* play(Stream *stream, const vec3 *pos = NULL, float volume = 1.0f, float pitch = 0.0f, int flags = 0, int id = - 1)
    {
    #ifndef NO_SOUND
        OS_LOCK(lock);

        ASSERT(pitch >= 0.0f);
        if (!stream) return NULL;
        if (volume > 0.001f) {
            if (pos && !(flags & (FLIPPED | UNFLIPPED | MUSIC)) && (flags & PAN)) {
                vec3 listenerPos = getListener(*pos).matrix.getPos();
                vec3 d = *pos - listenerPos;

                if (fabsf(d.x) > SND_FADEOFF_DIST || fabsf(d.y) > SND_FADEOFF_DIST || fabsf(d.z) > SND_FADEOFF_DIST) {
                    delete stream;
                    return NULL;
                }
            }

            if (flags & (UNIQUE | REPLAY))
            {
                Sample *ch = getChannel(id, pos);

                if (ch)
                {
                    if (pos)
                    {
                        ch->pos = *pos;
                    }

                    ch->pitch = pitch;

                    if (flags & REPLAY)
                    {
                        ch->replay();
                    }

                    delete stream;
                    return ch;
                }
            }

            if (channelsCount < SND_CHANNELS_MAX)
            {
                return channels[channelsCount++] = new Sample(stream, pos, volume, pitch, flags, id);
            }

            LOG("! no free channels\n");
        }
    #endif
        delete stream;
        return NULL;
    }

    Sample* play(Decoder *decoder)
    {
        OS_LOCK(lock);

        if (channelsCount < SND_CHANNELS_MAX)
        {
            return channels[channelsCount++] = new Sample(decoder, 1.0f, 1.0f, MUSIC, -1);
        }
        return NULL;
    }

    void stop(int id = -1)
    {
        OS_LOCK(lock);

        for (int i = 0; i < channelsCount; i++)
        {
            if (id == -1 || channels[i]->id == id)
            {
                channels[i]->stop();
            }
        }
    }

    void stopAll()
    {
        OS_LOCK(lock);
        reverb.clear();

        for (int i = 0; i < channelsCount; i++)
        {
            delete channels[i];
        }
        channelsCount = 0;
    #ifdef OL_ARNE
        Arne::flush();                   // the level's converted sounds (every channel is stopped now)
    #endif
    }
}

#endif
