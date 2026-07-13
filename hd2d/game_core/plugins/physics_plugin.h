#pragma once

#include "arimu/App.hpp"

#include <Jolt/Jolt.h>

#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <entt/entt.hpp>

#include <memory>

namespace hd2d {

// Object layers shared by every physics consumer (cloth, characters, bodies).
namespace PhysLayers {
constexpr JPH::ObjectLayer NON_MOVING = 0;
constexpr JPH::ObjectLayer MOVING = 1;
constexpr JPH::ObjectLayer NUM_LAYERS = 2;
} // namespace PhysLayers

// The Jolt world resource. Owned by the World; systems request ResMut<Physics>.
// The filter implementations live in physics_plugin.cpp behind interfaces.
struct Physics {
    std::unique_ptr<JPH::TempAllocatorImpl> temp;
    std::unique_ptr<JPH::JobSystemThreadPool> jobs;
    std::unique_ptr<JPH::BroadPhaseLayerInterface> bp_layers;
    std::unique_ptr<JPH::ObjectVsBroadPhaseLayerFilter> obj_vs_bp;
    std::unique_ptr<JPH::ObjectLayerPairFilter> obj_pairs;
    std::unique_ptr<JPH::PhysicsSystem> system;
    bool ready = false;
};

// Jolt physics: static map collision from Blender extras, dynamic rigid
// bodies, and CharacterVirtual-driven character movement. Add AFTER the
// gameplay plugins (Combat/Player write desired Transforms; these systems
// resolve them) and the map spawn (CorePlugin).
struct PhysicsPlugin {
    static void Build(Arimu::App& app);
};

// Remove any Jolt bodies attached to `e` (StaticBody / RigidBody / the
// character mirror / cloth). Called by the map reload path before despawning.
void physics_remove_entity_bodies(Arimu::World& world, entt::entity e);

} // namespace hd2d
