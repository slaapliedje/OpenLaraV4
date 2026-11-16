/* ahitone: 3 s 440 Hz tone through ahi.device unit 0, 16-bit stereo 44.1 kHz, double-buffered
   exactly like OpenLara's sound task. Prints each step.  ahitone [unit] [frequency] */
#include <proto/exec.h>
#include <proto/dos.h>
#include <exec/io.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

struct OLAHIRequest {
    struct IOStdReq ahir_Std;
    UWORD ahir_Version, ahir_Pad1;
    ULONG ahir_Private[2], ahir_Type, ahir_Frequency;
    LONG  ahir_Volume, ahir_Position;
    struct OLAHIRequest *ahir_Link;
};
#define N 1024

int main(int argc, char **argv)
{
    int unit = argc > 1 ? atoi(argv[1]) : 0;
    int freq = argc > 2 ? atoi(argv[2]) : 44100;
    struct MsgPort *port = CreateMsgPort();
    struct OLAHIRequest *req[2];
    WORD *buf[2];
    req[0] = (struct OLAHIRequest*)CreateIORequest(port, sizeof(struct OLAHIRequest));
    req[1] = (struct OLAHIRequest*)AllocVec(sizeof(struct OLAHIRequest), MEMF_PUBLIC);
    buf[0] = (WORD*)AllocVec(N * 4, MEMF_PUBLIC);
    buf[1] = (WORD*)AllocVec(N * 4, MEMF_PUBLIC);
    req[0]->ahir_Version = 4;
    BYTE err = OpenDevice((STRPTR)"ahi.device", unit, (struct IORequest*)req[0], 0);
    printf("OpenDevice ahi.device unit %d: %d\n", unit, err);
    if (err) return 20;
    CopyMem(req[0], req[1], sizeof(struct OLAHIRequest));
    struct OLAHIRequest *link = NULL;
    double ph = 0;
    int cur = 0, blocks = freq * 3 / N;
    for (int b = 0; b < blocks; b++) {
        for (int i = 0; i < N; i++) {
            WORD v = (WORD)(sin(ph) * 12000); ph += 2 * 3.14159265 * 440 / freq;
            buf[cur][i * 2] = v; buf[cur][i * 2 + 1] = v;
        }
        struct OLAHIRequest *r = req[cur];
        r->ahir_Std.io_Message.mn_Node.ln_Pri = 0;
        r->ahir_Std.io_Command = CMD_WRITE;
        r->ahir_Std.io_Data = buf[cur];
        r->ahir_Std.io_Length = N * 4;
        r->ahir_Std.io_Offset = 0;
        r->ahir_Type = 3;        /* AHIST_S16S */
        r->ahir_Frequency = freq;
        r->ahir_Volume = 0x10000;
        r->ahir_Position = 0x8000;
        r->ahir_Link = link;
        SendIO((struct IORequest*)r);
        if (link) { WaitIO((struct IORequest*)link); if (b < 4 || link->ahir_Std.io_Error) printf("block %d done, io_Error %d\n", b, link->ahir_Std.io_Error); }
        link = r; cur ^= 1;
    }
    if (link) WaitIO((struct IORequest*)link);
    printf("finished\n");
    CloseDevice((struct IORequest*)req[0]);
    DeleteIORequest((struct IORequest*)req[0]); FreeVec(req[1]); FreeVec(buf[0]); FreeVec(buf[1]); DeleteMsgPort(port);
    return 0;
}
