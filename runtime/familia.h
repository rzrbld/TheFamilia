#pragma once
/* Port-specific services shared by the shims. */
void fam_paths_init(const char *game_dir);   /* drive mapping, writable dirs */
void fam_register_shims(void);               /* d3d8, dinput, vorbisfile, ... */
const char *fam_user_dir(void);              /* writable: saves, registry, config */
