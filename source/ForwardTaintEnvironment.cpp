/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <Show.h>

#include <mariana-trench/Assert.h>
#include <mariana-trench/ForwardTaintEnvironment.h>

namespace marianatrench {

ForwardTaintEnvironment ForwardTaintEnvironment::initial() {
  return ForwardTaintEnvironment::bottom();
}

TaintTree ForwardTaintEnvironment::read(MemoryLocation* memory_location) const {
  return environment_.read(memory_location);
}

TaintTree ForwardTaintEnvironment::read(
    MemoryLocation* memory_location,
    const Path& path) const {
  return environment_.read(memory_location, path);
}

TaintTree ForwardTaintEnvironment::read(
    const MemoryLocationsDomain& memory_locations) const {
  return environment_.read(memory_locations);
}

TaintTree ForwardTaintEnvironment::read(
    const MemoryLocationsDomain& memory_locations,
    const Path& path) const {
  return environment_.read(memory_locations, path);
}

void ForwardTaintEnvironment::write(
    MemoryLocation* memory_location,
    TaintTree taint,
    UpdateKind kind) {
  environment_.write(memory_location, Path{}, std::move(taint), kind);
}

void ForwardTaintEnvironment::write(
    MemoryLocation* memory_location,
    const Path& path,
    TaintTree taint,
    UpdateKind kind) {
  environment_.write(memory_location, path, std::move(taint), kind);
}

void ForwardTaintEnvironment::write(
    MemoryLocation* memory_location,
    const Path& path,
    Taint taint,
    UpdateKind kind) {
  write(memory_location, path, TaintTree{std::move(taint)}, kind);
}

void ForwardTaintEnvironment::write(
    const MemoryLocationsDomain& memory_locations,
    TaintTree taint,
    UpdateKind kind) {
  if (memory_locations.empty()) {
    return;
  }

  environment_.write(memory_locations, Path{}, std::move(taint), kind);
}

void ForwardTaintEnvironment::write(
    const MemoryLocationsDomain& memory_locations,
    Taint taint,
    UpdateKind kind) {
  write(memory_locations, TaintTree(std::move(taint)), kind);
}

void ForwardTaintEnvironment::write(
    const MemoryLocationsDomain& memory_locations,
    const Path& path,
    TaintTree taint,
    UpdateKind kind) {
  if (memory_locations.empty()) {
    return;
  }

  environment_.write(memory_locations, path, std::move(taint), kind);
}

void ForwardTaintEnvironment::write(
    const MemoryLocationsDomain& memory_locations,
    const Path& path,
    Taint taint,
    UpdateKind kind) {
  write(memory_locations, path, TaintTree{std::move(taint)}, kind);
}

TaintTree ForwardTaintEnvironment::deep_read(
    const WideningPointsToResolver& widening_resolver,
    MemoryLocation* memory_location) const {
  auto taint_tree = environment_.deep_read(widening_resolver, memory_location);

  LOG(5,
      "Deep read of memory location {} resolves to: {}",
      show(memory_location),
      taint_tree);

  return taint_tree;
}

TaintTree ForwardTaintEnvironment::deep_read(
    const WideningPointsToResolver& widening_resolver,
    const MemoryLocationsDomain& memory_locations) const {
  auto taint_tree = environment_.deep_read(widening_resolver, memory_locations);

  LOG(5,
      "Deep read of memory location {} resolves to: {}",
      memory_locations,
      taint_tree);

  return taint_tree;
}

TaintTree ForwardTaintEnvironment::deep_read(
    const WideningPointsToResolver& widening_resolver,
    const MemoryLocationsDomain& memory_locations,
    const Path& path) const {
  auto taint_tree =
      environment_.deep_read(widening_resolver, memory_locations, path);

  LOG(5,
      "Deep read of memory location {} at path {} resolves to: {}",
      memory_locations,
      path,
      taint_tree);

  return taint_tree;
}

void ForwardTaintEnvironment::deep_write(
    const WideningPointsToResolver& widening_resolver,
    MemoryLocation* memory_location,
    TaintTree taint,
    UpdateKind kind) {
  environment_.deep_write(
      widening_resolver, memory_location, Path{}, std::move(taint), kind);
}

void ForwardTaintEnvironment::deep_write(
    const WideningPointsToResolver& widening_resolver,
    MemoryLocation* memory_location,
    const Path& path,
    TaintTree taint,
    UpdateKind kind) {
  environment_.deep_write(
      widening_resolver, memory_location, path, std::move(taint), kind);
}

void ForwardTaintEnvironment::deep_write(
    const WideningPointsToResolver& widening_resolver,
    const MemoryLocationsDomain& memory_locations,
    TaintTree taint,
    UpdateKind kind) {
  if (memory_locations.empty()) {
    return;
  }

  environment_.deep_write(
      widening_resolver, memory_locations, Path{}, std::move(taint), kind);
}

void ForwardTaintEnvironment::deep_write(
    const WideningPointsToResolver& widening_resolver,
    const MemoryLocationsDomain& memory_locations,
    Taint taint,
    UpdateKind kind) {
  environment_.deep_write(
      widening_resolver,
      memory_locations,
      Path{},
      TaintTree(std::move(taint)),
      kind);
}

void ForwardTaintEnvironment::deep_write(
    const WideningPointsToResolver& widening_resolver,
    const MemoryLocationsDomain& memory_locations,
    const Path& path,
    TaintTree taint,
    UpdateKind kind) {
  if (memory_locations.empty()) {
    return;
  }

  environment_.deep_write(
      widening_resolver, memory_locations, path, std::move(taint), kind);
}

std::ostream& operator<<(
    std::ostream& out,
    const ForwardTaintEnvironment& environment) {
  return out << environment.environment_;
}

} // namespace marianatrench
