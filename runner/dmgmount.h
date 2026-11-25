/*
 * dmgmount: read-only userspace "mount" of an Apple disk image.
 *
 * The host kernel cannot attach APFS/LZFSE images, so the launcher resolves a
 * .dmg the same way the platform's dmg viewer does: extract the image (and any
 * nested .apfs container) with 7-Zip into a content-addressed cache directory,
 * then treat that directory as the mounted volume.
 */
#ifndef PLSR_DMGMount_H
#define PLSR_DMGMount_H

#include <stdbool.h>

/* True when path names an existing regular file ending in ".dmg". */
bool plsr_is_dmg(const char *path);

/*
 * Mount image `dmg` and return the malloc'd path of the .app bundle inside it.
 * On success *mount_dir (when non-NULL) receives the malloc'd mount path.
 * On failure returns NULL and *err (when non-NULL) receives a malloc'd reason.
 */
char *plsr_mount_dmg(const char *dmg, char **mount_dir, char **err);

#endif
