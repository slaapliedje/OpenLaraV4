/* OpenLara V4 self-extractor: an Amiga program with an LHA archive (level 2 headers, -lh0- stored
   files and -lhd- directories) appended to it, written by tools/make-v4-package.py. Run from the
   Shell (OpenLaraV4.run [DESTINATION]) or from Workbench (a requester asks where to unpack). The
   archive is located through a 16-byte trailer at the end of the file:
       "OLSFX001", archive offset (BE32), archive length (BE32)
   Each file's CRC-16 is checked as it is written. */
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/asl.h>
#include <libraries/asl.h>
#include <workbench/startup.h>
#include <string.h>
#include <stdio.h>

extern struct WBStartup *_WBenchMsg;
struct Library *AslBase = NULL;


static BPTR out = 0;
static BPTR in = 0;
static UBYTE *buf = NULL;
#define BUFSIZE (256 * 1024)
static UWORD crcTab[256];

static void say(const char *s) { if (out) Write(out, (APTR)s, strlen(s)); }

static void crcInit(void)
{
    for (int i = 0; i < 256; i++) {
        UWORD c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0xA001 : c >> 1;
        crcTab[i] = c;
    }
}

static UWORD crcAdd(UWORD crc, const UBYTE *p, LONG n)
{
    while (n--) crc = crcTab[(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return crc;
}

static ULONG le16(const UBYTE *p) { return p[0] | (p[1] << 8); }
static ULONG le32(const UBYTE *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((ULONG)p[3] << 24); }
static ULONG be32(const UBYTE *p) { return ((ULONG)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

static BOOL readAll(BPTR fh, APTR p, LONG n) { return Read(fh, p, n) == n; }

/* every directory along path (a full AmigaDOS path ending in '/'-separated parts) */
static BOOL makeDirs(const char *path)
{
    char tmp[512];
    int len = strlen(path);
    if (len >= (int)sizeof(tmp)) return FALSE;
    for (int i = 0; i <= len; i++) {
        if (path[i] == '/' || path[i] == 0) {
            if (i == 0 || path[i - 1] == ':') continue;
            memcpy(tmp, path, i); tmp[i] = 0;
            BPTR l = Lock((STRPTR)tmp, ACCESS_READ);
            if (l) { UnLock(l); continue; }
            l = CreateDir((STRPTR)tmp);
            if (!l) return FALSE;
            UnLock(l);
        }
    }
    return TRUE;
}

static void setDate(const char *path, ULONG unixTime)
{
    struct DateStamp ds;
    LONG t = (LONG)unixTime - 252460800;      /* seconds since 1 Jan 1978 (the packager stores local time) */
    if (t < 0) t = 0;
    ds.ds_Days = t / 86400; t %= 86400;
    ds.ds_Minute = t / 60;
    ds.ds_Tick = (t % 60) * TICKS_PER_SECOND;
    SetFileDate((STRPTR)path, &ds);
}

/* the full path of this program's file */
static BOOL selfPath(char *path, int size)
{
    char name[256];
    BPTR dir;
    if (_WBenchMsg) {
        dir = _WBenchMsg->sm_ArgList[0].wa_Lock;
        strncpy(name, (char*)_WBenchMsg->sm_ArgList[0].wa_Name, sizeof(name) - 1); name[sizeof(name) - 1] = 0;
    } else {
        dir = GetProgramDir();
        if (!GetProgramName((STRPTR)name, sizeof(name))) return FALSE;
    }
    if (!dir || !NameFromLock(dir, (STRPTR)path, size)) return FALSE;
    return AddPart((STRPTR)path, FilePart((STRPTR)name), size);
}

static BOOL askDrawer(char *dest, int size, const char *initial)
{
    if (!(AslBase = OpenLibrary((STRPTR)"asl.library", 37))) return FALSE;
    struct FileRequester *fr = (struct FileRequester*)AllocAslRequestTags(ASL_FileRequest,
        ASLFR_TitleText, (ULONG)"Unpack OpenLara into which drawer?", ASLFR_DrawersOnly, TRUE,
        ASLFR_InitialDrawer, (ULONG)initial, ASLFR_PositiveText, (ULONG)"Unpack", TAG_DONE);
    BOOL ok = FALSE;
    if (fr) {
        if (AslRequest(fr, NULL)) { strncpy(dest, (char*)fr->fr_Drawer, size - 1); dest[size - 1] = 0; ok = TRUE; }
        FreeAslRequest(fr);
    }
    CloseLibrary(AslBase); AslBase = NULL;
    return ok;
}

int main(void)
{
    /* the command line through dos.library (ReadArgs), Workbench through its startup message: the
       overlay header links ahead of libnix's startup code, whose argument parsing then is not run */
    const BOOL fromWB = _WBenchMsg != NULL;
    LONG argArray[1] = { 0 };
    struct RDArgs *rda = NULL;
    static char self[512], dest[512], path[1024], line[1200];
    static UBYTE hdr[4096];
    int rc = 20, files = 0, dirs = 0;
    ULONG total = 0;

    if (fromWB) out = Open((STRPTR)"CON:20/30/600/200/OpenLara V4 installer/AUTO/CLOSE/WAIT", MODE_NEWFILE);
    else {
        out = Output();
        if (!(rda = ReadArgs((STRPTR)"DESTINATION", argArray, NULL))) { say("Usage: OpenLaraV4.run [DESTINATION drawer]\n"); goto done; }
    }
    crcInit();
    if (!(buf = (UBYTE*)AllocVec(BUFSIZE, MEMF_ANY))) { say("Not enough memory.\n"); goto done; }
    if (!selfPath(self, sizeof(self))) { say("Can't find my own file.\n"); goto done; }

    /* where to: the argument, the requester, or next to this file */
    strcpy(dest, self); *PathPart((STRPTR)dest) = 0;
    if (argArray[0]) { strncpy(dest, (char*)argArray[0], sizeof(dest) - 1); dest[sizeof(dest) - 1] = 0; }
    else if (fromWB) { char ini[512]; strcpy(ini, dest); if (!askDrawer(dest, sizeof(dest), ini)) { say("Cancelled.\n"); rc = 5; goto done; } }

    if (!(in = Open((STRPTR)self, MODE_OLDFILE))) { say("Can't open my own file.\n"); goto done; }
    UBYTE tr[16];
    if (Seek(in, -16, OFFSET_END) < 0 || !readAll(in, tr, 16) || memcmp(tr, "OLSFX001", 8)) { say("No archive found in this file.\n"); goto done; }
    ULONG arcOff = be32(tr + 8), arcLen = be32(tr + 12);
    if (Seek(in, arcOff, OFFSET_BEGINNING) < 0) { say("Bad archive offset.\n"); goto done; }
    sprintf(line, "OpenLara V4: unpacking %lu MB into \"%s\"\n\n", (unsigned long)(arcLen >> 20), dest); say(line);

    for (;;) {
        if (!readAll(in, hdr, 2)) { say("Archive ends early.\n"); goto done; }
        ULONG hsize = le16(hdr);
        if ((hsize & 0xFF) == 0 && hdr[0] == 0) break;          /* end of archive */
        if (hsize < 26 || hsize > sizeof(hdr) || !readAll(in, hdr + 2, hsize - 2) || hdr[20] != 2) { say("Unsupported archive header.\n"); goto done; }
        BOOL isDir = !memcmp(hdr + 2, "-lhd-", 5);
        if (!isDir && memcmp(hdr + 2, "-lh0-", 5)) { say("Compressed entries are not supported.\n"); goto done; }
        ULONG size = le32(hdr + 7), mtime = le32(hdr + 15);
        UWORD crc = le16(hdr + 21);
        char fname[256] = "", dname[512] = "";
        ULONG pos = 24, ext = le16(hdr + 24);
        pos += 2;
        while (ext >= 3 && pos + ext <= hsize) {
            UBYTE type = hdr[pos];
            ULONG n = ext - 3;
            if (type == 1 && n < sizeof(fname)) { memcpy(fname, hdr + pos + 1, n); fname[n] = 0; }
            if (type == 2 && n < sizeof(dname)) {
                for (ULONG i = 0; i < n; i++) dname[i] = hdr[pos + 1 + i] == 0xFF ? '/' : hdr[pos + 1 + i];
                dname[n] = 0;
            }
            pos += ext - 2;
            ext = le16(hdr + pos);
            pos += 2;
        }
        if (strstr(dname, "..") || strstr(fname, "..") || strchr(fname, ':') || strchr(dname, ':')) { say("Bad name in archive.\n"); goto done; }
        strcpy(path, dest);
        if (dname[0]) AddPart((STRPTR)path, (STRPTR)dname, sizeof(path));
        if (isDir) {
            int l = strlen(path);
            if (l && path[l - 1] == '/') path[l - 1] = 0;
            strcat(path, "/");
            if (!makeDirs(path)) { sprintf(line, "Can't create drawer %s\n", path); say(line); goto done; }
            dirs++;
            continue;
        }
        { int l = strlen(path); if (l && path[l - 1] != ':' && path[l - 1] != '/') strcat(path, "/"); }
        if (!makeDirs(path)) { sprintf(line, "Can't create drawer %s\n", path); say(line); goto done; }
        AddPart((STRPTR)path, (STRPTR)fname, sizeof(path));
        {
            static char shown[800];
            strcpy(shown, dname); strcat(shown, fname);
            sprintf(line, "  %-50s %9lu\n", shown, (unsigned long)size); say(line);
        }
        BPTR fh = Open((STRPTR)path, MODE_NEWFILE);
        if (!fh) { sprintf(line, "Can't write %s\n", path); say(line); goto done; }
        UWORD c = 0;
        ULONG left = size;
        while (left) {
            LONG n = left > BUFSIZE ? BUFSIZE : left;
            if (!readAll(in, buf, n)) { Close(fh); say("Archive ends early.\n"); goto done; }
            c = crcAdd(c, buf, n);
            if (Write(fh, buf, n) != n) { Close(fh); sprintf(line, "Write error on %s (disk full?)\n", path); say(line); goto done; }
            left -= n;
        }
        Close(fh);
        if (c != crc) { sprintf(line, "CRC error in %s: the archive is damaged.\n", path); say(line); goto done; }
        setDate(path, mtime);
        files++; total += size;
        if (CheckSignal(SIGBREAKF_CTRL_C)) { say("*** Break\n"); goto done; }
    }
    sprintf(line, "\nDone: %d files, %lu MB. Open the OpenLaraMain drawer and double-click\n\"OpenLara 4-3\" or \"OpenLara 16-9\".\n", files, (unsigned long)(total >> 20)); say(line);
    rc = 0;
done:
    if (in) Close(in);
    if (buf) FreeVec(buf);
    if (rda) FreeArgs(rda);
    if (fromWB && out) Close(out);
    return rc;
}
