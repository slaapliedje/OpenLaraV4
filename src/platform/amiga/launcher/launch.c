/* OpenLara launcher for Workbench icons: starts OpenLaraHW (next to it) at 640x360 when its own name
   contains "16" ("OpenLara 16-9"), else at 640x480 ("OpenLara 4-3"). The resolution goes to the game
   as a local variable, so ENV:OLRES is left as it is. */
#include <proto/exec.h>
#include <proto/dos.h>
#include <dos/dostags.h>
#include <dos/var.h>
#include <workbench/startup.h>
#include <string.h>

extern struct WBStartup *_WBenchMsg;

int main(int argc, char **argv)
{
    char name[108];
    name[0] = 0;
    if (argc == 0 && _WBenchMsg) strncpy(name, (char*)_WBenchMsg->sm_ArgList[0].wa_Name, sizeof(name) - 1);
    else if (argc > 0) strncpy(name, argv[0], sizeof(name) - 1);
    name[sizeof(name) - 1] = 0;
    const char *res = strstr(name, "16") ? "640x360" : "640x480";
    BPTR old = CurrentDir(GetProgramDir());
    SetVar((STRPTR)"OLRES", (STRPTR)res, -1, GVF_LOCAL_ONLY);
    BPTR in = Open((STRPTR)"NIL:", MODE_OLDFILE), out = Open((STRPTR)"NIL:", MODE_NEWFILE);
    LONG rc = SystemTags((STRPTR)"OpenLaraHW", SYS_Input, in, SYS_Output, out, NP_StackSize, 65536, TAG_DONE);
    if (in) Close(in);           /* a synchronous System() leaves them open */
    if (out) Close(out);
    CurrentDir(old);
    return rc < 0 ? 20 : 0;
}
