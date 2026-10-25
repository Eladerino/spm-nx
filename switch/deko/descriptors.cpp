// This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of
// the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/.
// Adapted from SwitchWaker (https://github.com/centollOS/SwitchWaker) at 92307ff:
// switch/deko/descriptors.cpp, itself from SwitchWakerHD's runtime/src/gfx/deko/descriptors.cpp (the
// slot free lists and the sampler cache with its LRU). Frames became submission serials.
// Image and sampler descriptor slots (dk.h). The sets live in memory.cpp's descriptor block and are
// bound at the start of every command list. Every write is a command (dkCmdBufPushData), ordered
// with the draws, so the CPU never writes a descriptor the GPU may be reading. A released image slot
// comes from a view whose last use the GPU has finished (wgpu_objects.cpp keeps views alive until
// then), so it is free again right away. Under queue_mutex() except image_slot_release.
#include "dk.h"

#include <cstring>
#include <unordered_map>
#include <vector>

namespace swdk {
namespace {

std::vector<uint32_t> g_freeImages;
uint32_t g_nextImage = kReservedImages;
std::mutex g_releasedMutex;
std::vector<uint32_t> g_released;
bool g_samplersWritten = false;

struct SamplerSlot {
    uint64_t key = 0;
    uint64_t lastSerial = 0;
};
SamplerSlot g_samplers[kSamplerDescriptors];
std::unordered_map<uint64_t, uint32_t> g_samplerByKey;
uint32_t g_nextSampler = kReservedSamplers;

void descriptor_barrier() { dkCmdBufBarrier(R.cmd, DkBarrier_None, DkInvalidateFlags_Descriptors); }

DkImage g_empty;
ImageAlloc g_emptyMem;
bool g_emptyReady = false;

}  // namespace

void descriptors_bind() {
    dkCmdBufBindImageDescriptorSet(R.cmd, image_descriptors(), kImageDescriptors);
    dkCmdBufBindSamplerDescriptorSet(R.cmd, sampler_descriptors(), kSamplerDescriptors);
    if (!g_samplersWritten) {
        // the renderer's samplers: 0 linear, 1 nearest, both clamped to the edge
        DkSamplerDescriptor d[2];
        for (int i = 0; i < 2; i++) {
            DkSampler s;
            dkSamplerDefaults(&s);
            s.minFilter = s.magFilter = i == kSamplerNearestClamp ? DkFilter_Nearest : DkFilter_Linear;
            s.wrapMode[0] = s.wrapMode[1] = s.wrapMode[2] = DkWrapMode_ClampToEdge;
            dkSamplerDescriptorInitialize(&d[i], &s);
        }
        dkCmdBufPushData(R.cmd, sampler_descriptors(), d, sizeof d);
        descriptor_barrier();
        g_samplersWritten = true;
    }
}

void write_image_view_descriptor(uint32_t slot, const DkImageView& view) {
    DkImageDescriptor d;
    dkImageDescriptorInitialize(&d, &view, false, false);
    dkCmdBufPushData(R.cmd, image_descriptors() + slot * sizeof(DkImageDescriptor), &d, sizeof d);
    descriptor_barrier();
}

uint32_t image_slot_alloc() {
    if (!g_freeImages.empty()) {
        const uint32_t slot = g_freeImages.back();
        g_freeImages.pop_back();
        return slot;
    }
    if (g_nextImage < kImageDescriptors) return g_nextImage++;
    descriptors_reclaim();
    if (!g_freeImages.empty()) return image_slot_alloc();
    static int logged = 0;
    if (logged++ < 10) dklog("descriptors: all %u image slots in use", kImageDescriptors);
    return 0;
}

void image_slot_release(uint32_t slot) {
    if (slot < kReservedImages || slot >= kImageDescriptors) return;
    std::lock_guard<std::mutex> lock(g_releasedMutex);
    g_released.push_back(slot);
}

void descriptors_reclaim() {
    std::lock_guard<std::mutex> lock(g_releasedMutex);
    g_freeImages.insert(g_freeImages.end(), g_released.begin(), g_released.end());
    g_released.clear();
}

uint32_t sampler_slot(const DkSampler& sampler, uint64_t key) {
    const uint64_t serial = recording_serial();
    auto it = g_samplerByKey.find(key);
    if (it != g_samplerByKey.end()) {
        g_samplers[it->second].lastSerial = serial;
        return it->second;
    }
    uint32_t slot = 0;
    if (g_nextSampler < kSamplerDescriptors) {
        slot = g_nextSampler++;
    } else {
        // the least recently used slot whose submits the GPU has finished
        uint64_t oldest = UINT64_MAX;
        for (uint32_t i = kReservedSamplers; i < kSamplerDescriptors; i++) {
            if (g_samplers[i].lastSerial < oldest) {
                oldest = g_samplers[i].lastSerial;
                slot = i;
            }
        }
        if (slot == 0 || oldest >= serial || oldest > completed_serial()) {
            static int logged = 0;
            if (logged++ < 10)
                dklog("descriptors: all %u sampler slots in use by work in flight; nearest sampler used",
                      kSamplerDescriptors - kReservedSamplers);
            return kSamplerNearestClamp;
        }
        g_samplerByKey.erase(g_samplers[slot].key);
    }
    g_samplers[slot] = {key, serial};
    g_samplerByKey.emplace(key, slot);
    DkSamplerDescriptor d;
    dkSamplerDescriptorInitialize(&d, &sampler);
    dkCmdBufPushData(R.cmd, sampler_descriptors() + slot * sizeof(DkSamplerDescriptor), &d, sizeof d);
    descriptor_barrier();
    return slot;
}

void empty_image_init() {
    if (g_emptyReady) return;
    DkImageLayoutMaker m;
    dkImageLayoutMakerDefaults(&m, R.device);
    m.format = DkImageFormat_RGBA8_Unorm;
    m.dimensions[0] = 1;
    m.dimensions[1] = 1;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &m);
    const UploadAlloc s = upload_alloc(4, DK_IMAGE_LINEAR_STRIDE_ALIGNMENT);
    g_emptyMem = image_alloc(uint32_t(dkImageLayoutGetSize(&layout)), dkImageLayoutGetAlignment(&layout));
    dkImageInitialize(&g_empty, &layout, g_emptyMem.block, g_emptyMem.offset);
    const uint8_t black[4] = {0, 0, 0, 0xFF};
    memcpy(s.cpu, black, 4);
    DkImageView view;
    dkImageViewDefaults(&view, &g_empty);
    const DkCopyBuf src = {s.gpu, 0, 0};
    const DkImageRect r = {0, 0, 0, 1, 1, 1};
    dkCmdBufCopyBufferToImage(R.cmd, &src, &view, &r, 0);
    dkCmdBufBarrier(R.cmd, DkBarrier_Full, DkInvalidateFlags_Image);
    write_image_view_descriptor(kEmptyImage, view);
    g_emptyReady = true;
}

}  // namespace swdk
