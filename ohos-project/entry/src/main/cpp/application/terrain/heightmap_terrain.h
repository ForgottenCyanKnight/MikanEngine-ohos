#pragma once

#include "../model_loader.h"
#include "../scene/scene_definition.h"

namespace terrain
{
// Geometry is in runtime world space, without the glTF loader's normalization.
// PNG row zero is top; local +Z uses 1-v, matching Mikan's terrain sampler.
// collisionOnly avoids loading/baking materials and honors collisionEnabled.
bool BuildModel(const scene::Definition &scene, float runtimeGroundTopY,
                ohos_model::Model &output, std::string &error, bool collisionOnly = false);
} // namespace terrain
