/* shims.c - registers this port's shim modules on top of win32hle's. */
#include "win32hle.h"
#include "familia.h"
void fam_register_misc(void);
void fam_register_pending(void);
void fam_register_d3d8(void);
void fam_register_dinput(void);
void fam_register_vorbis(void);
void fam_register_dsound8(void);
void fam_register_shims(void) {
    fam_register_misc();
    fam_register_d3d8();
    fam_register_dinput();
    fam_register_vorbis();
    fam_register_dsound8();
    fam_register_pending();
}
