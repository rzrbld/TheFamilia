/*
 * shims_pending.c - TEMPORARY bring-up answers for interfaces whose real
 * implementations are not written yet. Each fails the way the API documents
 * failure, so the engine's own error path runs and is visible in the log.
 * Remove an entry here when its real module lands.
 */
#include <stdio.h>
#include "win32hle.h"
#include "familia.h"

#define E_FAIL 0x80004005u

static const win32hle_shim g_pending[] = {
    { 0, 0 }
};
void fam_register_pending(void) { win32hle_register(g_pending); }
