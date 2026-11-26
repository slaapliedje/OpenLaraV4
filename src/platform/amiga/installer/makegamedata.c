/* MakeGameData: builds OpenLara's game data on the Amiga from your own Tomb Raider (PC) CD image - the
   .cue sheet and its image data (a .bin, or GOG's GAME.GOG / Steam's GAME.DAT; music tracks inside it or
   as separate WAV files). Run it in the OpenLaraMain drawer with the image beside it (or give the cue):
       MakeGameData [CUE <file.cue>] [TO <drawer>]
   It writes DATA/ and FMV/ from the CD's data track (ISO 9660) and the CD music as audio/1/track_NN.aiff
   (22.05 kHz 16-bit stereo, as OpenLara plays it from RAM). Same steps as tools/make-v4-package.py. */
#include <proto/exec.h>
#include <proto/dos.h>
#include <workbench/startup.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

extern struct WBStartup *_WBenchMsg;

static BPTR out;
static char line[640];
static void say(const char *s) { if (out) Write(out, (APTR)s, strlen(s)); }
#define SAY(...) do { snprintf(line, sizeof(line), __VA_ARGS__); say(line); } while (0)

#define BUFSIZE (512 * 1024)
static UBYTE *buf, *buf2;

/* ------------------------------------------------------------------ cue sheet */
#define MAXTRACKS 99
struct Track {
    int number;
    char mode[16];                 /* AUDIO, MODE1/2352, MODE1/2048, MODE2/2352 */
    char file[256];                /* full path */
    char ftype[12];                /* BINARY, MOTOROLA, WAVE, MP3, ... */
    LONG index0, index1;           /* frames (75 a second) into the file, -1 if absent */
};
static struct Track tracks[MAXTRACKS];
static int ntracks;

static LONG msf(const char *s)
{
    int m = 0, sec = 0, f = 0;
    sscanf(s, "%d:%d:%d", &m, &sec, &f);
    return (m * 60 + sec) * 75 + f;
}

static BOOL parseCue(const char *cue, const char *dir)
{
    BPTR fh = Open((STRPTR)cue, MODE_OLDFILE);
    if (!fh) { SAY("Can't open %s\n", cue); return FALSE; }
    char l[512], curFile[256] = "", curType[12] = "";
    while (FGets(fh, (STRPTR)l, sizeof(l))) {
        char *p = l;
        while (*p == ' ' || *p == '\t') p++;
        if (!strncasecmp(p, "FILE", 4)) {
            char *q = p + 4, name[200];
            while (*q == ' ') q++;
            if (*q == '"') { char *e = strchr(q + 1, '"'); if (!e) continue; memcpy(name, q + 1, e - q - 1); name[e - q - 1] = 0; q = e + 1; }
            else { int n = 0; while (*q && *q != ' ' && n < 199) name[n++] = *q++; name[n] = 0; }
            while (*q == ' ') q++;
            int n = 0; while (*q && *q != ' ' && *q != '\r' && *q != '\n' && n < 11) curType[n++] = *q++; curType[n] = 0;
            char *slash = strrchr(name, '\\'); if (slash) memmove(name, slash + 1, strlen(slash));   /* DOS paths */
            strcpy(curFile, dir); AddPart((STRPTR)curFile, (STRPTR)name, sizeof(curFile));
        } else if (!strncasecmp(p, "TRACK", 5) && ntracks < MAXTRACKS) {
            struct Track *t = &tracks[ntracks++];
            char mode[16] = "";
            sscanf(p + 5, "%d %15s", &t->number, mode);
            strcpy(t->mode, mode); strcpy(t->file, curFile); strcpy(t->ftype, curType);
            t->index0 = t->index1 = -1;
        } else if (!strncasecmp(p, "INDEX", 5) && ntracks) {
            int idx; char tm[16];
            if (sscanf(p + 5, "%d %15s", &idx, tm) == 2) {
                if (idx == 0) tracks[ntracks - 1].index0 = msf(tm);
                if (idx == 1) tracks[ntracks - 1].index1 = msf(tm);
            }
        }
    }
    Close(fh);
    return ntracks > 0;
}

/* ------------------------------------------------------------------ the data track's sectors */
static BPTR dataFh;
static LONG dataStart, secSize, secSkip;

static BOOL readSectors(ULONG lba, ULONG count, UBYTE *dst)
{
    if (secSize == 2048) {
        if (Seek(dataFh, (dataStart + lba) * 2048, OFFSET_BEGINNING) < 0) return FALSE;
        return Read(dataFh, dst, count * 2048) == (LONG)(count * 2048);
    }
    while (count) {                /* raw sectors: 2048 user bytes after a 16 / 24 byte header */
        ULONG n = count > BUFSIZE / 2352 ? BUFSIZE / 2352 : count;
        if (Seek(dataFh, (dataStart + lba) * 2352, OFFSET_BEGINNING) < 0) return FALSE;
        if (Read(dataFh, buf2, n * 2352) != (LONG)(n * 2352)) return FALSE;
        for (ULONG i = 0; i < n; i++) memcpy(dst + i * 2048, buf2 + i * 2352 + secSkip, 2048);
        dst += n * 2048; lba += n; count -= n;
    }
    return TRUE;
}

static ULONG le32(const UBYTE *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((ULONG)p[3] << 24); }

struct DirRec { ULONG lba, size; BOOL dir; char name[64]; };

static void rec(const UBYTE *r, struct DirRec *d)
{
    int n = r[32];
    d->lba = le32(r + 2); d->size = le32(r + 10); d->dir = (r[25] & 2) != 0;
    if (n > 63) n = 63;
    memcpy(d->name, r + 33, n); d->name[n] = 0;
    char *semi = strchr(d->name, ';'); if (semi) *semi = 0;
    int l = strlen(d->name); if (l && d->name[l - 1] == '.') d->name[l - 1] = 0;
}

/* calls fn for each entry of directory d (records never cross a sector's end) */
static int listDir(const struct DirRec *d, struct DirRec *list, int max)
{
    ULONG secs = (d->size + 2047) / 2048, n = 0;
    static UBYTE dirbuf[2048 * 32];
    if (secs > 32 || !readSectors(d->lba, secs, dirbuf)) return -1;
    for (ULONG p = 0; p < d->size && (int)n < max; ) {
        UBYTE len = dirbuf[p];
        if (!len) { p = (p / 2048 + 1) * 2048; continue; }
        struct DirRec e;
        rec(dirbuf + p, &e);
        if (e.name[0] > 1) list[n++] = e;
        p += len;
    }
    return n;
}

static BOOL extractDir(const struct DirRec *root, const char *name, const char *dest, BOOL required)
{
    static struct DirRec top[256], files[512];
    int n = listDir(root, top, 256);
    const struct DirRec *d = NULL;
    for (int i = 0; i < n; i++) if (top[i].dir && !strcasecmp(top[i].name, name)) d = &top[i];
    if (!d) {
        if (required) { SAY("No %s folder on the CD: is this the PC (DOS/Windows) Tomb Raider?\n", name); return FALSE; }
        SAY("No %s folder: the game runs without its movies.\n", name);
        return TRUE;
    }
    char path[512];
    strcpy(path, dest); AddPart((STRPTR)path, (STRPTR)name, sizeof(path));
    BPTR l = CreateDir((STRPTR)path); if (l) UnLock(l);
    int nf = listDir(d, files, 512), done = 0;
    for (int i = 0; i < nf; i++) {
        if (files[i].dir) continue;
        char fp[600];
        strcpy(fp, path); AddPart((STRPTR)fp, (STRPTR)files[i].name, sizeof(fp));
        BPTR fh = Open((STRPTR)fp, MODE_NEWFILE);
        if (!fh) { SAY("Can't write %s\n", fp); return FALSE; }
        ULONG left = files[i].size, lba = files[i].lba;
        while (left) {
            ULONG chunk = left > BUFSIZE ? BUFSIZE : left;
            if (!readSectors(lba, (chunk + 2047) / 2048, buf)) { Close(fh); SAY("Read error in the CD image (%s)\n", files[i].name); return FALSE; }
            if (Write(fh, buf, chunk) != (LONG)chunk) { Close(fh); SAY("Write error on %s (disk full?)\n", fp); return FALSE; }
            left -= chunk; lba += chunk / 2048;
        }
        Close(fh);
        done++;
    }
    SAY("%s: %d files\n", name, done);
    return TRUE;
}

/* ------------------------------------------------------------------ music: CD audio -> 22.05 kHz AIFF */
static BOOL writeAiffHeader(BPTR fh, ULONG frames)
{
    static const UBYTE rate22050[10] = { 0x40, 0x0D, 0xAC, 0x44, 0, 0, 0, 0, 0, 0 };
    UBYTE h[54];
    ULONG data = frames * 4;
    memcpy(h, "FORM", 4); *(ULONG*)(h + 4) = 46 + data; memcpy(h + 8, "AIFF", 4);
    memcpy(h + 12, "COMM", 4); *(ULONG*)(h + 16) = 18; *(UWORD*)(h + 20) = 2; *(ULONG*)(h + 22) = frames;
    *(UWORD*)(h + 26) = 16; memcpy(h + 28, rate22050, 10);
    memcpy(h + 38, "SSND", 4); *(ULONG*)(h + 42) = 8 + data; *(ULONG*)(h + 46) = 0; *(ULONG*)(h + 50) = 0;
    return Write(fh, h, 54) == 54;                     /* the 68k is big-endian, as AIFF is */
}

/* 44.1 kHz 16-bit stereo, little-endian (CD audio, WAV) or big-endian (MOTOROLA) -> 22.05 kHz big-endian:
   each output frame the mean of two input frames (a simple low-pass) */
static ULONG halve(const UBYTE *in, ULONG inFrames, WORD *outp, BOOL bigEndian)
{
    ULONG n = inFrames / 2;
    for (ULONG i = 0; i < n; i++, in += 8) {
        LONG l0, r0, l1, r1;
        if (bigEndian) { l0 = (WORD)((in[0] << 8) | in[1]); r0 = (WORD)((in[2] << 8) | in[3]); l1 = (WORD)((in[4] << 8) | in[5]); r1 = (WORD)((in[6] << 8) | in[7]); }
        else           { l0 = (WORD)(in[0] | (in[1] << 8)); r0 = (WORD)(in[2] | (in[3] << 8)); l1 = (WORD)(in[4] | (in[5] << 8)); r1 = (WORD)(in[6] | (in[7] << 8)); }
        *outp++ = (WORD)((l0 + l1) >> 1);
        *outp++ = (WORD)((r0 + r1) >> 1);
    }
    return n;
}

static BOOL convertRaw(BPTR in, LONG start, LONG bytes, BOOL bigEndian, const char *dest)
{
    if (Seek(in, start, OFFSET_BEGINNING) < 0) return FALSE;
    BPTR fh = Open((STRPTR)dest, MODE_NEWFILE);
    if (!fh) { SAY("Can't write %s\n", dest); return FALSE; }
    ULONG frames = (ULONG)bytes / 8;                   /* output frames */
    BOOL ok = writeAiffHeader(fh, frames);
    ULONG left = frames * 8;
    while (ok && left) {
        ULONG chunk = left > BUFSIZE ? BUFSIZE : left;
        if (Read(in, buf, chunk) != (LONG)chunk) { ok = FALSE; break; }
        ULONG n = halve(buf, chunk / 4, (WORD*)buf2, bigEndian);
        if (Write(fh, buf2, n * 4) != (LONG)(n * 4)) { SAY("Write error on %s (disk full?)\n", dest); ok = FALSE; }
        left -= chunk;
    }
    Close(fh);
    return ok;
}

static BOOL convertWav(const char *src, const char *dest)
{
    BPTR in = Open((STRPTR)src, MODE_OLDFILE);
    if (!in) { SAY("Can't open %s\n", src); return FALSE; }
    UBYTE h[12];
    if (Read(in, h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) { Close(in); SAY("%s is not a WAV file\n", src); return FALSE; }
    LONG pos = 12, dataPos = -1, dataLen = 0;
    BOOL fmtOk = FALSE;
    for (;;) {                                         /* chunks: fmt (44.1 kHz 16-bit stereo PCM) and data */
        UBYTE c[8];
        if (Read(in, c, 8) != 8) break;
        LONG len = le32(c + 4);
        if (!memcmp(c, "fmt ", 4)) {
            UBYTE f[16];
            if (Read(in, f, 16) != 16) break;
            fmtOk = f[0] == 1 && f[2] == 2 && le32(f + 4) == 44100 && f[14] == 16;
            Seek(in, len - 16 + (len & 1), OFFSET_CURRENT);
        } else if (!memcmp(c, "data", 4)) { dataPos = pos + 8; dataLen = len; break; }
        else Seek(in, len + (len & 1), OFFSET_CURRENT);
        pos += 8 + len + (len & 1);
    }
    BOOL ok = FALSE;
    if (!fmtOk || dataPos < 0) SAY("%s: only 44.1 kHz 16-bit stereo WAV can be converted here\n", src);
    else ok = convertRaw(in, dataPos, dataLen, FALSE, dest);
    Close(in);
    return ok;
}

/* ------------------------------------------------------------------ */
static BOOL findCue(const char *dir, char *cue, int size)
{
    BPTR lock = Lock((STRPTR)dir, ACCESS_READ);
    if (!lock) return FALSE;
    struct FileInfoBlock *fib = (struct FileInfoBlock*)AllocDosObject(DOS_FIB, NULL);
    BOOL found = FALSE;
    if (fib && Examine(lock, fib)) {
        while (ExNext(lock, fib)) {
            const char *n = (const char*)fib->fib_FileName;
            int l = strlen(n);
            if (fib->fib_DirEntryType < 0 && l > 4 && (!strcasecmp(n + l - 4, ".cue") || !strcasecmp(n + l - 5, ".inst"))) {
                strcpy(cue, dir); AddPart((STRPTR)cue, (STRPTR)n, size); found = TRUE; break;
            }
        }
    }
    if (fib) FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    return found;
}

int main(void)
{
    static char here[512], cue[512], dest[512], cuedir[512];
    const BOOL fromWB = _WBenchMsg != NULL;
    LONG args[2] = { 0, 0 };
    struct RDArgs *rda = NULL;
    int rc = 20;

    if (fromWB) out = Open((STRPTR)"CON:20/30/620/220/OpenLara: MakeGameData/AUTO/CLOSE/WAIT", MODE_NEWFILE);
    else {
        out = Output();
        if (!(rda = ReadArgs((STRPTR)"CUE,TO/K", args, NULL))) { say("Usage: MakeGameData [CUE <file.cue>] [TO <drawer>]\n"); return 20; }
    }
    buf = (UBYTE*)AllocVec(BUFSIZE, MEMF_ANY);
    buf2 = (UBYTE*)AllocVec(BUFSIZE, MEMF_ANY);
    if (!buf || !buf2) { say("Not enough memory.\n"); goto done; }

    if (!NameFromLock(GetProgramDir(), (STRPTR)here, sizeof(here))) here[0] = 0;
    strcpy(dest, args[1] ? (char*)args[1] : here);
    if (args[0]) strcpy(cue, (char*)args[0]);
    else if (!findCue(here, cue, sizeof(cue))) {
        say("No CD image here.\n\nCopy your Tomb Raider CD image into this drawer: the .cue file together with its\n"
            "image data (a .bin, or GOG's GAME.GOG / Steam's GAME.DAT, plus any music track files),\n"
            "then run MakeGameData again. See ReadMe.txt for where to buy the game.\n");
        goto done;
    }
    strcpy(cuedir, cue); *PathPart((STRPTR)cuedir) = 0;
    SAY("OpenLara MakeGameData\nCD image: %s\nWriting into: %s\n\n", cue, dest);
    if (!parseCue(cue, cuedir)) { say("No tracks in the cue sheet.\n"); goto done; }

    /* the data track: DATA and FMV */
    struct Track *data = NULL;
    for (int i = 0; i < ntracks; i++) if (strcasecmp(tracks[i].mode, "AUDIO")) { data = &tracks[i]; break; }
    if (!data) { say("No data track in the cue sheet.\n"); goto done; }
    if (!(dataFh = Open((STRPTR)data->file, MODE_OLDFILE))) { SAY("Can't open %s\n", data->file); goto done; }
    secSize = strstr(data->mode, "2352") ? 2352 : 2048;
    secSkip = !strncasecmp(data->mode, "MODE2", 5) ? 24 : 16;
    dataStart = data->index1 > 0 ? data->index1 : 0;
    {
        UBYTE pvd[2048];
        if (!readSectors(16, 1, pvd) || memcmp(pvd + 1, "CD001", 5)) { say("The data track is not an ISO 9660 filesystem.\n"); goto done; }
        struct DirRec root;
        rec(pvd + 156, &root);
        if (!extractDir(&root, "DATA", dest, TRUE) || !extractDir(&root, "FMV", dest, FALSE)) goto done;
    }
    Close(dataFh); dataFh = 0;

    /* the music */
    char adir[512];
    strcpy(adir, dest); AddPart((STRPTR)adir, (STRPTR)"audio", sizeof(adir));
    BPTR l = CreateDir((STRPTR)adir); if (l) UnLock(l);
    AddPart((STRPTR)adir, (STRPTR)"1", sizeof(adir));
    l = CreateDir((STRPTR)adir); if (l) UnLock(l);
    int audio = 0, ok = 0, skipped = 0;
    for (int i = 0; i < ntracks; i++) if (!strcasecmp(tracks[i].mode, "AUDIO")) audio++;
    SAY("Music: %d tracks\n", audio);
    for (int i = 0; i < ntracks; i++) {
        struct Track *t = &tracks[i];
        if (strcasecmp(t->mode, "AUDIO")) continue;
        char name[32], dst[600];
        sprintf(name, "track_%02d.aiff", t->number);
        strcpy(dst, adir); AddPart((STRPTR)dst, (STRPTR)name, sizeof(dst));
        BOOL good = FALSE;
        if (!strcasecmp(t->ftype, "BINARY") || !strcasecmp(t->ftype, "MOTOROLA")) {
            BPTR in = Open((STRPTR)t->file, MODE_OLDFILE);
            if (!in) { SAY("Can't open %s\n", t->file); continue; }
            LONG start = (t->index1 >= 0 ? t->index1 : (t->index0 >= 0 ? t->index0 : 0)) * 2352, end;
            if (i + 1 < ntracks && !strcmp(tracks[i + 1].file, t->file))
                end = (tracks[i + 1].index0 >= 0 ? tracks[i + 1].index0 : tracks[i + 1].index1) * 2352;
            else { Seek(in, 0, OFFSET_END); end = Seek(in, 0, OFFSET_CURRENT); }
            good = end > start && convertRaw(in, start, end - start, !strcasecmp(t->ftype, "MOTOROLA"), dst);
            Close(in);
        } else if (!strcasecmp(t->ftype, "WAVE")) {
            const char *f = t->file; int fl = strlen(f);
            if (fl > 4 && !strcasecmp(f + fl - 4, ".wav")) good = convertWav(f, dst);
            else { SAY("Track %02d is %s: convert it on a PC (tools/make-v4-package.py) or to WAV first\n", t->number, FilePart((STRPTR)f)); skipped++; continue; }
        } else { SAY("Track %02d: %s files can't be converted on the Amiga (use the PC tool)\n", t->number, t->ftype); skipped++; continue; }
        if (good) { ok++; SAY("  track %02d  (%d/%d)\n", t->number, ok, audio); }
        else SAY("  track %02d failed\n", t->number);
        if (CheckSignal(SIGBREAKF_CTRL_C)) { say("*** Break\n"); goto done; }
    }
    SAY("\nDone: %d of %d music tracks%s.\nYou can delete the CD image now. Start the game with \"OpenLara 4-3\" or \"OpenLara 16-9\".\n",
        ok, audio, skipped ? " (the others need the PC tool)" : "");
    rc = 0;
done:
    if (dataFh) Close(dataFh);
    if (buf) FreeVec(buf);
    if (buf2) FreeVec(buf2);
    if (rda) FreeArgs(rda);
    if (fromWB && out) Close(out);
    return rc;
}
