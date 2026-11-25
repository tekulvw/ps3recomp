/*
 * ps3recomp - the optional file overlay over the VFS root (PS3_VFS_OVERLAY,
 * docs/VFS_OVERLAY.md), through the real translators: libs/filesystem/cellFs.c,
 * runtime/syscalls/sys_fs.c and the PPU cellFs HLE in ppu_fs.cpp, on a
 * synthetic tree tools/test_vfs_overlay.py builds (no game data).
 *
 *   test_vfs_overlay MODE ROOT OVERLAY
 *
 * MODE on       PS3_VFS_ROOT=ROOT, PS3_VFS_OVERLAY=OVERLAY: only the file the
 *               overlay holds changes, for every operation.
 * MODE off      no PS3_VFS_OVERLAY: everything comes from ROOT.
 * MODE inert    PS3_VFS_OVERLAY set, but without PS3_VFS_ROOT or with no such
 *               directory: everything comes from ROOT (the driver checks the
 *               warning on stderr).
 */
#include "ppu_fs.cpp"
#include "../../../libs/filesystem/edat.c"
#include "../../../runtime/memory/vm_watch.c"
#include <set>
#include <string>

extern "C" int cellfs_translate_path(const char* ps3_path, char* host_buf, size_t buf_size);
extern "C" void sys_fs_translate_path(const char* ps3_path, char* host_path, int host_path_size);

/* ---- link stubs: nothing on the paths this test drives calls them ---- */
extern "C" {
uint8_t* vm_base = nullptr;
uint32_t ppu_vm_size = 0;
void vm_write32(uint64_t a, uint32_t v) { uint32_t be = __builtin_bswap32(v); memcpy(vm_base + a, &be, 4); }
void vm_write64(uint64_t a, uint64_t v) { uint64_t be = __builtin_bswap64(v); memcpy(vm_base + a, &be, 8); }
void ppu_guest_caller(char* out, size_t n) { if (n) out[0] = 0; }
void ydkj_host_bt(const char* tag) { (void)tag; }
void ps3_hle_register_ctx(uint32_t, const char*, void (*)(ppu_context*)) {}
const char* cellGame_get_hdd_title_id(void) { return nullptr; }
}
ps3_guest_caller_fn g_ps3_guest_caller = nullptr;

static uint32_t rd32(uint32_t a) { uint32_t v; memcpy(&v, vm_base + a, 4); return __builtin_bswap32(v); }
static uint64_t rd64(uint32_t a) { uint64_t v; memcpy(&v, vm_base + a, 8); return __builtin_bswap64(v); }

static int fails = 0;
#define CHECK(c, ...) do { printf((c) ? "[PASS] " : "[FAIL] "); printf(__VA_ARGS__); printf("\n"); \
                           if (!(c)) fails++; } while (0)

static const char* const kSettings = "/dev_bdvd/PS3_GAME/USRDIR/run/settings.bin";
static const char* const kOther = "/dev_bdvd/PS3_GAME/USRDIR/run/other.bin";
static const std::string kOriginal = "original-settings";
static const std::string kReplaced = "OVERLAY-settings-and-more";

enum : uint32_t { PATH = 0x1000, FDP = 0x1800, NREAD = 0x1808, STAT = 0x1900, BUF = 0x2000,
                  ENTRIES = 0x10000, COUNT = 0x1810 };

static void put_path(const char* guest) { strcpy(reinterpret_cast<char*>(vm_base + PATH), guest); }

static std::string file_bytes(const std::string& host)
{
    FILE* f = fopen(host.c_str(), "rb");
    if (!f) return "<missing>";
    std::string s; char b[256]; size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
    fclose(f);
    return s;
}

static int ppu_open(const char* guest, uint32_t flags)
{
    put_path(guest);
    ppu_context c = {}; c.gpr[3] = PATH; c.gpr[4] = flags; c.gpr[5] = FDP;
    cellFsOpen(&c);
    return c.gpr[3] == 0 ? int(rd32(FDP)) : -1;
}

static void ppu_close(int fd) { ppu_context c = {}; c.gpr[3] = uint32_t(fd); cellFsClose(&c); }

static std::string ppu_read_all(const char* guest)
{
    int fd = ppu_open(guest, CELL_FS_O_RDONLY);
    if (fd < 0) return "<open failed>";
    ppu_context c = {}; c.gpr[3] = uint32_t(fd); c.gpr[4] = BUF; c.gpr[5] = 4096; c.gpr[6] = NREAD;
    cellFsRead(&c);
    std::string s(reinterpret_cast<char*>(vm_base + BUF), size_t(rd64(NREAD)));
    ppu_close(fd);
    return s;
}

static int64_t ppu_stat_size(const char* guest)
{
    put_path(guest);
    ppu_context c = {}; c.gpr[3] = PATH; c.gpr[4] = STAT;
    cellFsStat(&c);
    return c.gpr[3] == 0 ? int64_t(rd64(STAT + 0x24)) : -1;
}

static int64_t ppu_fstat_size(const char* guest)
{
    int fd = ppu_open(guest, CELL_FS_O_RDONLY);
    if (fd < 0) return -1;
    ppu_context c = {}; c.gpr[3] = uint32_t(fd); c.gpr[4] = STAT;
    cellFsFstat(&c);
    int64_t size = c.gpr[3] == 0 ? int64_t(rd64(STAT + 0x24)) : -1;
    ppu_close(fd);
    return size;
}

/* name -> size for every entry of a guest directory, via cellFsGetDirectoryEntries. */
static std::set<std::string> ppu_listing(const char* guest, int64_t* settings_size)
{
    std::set<std::string> names;
    put_path(guest);
    ppu_context c = {}; c.gpr[3] = PATH; c.gpr[4] = FDP;
    cellFsOpendir(&c);
    if (c.gpr[3] != 0) return {"<opendir failed>"};
    const int fd = int(rd32(FDP));
    for (;;) {
        ppu_context e = {}; e.gpr[3] = uint32_t(fd); e.gpr[4] = ENTRIES; e.gpr[5] = 312 * 8; e.gpr[6] = COUNT;
        cellFsGetDirectoryEntries(&e);
        const uint32_t n = rd32(COUNT);
        if (e.gpr[3] != 0 || !n) break;
        for (uint32_t i = 0; i < n; i++) {
            const uint32_t ea = ENTRIES + i * 312;
            std::string name(reinterpret_cast<char*>(vm_base + ea + 54));
            if (name == "." || name == "..") continue;
            if (name == "settings.bin" && settings_size) *settings_size = int64_t(rd64(ea + 0x24));
            names.insert(name);
        }
    }
    ppu_context d = {}; d.gpr[3] = uint32_t(fd); cellFsClosedir(&d);
    return names;
}

static std::string cellfs_path(const char* guest)
{
    char out[1024];
    return cellfs_translate_path(guest, out, sizeof out) == 0 ? out : "<failed>";
}

static std::string sysfs_path(const char* guest)
{
    char out[1024];
    sys_fs_translate_path(guest, out, sizeof out);
    return out;
}

static std::string fallback(const std::string& host)
{
    char path[1024];
    snprintf(path, sizeof path, "%s", host.c_str());
    ps3_vfs_ps3game_fallback(path, sizeof path);
    return path;
}

int main(int argc, char** argv)
{
    if (argc != 4) { fprintf(stderr, "usage: test_vfs_overlay MODE ROOT OVERLAY\n"); return 2; }
    const std::string mode = argv[1], root = argv[2], overlay = argv[3];
    static uint8_t memory[0x20000];
    vm_base = memory;
    const bool on = mode == "on";
    const std::string want = on ? kReplaced : kOriginal;
    const std::string label = on ? "the overlay's" : "the root's";
    const std::string root_settings = root + "/PS3_GAME/USRDIR/run/settings.bin";
    const std::string overlay_settings = overlay + "/PS3_GAME/USRDIR/run/settings.bin";

    /* The boot harness points the PPU layer at the game tree (from PS3_VFS_ROOT,
     * or from the EBOOT's location without it). */
    ppu_vfs_root = argv[2];
    const bool has_root = getenv("PS3_VFS_ROOT") && *getenv("PS3_VFS_ROOT");

    /* The three translators (cellFs and sys_fs find the tree through PS3_VFS_ROOT). */
    if (has_root) {
        CHECK(file_bytes(cellfs_path(kSettings)) == want, "cellfs_translate_path: settings.bin is %s file", label.c_str());
        CHECK(file_bytes(sysfs_path(kSettings)) == want, "sys_fs_translate_path: settings.bin is %s file", label.c_str());
        CHECK(file_bytes(sysfs_path("/dev_bdvd/USRDIR/run/settings.bin")) == want,
              "sys_fs flattened USRDIR spelling: settings.bin is %s file", label.c_str());
        CHECK(file_bytes(cellfs_path(kOther)) == "other-data" && file_bytes(sysfs_path(kOther)) == "other-data",
              "other.bin comes from the root through cellFs and sys_fs");
        CHECK(cellfs_path("/dev_bdvd/PS3_GAME/USRDIR/run").find(overlay) == std::string::npos &&
              sysfs_path("/dev_bdvd/PS3_GAME/USRDIR/run/sub").find(overlay) == std::string::npos,
              "directories resolve to the root even where the overlay has the directory");
        CHECK(sysfs_path("/dev_hdd0/game/X/USRDIR/run/settings.bin").find(overlay) == std::string::npos,
              "/dev_hdd0 (PS3_HDD0_ROOT) never uses the overlay");
    }
    CHECK(ppu_read_all(kSettings) == want, "PPU cellFsOpen/cellFsRead: settings.bin is %s file", label.c_str());
    CHECK(ppu_stat_size(kSettings) == int64_t(want.size()), "PPU cellFsStat: settings.bin has %s size", label.c_str());
    CHECK(ppu_fstat_size(kSettings) == int64_t(want.size()), "PPU cellFsFstat: settings.bin has %s size", label.c_str());

    /* Everything else is the root's. */
    CHECK(ppu_read_all(kOther) == "other-data", "PPU: other.bin comes from the root");
    CHECK(ppu_read_all("/dev_bdvd/PS3_GAME/USRDIR/run/settings.bin.bak") == "backup",
          "a name that only starts like the overlaid file is not redirected");

    int64_t listed = -1;
    const auto run = ppu_listing("/dev_bdvd/PS3_GAME/USRDIR/run", &listed);
    CHECK(run == std::set<std::string>({"settings.bin", "settings.bin.bak", "other.bin", "sub"}),
          "the run listing is the root's entries (%zu)", run.size());
    CHECK(listed == int64_t(want.size()), "the listing's settings.bin size is %s (%lld)", label.c_str(), (long long)listed);
    const auto sub = ppu_listing("/dev_bdvd/PS3_GAME/USRDIR/run/sub", nullptr);
    CHECK(sub == std::set<std::string>({"inner.bin"}), "a directory listing never shows overlay-only entries");

    /* Direct calls on the shared tail. */
    CHECK(fallback(root + "//PS3_GAME/./USRDIR//run/settings.bin") == (on ? overlay_settings : root + "//PS3_GAME/./USRDIR//run/settings.bin"),
          "repeated separators and '.' components still match");
    CHECK(fallback(root + "/PS3_GAME/USRDIR/run/../run/settings.bin") == root + "/PS3_GAME/USRDIR/run/../run/settings.bin",
          "a path with '..' is never redirected");
    CHECK(fallback(root + "-sibling/PS3_GAME/USRDIR/run/settings.bin") == root + "-sibling/PS3_GAME/USRDIR/run/settings.bin",
          "a sibling directory sharing the root's prefix is not under the root");
    CHECK(fallback(root) == root, "the root itself is unchanged");
    {
        /* The driver makes the overlay's path longer than the root's. */
        char exact[1024];
        snprintf(exact, sizeof exact, "%s", root_settings.c_str());
        ps3_vfs_ps3game_fallback(exact, root_settings.size() + 1);
        CHECK(overlay_settings.size() > root_settings.size() && std::string(exact) == root_settings,
              "an overlay path longer than the caller's buffer keeps the root's path");
    }

    /* Writes reach the overlay's copy; new files land in the root. */
    if (on) {
        int fd = ppu_open(kSettings, CELL_FS_O_RDWR);
        strcpy(reinterpret_cast<char*>(vm_base + BUF), "W");
        ppu_context w = {}; w.gpr[3] = uint32_t(fd); w.gpr[4] = BUF; w.gpr[5] = 1; w.gpr[6] = NREAD;
        if (fd >= 0) { cellFsWrite(&w); ppu_close(fd); }
        CHECK(fd >= 0 && file_bytes(overlay_settings) == "W" + kReplaced.substr(1),
              "a write to settings.bin changes the overlay's copy");
        CHECK(file_bytes(root_settings) == kOriginal, "the root's settings.bin is never changed");
        fd = ppu_open("/dev_bdvd/PS3_GAME/USRDIR/run/created.bin", CELL_FS_O_WRONLY | CELL_FS_O_CREAT);
        if (fd >= 0) ppu_close(fd);
        CHECK(fd >= 0 && file_bytes(root + "/PS3_GAME/USRDIR/run/created.bin") == "" &&
              file_bytes(overlay + "/PS3_GAME/USRDIR/run/created.bin") == "<missing>",
              "a new file is created in the root, as without an overlay");
    }

    printf("%s: %d failure(s)\n", mode.c_str(), fails);
    return fails ? 1 : 0;
}
