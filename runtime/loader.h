#pragma once
#include <stdint.h>
#define FAM_MAX_MODULES 8
typedef struct {
    char file[64];
    uint32_t base, span, entry, import_rva, export_rva, export_size;
    int is_dll;
} fam_module;
int fam_map(const char *game_dir, const char *file, uint32_t base, int main_image);
int fam_bind(fam_module *m, int permissive);
int fam_attach_dlls(void);
uint32_t fam_export(const fam_module *m, const char *name, uint32_t ordinal);
fam_module *fam_find_module(const char *dll);
int fam_module_count(void);
fam_module *fam_module_at(int i);
