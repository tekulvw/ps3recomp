/* NP DRM upgrade-license lookup, with no entitlement-success shortcut.
 *
 * Primary behavioral reference: RPCS3 sceNp.cpp npDrmVerifyUpgradeLicense and
 * Emu/system_utils.cpp get_c00_unlock_edat_path/verify_c00_unlock_edat:
 * https://github.com/RPCS3/rpcs3/blob/6ecaa5607ad51159291a82ba78d54fae2fcb8f35/rpcs3/Emu/Cell/Modules/sceNp.cpp
 * https://github.com/RPCS3/rpcs3/blob/6ecaa5607ad51159291a82ba78d54fae2fcb8f35/rpcs3/Emu/system_utils.cpp
 * Both firmware entry points take one content-ID string and search local user
 * exdata for its unlock EDAT. Real success additionally requires verified EDAT
 * headers, matching content IDs and the decrypted GOMA payload. This runtime
 * deliberately reports unsupported verification rather than accepting a file.
 */
#include "sceNpDrm.h"
#include "../filesystem/cellFs.h"
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <windows.h>
#define DRM_STAT _stat64
#define DRM_STAT_T struct __stat64
#define DRM_ISREG(m) (((m) & _S_IFMT) == _S_IFREG)
#else
#include <dirent.h>
#define DRM_STAT stat
#define DRM_STAT_T struct stat
#define DRM_ISREG(m) S_ISREG(m)
#endif
extern uint8_t* vm_base;
extern uint32_t ppu_vm_size; /* zero represents the full 32-bit address space */

static int read_content_id(const char* guest, char out[48])
{
    const uintptr_t raw = (uintptr_t)guest;
    const uint64_t limit = ppu_vm_size ? ppu_vm_size : UINT64_C(0x100000000);
    if (!vm_base || !raw || raw > UINT32_MAX) return -1;
    for (unsigned i = 0; i < 48; i++) {
        if ((uint64_t)raw + i >= limit) return -1;
        out[i] = (char)vm_base[raw + i];
        if (!out[i]) return 0;
    }
    return -1;
}

static int safe_content_id(const char* id)
{
    if (!*id) return 0;
    for (; *id; id++) {
        unsigned char c = (unsigned char)*id;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
    }
    return 1;
}

static int user_directory_name(const char* name)
{
    unsigned nonzero = 0;
    for (unsigned i = 0; i < 8; i++) {
        if (name[i] < '0' || name[i] > '9') return 0;
        nonzero |= (unsigned)(name[i] - '0');
    }
    return name[8] == 0 && nonzero;
}

/* 1: regular candidate exists, 0: absent/non-user, -1: I/O/mapping error.
 * Translate the complete virtual filename so explicit user/exdata mappings
 * retain the VFS's usual longest-prefix behavior. Never open or modify it. */
static int candidate_for_user(const char* name, const char* id)
{
    char guest[128], host[CELL_FS_MAX_FS_PATH_LENGTH];
    DRM_STAT_T st;
    if (!user_directory_name(name)) return 0;
    int n = snprintf(guest, sizeof guest, "/dev_hdd0/home/%s/exdata/%s.edat", name, id);
    if (n < 0 || (size_t)n >= sizeof guest ||
        cellfs_translate_path(guest, host, sizeof host) != 0) return -1;
    if (DRM_STAT(host, &st) == 0) return DRM_ISREG(st.st_mode) ? 1 : 0;
    return errno == ENOENT ? 0 : -1;
}

static s32 verify_upgrade_license(const char* content_id)
{
    char id[48], home[CELL_FS_MAX_FS_PATH_LENGTH];
    int found = 0, io_error = 0;
    if (read_content_id(content_id, id) != 0) return SCE_NP_DRM_ERROR_INVALID_PARAM;
    /* RPCS3's invalid content-ID lookup cannot produce a license path. */
    if (!safe_content_id(id)) return SCE_NP_DRM_ERROR_LICENSE_NOT_FOUND;
    if (cellfs_translate_path("/dev_hdd0/home", home, sizeof home) != 0)
        return SCE_NP_DRM_ERROR_IO;
#ifdef _WIN32
    char pattern[CELL_FS_MAX_FS_PATH_LENGTH];
    WIN32_FIND_DATAA ent;
    int n = snprintf(pattern, sizeof pattern, "%s/*", home);
    if (n < 0 || (size_t)n >= sizeof pattern) return SCE_NP_DRM_ERROR_IO;
    HANDLE dir = FindFirstFileA(pattern, &ent);
    if (dir == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        DWORD attrs = GetFileAttributesA(home);
        if (attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY))
            return SCE_NP_DRM_ERROR_IO;
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
            ? SCE_NP_DRM_ERROR_LICENSE_NOT_FOUND : SCE_NP_DRM_ERROR_IO;
    }
    do {
        if (!(ent.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        int result = candidate_for_user(ent.cFileName, id);
        if (result > 0) { found = 1; break; }
        if (result < 0) io_error = 1;
    } while (FindNextFileA(dir, &ent));
    if (!found && GetLastError() != ERROR_NO_MORE_FILES) io_error = 1;
    if (!FindClose(dir)) io_error = 1;
#else
    DIR* dir = opendir(home);
    if (!dir) return errno == ENOENT ? SCE_NP_DRM_ERROR_LICENSE_NOT_FOUND : SCE_NP_DRM_ERROR_IO;
    for (;;) {
        errno = 0;
        struct dirent* ent = readdir(dir);
        if (!ent) { if (errno) io_error = 1; break; }
        if (!user_directory_name(ent->d_name)) continue;
        char user[CELL_FS_MAX_FS_PATH_LENGTH];
        DRM_STAT_T st;
        int n = snprintf(user, sizeof user, "%s/%s", home, ent->d_name);
        if (n < 0 || (size_t)n >= sizeof user) { io_error = 1; continue; }
        if (DRM_STAT(user, &st) != 0) { io_error = 1; continue; }
        if (!S_ISDIR(st.st_mode)) continue;
        int result = candidate_for_user(ent->d_name, id);
        if (result > 0) { found = 1; break; }
        if (result < 0) io_error = 1;
    }
    if (closedir(dir) != 0) io_error = 1;
#endif
    if (found) {
        fprintf(stderr, "[sceNpDrm] upgrade license present; EDAT verification unsupported\n");
        return SCE_NP_DRM_ERROR_INTERNAL;
    }
    return io_error ? SCE_NP_DRM_ERROR_IO : SCE_NP_DRM_ERROR_LICENSE_NOT_FOUND;
}

s32 sceNpDrmVerifyUpgradeLicense(const char* content_id)
{
    return verify_upgrade_license(content_id);
}
s32 sceNpDrmVerifyUpgradeLicense2(const char* content_id)
{
    return verify_upgrade_license(content_id);
}
