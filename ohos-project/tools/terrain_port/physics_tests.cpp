#include "physics/jolt_gameplay_physics.h"
#include <SDL3/SDL_stdinc.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>

// Only platform IO and logging are stubbed; collision and motion use real Jolt.
static bool SDL_SetError(const char *, ...) { return false; }
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#define STBI_OHOS_TERRAIN
#include "stb_image.h"
extern "C" void SDL_Log(const char *, ...) {}
extern "C" void SDL_LogWarn(int, const char *, ...) {}
extern "C" void SDL_LogError(int, const char *, ...) {}
static std::string root;
extern "C" int OHOS_ReadRawFile(const char *path, void **data, size_t *size)
{
    std::ifstream file(root + "/" + path, std::ios::binary);
    if (!file) return 0;
    const std::string bytes((std::istreambuf_iterator<char>(file)), {});
    *size = bytes.size();
    *data = std::malloc(*size);
    if (!*data) return 0;
    std::memcpy(*data, bytes.data(), *size);
    return 1;
}
extern "C" void OHOS_FreeRawFile(void *data) { std::free(data); }
static int checks = 0;
static void Check(bool condition, const char *message)
{
    ++checks;
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
static void Step(physics::JoltGameplayPhysics &world, int frames)
{
    for (int i = 0; i < frames; ++i) world.MovePlayer(0, 0, 1.0f / 60, false);
}
int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    root = argv[1];
    physics::JoltGameplayPhysics world;
    Check(world.Initialize(), "initialize real Jolt");
    world.Reset(0, 3, 0, 5, 3, 5);
    Step(world, 120);
    auto state = world.GetPlayerState();
    Check(state.grounded && std::fabs(state.y + 0.94f) < 0.06f, "fall onto finite cube platform");
    world.MovePlayer(0, 0, 1.0f / 60, true);
    Step(world, 10);
    Check(world.GetPlayerState().y > state.y + 0.4f, "jump leaves platform");
    Step(world, 120);
    Check(world.GetPlayerState().grounded, "jump lands on platform");
    world.Reset(25, -0.94f, 0, -25, -0.94f, 0);
    Step(world, 60);
    state = world.GetPlayerState();
    std::printf("Outside platform without terrain: y=%.4f vy=%.4f\n", state.y, state.velocityY);
    Check(state.y < -4 && state.velocityY < -8, "outside platform falls below old global plane");
    for (int i = 0; i < 60; ++i) world.MoveEnemy(0, 0, 1.0f / 60);
    Check(world.GetEnemyState().y < -4, "enemy also follows gravity outside platform");
    world.Reset(0, -0.94f, 0, 1.5f, -0.94f, 0);
    for (int i=0; i<120; ++i) {
        world.MoveEnemy(0, 0, 1.0f/60);
        world.MovePlayer(2, 0, 1.0f/60, false);
    }
    Check(world.GetPlayerState().x < 1.0f, "living enemy capsule blocks player");
    world.SetEnemyCollisionEnabled(false);
    world.SetEnemyCollisionEnabled(false); // Repeated death notifications are safe.
    for (int i=0; i<120; ++i) world.MovePlayer(2, 0, 1.0f/60, false);
    Check(world.GetPlayerState().x > 3.0f, "dead enemy capsule no longer blocks player");
    auto deadEnemy = world.GetEnemyState();
    world.MoveEnemy(10, 0, 1.0f);
    Check(std::fabs(world.GetEnemyState().x - deadEnemy.x) < 0.001f,
          "disabled enemy physics preserves death pose");
    world.Reset(0, -0.94f, 0, 1.5f, -0.94f, 0);
    for (int i=0; i<120; ++i) {
        world.MoveEnemy(0, 0, 1.0f/60);
        world.MovePlayer(2, 0, 1.0f/60, false);
    }
    Check(world.GetPlayerState().x < 1.0f, "scene reset restores enemy collision");
    scene::Definition definition;
    std::string error;
    Check(scene::LoadMikanSceneRawFile("scenes/main.json", definition, error), "load packaged default scene");
    definition.water.enabled = false; // Test land collision independently of buoyancy.
    Check(world.SetTerrainColliders(definition), "install real default heightmap mesh colliders");
    world.Reset(20.8f, 3, 0, -20.8f, 3, 0);
    Step(world, 180);
    state = world.GetPlayerState();
    std::printf("Terrain apron: y=%.4f grounded=%d\n", state.y, state.grounded);
    Check(state.grounded && std::fabs(state.y + 1.34f) < 0.06f, "land on lower terrain apron");
    world.Reset(29, 3, 0, -29, 3, 0);
    Step(world, 180);
    state = world.GetPlayerState();
    std::printf("Terrain slope: y=%.4f grounded=%d\n", state.y, state.grounded);
    Check(state.grounded && state.y < -2 && state.y > -6.5f, "land on lower heightmap slope");
    world.Reset(0, -0.94f, 0, -24, 3, 0);
    for (int i = 0; i < 360; ++i) world.MovePlayer(4, 0, 1.0f / 60, false);
    state = world.GetPlayerState();
    Check(state.x > 21 && state.y < -1.1f && state.grounded, "walk off platform onto terrain");
    world.Reset(54, 20, -46, -24, 3, 0);
    Step(world, 240);
    state = world.GetPlayerState();
    Check(state.grounded && state.y > 2.5f, "island mesh provides matching physical ground above sea");
    definition.water.enabled = true;
    Check(world.SetTerrainColliders(definition), "enable scene water for buoyancy");
    world.Reset(80, 2, 0, -24, 3, 0);
    Step(world, 600);
    state = world.GetPlayerState();
    Check(std::fabs(state.y - (definition.water.height - 0.95f)) < 0.03f &&
          std::fabs(state.velocityY) < 0.03f && !state.grounded, "swimmer floats at sea level without sinking");
    world.Reset(80, -6, 0, -24, 3, 0);
    Step(world, 300);
    Check(std::fabs(world.GetPlayerState().y - (definition.water.height - 0.95f)) < 0.03f,
          "submerged player rises off seabed");
    world.Reset(29, definition.water.height - 0.95f, 0, -24, 3, 0);
    for (int i=0; i<240; ++i) world.MovePlayer(-2, 0, 1.0f/60, false);
    state = world.GetPlayerState();
    Check(state.grounded && state.y > -1.5f, "swimmer walks ashore and resumes terrain support");
    std::printf("PASS: %d real Jolt gravity checks\n", checks);
}
