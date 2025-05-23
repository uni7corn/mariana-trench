/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <boost/algorithm/string.hpp>

#include <mariana-trench/model-generator/ModelGenerator.h>
#include <mariana-trench/model-generator/TaintInTaintThisGenerator.h>

namespace marianatrench {

namespace {
const std::vector<std::string> k_allowlist_setter_method_prefixes{
    "<init>",
    "add",
    "update",
    "push"
    "replace",
    "set",
    "put",
    "append",
    "unmarshall",
    "write"};
} // namespace

std::vector<Model> TaintInTaintThisGenerator::visit_method(
    const Method* method) const {
  if (method->get_code() || method->is_static()) {
    return {};
  }

  if (preloaded_models_ && preloaded_models_->has_model(method)) {
    // Do not overwrite preloaded models with taint-in-taint-this.
    return {};
  }

  if (method->is_abstract() || method->is_interface()) {
    const auto& overrides = context_.overrides->get(method);
    if (!overrides.empty() &&
        overrides.size() < context_.heuristics->join_override_threshold() &&
        !context_.overrides->has_obscure_override_for(method)) {
      return {};
    }
  }

  const auto class_name = generator::get_class_name(method);
  const auto method_name = generator::get_method_name(method);
  if (boost::ends_with(class_name, "$Builder;") ||
      std::any_of(
          k_allowlist_setter_method_prefixes.begin(),
          k_allowlist_setter_method_prefixes.end(),
          [method_name](const auto& prefix) {
            return boost::starts_with(method_name, prefix);
          })) {
    auto model = Model(method, context_);
    for (ParameterPosition parameter_position = 1;
         parameter_position < method->number_of_parameters();
         parameter_position++) {
      generator::add_propagation_to_self(
          context_,
          model,
          parameter_position,
          CollapseDepth::collapse(),
          {"via-obscure-taint-in-taint-this"});
    }
    return {model};
  }

  return {};
}

} // namespace marianatrench
