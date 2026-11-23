/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Cyclon (CYC-2975): an output must not composite output layers that are older than the last
 * geometry update. See SurfaceFlinger::setVisibleRegionDirtyIfNeeded and
 * DisplayDevice::hasStaleGeometry.
 */

#undef LOG_TAG
#define LOG_TAG "LibSurfaceFlingerUnittests"

#include <initializer_list>
#include <memory>
#include <optional>

#include <com_android_graphics_surfaceflinger_flags.h>
#include <common/test/FlagUtils.h>
#include <compositionengine/CompositionRefreshArgs.h>
#include <compositionengine/Output.h>
#include <compositionengine/impl/Output.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <scheduler/Fps.h>
#include <ui/Fence.h>
#include <ui/LayerStack.h>

#include "CommitAndCompositeTest.h"
#include "FrontEnd/LayerSnapshot.h"
#include "LayerFE.h"
#include "mock/MockVSyncTracker.h"

namespace android {
namespace {

using testing::_;
using testing::DoAll;
using testing::Return;
using testing::ReturnRef;
using testing::SetArgPointee;

using compositionengine::CompositionRefreshArgs;

namespace flags = com::android::graphics::surfaceflinger::flags;

constexpr ui::LayerStack kLayerStack{123u};
constexpr ui::LayerStack kOtherLayerStack{456u};

constexpr int kVirtualDisplayWidth = 640;
constexpr int kVirtualDisplayHeight = 480;

class StaleOutputLayersTest : public CommitAndCompositeTest {
protected:
    // An enabled output for the layers of `layerStack`, with the real Output::includesLayer.
    std::shared_ptr<compositionengine::Output> createOutput(ui::LayerStack layerStack) {
        std::shared_ptr<compositionengine::Output> output =
                compositionengine::impl::createOutput(mFlinger.getCompositionEngine());
        output->setLayerFilter({layerStack, /*toInternalDisplay=*/false});
        output->setCompositionEnabled(true);
        return output;
    }

    // A LayerFE as it is while a frame composites it: it holds the snapshot of its layer, here a
    // layer on `layerStack`. Without a layer stack it has no snapshot, as between frames.
    static sp<LayerFE> createLayerFE(std::optional<ui::LayerStack> layerStack) {
        auto layerFE = sp<LayerFE>::make("test-layer");
        if (layerStack) {
            layerFE->mSnapshot = std::make_unique<surfaceflinger::frontend::LayerSnapshot>();
            layerFE->mSnapshot->outputFilter.layerStack = *layerStack;
        }
        return layerFE;
    }

    // A powered-on GPU virtual display, which is what SurfaceFlinger leaves out of the frames that
    // are out of phase with `requestedRefreshRate`.
    sp<DisplayDevice> injectVirtualDisplay(VirtualDisplayId::BaseId baseId,
                                           ui::LayerStack layerStack,
                                           Fps requestedRefreshRate = 0_Hz) {
        auto compositionEngineDisplayArgs =
                compositionengine::DisplayCreationArgsBuilder()
                        .setId(GpuVirtualDisplayId(baseId))
                        .setPixels({kVirtualDisplayWidth, kVirtualDisplayHeight})
                        .setPowerAdvisor(mPowerAdvisor)
                        .setName("Virtual display")
                        .build();
        auto compositionDisplay =
                compositionengine::impl::createDisplay(mFlinger.getCompositionEngine(),
                                                       std::move(compositionEngineDisplayArgs));

        auto nativeWindow = sp<mock::NativeWindow>::make();
        ON_CALL(*nativeWindow, query(NATIVE_WINDOW_WIDTH, _))
                .WillByDefault(DoAll(SetArgPointee<1>(kVirtualDisplayWidth), Return(0)));
        ON_CALL(*nativeWindow, query(NATIVE_WINDOW_HEIGHT, _))
                .WillByDefault(DoAll(SetArgPointee<1>(kVirtualDisplayHeight), Return(0)));

        // A display without a HWC display counts as client composited, so presenting it asks for
        // this fence even while it has no layers to draw.
        auto displaySurface = sp<compositionengine::mock::DisplaySurface>::make();
        ON_CALL(*displaySurface, getClientTargetAcquireFence())
                .WillByDefault(ReturnRef(mClientTargetAcquireFence));

        constexpr bool kIsPrimary = false;
        auto display = FakeDisplayDeviceInjector(mFlinger, compositionDisplay,
                                                 /*connectionType=*/std::nullopt,
                                                 /*port=*/std::nullopt,
                                                 /*hwcDisplayId=*/std::nullopt, kIsPrimary)
                               .setDisplaySurface(displaySurface)
                               .setNativeWindow(nativeWindow)
                               .setPowerMode(hal::PowerMode::ON)
                               .setRequestedRefreshRate(requestedRefreshRate)
                               .inject();
        display->setLayerFilter({layerStack, /*toInternalDisplay=*/false});
        // The pacesetter of CommitAndCompositeTest refreshes at 60 Hz.
        display->adjustRefreshRate(60_Hz);
        return display;
    }

    static CompositionRefreshArgs refreshArgsFor(
            std::initializer_list<sp<DisplayDevice>> displays) {
        CompositionRefreshArgs refreshArgs;
        for (const auto& display : displays) {
            refreshArgs.outputs.push_back(display->getCompositionDisplay());
        }
        return refreshArgs;
    }

    static bool hasStaleGeometry(const sp<DisplayDevice>& display) {
        return TestableSurfaceFlinger::hasStaleGeometry(display);
    }

    // Whether the virtual displays with a refresh rate are in phase with the frames that follow.
    void setVirtualDisplaysInPhase(bool inPhase) {
        auto& vsyncTracker = static_cast<mock::VSyncTracker&>(
                mFlinger.scheduler()->getVsyncSchedule()->getTracker());
        ON_CALL(vsyncTracker, isVSyncInPhase(_, _)).WillByDefault(Return(inPhase));
    }

    // Output::rebuildLayerStacks is the only writer of the undefined region, and sets it to the
    // whole display for a display without layers.
    static bool hasRebuiltLayerStack(const sp<DisplayDevice>& display) {
        return !display->getUndefinedRegion().isEmpty();
    }

    sp<Fence> mClientTargetAcquireFence = Fence::NO_FENCE;

    // The frames of these tests composite no display on the offload thread.
    std::optional<CompositionRefreshArgs> mNoOffloadedRefreshArgs;
};

// ---------------------------------------------------------------------------
// SurfaceFlinger::setVisibleRegionDirtyIfNeeded

TEST_F(StaleOutputLayersTest, snapshotOfAnotherLayerStackForcesRebuild) {
    const auto output = createOutput(kLayerStack);
    // The layer left this output for a display with another layer stack, and the frame put that
    // display's snapshot into the LayerFE, which the layer has on both displays.
    output->injectOutputLayerForTest(createLayerFE(kOtherLayerStack));

    CompositionRefreshArgs refreshArgs;
    refreshArgs.outputs.push_back(output);
    mFlinger.setVisibleRegionDirtyIfNeeded(refreshArgs);

    EXPECT_TRUE(refreshArgs.updatingOutputGeometryThisFrame);
    EXPECT_TRUE(refreshArgs.updatingGeometryThisFrame);
    EXPECT_TRUE(mFlinger.getVisibleRegionsDirty());
}

TEST_F(StaleOutputLayersTest, snapshotWithoutLayerStackForcesRebuild) {
    const auto output = createOutput(kLayerStack);
    // The layer is no longer on any display.
    output->injectOutputLayerForTest(createLayerFE(ui::UNASSIGNED_LAYER_STACK));

    CompositionRefreshArgs refreshArgs;
    refreshArgs.outputs.push_back(output);
    mFlinger.setVisibleRegionDirtyIfNeeded(refreshArgs);

    EXPECT_TRUE(refreshArgs.updatingOutputGeometryThisFrame);
    EXPECT_TRUE(refreshArgs.updatingGeometryThisFrame);
    EXPECT_TRUE(mFlinger.getVisibleRegionsDirty());
}

TEST_F(StaleOutputLayersTest, nullSnapshotForcesRebuild) {
    const auto output = createOutput(kLayerStack);
    output->injectOutputLayerForTest(createLayerFE(std::nullopt));

    CompositionRefreshArgs refreshArgs;
    refreshArgs.outputs.push_back(output);
    mFlinger.setVisibleRegionDirtyIfNeeded(refreshArgs);

    EXPECT_TRUE(refreshArgs.updatingOutputGeometryThisFrame);
    EXPECT_TRUE(refreshArgs.updatingGeometryThisFrame);
    EXPECT_TRUE(mFlinger.getVisibleRegionsDirty());
}

TEST_F(StaleOutputLayersTest, snapshotOfOwnLayerStackChangesNothing) {
    const auto output = createOutput(kLayerStack);
    output->injectOutputLayerForTest(createLayerFE(kLayerStack));
    output->injectOutputLayerForTest(createLayerFE(kLayerStack));

    CompositionRefreshArgs refreshArgs;
    refreshArgs.outputs.push_back(output);
    mFlinger.setVisibleRegionDirtyIfNeeded(refreshArgs);

    EXPECT_FALSE(refreshArgs.updatingOutputGeometryThisFrame);
    EXPECT_FALSE(refreshArgs.updatingGeometryThisFrame);
    EXPECT_FALSE(mFlinger.getVisibleRegionsDirty());
}

TEST_F(StaleOutputLayersTest, staleOutputLayerBehindCurrentOnesForcesRebuild) {
    const auto output = createOutput(kLayerStack);
    output->injectOutputLayerForTest(createLayerFE(kLayerStack));
    output->injectOutputLayerForTest(createLayerFE(kOtherLayerStack));

    // An output without stale output layers does not hide the one that has them.
    const auto otherOutput = createOutput(kOtherLayerStack);
    otherOutput->injectOutputLayerForTest(createLayerFE(kOtherLayerStack));

    CompositionRefreshArgs refreshArgs;
    refreshArgs.outputs.push_back(otherOutput);
    refreshArgs.outputs.push_back(output);
    mFlinger.setVisibleRegionDirtyIfNeeded(refreshArgs);

    EXPECT_TRUE(refreshArgs.updatingOutputGeometryThisFrame);
    EXPECT_TRUE(refreshArgs.updatingGeometryThisFrame);
    EXPECT_TRUE(mFlinger.getVisibleRegionsDirty());
}

TEST_F(StaleOutputLayersTest, disabledOutputIsNotInspected) {
    // A disabled output does not rebuild its layer stack, and nothing reads its output layers.
    const auto output = createOutput(kLayerStack);
    output->injectOutputLayerForTest(createLayerFE(kOtherLayerStack));
    output->setCompositionEnabled(false);

    CompositionRefreshArgs refreshArgs;
    refreshArgs.outputs.push_back(output);
    mFlinger.setVisibleRegionDirtyIfNeeded(refreshArgs);

    EXPECT_FALSE(refreshArgs.updatingOutputGeometryThisFrame);
    EXPECT_FALSE(refreshArgs.updatingGeometryThisFrame);
    EXPECT_FALSE(mFlinger.getVisibleRegionsDirty());
}

TEST_F(StaleOutputLayersTest, outputsThatRebuildAnywayAreNotReported) {
    const auto output = createOutput(kLayerStack);
    output->injectOutputLayerForTest(createLayerFE(kOtherLayerStack));

    // forceGeometryUpdateForStaleDisplays raised the flags for these outputs. Their rebuild drops
    // the stale output layer, which is no reason to make the frame dirty for every display.
    CompositionRefreshArgs refreshArgs;
    refreshArgs.outputs.push_back(output);
    refreshArgs.updatingOutputGeometryThisFrame = true;
    refreshArgs.updatingGeometryThisFrame = true;
    mFlinger.setVisibleRegionDirtyIfNeeded(refreshArgs);

    EXPECT_TRUE(refreshArgs.updatingOutputGeometryThisFrame);
    EXPECT_TRUE(refreshArgs.updatingGeometryThisFrame);
    EXPECT_FALSE(mFlinger.getVisibleRegionsDirty());
}

TEST_F(StaleOutputLayersTest, visibleRegionsDirtyReachesTheRefreshArgs) {
    // As before: the outputs of the offload thread get the flags that the check of the main
    // thread's outputs raised late.
    const auto output = createOutput(kLayerStack);
    mFlinger.mutableVisibleRegionsDirty() = true;

    CompositionRefreshArgs refreshArgs;
    refreshArgs.outputs.push_back(output);
    mFlinger.setVisibleRegionDirtyIfNeeded(refreshArgs);

    EXPECT_TRUE(refreshArgs.updatingOutputGeometryThisFrame);
    EXPECT_TRUE(refreshArgs.updatingGeometryThisFrame);
}

// ---------------------------------------------------------------------------
// DisplayDevice::hasStaleGeometry, as SurfaceFlinger::composite keeps it

TEST_F(StaleOutputLayersTest, displayLeftOutOfGeometryDirtyFrameRebuildsOnItsNextFrame) {
    const auto virtualDisplay = injectVirtualDisplay(0, kLayerStack);

    // A geometry-dirty frame that leaves the virtual display out.
    auto refreshArgs = refreshArgsFor({mDisplay});
    refreshArgs.updatingOutputGeometryThisFrame = true;
    refreshArgs.updatingGeometryThisFrame = true;
    mFlinger.forceGeometryUpdateForStaleDisplays(refreshArgs, mNoOffloadedRefreshArgs);
    mFlinger.updateDisplaysWithStaleGeometry(true, true, refreshArgs, mNoOffloadedRefreshArgs);

    EXPECT_FALSE(hasStaleGeometry(mDisplay));
    EXPECT_TRUE(hasStaleGeometry(virtualDisplay));

    // Its next frame, which has no geometry update of its own.
    refreshArgs = refreshArgsFor({mDisplay, virtualDisplay});
    mFlinger.forceGeometryUpdateForStaleDisplays(refreshArgs, mNoOffloadedRefreshArgs);

    EXPECT_TRUE(refreshArgs.updatingOutputGeometryThisFrame);
    EXPECT_TRUE(refreshArgs.updatingGeometryThisFrame);

    mFlinger.updateDisplaysWithStaleGeometry(false, false, refreshArgs, mNoOffloadedRefreshArgs);

    EXPECT_FALSE(hasStaleGeometry(mDisplay));
    EXPECT_FALSE(hasStaleGeometry(virtualDisplay));

    // The frame after that is a normal one again.
    refreshArgs = refreshArgsFor({mDisplay, virtualDisplay});
    mFlinger.forceGeometryUpdateForStaleDisplays(refreshArgs, mNoOffloadedRefreshArgs);

    EXPECT_FALSE(refreshArgs.updatingOutputGeometryThisFrame);
    EXPECT_FALSE(refreshArgs.updatingGeometryThisFrame);
}

TEST_F(StaleOutputLayersTest, normalFrameIsUnchanged) {
    const auto virtualDisplay = injectVirtualDisplay(0, kLayerStack);

    // No geometry update, and a display that sits the frame out.
    auto refreshArgs = refreshArgsFor({mDisplay});
    mFlinger.forceGeometryUpdateForStaleDisplays(refreshArgs, mNoOffloadedRefreshArgs);
    mFlinger.updateDisplaysWithStaleGeometry(false, false, refreshArgs, mNoOffloadedRefreshArgs);

    EXPECT_FALSE(refreshArgs.updatingOutputGeometryThisFrame);
    EXPECT_FALSE(refreshArgs.updatingGeometryThisFrame);
    EXPECT_FALSE(hasStaleGeometry(mDisplay));
    EXPECT_FALSE(hasStaleGeometry(virtualDisplay));

    // No geometry update, with every display.
    refreshArgs = refreshArgsFor({mDisplay, virtualDisplay});
    mFlinger.forceGeometryUpdateForStaleDisplays(refreshArgs, mNoOffloadedRefreshArgs);
    mFlinger.updateDisplaysWithStaleGeometry(false, false, refreshArgs, mNoOffloadedRefreshArgs);

    EXPECT_FALSE(refreshArgs.updatingOutputGeometryThisFrame);
    EXPECT_FALSE(refreshArgs.updatingGeometryThisFrame);
    EXPECT_FALSE(hasStaleGeometry(mDisplay));
    EXPECT_FALSE(hasStaleGeometry(virtualDisplay));

    // A geometry update, with every display.
    refreshArgs = refreshArgsFor({mDisplay, virtualDisplay});
    refreshArgs.updatingOutputGeometryThisFrame = true;
    refreshArgs.updatingGeometryThisFrame = true;
    mFlinger.forceGeometryUpdateForStaleDisplays(refreshArgs, mNoOffloadedRefreshArgs);
    mFlinger.updateDisplaysWithStaleGeometry(true, true, refreshArgs, mNoOffloadedRefreshArgs);

    EXPECT_FALSE(hasStaleGeometry(mDisplay));
    EXPECT_FALSE(hasStaleGeometry(virtualDisplay));
}

TEST_F(StaleOutputLayersTest, offloadedDisplayRebuildsWithoutTheMainThreadDisplays) {
    const auto virtualDisplay = injectVirtualDisplay(0, kLayerStack);
    TestableSurfaceFlinger::setHasStaleGeometry(virtualDisplay, true);

    auto refreshArgs = refreshArgsFor({mDisplay});
    std::optional<CompositionRefreshArgs> offloadedRefreshArgs = refreshArgsFor({virtualDisplay});
    mFlinger.forceGeometryUpdateForStaleDisplays(refreshArgs, offloadedRefreshArgs);

    EXPECT_FALSE(refreshArgs.updatingOutputGeometryThisFrame);
    EXPECT_FALSE(refreshArgs.updatingGeometryThisFrame);
    EXPECT_TRUE(offloadedRefreshArgs->updatingOutputGeometryThisFrame);
    EXPECT_TRUE(offloadedRefreshArgs->updatingGeometryThisFrame);

    mFlinger.updateDisplaysWithStaleGeometry(false, false, refreshArgs, offloadedRefreshArgs);

    EXPECT_FALSE(hasStaleGeometry(mDisplay));
    EXPECT_FALSE(hasStaleGeometry(virtualDisplay));
}

TEST_F(StaleOutputLayersTest, poweredOffDisplayStaysStaleUntilAFrameCompositesItPoweredOn) {
    const auto virtualDisplay = injectVirtualDisplay(0, kLayerStack);
    virtualDisplay->setPowerMode(hal::PowerMode::OFF);

    // A geometry-dirty frame has the display among its outputs, but a disabled output does not
    // rebuild its layer stack.
    auto refreshArgs = refreshArgsFor({mDisplay, virtualDisplay});
    refreshArgs.updatingOutputGeometryThisFrame = true;
    refreshArgs.updatingGeometryThisFrame = true;
    mFlinger.forceGeometryUpdateForStaleDisplays(refreshArgs, mNoOffloadedRefreshArgs);
    mFlinger.updateDisplaysWithStaleGeometry(true, true, refreshArgs, mNoOffloadedRefreshArgs);

    EXPECT_FALSE(hasStaleGeometry(mDisplay));
    EXPECT_TRUE(hasStaleGeometry(virtualDisplay));

    // While it is off, it costs the other displays of its frames nothing.
    refreshArgs = refreshArgsFor({mDisplay, virtualDisplay});
    mFlinger.forceGeometryUpdateForStaleDisplays(refreshArgs, mNoOffloadedRefreshArgs);
    mFlinger.updateDisplaysWithStaleGeometry(false, false, refreshArgs, mNoOffloadedRefreshArgs);

    EXPECT_FALSE(refreshArgs.updatingOutputGeometryThisFrame);
    EXPECT_FALSE(refreshArgs.updatingGeometryThisFrame);
    EXPECT_TRUE(hasStaleGeometry(virtualDisplay));

    // SurfaceFlinger::setVirtualDisplayPowerMode does not make the frame geometry-dirty.
    virtualDisplay->setPowerMode(hal::PowerMode::ON);

    refreshArgs = refreshArgsFor({mDisplay, virtualDisplay});
    mFlinger.forceGeometryUpdateForStaleDisplays(refreshArgs, mNoOffloadedRefreshArgs);

    EXPECT_TRUE(refreshArgs.updatingOutputGeometryThisFrame);
    EXPECT_TRUE(refreshArgs.updatingGeometryThisFrame);

    mFlinger.updateDisplaysWithStaleGeometry(false, false, refreshArgs, mNoOffloadedRefreshArgs);

    EXPECT_FALSE(hasStaleGeometry(virtualDisplay));
}

TEST_F(StaleOutputLayersTest, rebuildForStaleDisplayIsNoGeometryUpdateForTheOthers) {
    // Two displays that are out of phase with each other, as two displays of the same refresh
    // rate may be.
    const auto displayA = injectVirtualDisplay(0, kLayerStack);
    const auto displayB = injectVirtualDisplay(1, kOtherLayerStack);

    // Both miss a geometry update.
    auto refreshArgs = refreshArgsFor({mDisplay});
    refreshArgs.updatingOutputGeometryThisFrame = true;
    refreshArgs.updatingGeometryThisFrame = true;
    mFlinger.forceGeometryUpdateForStaleDisplays(refreshArgs, mNoOffloadedRefreshArgs);
    mFlinger.updateDisplaysWithStaleGeometry(true, true, refreshArgs, mNoOffloadedRefreshArgs);

    ASSERT_TRUE(hasStaleGeometry(displayA));
    ASSERT_TRUE(hasStaleGeometry(displayB));

    // A frame of display A.
    refreshArgs = refreshArgsFor({mDisplay, displayA});
    mFlinger.forceGeometryUpdateForStaleDisplays(refreshArgs, mNoOffloadedRefreshArgs);
    EXPECT_TRUE(refreshArgs.updatingOutputGeometryThisFrame);
    mFlinger.updateDisplaysWithStaleGeometry(false, false, refreshArgs, mNoOffloadedRefreshArgs);

    EXPECT_FALSE(hasStaleGeometry(mDisplay));
    EXPECT_FALSE(hasStaleGeometry(displayA));
    EXPECT_TRUE(hasStaleGeometry(displayB));

    // A frame of display B. Display A sits it out, and has nothing to catch up with after it.
    refreshArgs = refreshArgsFor({mDisplay, displayB});
    mFlinger.forceGeometryUpdateForStaleDisplays(refreshArgs, mNoOffloadedRefreshArgs);
    EXPECT_TRUE(refreshArgs.updatingOutputGeometryThisFrame);
    mFlinger.updateDisplaysWithStaleGeometry(false, false, refreshArgs, mNoOffloadedRefreshArgs);

    EXPECT_FALSE(hasStaleGeometry(mDisplay));
    EXPECT_FALSE(hasStaleGeometry(displayA));
    EXPECT_FALSE(hasStaleGeometry(displayB));

    // So neither display forces a rebuild from here on.
    refreshArgs = refreshArgsFor({mDisplay, displayA});
    mFlinger.forceGeometryUpdateForStaleDisplays(refreshArgs, mNoOffloadedRefreshArgs);
    EXPECT_FALSE(refreshArgs.updatingOutputGeometryThisFrame);
    EXPECT_FALSE(refreshArgs.updatingGeometryThisFrame);

    refreshArgs = refreshArgsFor({mDisplay, displayB});
    mFlinger.forceGeometryUpdateForStaleDisplays(refreshArgs, mNoOffloadedRefreshArgs);
    EXPECT_FALSE(refreshArgs.updatingOutputGeometryThisFrame);
    EXPECT_FALSE(refreshArgs.updatingGeometryThisFrame);
}

TEST_F(StaleOutputLayersTest, geometryUpdateWithoutVisibleRegionsIsMissedToo) {
    const auto virtualDisplay = injectVirtualDisplay(0, kLayerStack);

    // SurfaceFlinger::scheduleRepaint or a new color matrix: updatingGeometryThisFrame alone.
    auto refreshArgs = refreshArgsFor({mDisplay});
    refreshArgs.updatingGeometryThisFrame = true;
    mFlinger.forceGeometryUpdateForStaleDisplays(refreshArgs, mNoOffloadedRefreshArgs);
    mFlinger.updateDisplaysWithStaleGeometry(false, true, refreshArgs, mNoOffloadedRefreshArgs);

    EXPECT_FALSE(refreshArgs.updatingOutputGeometryThisFrame);
    // The display of that frame applied the update...
    EXPECT_FALSE(hasStaleGeometry(mDisplay));
    // ...and the one that sat it out did not.
    EXPECT_TRUE(hasStaleGeometry(virtualDisplay));
}

TEST_F(StaleOutputLayersTest, frameMadeDirtyAfterItsOutputsWereChosenIsMissedToo) {
    const auto displayA = injectVirtualDisplay(0, kLayerStack);
    const auto displayB = injectVirtualDisplay(1, kOtherLayerStack);

    // setVisibleRegionDirtyIfNeeded found a stale output layer on the offload thread's display A,
    // after the main thread's outputs were prepared without a rebuild. Display B sits it out.
    auto refreshArgs = refreshArgsFor({mDisplay});
    std::optional<CompositionRefreshArgs> offloadedRefreshArgs = refreshArgsFor({displayA});
    offloadedRefreshArgs->updatingOutputGeometryThisFrame = true;
    offloadedRefreshArgs->updatingGeometryThisFrame = true;
    mFlinger.updateDisplaysWithStaleGeometry(true, true, refreshArgs, offloadedRefreshArgs);

    EXPECT_FALSE(hasStaleGeometry(displayA));
    EXPECT_TRUE(hasStaleGeometry(displayB));
    EXPECT_TRUE(hasStaleGeometry(mDisplay));
}

// ---------------------------------------------------------------------------
// SurfaceFlinger::composite

TEST_F(StaleOutputLayersTest, compositeMarksVirtualDisplayLeftOutOfGeometryDirtyFrame) {
    SET_FLAG_FOR_TEST(flags::frontend_caching_v0, false);
    const auto virtualDisplay = injectVirtualDisplay(0, kLayerStack, 30_Hz);
    setVirtualDisplaysInPhase(false);

    mFlinger.mutableVisibleRegionsDirty() = true;
    mFlinger.commitAndComposite();

    EXPECT_FALSE(mFlinger.getVisibleRegionsDirty());
    EXPECT_FALSE(hasStaleGeometry(mDisplay));
    EXPECT_TRUE(hasStaleGeometry(virtualDisplay));
    EXPECT_FALSE(hasRebuiltLayerStack(virtualDisplay));
}

TEST_F(StaleOutputLayersTest, compositeRebuildsVirtualDisplayOnItsNextFrame) {
    // With this flag every frame is geometry-dirty, and no display has anything to catch up with.
    SET_FLAG_FOR_TEST(flags::frontend_caching_v0, false);
    const auto virtualDisplay = injectVirtualDisplay(0, kLayerStack, 30_Hz);

    // A geometry-dirty frame that the virtual display is out of phase with.
    setVirtualDisplaysInPhase(false);
    mFlinger.mutableVisibleRegionsDirty() = true;
    mFlinger.commitAndComposite();

    ASSERT_TRUE(hasStaleGeometry(virtualDisplay));
    ASSERT_FALSE(hasRebuiltLayerStack(virtualDisplay));
    ASSERT_FALSE(mFlinger.getVisibleRegionsDirty());

    // Its next frame.
    setVirtualDisplaysInPhase(true);
    mFlinger.commitAndComposite();

    EXPECT_TRUE(hasRebuiltLayerStack(virtualDisplay));
    EXPECT_FALSE(hasStaleGeometry(virtualDisplay));
    EXPECT_FALSE(hasStaleGeometry(mDisplay));
}

TEST_F(StaleOutputLayersTest, compositeOfNormalFrameRebuildsNothing) {
    SET_FLAG_FOR_TEST(flags::frontend_caching_v0, false);
    const auto virtualDisplay = injectVirtualDisplay(0, kLayerStack, 30_Hz);
    setVirtualDisplaysInPhase(true);

    // The first frame applies the initial color matrix, which is a geometry update, though not
    // one that rebuilds a layer stack.
    mFlinger.commitAndComposite();
    ASSERT_FALSE(mFlinger.drawingState().colorMatrixChanged);

    ASSERT_FALSE(mFlinger.getVisibleRegionsDirty());
    mFlinger.commitAndComposite();

    EXPECT_FALSE(hasRebuiltLayerStack(virtualDisplay));
    EXPECT_FALSE(hasStaleGeometry(virtualDisplay));
    EXPECT_FALSE(hasStaleGeometry(mDisplay));
}

} // namespace
} // namespace android
