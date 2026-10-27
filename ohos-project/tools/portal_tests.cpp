#include "physics/jolt_gameplay_physics.h"
#include "terrain/heightmap_terrain.h"
#include "camera/camera_collision.h"
#include "gameplay/parkour_course.h"
#include "water/water_surface.h"
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
static int rawfileReads = 0;
extern "C" int OHOS_ReadRawFile(const char *path, void **data, size_t *size)
{
    ++rawfileReads;
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
    scene::Definition original, forest;
    std::string error;
    Check(scene::LoadMikanSceneRawFile("scenes/main.json", original, error), "parse original scene");
    Check(scene::LoadMikanSceneRawFile("scenes/forest_test.json", forest, error), "parse forest scene");
    Check(original.portals[0].target == "scenes/forest_test.json" && forest.portals[0].target == "scenes/main.json", "reciprocal destinations");
    Check(original.enemyEnabled && original.demoPropsEnabled && !forest.enemyEnabled && !forest.demoPropsEnabled, "scene-specific gameplay flags");
    const auto defaults = R"({"formatVersion":1,"entities":[]})";
    scene::Definition parsed;
    Check(scene::ParseMikanScene(defaults, std::strlen(defaults), parsed, error) && parsed.portals.empty() && parsed.enemyEnabled && parsed.demoPropsEnabled, "backward-compatible optional metadata");
    for (const char* bad : {
        R"({"formatVersion":1,"entities":[],"gameplay":{"portal":{"position":[0,0,0],"target":"../main.json"}}})",
        R"({"formatVersion":1,"entities":[],"gameplay":{"portal":{"position":[0,0,0],"target":"scenes/a.json","radius":0}}})",
        R"({"formatVersion":1,"entities":[],"gameplay":{"playerSpawn":[0,1e300,0]}})",
        R"({"formatVersion":1,"entities":[],"gameplay":{"portal":{"position":[0,0],"target":"scenes/a.json"}}})"})
        Check(!scene::ParseMikanScene(bad, std::strlen(bad), parsed, error), "reject invalid transition metadata");
    ohos_model::Model terrain;
    Check(terrain::BuildModel(forest, -.94f, terrain, error), "build actual 16-bit forest terrain");
    float low=100, high=-100, minX=100, maxX=-100;
    for (const auto& mesh : terrain.meshes) for (const auto& v : mesh.vertices) {
        low=std::min(low,v.position[1]); high=std::max(high,v.position[1]);
        minX=std::min(minX,v.position[0]); maxX=std::max(maxX,v.position[0]);
    }
    Check(std::fabs(minX+12)<.001f && std::fabs(maxX-12)<.001f && high-low>.7f, "24 metre terrain with non-flat relief");
    int trees=0;
    for (const auto& e : forest.entities) if (e.importAsStaticModel) {
        std::ifstream asset(root+"/"+e.modelPath,std::ios::binary);
        Check(bool(asset), "all forest model resources packaged"); ++trees;
    }
    Check(trees==30, "27 EZTree instances plus three save-pillar pieces recognised by runtime parser");
    Check(original.savePoints.size()==1 && forest.savePoints.size()==1,"hub and forest save positions parsed");
    physics::JoltGameplayPhysics world;
    Check(world.Initialize(), "initialize real Jolt");
    Check(world.PrepareTerrainColliders(original, "scenes/main.json"), "preload original terrain BVH");
    Check(world.PrepareTerrainColliders(forest, "scenes/forest_test.json"), "preload forest terrain BVH");
    const int warmReads = rawfileReads;
    for (int cycle=0;cycle<3;++cycle) {
        Check(world.SetTerrainColliders(original, "scenes/main.json") && world.SetStaticSceneColliders(scene::BuildStaticBoxColliders(original,-.94f)), "install original collision world");
        const float originalHit=world.RaycastWorld(3,4,-2,0,-1,0,10);
        Check(world.SetTerrainColliders(forest, "scenes/forest_test.json") && world.SetStaticSceneColliders(scene::BuildStaticBoxColliders(forest,-.94f)), "replace with forest collision world");
        const float forestHit=world.RaycastWorld(3,4,-2,0,-1,0,10);
        Check(forestHit>originalHit+.7f, "original block collision removed after switch");
        world.Reset(forest.playerSpawn.x,forest.playerSpawn.y-physics::kPlayerVisualOriginOffset,forest.playerSpawn.z,8,0,8);
        world.SetEnemyCollisionEnabled(false);
        Step(world,120);
        const auto player=world.GetPlayerState();
        Check(player.grounded && std::fabs(player.y-(forest.playerSpawn.y-physics::kPlayerVisualOriginOffset))<.06f, "forest spawn stands on matching heightmap collision");
        Check(world.SetTerrainColliders(original, "scenes/main.json") && world.SetStaticSceneColliders(scene::BuildStaticBoxColliders(original,-.94f)), "return restores original world");
        Check(std::fabs(world.RaycastWorld(3,4,-2,0,-1,0,10)-originalHit)<.01f, "return restores original block collision");
    }
    Check(rawfileReads == warmReads, "cached round trips perform no rawfile reads or heightmap decoding");
    // Exercise real swept-sphere queries, not ray-only mock geometry.
    scene::Definition empty;
    Check(world.SetTerrainColliders(empty), "clear terrain for camera wall fixtures");
    scene::StaticBoxCollider wall;
    wall.entityName = "Camera test wall";
    wall.center = {0,0,2}; wall.halfExtents = {3,3,.08f};
    Check(world.SetStaticSceneColliders({wall}), "install thin camera wall");
    auto sweep = [&](float x,float y,float z,float dx,float dy,float dz,float length,float radius) {
        return world.SweepCamera(x,y,z,dx,dy,dz,length,radius);
    };
    camera::ObstructionCamera camera;
    auto blocked = camera.Resolve({0,0,0}, {0,0,0}, {0,0,1}, 2.8f, .24f, .016f, sweep);
    Check(blocked.obstructed && blocked.eye.z < 1.66f && blocked.eye.z > 1.5f,
        "sphere sweep stops before thin wall including camera radius and clearance");
    Check(world.SweepCamera(0,0,2,0,0,1,2,.24f) < .01f,
        "initial overlap cannot force camera through wall");
    wall.center = {.32f,0,1.8f}; wall.halfExtents = {.1f,2,.08f};
    Check(world.SetStaticSceneColliders({wall}), "install wall-edge fixture");
    Check(world.RaycastWorld(0,0,0,0,0,1,2.8f) > 2.79f &&
          world.SweepCamera(0,0,0,0,0,1,2.8f,.24f) < 2,
        "camera volume catches corner missed by centre ray");
    Check(world.SetStaticSceneColliders({}), "remove camera obstacle");
    auto recovering = camera.Resolve({0,0,0}, {0,0,0}, {0,0,1}, 2.8f, .24f, .016f, sweep);
    Check(!recovering.obstructed && recovering.distance > blocked.distance && recovering.distance < 2.8f,
        "camera restores outward smoothly without changing requested zoom");
    for (int i=0;i<90;++i) recovering = camera.Resolve({0,0,0}, {0,0,0}, {0,0,1}, 2.8f,.24f,.016f,sweep);
    Check(std::fabs(recovering.distance-2.8f)<.001f, "clear orbit returns to user distance");
    auto zoomed = camera.Resolve({0,0,0}, {0,0,0}, {0,0,1}, 1.2f,.24f,.016f,sweep);
    Check(std::fabs(zoomed.distance-1.2f)<.001f, "zooming in stays immediate");
    wall.center = {.5f,0,0}; wall.halfExtents = {.1f,2,3};
    Check(world.SetStaticSceneColliders({wall}), "install shoulder-offset wall");
    camera.Reset();
    auto shoulder = camera.Resolve({0,0,0}, {.55f,0,0}, {0,0,1}, 2.8f,.24f,.016f,sweep);
    Check(shoulder.obstructed && shoulder.focus.x < .13f,
        "aim shoulder and damped pivot cannot cross wall");
    Check(camera::CollisionRadius(.1f,60,2.2f)>=.24f && camera::CollisionRadius(.3f,90,2.2f)>.7f,
        "sweep radius covers wide-aspect near plane");
    Check(world.SetStaticSceneColliders({}) && world.SetTerrainColliders(forest,"scenes/forest_test.json"),
        "restore forest terrain for camera test");
    Check(world.SweepCamera(0,2,0,0,-1,0,5,.24f)<3,
        "camera sweep also blocks heightmap terrain");
    scene::Definition parkour;
    Check(scene::LoadMikanSceneRawFile("scenes/parkour.json",parkour,error), "parse parkour scene");
    Check(original.portals.size()==2 && original.portals[1].target=="scenes/physics_parkour.json" &&
        parkour.portals.size()==2 && !parkour.groundEnabled && parkour.parkour.checkpoints.size()==4,
        "hub has two destinations, course has safe return gates and four checkpoints");
    Check(world.SetTerrainColliders(parkour,"scenes/parkour.json") &&
        world.SetStaticSceneColliders(scene::BuildStaticBoxColliders(parkour,-.94f)), "install floating course collision");
    Check(world.RaycastWorld(12,4,0,0,-1,0,15)>14.99f, "no invisible demo floor under floating course");
    std::vector<const scene::Entity*> platforms;
    for (const auto& e : parkour.entities) {
        if (e.name.rfind("Course_",0)==0) platforms.push_back(&e);
        if (e.importAsStaticModel) Check(bool(std::ifstream(root+"/"+e.modelPath,std::ios::binary)), "course marker asset exists");
    }
    Check(platforms.size()==13,"thirteen solid landing platforms");
    for (size_t i=1;i<platforms.size();++i) {
        const auto& a=platforms[i-1]->transform;
        const auto& b=platforms[i]->transform;
        const float dx=b.position.x-a.position.x,dz=b.position.z-a.position.z;
        const float distance=std::sqrt(dx*dx+dz*dz),ux=dx/distance,uz=dz/distance;
        const float edge=std::min(std::fabs(ux)>.001f ? a.scale.x/std::fabs(ux) : 1000.f,
            std::fabs(uz)>.001f ? a.scale.z/std::fabs(uz) : 1000.f)-.12f;
        world.Reset(a.position.x+ux*edge,a.position.y+a.scale.y+.02f,a.position.z+uz*edge,20,0,20);
        world.SetEnemyCollisionEnabled(false); Step(world,15);
        Check(world.GetPlayerState().grounded,"jump approach supported by source platform");
        bool landed=false, airborne=false;
        for (int frame=0;frame<140;++frame) {
            const auto before=world.GetPlayerState();
            const float remaining=(b.position.x-before.x)*ux+(b.position.z-before.z)*uz;
            const float speed=remaining>.08f ? 2.6f : 0.f;
            world.MovePlayer(ux*speed,uz*speed,1.f/60,frame==0);
            const auto after=world.GetPlayerState();
            airborne |= !after.grounded;
            if (airborne && after.grounded && std::fabs(after.x-b.position.x)<b.scale.x &&
                std::fabs(after.z-b.position.z)<b.scale.z &&
                std::fabs(after.y-(b.position.y+b.scale.y))<.08f) { landed=true; break; }
        }
        if (!landed) std::fprintf(stderr,"Unreachable course jump %zu\n",i);
        Check(landed,"every authored gap can be jumped using real Jolt and current sprint speed");
    }
    // Reproduce the mobile case: build sprint speed, press jump, then release
    // the stick. Earlier gap tests continuously supplied the target velocity.
    const auto& start = platforms.front()->transform;
    world.Reset(0,start.position.y+start.scale.y+.02f,0,20,0,20);
    world.SetEnemyCollisionEnabled(false); Step(world,10);
    for (int i=0;i<12;++i) world.MovePlayer(0,-2.6f,1.f/60,false);
    const auto takeoff = world.GetPlayerState();
    Check(takeoff.velocityZ < -2.5f,"ground acceleration reaches sprint speed");
    world.MovePlayer(0,0,1.f/60,true);
    const auto jumped = world.GetPlayerState();
    Check(jumped.velocityY>4 && jumped.velocityZ < -2.5f,"jump with stick release preserves takeoff sprint velocity");
    for (int i=0;i<30;++i) world.MovePlayer(0,0,1.f/60,false);
    const auto apex = world.GetPlayerState();
    Check(apex.z < takeoff.z-1.2f && apex.y > takeoff.y+.9f && apex.velocityZ < -2.5f,
        "released stick keeps forward travel through jump apex");
    world.MovePlayer(2.6f,0,1.f/60,false);
    const auto steering=world.GetPlayerState();
    Check(steering.velocityZ < -2.4f && steering.velocityX>0 && steering.velocityX<.05f,
        "air steering accelerates gradually without instantly replacing momentum");
    world.Reset(0,start.position.y+start.scale.y+.02f,0,20,0,20);
    world.SetEnemyCollisionEnabled(false); Step(world,10);
    world.MovePlayer(0,0,1.f/60,true);
    for (int i=0;i<30;++i) world.MovePlayer(0,0,1.f/60,false);
    Check(std::fabs(world.GetPlayerState().z)<.02f,"standing jump stays vertical");
    world.Reset(0,start.position.y+start.scale.y+.02f,0,20,0,20);
    world.SetEnemyCollisionEnabled(false); Step(world,10);
    for (int i=0;i<12;++i) world.MovePlayer(0,-2.6f,1.f/60,false);
    for (int i=0;i<10;++i) world.MovePlayer(0,0,1.f/60,false);
    Check(std::fabs(world.GetPlayerState().velocityZ)<.01f,"ground release brakes to rest");
    gameplay::ParkourProgress progress;
    const auto& course=parkour.parkour;
    progress.Update(course,course.checkpoints[2].spawn,true,.1f);
    Check(progress.checkpoint==0,"cannot skip checkpoints");
    progress.Update(course,course.checkpoints[1].spawn,false,.1f);
    Check(progress.checkpoint==0,"airborne pass does not activate checkpoint");
    progress.Update(course,course.checkpoints[1].spawn,true,.1f);
    Check(progress.checkpoint==1,"standing on rest island activates checkpoint");
    Check(progress.Update(course,{0,0,-19},false,.1f) && progress.falls==1 && progress.checkpoint==1,
        "fall requests respawn without losing checkpoint");
    for (size_t i=2;i<course.checkpoints.size();++i) progress.Update(course,course.checkpoints[i].spawn,true,.1f);
    const auto elapsed=progress.seconds;
    progress.Update(course,course.checkpoints.back().spawn,true,1);
    Check(progress.finished && progress.seconds==elapsed,"finish freezes completion time");
    Check(world.SetTerrainColliders(original,"scenes/main.json") &&
        world.SetStaticSceneColliders(scene::BuildStaticBoxColliders(original,-.94f)), "restore hub after parkour");
    Check(world.RaycastWorld(12,4,0,0,-1,0,15)<6,"hub floor restored after course exit");
    for (const char* bad : {
        R"({"formatVersion":1,"entities":[],"gameplay":{"portals":{}}})",
        R"({"formatVersion":1,"entities":[],"gameplay":{"portals":[{"target":"../escape.json","position":[0,0,0]}]}})",
        R"({"formatVersion":1,"entities":[],"gameplay":{"parkour":{"checkpoints":[]}}})"})
        Check(!scene::ParseMikanScene(bad,std::strlen(bad),parsed,error),"reject malformed course or multi-portal data");
    scene::Definition workshop;
    Check(scene::LoadMikanSceneRawFile("scenes/workshop.json",workshop,error),"parse mechanical workshop");
    Check(workshop.workshop.enabled && workshop.portals.size()==2 && !workshop.groundEnabled,
        "workshop metadata and two return routes");
    const auto installWorkshop=[&](const scene::Definition& definition) {
        return world.SetTerrainColliders(definition) &&
            world.SetStaticSceneColliders(scene::BuildStaticBoxColliders(definition,-.94f));
    };
    Check(installWorkshop(workshop),"create dynamic crate and kinematic gate/lift");
    world.Reset(0,.02f,-1.2f,20,0,20); world.SetEnemyCollisionEnabled(false); Step(world,90);
    Check(!world.GetWorkshopState().platePressed && !world.GetWorkshopState().gateOpen,
        "player alone cannot activate crate plate");
    Check(world.RaycastWorld(0,1,-3,0,0,-1,3)<1.6f,"gate initially blocks corridor");
    world.Reset(0,.02f,4,20,0,20); world.SetEnemyCollisionEnabled(false); Step(world,10);
    for (int frame=0;frame<600 && !world.GetWorkshopState().gateOpen;++frame)
        world.MovePlayer(0,-1.3f,1.f/60,false);
    scene::Transform crate;
    Check(world.GetWorkshopTransform(workshop.workshop.crateId,crate) && crate.position.z<-.5f,
        "character contact pushes real dynamic crate toward pressure plate");
    Check(world.GetWorkshopState().gateOpen,"crate held on plate latches gate");
    Step(world,180);
    Check(world.RaycastWorld(0,1,-3,0,0,-1,3)>2.99f,"raised gate collision follows visual transform");
    // Begin on the lower lift with the crate on the plate to observe one full
    // boarding/ascent/descent cycle, without teleporting an active mechanism.
    auto liftFixture=workshop;
    for (auto& e:liftFixture.entities) if (e.id==workshop.workshop.crateId) e.transform.position.z=-1.2f;
    Check(installWorkshop(liftFixture),"install lift carrying fixture");
    world.Reset(0,.02f,-7.8f,20,0,20); world.SetEnemyCollisionEnabled(false);
    float maxRiderY=-100; int carriedFrames=0;
    for (int frame=0;frame<1000;++frame) {
        world.MovePlayer(0,0,1.f/60,false);
        const auto rider=world.GetPlayerState(); const auto state=world.GetWorkshopState();
        maxRiderY=std::max(maxRiderY,rider.y);
        if (state.liftHeight>.2f && state.liftHeight<3.8f && rider.grounded &&
            std::fabs(rider.y-state.liftHeight)<.12f) ++carriedFrames;
    }
    Check(maxRiderY>3.9f && carriedFrames>200,"standing player rides the lift through ascent and descent");
    world.Reset(0,.02f,6,20,0,20); world.SetEnemyCollisionEnabled(false);
    Check(!world.GetWorkshopState().gateOpen && world.GetWorkshopState().liftHeight<.01f,
        "retry resets crate, gate, lift and latch together");
    Check(world.SetTerrainColliders(original,"scenes/main.json") &&
        world.SetStaticSceneColliders(scene::BuildStaticBoxColliders(original,-.94f)) &&
        !world.GetWorkshopState().enabled && !world.GetWorkshopTransform(workshop.workshop.crateId,crate),
        "exit removes workshop bodies and restores hub");
    scene::Definition combined;
    Check(scene::LoadMikanSceneRawFile("scenes/physics_parkour.json",combined,error),"parse combined physics parkour course");
    Check(combined.parkour.enabled && combined.workshop.enabled && combined.workshop.movingPlatforms.size()==2 &&
        combined.parkour.checkpoints.size()==7 && combined.water.enabled,"merged route contains all stages and checkpoints");
    Check(!water::PlayerSubmerged(combined,0,0,-67,false) &&
        !water::PlayerSubmerged(combined,5,11,-89,false) && water::PlayerSubmerged(combined,0,11.45f,-89,false),
        "rectangular pool swimming is bounded in both X and Z");
    Check(installWorkshop(combined),"install combined collision world");
    for (const auto& moving:combined.workshop.movingPlatforms) {
        scene::Transform platform;
        Check(world.GetWorkshopTransform(moving.entityId,platform),"moving platform has a live body");
        world.Reset(platform.position.x,platform.position.y+platform.scale.y+.02f,platform.position.z,20,0,20);
        world.SetEnemyCollisionEnabled(false);
        const auto initial=platform.position;
        float maximumTravel=0; int supportedFrames=0;
        for (int frame=0;frame<720;++frame) {
            world.MovePlayer(0,0,1.f/60,false);
            world.GetWorkshopTransform(moving.entityId,platform);
            const auto rider=world.GetPlayerState();
            maximumTravel=std::max(maximumTravel,std::fabs(platform.position.x-initial.x)+std::fabs(platform.position.z-initial.z));
            if (rider.grounded && std::fabs(rider.x-platform.position.x)<.2f &&
                std::fabs(rider.z-platform.position.z)<.2f) ++supportedFrames;
        }
        if (supportedFrames<650) std::fprintf(stderr,"Moving platform %u rider support frames %d\n",moving.entityId,supportedFrames);
        Check(maximumTravel>.95f*(std::fabs(moving.travel.x)+std::fabs(moving.travel.z)) && supportedFrames>650,
            "stationary player rides horizontal/longitudinal platform through reversals");
    }
    world.RespawnPlayer(0,11.45f,-90);
    bool swam=false, exited=false;
    for (int frame=0;frame<800;++frame) {
        world.MovePlayer(0,-1.3f,1.f/60,false);
        const auto swimmer=world.GetPlayerState();
        swam |= water::PlayerSubmerged(combined,swimmer.x,swimmer.y,swimmer.z,false);
        if (swimmer.z<-97 && swimmer.y>12.7f && swimmer.grounded) { exited=true; break; }
    }
    if (!exited) { const auto p=world.GetPlayerState(); std::fprintf(stderr,"Pool exit blocked at %.2f %.2f %.2f\n",p.x,p.y,p.z); }
    Check(swam && exited,"player swims lane then walks up exit stairs onto final deck");
    // Respawn a checkpoint without resetting the already solved mechanism.
    auto solvedFixture=combined;
    for (auto& e:solvedFixture.entities) if (e.id==combined.workshop.crateId)
        e.transform.position.z=combined.workshop.plate.z;
    Check(installWorkshop(solvedFixture),"install checkpoint latch fixture");
    world.Reset(2,8.92f,-50,20,0,20); world.SetEnemyCollisionEnabled(false); Step(world,120);
    Check(world.GetWorkshopState().gateOpen,"combined mechanism latches normally");
    const auto checkpoint=combined.parkour.checkpoints[4].spawn;
    world.RespawnPlayer(checkpoint.x,checkpoint.y-physics::kPlayerVisualOriginOffset,checkpoint.z);
    Step(world,15);
    Check(world.GetWorkshopState().gateOpen && world.GetPlayerState().grounded,
        "checkpoint respawn preserves solved gate and lands on upper deck");
    world.Reset(checkpoint.x,checkpoint.y-physics::kPlayerVisualOriginOffset,checkpoint.z,20,0,20);
    Check(!world.GetWorkshopState().gateOpen,"fresh physics world clears solved progress");
    world.RestoreGateProgress(true);
    Step(world,15);
    Check(world.GetWorkshopState().gateOpen && world.GetPlayerState().grounded,"saved gate progress restored at upper checkpoint");
    Check(combined.savePoints.size()==7,"all seven stationary course islands have save points");
    std::printf("Scene tests passed: %d checks, jumps, crate puzzle, moving platform rides and swimming exit.\n",checks);
}
