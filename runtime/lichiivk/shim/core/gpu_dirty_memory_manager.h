// SwitchRecomp shim for Core::GPUDirtyMemoryManager: CPU writes are found by the runtime's write tracking
// (glue/edenvk_dirty.cpp through System::GatherGPUDirtyMemory), so no manager reports pending writes here
#pragma once
#include "common/common_types.h"
namespace Core {
class GPUDirtyMemoryManager {
public:
    bool Overlaps(DAddr, u64) const { return false; }
    template <typename Func> void Gather(Func&&) {}
};
} // namespace Core
