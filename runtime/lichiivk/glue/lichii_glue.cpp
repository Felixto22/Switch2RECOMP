/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the GNU Affero General Public License v3.0, see LICENSE.txt and NOTICE.txt.
   Forks and derived works have to keep the credit to SwitchRecomp by MZ Labs and the startup screen. */
/* renderer=eden with liblichiivk: the adapter between the SwitchRecomp runtime and Lichii's video_core (Eden's, with
   the Lichii emulator's Switch 2 / NVN2 GPU work; GPL-3.0-or-later).

   Unlike edenvk_glue.cpp (Eden fed method by method by our own front end), Lichii gets the pushbuffers themselves:
   its DMA pusher, puller, Ampere classes, Turing macros, QMD 3.0 and SM86 shaders handle Switch 2 command streams
   the way the Lichii emulator does, on its own GPU thread. What this file does is what Lichii's nvhost_gpu and vi2
   do in the emulator: one Lichii channel per GPU channel of ours (keyed by its syncpoint), the GPFIFO entries as a
   CommandList (their words copied when submitted, as Lichii does for NVN2), the submission's syncpoint increments
   appended, the GMMU maps and the presents. Syncpoints: Lichii's SyncpointManager increments call back into the
   runtime's (lichii_on_syncpt_incr), so the game's fences and NVN's syncpoint shim see the GPU's progress.

   The edenvk_* functions keep their names so the runtime's existing renderer=eden paths (maps, presents) reach
   Lichii; the method-level ones are unused here (gpu.c hands whole submissions to lichii_submit). */
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <chrono>
#include <vector>

#include "common/settings.h"
#include "core/core.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/framebuffer_layout.h"
#include "core/frontend/graphics_context.h"
#include "video_core/control/channel_state.h"
#include "video_core/dma_pusher.h"
#include "video_core/engines/puller.h"
#include "video_core/framebuffer_config.h"
#include "video_core/gpu.h"
#include "video_core/host1x/host1x.h"
#include "video_core/memory_manager.h"
#include "video_core/nx2.h"
#include "video_core/renderer_base.h"
#include "video_core/renderer_vulkan/renderer_vulkan.h"

extern "C" {
extern unsigned char* g_mem;
extern u64 g_title_id;
void logf_(int lvl, const char* fmt, ...);
u64 plat_ns(void);
extern float g_opt_resf;     /* resolution= in the .ini */
extern int g_opt_docked;     /* docked= in the .ini */
u32 syncpt_read(u32 id);
void syncpt_incr_from_gpu(u32 id);   /* nvdrv.c: the runtime's syncpoint, without echoing back here */
}
extern thread_local bool g_lichii_echo_off;   /* host1x/syncpoint_manager.cpp */

namespace Core {
namespace Memory {
u8* SwrGuestBase() { return g_mem; }
} // namespace Memory

static System* g_system;
System::System() { g_system = this; }
System::~System() = default;
System& System::GetInstance() { return *g_system; }

namespace Timing {
u64 CoreTiming::GetGPUTicks() const { return plat_ns() * 384 / 625; }   // 614.4 MHz
u64 CoreTiming::GetClockTicks() const { return (g_title_id >> 56) == 0x04 ? plat_ns() / 32 : plat_ns() * 24 / 1250; }   // 31.25 / 19.2 MHz
std::chrono::nanoseconds CoreTiming::GetGlobalTimeNs() const { return std::chrono::nanoseconds(plat_ns()); }
} // namespace Timing
} // namespace Core

/* CPU write tracking (edenvk_dirty.cpp) */
void edenvk_dirty_scan(const std::function<void(PAddr, size_t)>& cb);
void edenvk_dirty_self(DAddr a, size_t n);
void Core::System::GatherGPUDirtyMemory(std::function<void(PAddr, size_t)>& cb) { edenvk_dirty_scan(cb); }
void Core::SwrDeviceWritten(DAddr a, size_t n) { edenvk_dirty_self(a, n); }

namespace {

class SwrContext final : public Core::Frontend::GraphicsContext {};

class SwrWindow final : public Core::Frontend::EmuWindow {
public:
    SwrWindow(Core::Frontend::WindowSystemType type, void* display, void* surface, u32 w, u32 h) {
        window_info.type = type;
        window_info.display_connection = display;
        window_info.render_surface = surface;
        UpdateCurrentFramebufferLayout(w, h);
    }
    std::unique_ptr<Core::Frontend::GraphicsContext> CreateSharedContext() const override {
        return std::make_unique<SwrContext>();
    }
    bool IsShown() const override { return true; }
    void Resize(u32 w, u32 h) { UpdateCurrentFramebufferLayout(w, h); }
};

struct Glue {
    std::unique_ptr<Core::System> sys;
    std::unique_ptr<SwrWindow> window;
    Tegra::GPU* gpu = nullptr;   // owned by sys
    std::unique_ptr<Tegra::MemoryManager> gmmu;
    std::map<u32, std::shared_ptr<Tegra::Control::ChannelState>> chans;   // by our channel id (its syncpoint)
    std::mutex lk;
};
Glue* g;
u64 g_shim;
std::atomic<u64> st_submits, st_lists, st_composites, st_maps;
std::atomic<u64> busy_since;          // plat_ns() while a call into Lichii runs on the runtime's GPU thread, else 0
std::atomic<const char*> busy_what;
struct Busy {
    explicit Busy(const char* w) { busy_what = w; busy_since = plat_ns(); }
    ~Busy() { busy_since = 0; }
};   // Switch 2 read-only syncpoint shim (GET_SYNC_RO_MAP before the renderer is up)

Tegra::CommandHeader FenceAction(Tegra::Engines::Puller::FenceOperation op, u32 id) {
    Tegra::Engines::Puller::FenceAction a{};
    a.op.Assign(op);
    a.syncpoint_id.Assign(id);
    return {a.raw};
}

/* the submission's syncpoint increments, after its commands (Lichii's BuildIncrementCountCommandList: Switch 2
   pushbuffers end in semaphore releases; the driver still raises the syncpoint by the increment count) */
boost::container::small_vector<Tegra::CommandHeader, 512> IncrementList(u32 id, u32 count) {
    boost::container::small_vector<Tegra::CommandHeader, 512> r{
        Tegra::BuildCommandHeader(Tegra::BufferMethods::WaitForIdle, 1, Tegra::SubmissionMode::Increasing), {},
        Tegra::BuildCommandHeader(Tegra::BufferMethods::SyncpointPayload, 1, Tegra::SubmissionMode::Increasing), {}};
    for (u32 i = 0; i < count; ++i) {
        r.push_back(Tegra::BuildCommandHeader(Tegra::BufferMethods::SyncpointOperation, 1, Tegra::SubmissionMode::Increasing));
        r.push_back(FenceAction(Tegra::Engines::Puller::FenceOperation::Increment, id));
    }
    return r;
}

Tegra::Control::ChannelState& chan(u32 id) {
    auto it = g->chans.find(id);
    if (it == g->chans.end()) {
        auto cs = g->gpu->AllocateChannel();
        cs->memory_manager = g->gmmu.get();
        g->gpu->InitChannel(*cs, g_title_id);
        it = g->chans.emplace(id, std::move(cs)).first;
        logf_(1, "lichii: GPU channel %u created\n", id);
    }
    return *it->second;
}

} // namespace

extern "C" {

int g_edenvk_on;
int g_lichii_on;
extern const int g_lichii_lib = 1;   /* this runtime was linked with liblichiivk (edenvk_stub.c / edenvk_glue.cpp: 0) */

/* Lichii's SyncpointManager::IncrementGuest */
void lichii_on_syncpt_incr(u32 id) { syncpt_incr_from_gpu(id); }

/* a syncpoint the runtime incremented itself (not the GPU): Lichii's copy follows, for its waits and the shim */
void lichii_syncpt_external(u32 id) {
    if (!g) return;
    g_lichii_echo_off = true;
    g->gpu->Host1x().GetSyncpointManager().IncrementGuest(id);
    g_lichii_echo_off = false;
}

/* type: 1 X11 (display, window), 2 Win32 (hwnd). 1 = the renderer is up */
int edenvk_start(int type, void* display, void* surface, u32 w, u32 h) {
    try {
        g = new Glue;
        const bool nx2 = (g_title_id >> 56) == 0x04;
        Settings::values.renderer_backend.SetValue(Settings::RendererBackend::Vulkan);
        Settings::values.use_disk_shader_cache.SetValue(false);
        const bool async = !getenv("SWR_LICHII_SYNC");
        Settings::values.use_asynchronous_gpu_emulation.SetValue(async);
        Settings::values.use_docked_mode.SetValue(g_opt_docked ? Settings::ConsoleMode::Docked : Settings::ConsoleMode::Handheld);
        if (const char* e = getenv("SWR_EVK_ASTC"))
            Settings::values.accelerate_astc.SetValue(!strcmp(e, "cpu") ? Settings::AstcDecodeMode::Cpu : Settings::AstcDecodeMode::Gpu);
        {
            using RS = Settings::ResolutionSetup;
            const float r = g_opt_resf;
            Settings::values.resolution_setup.SetValue(r <= 0.3f ? RS::Res1_4X : r <= 0.6f ? RS::Res1_2X : r <= 0.8f ? RS::Res3_4X : r <= 1.1f ? RS::Res1X :
                                                       r <= 1.6f ? RS::Res3_2X : r <= 2.1f ? RS::Res2X : RS::Res3X);
        }
        Settings::UpdateRescalingInfo();
        Settings::UpdateGPUAccuracy();
        if (nx2) Tegra::g_nx2_ampere = true;
        g->sys = std::make_unique<Core::System>();
        g->sys->host1x = std::make_unique<Tegra::Host1x::Host1x>(*g->sys);
        const auto wt = type == 2 ? Core::Frontend::WindowSystemType::Windows : Core::Frontend::WindowSystemType::X11;
        g->window = std::make_unique<SwrWindow>(wt, display, surface, w, h);
        // Lichii's own GPU thread (asynchronous, as in the emulator): submissions return at once, the syncpoint
        // increments at their end tell the game when the work is done
        g->sys->gpu = std::make_unique<Tegra::GPU>(*g->sys, async, false);
        g->gpu = g->sys->gpu.get();
        auto ctx = g->window->CreateSharedContext();
        auto renderer = std::make_unique<Vulkan::RendererVulkan>(*g->window, g->sys->Host1x().MemoryManager(), *g->gpu, std::move(ctx));
        g->gpu->BindRenderer(std::move(renderer));
        g->gmmu = std::make_unique<Tegra::MemoryManager>(*g->sys, 40, 1ULL << 34, 16, 12);
        g->gpu->InitAddressSpace(*g->gmmu);
        if (g_shim) g->gmmu->SetNativeSyncpointShim(g_shim);
        // the first 64 presents log whether Lichii found the game's image in its texture cache (black screen: "accelerated=false")
        if (!getenv("EDEN_NATIVE_FRAME_CAPTURE") && !getenv("SWR_LICHII_QUIET")) {
#ifdef _WIN32
            _putenv("EDEN_NATIVE_FRAME_CAPTURE=1");
#else
            setenv("EDEN_NATIVE_FRAME_CAPTURE", "1", 1);
#endif
        }
        g->gpu->Start();
        if (!getenv("SWR_LICHII_QUIET")) {
            // every 5 s while the game runs: what Lichii's GPU got and where it is (a black screen or a stall shows up here)
            std::thread([] {
                u64 last_frames = ~0ull, last_sub = ~0ull; int same = 0;
                for (;;) {
                    std::this_thread::sleep_for(std::chrono::seconds(5));
                    if (!g) continue;
                    const u64 frames = static_cast<u64>(g->gpu->Renderer().GetCurrentFrame());
                    const u64 sub = st_submits.load();
                    const u64 bs = busy_since.load();
                    char chs[600] = ""; size_t o = 0;
                    {
                        std::scoped_lock l{g->lk};
                        auto& sm = g->gpu->Host1x().GetSyncpointManager();
                        for (auto& [id, cs] : g->chans) {
                            const bool acq = cs->payload && cs->payload->dma_pusher.HasPendingAcquire();
                            o += snprintf(chs + o, sizeof chs - o, " ch%u: sync %u/%u%s", id, sm.GetGuestSyncpointValue(id), syncpt_read(id), acq ? " WAITING(acquire)" : "");
                            if (o >= sizeof chs - 60) break;
                        }
                    }
                    const bool changed = frames != last_frames || sub != last_sub;
                    same = changed ? 0 : same + 1;
                    if (changed || same <= 2)
                        logf_(1, "lichii: status: %llu submits (%llu lists), %llu presents, %llu frames shown, suspended channels %s%s%s%s\n",
                              (unsigned long long)sub, (unsigned long long)st_lists.load(), (unsigned long long)st_composites.load(), (unsigned long long)frames,
                              g->gpu->HasSuspendedChannels() ? "yes" : "no", chs,
                              bs ? " | BLOCKED in " : "", bs ? busy_what.load() : "");
                    last_frames = frames; last_sub = sub;
                }
            }).detach();
        }
        g_edenvk_on = 1;
        g_lichii_on = 1;
        logf_(1, "lichii: Lichii's Switch 2 GPU started (%ux%u, %s%s)\n", w, h, async ? "own GPU thread" : "synchronous",
              nx2 ? ", Ampere/NVN2" : "");
        return 1;
    } catch (const std::exception& e) {
        logf_(0, "lichii: the renderer could not start: %s\n", e.what());
    } catch (...) {
        logf_(0, "lichii: the renderer could not start\n");
    }
    g = nullptr;   // a half-built renderer is left alone: OpenGL takes over
    return 0;
}

void edenvk_resize(u32 w, u32 h) { if (g) g->window->Resize(w, h); }

/* GMMU changes run on Lichii's GPU thread in submission order (MapInOrder / RunInOrder, as its nvhost_as_gpu does):
   mapping from the game's thread while the GPU thread translates addresses raced with it */
void edenvk_map(u64 gva, u64 cpu, u64 size) {
    if (!g) return;
    auto* mm = g->gmmu.get();
    g->gpu->MapInOrder(gva, size, [mm, gva, cpu, size] { mm->Map(gva, cpu, size, Tegra::PTEKind::PITCH, false); });
}

void edenvk_unmap(u64 gva, u64 size) {
    if (!g) return;
    auto* mm = g->gmmu.get();
    g->gpu->RunInOrder([mm, gva, size] { mm->Unmap(gva, size); });
}

/* GET_SYNC_RO_MAP (nvdrv.c): Lichii's shim layout, a u32 a syncpoint */
void lichii_set_shim(u64 gva) {
    g_shim = gva;
    if (g) g->gmmu->SetNativeSyncpointShim(gva);
}

/* one GPFIFO submission of our channel `id`: its entries, then `incs` increments of syncpoint `id` */
void lichii_submit(u32 id, const u64* ent, u32 n, u32 incs) {
    if (!g) return;
    std::scoped_lock l{g->lk};
    try {
        Busy b_("submit");
        st_submits++;
        auto& cs = chan(id);
        Tegra::CommandList list(n);
        size_t words = 0;
        for (u32 i = 0; i < n; ++i) {
            list.command_lists[i].raw = ent[i];
            words += list.command_lists[i].size;
        }
        // the words as they were when submitted (Lichii's EDEN_NX2_PB_SNAPSHOT): NVN refills command memory
        // it already submitted while the GPU thread is frames behind
        if (Tegra::g_nx2_ampere && !getenv("SWR_LICHII_NO_SNAPSHOT")) {
            list.snapshot_suspensions = cs.payload->dma_pusher.suspensions.load(std::memory_order_acquire);
            list.submitted_words.resize(words);
            list.submitted_offsets.reserve(n);
            size_t off = 0;
            for (u32 i = 0; i < n; ++i) {
                const auto& e = list.command_lists[i];
                list.submitted_offsets.push_back(static_cast<u32>(off));
                const u64 bytes = static_cast<u64>(e.size) * sizeof(u32);
                if (g->gpu->IsMapPending(e.addr, bytes)) list.deferred_entries.push_back(i);
                else g->gmmu->ReadBlockUnsafe(e.addr, list.submitted_words.data() + off, bytes);
                off += e.size;
            }
        }
        g->gpu->PushGPUEntries(cs.bind_id, std::move(list));
        st_lists += n;
        if (incs) g->gpu->PushGPUEntries(cs.bind_id, Tegra::CommandList{IncrementList(id, incs)});
    } catch (const std::exception& e) {
        static int n_; if (n_++ < 20) logf_(0, "lichii: submit on channel %u failed: %s\n", id, e.what());
    }
}

void edenvk_present(u64 addr, u32 w, u32 h, u32 pitch, u32 layout, u32 bhl, u32 fmt, const int crop[4]) {
    if (!g) return;
    try {
        (void)layout; (void)bhl;
        Tegra::FramebufferConfig f{};
        f.address = addr;
        f.offset = 0;
        f.width = w;
        f.height = h;
        f.stride = pitch >= w * 4 ? pitch / 4 : w;
        // Switch 2 presents carry NVN's format: 1 RGBA8, 4 RGB10A2 (vi2 preference word 6); 152 is NVN's RGBA8
        f.pixel_format = fmt == 4 ? Service::android::PixelFormat::Rgba1010102 : Service::android::PixelFormat::Rgba8888;
        f.transform_flags = Service::android::BufferTransformFlags::Unset;
        if (crop && crop[2] > crop[0] && crop[3] > crop[1]) f.crop_rect = Common::Rectangle<int>(crop[0], crop[1], crop[2], crop[3]);
        else if (!g_opt_docked && w >= 1920) f.crop_rect = Common::Rectangle<int>(0, 0, 1920, 1080);   // handheld: the top-left 1080p (Lichii's vi2)
        f.blending = Tegra::BlendMode::Opaque;
        f.native_tiling = (g_title_id >> 56) == 0x04;
        { static int n; if (n < 6) { n++; logf_(1, "lichii: present %llx %ux%u pitch %u format %u\n", (unsigned long long)addr, w, h, pitch, fmt); } }
        std::vector<Tegra::FramebufferConfig> layers{f};
        std::vector<Service::Nvidia::NvFence> fences;
        Busy b_("present");
        st_composites++;
        g->gpu->RequestComposite(std::move(layers), std::move(fences));
    } catch (const std::exception& e) {
        static int n_; if (n_++ < 20) logf_(0, "lichii: present failed: %s\n", e.what());
    }
}

/* the method-level interface of edenvk_glue.cpp: not used with Lichii (gpu.c hands it whole submissions) */
void edenvk_method(u32, u32, u32, u32, int) {}
void edenvk_set_ampere(u32) {}
void edenvk_stage_addr(u32, u32, u64) {}
void edenvk_draw(u32, int, u32, u32, u32, u32, u32, u32) {}
void edenvk_compute(u32, const u32*, u64) {}
void edenvk_submit_begin(void) {}
void edenvk_submit_end(void) {}
void edenvk_sync(void) {}

} // extern "C"
