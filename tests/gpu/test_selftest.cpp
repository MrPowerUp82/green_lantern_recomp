#include "gpu/selftest.h"
#include "gpu/selftest_scene.h"
#include "vd_rig.h"

TEST(selftest_drives_swaps_through_vd_exports_and_the_command_processor) {
    VdRig rig;
    CHECK(RunSelfTest(5, 1));
    CHECK_EQ(rig.backend.swapCount.load(), 5);
    CHECK_EQ(rig.backend.lastWidth, 1280);
    CHECK_EQ(rig.backend.lastHeight, 720);
    CHECK_EQ(GpuSystem::Get().Cp().Stats().malformed, 0);
}

TEST(selftest_keeps_ring_space_when_producer_is_faster_than_backend) {
    struct SlowBackend final : IGpuBackend {
        std::atomic<uint32_t> swaps{0};
        void Swap(uint32_t, uint32_t, uint32_t) override {
            // Espera ativa de 1 ms: sleep_for(1ms) dura ~15 ms no timer padrao do Windows.
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(1);
            while (std::chrono::steady_clock::now() < until) std::this_thread::yield();
            swaps.fetch_add(1);
        }
        void OnResize(uint32_t, uint32_t) override {}
    } backend;
    GpuSystemConfig config;
    config.base = GuestBase();
    config.backend = &backend;
    config.allocate = [](uint32_t size) { return Allocate(size); };
    config.enableInterrupts = false;
    CHECK(GpuSystem::Get().Initialize(config));
    const bool ok = RunSelfTest(600, 0);  // Mais de duas voltas no ring de 256 reservas.
    GpuSystem::Get().Shutdown();
    CHECK(ok);
    CHECK_EQ(backend.swaps.load(), 600);
}

TEST(draw_selftest_scene_issues_draws_copies_and_swaps) {
    VdRig rig;
    const Gpu::SceneResult scene = Gpu::RunDrawSelfTest(3, 0);
    CHECK(scene.ok);
    CHECK_EQ(rig.backend.swapCount.load(), 3);
    CHECK_EQ(rig.backend.drawCount.load(), 9);    // dois triangulos e uma rect list por quadro
    CHECK_EQ(rig.backend.copyCount.load(), 9);    // clear + resolve de cor + resolve de profundidade
    CHECK(rig.backend.lastDraw.vertexShader != nullptr);
    CHECK(rig.backend.lastDraw.pixelShader != nullptr);
    CHECK(rig.backend.lastDraw.primitive == xenos::PrimitiveType::kRectangleList);
    CHECK_EQ(rig.backend.lastFrontbuffer, scene.frontbuffer);
    CHECK_EQ(GpuSystem::Get().Cp().Stats().malformed, 0);
}
