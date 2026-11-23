/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Cyclon (CYC-2975): an output must not composite output layers that are older than the last
 * geometry update. See SurfaceFlinger::setVisibleRegionDirtyIfNeeded.
 */

#undef LOG_TAG
#define LOG_TAG "LibSurfaceFlingerUnittests"

#include <memory>
#include <optional>

#include <compositionengine/CompositionRefreshArgs.h>
#include <compositionengine/Output.h>
#include <compositionengine/impl/Output.h>
#include <gtest/gtest.h>
#include <ui/LayerStack.h>

#include "CommitAndCompositeTest.h"
#include "FrontEnd/LayerSnapshot.h"
#include "LayerFE.h"

namespace android {
namespace {

using compositionengine::CompositionRefreshArgs;

constexpr ui::LayerStack kLayerStack{123u};
constexpr ui::LayerStack kOtherLayerStack{456u};

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

} // namespace
} // namespace android
