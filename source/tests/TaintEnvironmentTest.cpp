/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <gtest/gtest.h>

#include <mariana-trench/Redex.h>
#include <mariana-trench/TaintEnvironment.h>
#include <mariana-trench/tests/Test.h>

#include <memory>

namespace marianatrench {

class TaintEnvironmentTest : public test::Test {};

TEST_F(TaintEnvironmentTest, LessOrEqualSuperSet) {
  auto context = test::make_empty_context();
  const auto* source_kind = context.kind_factory->get("TestSource");

  Scope scope;
  auto* method = context.methods->create(
      marianatrench::redex::create_void_method(scope, "LClass;", "method"));
  auto* return_port =
      context.access_path_factory->get(AccessPath(Root(Root::Kind::Return)));
  auto* method_origin =
      context.origin_factory->method_origin(method, return_port);

  auto memory_location = std::make_unique<ParameterMemoryLocation>(0);
  auto domain1 = TaintEnvironment{
      {memory_location.get(),
       TaintTree{Taint{test::make_leaf_taint_config(source_kind)}}}};
  EXPECT_TRUE(TaintEnvironment{}.leq(domain1));
  EXPECT_FALSE(domain1.leq(TaintEnvironment{}));

  auto domain2 = TaintEnvironment{
      {memory_location.get(),
       TaintTree{Taint{
           test::make_leaf_taint_config(source_kind),
           test::make_taint_config(
               source_kind,
               test::FrameProperties{
                   .callee_port = context.access_path_factory->get(
                       AccessPath(Root(Root::Kind::Return))),
                   .callee = method,
                   .distance = 1,
                   .origins = OriginSet{method_origin},
                   .call_kind = CallKind::callsite()})}}}};

  EXPECT_TRUE(domain1.leq(domain2));
  EXPECT_FALSE(domain2.leq(domain1));
}

TEST_F(TaintEnvironmentTest, LessOrEqualDifferentSources) {
  auto context = test::make_empty_context();
  const auto* source_kind = context.kind_factory->get("TestSource");

  Scope scope;
  auto* method = context.methods->create(
      marianatrench::redex::create_void_method(scope, "LClass;", "method"));
  auto* return_port =
      context.access_path_factory->get(AccessPath(Root(Root::Kind::Return)));
  auto* method_origin =
      context.origin_factory->method_origin(method, return_port);
  auto memory_location = std::make_unique<ParameterMemoryLocation>(0);

  auto domain1 = TaintEnvironment{
      {memory_location.get(),
       TaintTree{Taint{test::make_leaf_taint_config(source_kind)}}}};

  auto domain2 = TaintEnvironment{
      {memory_location.get(),
       TaintTree{Taint{test::make_taint_config(
           source_kind,
           test::FrameProperties{
               .callee_port = context.access_path_factory->get(
                   AccessPath(Root(Root::Kind::Return))),
               .callee = method,
               .call_position = context.positions->unknown(),
               .distance = 1,
               .origins = OriginSet{method_origin},
               .call_kind = CallKind::callsite()})}}}};

  EXPECT_FALSE(domain1.leq(domain2));
  EXPECT_FALSE(domain2.leq(domain1));
}

TEST_F(TaintEnvironmentTest, JoinSuperSet) {
  auto context = test::make_empty_context();
  const auto* source_kind = context.kind_factory->get("TestSource");

  Scope scope;
  auto* method = context.methods->create(
      marianatrench::redex::create_void_method(scope, "LClass;", "method"));
  auto* return_port =
      context.access_path_factory->get(AccessPath(Root(Root::Kind::Return)));
  auto* method_origin =
      context.origin_factory->method_origin(method, return_port);
  auto memory_location = std::make_unique<ParameterMemoryLocation>(0);

  auto domain1 = TaintEnvironment{
      {memory_location.get(),
       TaintTree{Taint{test::make_leaf_taint_config(source_kind)}}}};
  auto domain2 = TaintEnvironment{
      {memory_location.get(),
       TaintTree{Taint{
           test::make_leaf_taint_config(source_kind),
           test::make_taint_config(
               source_kind,
               test::FrameProperties{
                   .callee_port = context.access_path_factory->get(
                       AccessPath(Root(Root::Kind::Return))),
                   .callee = method,
                   .call_position = context.positions->unknown(),
                   .distance = 1,
                   .origins = OriginSet{method_origin},
                   .call_kind = CallKind::callsite()})}}}};
  domain1.join_with(domain2);
  EXPECT_TRUE(domain1 == domain2);
}

TEST_F(TaintEnvironmentTest, JoinTwoDifferent) {
  auto context = test::make_empty_context();
  const auto* source_kind = context.kind_factory->get("TestSource");

  Scope scope;
  auto* method = context.methods->create(
      marianatrench::redex::create_void_method(scope, "LClass;", "method"));
  auto* return_port =
      context.access_path_factory->get(AccessPath(Root(Root::Kind::Return)));
  auto* method_origin =
      context.origin_factory->method_origin(method, return_port);
  auto memory_location = std::make_unique<ParameterMemoryLocation>(0);

  auto domain1 = TaintEnvironment{
      {memory_location.get(),
       TaintTree{Taint{test::make_leaf_taint_config(source_kind)}}}};

  auto domain2 = TaintEnvironment{
      {memory_location.get(),
       TaintTree{Taint{test::make_taint_config(
           source_kind,
           test::FrameProperties{
               .callee_port = context.access_path_factory->get(
                   AccessPath(Root(Root::Kind::Return))),
               .callee = method,
               .call_position = context.positions->unknown(),
               .distance = 1,
               .origins = OriginSet{method_origin},
               .call_kind = CallKind::callsite()})}}}};

  auto domain3 = TaintEnvironment{
      {memory_location.get(),
       TaintTree{Taint{
           test::make_leaf_taint_config(source_kind),
           test::make_taint_config(
               source_kind,
               test::FrameProperties{
                   .callee_port = context.access_path_factory->get(
                       AccessPath(Root(Root::Kind::Return))),
                   .callee = method,
                   .call_position = context.positions->unknown(),
                   .distance = 1,
                   .origins = OriginSet{method_origin},
                   .call_kind = CallKind::callsite()})}}}};
  domain1.join_with(domain2);
  EXPECT_TRUE(domain1 == domain3);
}

} // namespace marianatrench
