#pragma once

#include "ImguiShaderCompiler.h"
#include "imgui/imgui.h"
#include "nvn/nvn_Cpp.h"
#include "nvn/nvn_CppMethods.h"
#include "MemoryBuffer.h"

namespace ImguiNvnBackend {

    struct NvnBackendInitInfo {
        nvn::Device *device;
        nvn::Queue *queue;
    };

    struct NvnBackendData {
        nvn::Device *device;
        nvn::Queue *queue;

        static constexpr int FramesInFlight = 3;
        int frame_index = 0;

        // Dedicated independent command buffers for ImGui (Prevents game buffer overflow)
        nvn::CommandBuffer cmdBuf[FramesInFlight];
        nvn::MemoryPool cmdMemPool[FramesInFlight];
        void* controlMemory[FramesInFlight];

        nvn::BufferBuilder bufferBuilder;
        nvn::MemoryPoolBuilder memPoolBuilder;

        nvn::Program shaderProgram;
        MemoryBuffer *shaderMemory;

        MemoryBuffer *uniformMemory[FramesInFlight];
        nvn::ShaderData shaderDatas[2];

        nvn::VertexStreamState streamState;
        nvn::VertexAttribState attribStates[3];

        nvn::Texture fontTexture;
        nvn::Sampler fontSampler;
        nvn::MemoryPool fontTexMemPool;
        nvn::MemoryPool sampTexMemPool;
        nvn::TexturePool texPool;
        nvn::SamplerPool samplerPool;
        nvn::TextureHandle fontTexHandle;

        MemoryBuffer *vtxBuffer[FramesInFlight];
        MemoryBuffer *idxBuffer[FramesInFlight];
        ImVec2 viewportSize;

        int64_t lastTick;
        bool isInitialized;
        bool isDisableInput = true;

        CompiledData imguiShaderBinary;
    };

    bool createShaders();
    bool setupShaders(uint8_t *shaderBinary, unsigned long binarySize);
    bool createFontTexture();

    void InitBackend(const NvnBackendInitInfo &initInfo);
    void ShutdownBackend();
    void newFrame();
    void setRenderStates(nvn::CommandBuffer* cmdBuf);
    void renderDrawData(ImDrawData *drawData);

    void SetRenderTarget(const nvn::Texture *colorTarget);
    NvnBackendData *getBackendData();

    /* RGBA8 texture owned by the backend (pool slots 2..15, linear/clamp).
     * Returns 0 when no slot is free. Destroy only once the GPU can no longer
     * be reading it (defer a few frames after the last draw). */
    ImTextureID CreateRgbaTexture(int w, int h, const void *rgba, int *outSlot);
    void DestroyRgbaTexture(int slot);

    /* Async GPU->CPU copy of whole mip levels 0..count-1 (tightly packed,
     * back to back; for block-compressed formats sizes[i] is the raw block
     * data). Submits on the backend queue with a fence and never waits.
     * One readback at a time; call both from the present thread. */
    bool BeginTextureReadback(const nvn::Texture *src, int count, const int *w, const int *h,
                              const size_t *sizes);
    /* 1 = done (data copied to dst), 0 = still on the GPU, -1 = failed */
    int PollTextureReadback(void *dst, size_t size);
}
