/* ahimodes: list the audio modes ahi.device knows (opens AHI_NO_UNIT, no hardware). */
#include <proto/exec.h>
#include <exec/io.h>
#include <utility/tagitem.h>
#include <stdio.h>

struct OLAHIRequest {
    struct IOStdReq ahir_Std;
    UWORD ahir_Version, ahir_Pad1;
    ULONG ahir_Private[2], ahir_Type, ahir_Frequency;
    LONG  ahir_Volume, ahir_Position;
    struct OLAHIRequest *ahir_Link;
};
#define TB      TAG_USER
#define TBR     (TAG_USER | 0x8000)
#define AHIDB_Driver    (TBR + 101)
#define AHIDB_Name      (TBR + 109)
#define AHIDB_Bits      (TB + 110)
#define AHIDB_BufferLen (TB + 122)

static ULONG nextID(struct Library *base, ULONG last)
{
    register ULONG d0 __asm("d0") = last;
    register struct Library *a6 __asm("a6") = base;
    __asm volatile ("jsr -102(a6)" : "+r"(d0) : "r"(a6) : "d1", "a0", "a1", "memory");
    return d0;
}

static ULONG getAttrs(struct Library *base, ULONG id, struct TagItem *tags)
{
    register ULONG d0 __asm("d0") = id;
    register APTR a2 __asm("a2") = 0;
    register struct TagItem *a1 __asm("a1") = tags;
    register struct Library *a6 __asm("a6") = base;
    __asm volatile ("jsr -108(a6)" : "+r"(d0), "+r"(a1) : "r"(a2), "r"(a6) : "d1", "a0", "memory");
    return d0;
}

int main(void)
{
    struct MsgPort *port = CreateMsgPort();
    struct OLAHIRequest *req = (struct OLAHIRequest*)CreateIORequest(port, sizeof(struct OLAHIRequest));
    req->ahir_Version = 4;
    BYTE err = OpenDevice((STRPTR)"ahi.device", 255, (struct IORequest*)req, 0);
    printf("OpenDevice ahi.device AHI_NO_UNIT: %d\n", err);
    if (err) return 20;
    struct Library *base = (struct Library*)req->ahir_Std.io_Device;
    printf("ahi.device %d.%d\n", base->lib_Version, base->lib_Revision);
    ULONG id = ~0UL;
    int n = 0;
    while ((id = nextID(base, id)) != ~0UL && n < 100) {
        char name[64] = "?", driver[64] = "?";
        ULONG bits = 0;
        struct TagItem tags[] = {
            { AHIDB_BufferLen, 64 }, { AHIDB_Name, (ULONG)name }, { AHIDB_Driver, (ULONG)driver },
            { AHIDB_Bits, (ULONG)&bits }, { TAG_DONE, 0 }
        };
        getAttrs(base, id, tags);
        printf("  %08lx  %-40s driver %-10s %lu bit\n", id, name, driver, bits);
        n++;
    }
    printf("%d modes\n", n);
    CloseDevice((struct IORequest*)req);
    DeleteIORequest((struct IORequest*)req);
    DeleteMsgPort(port);
    return 0;
}
