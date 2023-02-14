/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fmt/format.h>

#include <Show.h>
#include <TypeUtil.h>

#include <mariana-trench/Assert.h>
#include <mariana-trench/CallEffects.h>
#include <mariana-trench/ClassHierarchies.h>
#include <mariana-trench/ClassProperties.h>
#include <mariana-trench/Compiler.h>
#include <mariana-trench/Constants.h>
#include <mariana-trench/EventLogger.h>
#include <mariana-trench/Features.h>
#include <mariana-trench/FieldCache.h>
#include <mariana-trench/Heuristics.h>
#include <mariana-trench/JsonValidation.h>
#include <mariana-trench/Log.h>
#include <mariana-trench/Model.h>
#include <mariana-trench/Overrides.h>
#include <mariana-trench/Positions.h>

namespace marianatrench {

namespace {

class ModelConsistencyError {
 public:
  static void raise(const std::string& what) {
    ERROR(1, "Model Consistency Error: {}", what);
    EventLogger::log_event("model_consistency_error", what);
  }
};

template <typename DomainType>
bool leq_frozen(
    const DomainType& left,
    const DomainType& right,
    bool left_is_frozen,
    bool right_is_frozen) {
  if (left_is_frozen == right_is_frozen) {
    return left.leq(right);
  }

  return right_is_frozen;
}

template <typename DomainType>
void join_with_frozen(
    DomainType& left,
    const DomainType& right,
    bool left_is_frozen,
    bool right_is_frozen) {
  if (left_is_frozen == right_is_frozen) {
    left.join_with(right);
  } else if (right_is_frozen) {
    left = right;
  }
}

} // namespace

std::string model_mode_to_string(Model::Mode mode) {
  switch (mode) {
    case Model::Mode::Normal:
      return "normal";
    case Model::Mode::OverrideDefault:
      return "override-default";
    case Model::Mode::SkipAnalysis:
      return "skip-analysis";
    case Model::Mode::AddViaObscureFeature:
      return "add-via-obscure-feature";
    case Model::Mode::TaintInTaintOut:
      return "taint-in-taint-out";
    case Model::Mode::TaintInTaintThis:
      return "taint-in-taint-this";
    case Model::Mode::NoJoinVirtualOverrides:
      return "no-join-virtual-overrides";
    case Model::Mode::NoCollapseOnPropagation:
      return "no-collapse-on-propagation";
    case Model::Mode::AliasMemoryLocationOnInvoke:
      return "alias-memory-location-on-invoke";
    case Model::Mode::StrongWriteOnPropagation:
      return "strong-write-on-propagation";
    default:
      mt_unreachable();
  }
}

std::optional<Model::Mode> string_to_model_mode(const std::string& mode) {
  if (mode == "normal") {
    return Model::Mode::Normal;
  } else if (mode == "override-default") {
    return Model::Mode::OverrideDefault;
  } else if (mode == "skip-analysis") {
    return Model::Mode::SkipAnalysis;
  } else if (mode == "add-via-obscure-feature") {
    return Model::Mode::AddViaObscureFeature;
  } else if (mode == "taint-in-taint-out") {
    return Model::Mode::TaintInTaintOut;
  } else if (mode == "taint-in-taint-this") {
    return Model::Mode::TaintInTaintThis;
  } else if (mode == "no-join-virtual-overrides") {
    return Model::Mode::NoJoinVirtualOverrides;
  } else if (mode == "no-collapse-on-propagation") {
    return Model::Mode::NoCollapseOnPropagation;
  } else if (mode == "alias-memory-location-on-invoke") {
    return Model::Mode::AliasMemoryLocationOnInvoke;
  } else if (mode == "strong-write-on-propagation") {
    return Model::Mode::StrongWriteOnPropagation;
  } else {
    return std::nullopt;
  }
}

std::string model_freeze_kind_to_string(Model::FreezeKind freeze_kind) {
  switch (freeze_kind) {
    case Model::FreezeKind::None:
      return "none";
    case Model::FreezeKind::Generations:
      return "generations";
    case Model::FreezeKind::ParameterSources:
      return "parameter_sources";
    case Model::FreezeKind::Sinks:
      return "sinks";
    case Model::FreezeKind::Propagations:
      return "propagation";
  }
}

std::optional<Model::FreezeKind> string_to_freeze_kind(
    const std::string& freeze_kind) {
  if (freeze_kind == "none") {
    return Model::FreezeKind::None;
  } else if (freeze_kind == "generations") {
    return Model::FreezeKind::Generations;
  } else if (freeze_kind == "parameter_sources") {
    return Model::FreezeKind::ParameterSources;
  } else if (freeze_kind == "sinks") {
    return Model::FreezeKind::Sinks;
  } else if (freeze_kind == "propagation") {
    return Model::FreezeKind::Propagations;
  } else {
    return std::nullopt;
  }
}

Model::Model() : method_(nullptr) {}

Model::Model(
    const Method* method,
    Context& context,
    Modes modes,
    Frozen frozen,
    const std::vector<std::pair<AccessPath, TaintConfig>>& generations,
    const std::vector<std::pair<AccessPath, TaintConfig>>& parameter_sources,
    const std::vector<std::pair<AccessPath, TaintConfig>>& sinks,
    const std::vector<PropagationConfig>& propagations,
    const std::vector<Sanitizer>& global_sanitizers,
    const std::vector<std::pair<Root, SanitizerSet>>& port_sanitizers,
    const std::vector<std::pair<Root, FeatureSet>>& attach_to_sources,
    const std::vector<std::pair<Root, FeatureSet>>& attach_to_sinks,
    const std::vector<std::pair<Root, FeatureSet>>& attach_to_propagations,
    const std::vector<std::pair<Root, FeatureSet>>& add_features_to_arguments,
    const AccessPathConstantDomain& inline_as,
    const IssueSet& issues)
    : method_(method), modes_(modes), frozen_(frozen) {
  if (method_ && !modes_.test(Model::Mode::OverrideDefault)) {
    // Use a set of heuristics to infer the modes of this method.

    auto* code = method_->get_code();
    if (!code) {
      modes_ |= Model::Mode::SkipAnalysis;
      modes_ |= Model::Mode::AddViaObscureFeature;
    }
  }

  if (modes_.test(Model::Mode::TaintInTaintOut)) {
    add_taint_in_taint_out(context);
  }
  if (modes_.test(Model::Mode::TaintInTaintThis)) {
    add_taint_in_taint_this(context);
  }

  for (const auto& [port, source] : generations) {
    add_generation(port, source);
  }

  for (const auto& [port, source] : parameter_sources) {
    add_parameter_source(port, source);
  }

  for (const auto& [port, sink] : sinks) {
    add_sink(port, sink);
  }

  for (const auto& propagation : propagations) {
    add_propagation(propagation);
  }

  for (const auto& sanitizer : global_sanitizers) {
    add_global_sanitizer(sanitizer);
  }

  for (const auto& [root, sanitizers] : port_sanitizers) {
    add_port_sanitizers(sanitizers, root);
  }

  for (const auto& [root, features] : attach_to_sources) {
    add_attach_to_sources(root, features);
  }

  for (const auto& [root, features] : attach_to_sinks) {
    add_attach_to_sinks(root, features);
  }

  for (const auto& [root, features] : attach_to_propagations) {
    add_attach_to_propagations(root, features);
  }

  for (const auto& [root, features] : add_features_to_arguments) {
    add_add_features_to_arguments(root, features);
  }

  set_inline_as(inline_as);

  for (const auto& issue : issues) {
    add_issue(issue);
  }
}

bool Model::operator==(const Model& other) const {
  return modes_ == other.modes_ && frozen_ == other.frozen_ &&
      generations_ == other.generations_ &&
      parameter_sources_ == other.parameter_sources_ &&
      call_effect_sources_ == other.call_effect_sources_ &&
      call_effect_sinks_ == other.call_effect_sinks_ &&
      sinks_ == other.sinks_ && propagations_ == other.propagations_ &&
      global_sanitizers_ == other.global_sanitizers_ &&
      port_sanitizers_ == other.port_sanitizers_ &&
      attach_to_sources_ == other.attach_to_sources_ &&
      attach_to_sinks_ == other.attach_to_sinks_ &&
      attach_to_propagations_ == other.attach_to_propagations_ &&
      add_features_to_arguments_ == other.add_features_to_arguments_ &&
      inline_as_ == other.inline_as_ && issues_ == other.issues_;
}

bool Model::operator!=(const Model& other) const {
  return !(*this == other);
}

Model Model::instantiate(const Method* method, Context& context) const {
  Model model(method, context, modes_, frozen_);

  for (const auto& [port, generation_taint] : generations_.elements()) {
    model.add_generation(port, generation_taint);
  }

  for (const auto& [port, parameter_source_taint] :
       parameter_sources_.elements()) {
    model.add_parameter_source(port, parameter_source_taint);
  }

  for (const auto& [port, sink_taint] : sinks_.elements()) {
    model.add_sink(port, sink_taint);
  }

  for (const auto& [effect, source_taint] : call_effect_sources_) {
    model.add_call_effect_source(effect, source_taint);
  }

  for (const auto& [effect, sink_taint] : call_effect_sinks_) {
    model.add_call_effect_sink(effect, sink_taint);
  }

  for (const auto& [input_path, output] : propagations_.elements()) {
    model.add_propagation(input_path, output);
  }

  for (const auto& sanitizer : global_sanitizers_) {
    model.add_global_sanitizer(sanitizer);
  }

  for (const auto& [root, sanitizers] : port_sanitizers_) {
    model.add_port_sanitizers(sanitizers, root);
  }

  for (const auto& [root, features] : attach_to_sources_) {
    model.add_attach_to_sources(root, features);
  }

  for (const auto& [root, features] : attach_to_sinks_) {
    model.add_attach_to_sinks(root, features);
  }

  for (const auto& [root, features] : attach_to_propagations_) {
    model.add_attach_to_propagations(root, features);
  }

  for (const auto& [root, features] : add_features_to_arguments_) {
    model.add_add_features_to_arguments(root, features);
  }

  model.set_inline_as(inline_as_);

  return model;
}

Model Model::at_callsite(
    const Method* caller,
    const Position* call_position,
    Context& context,
    const std::vector<const DexType * MT_NULLABLE>& source_register_types,
    const std::vector<std::optional<std::string>>& source_constant_arguments)
    const {
  const auto* callee = method_;

  Model model;
  model.modes_ = modes_;
  model.frozen_ = frozen_;

  auto maximum_source_sink_distance =
      context.options->maximum_source_sink_distance();

  // Add special features that cannot be done in model generators.
  mt_assert(context.features != nullptr);
  auto extra_features = context.class_properties->propagate_features(
      caller, callee, *context.features);

  generations_.visit(
      [&model,
       callee,
       call_position,
       maximum_source_sink_distance,
       &extra_features,
       &context,
       &source_register_types,
       &source_constant_arguments](
          const AccessPath& callee_port, const Taint& generations) {
        model.generations_.write(
            callee_port,
            generations.propagate(
                callee,
                callee_port,
                call_position,
                maximum_source_sink_distance,
                extra_features,
                context,
                source_register_types,
                source_constant_arguments),
            UpdateKind::Weak);
      });

  sinks_.visit([&model,
                callee,
                call_position,
                maximum_source_sink_distance,
                &extra_features,
                &context,
                &source_register_types,
                &source_constant_arguments](
                   const AccessPath& callee_port, const Taint& sinks) {
    model.sinks_.write(
        callee_port,
        sinks.propagate(
            callee,
            callee_port,
            call_position,
            maximum_source_sink_distance,
            extra_features,
            context,
            source_register_types,
            source_constant_arguments),
        UpdateKind::Weak);
  });

  call_effect_sinks_.visit([&](const CallEffect& effect, const Taint& sinks) {
    switch (effect.kind()) {
      case CallEffect::Kind::CALL_CHAIN: {
        model.call_effect_sinks_.write(
            effect,
            sinks.propagate(
                callee,
                effect.access_path(),
                call_position,
                Heuristics::kMaxCallChainSourceSinkDistance,
                /* extra features */ {},
                context,
                /* source register types */ {},
                /* source constant arguments */ {}));
      } break;

      default:
        mt_unreachable();
    }
  });

  model.propagations_ = propagations_;
  model.add_features_to_arguments_ = add_features_to_arguments_;

  model.inline_as_ = inline_as_;
  if (inline_as_.is_bottom()) {
    // This is bottom when the method was never analyzed.
    // Set it to top to be sound when joining models.
    model.inline_as_.set_to_top();
  }

  return model;
}

void Model::collapse_invalid_paths(Context& context) {
  if (!method_) {
    return;
  }

  using FieldTypesAccumulator = std::unordered_set<const DexType*>;

  auto is_valid = [this, &context](
                      const FieldTypesAccumulator& previous_field_types,
                      Path::Element path_element) {
    FieldTypesAccumulator current_field_types;

    if (path_element.is_field()) {
      for (const auto* previous_field_type : previous_field_types) {
        if (previous_field_type == type::java_lang_Object()) {
          // Object is too generic to determine the set of possible field names.
          continue;
        }

        const auto& cached_types = context.field_cache->field_types(
            previous_field_type, path_element.name());
        current_field_types.insert(cached_types.begin(), cached_types.end());
      }

      if (current_field_types.empty()) {
        LOG(5,
            "Model for method `{}` has invalid path element `{}`",
            show(method_),
            show(path_element));
        return std::make_pair(false, current_field_types);
      }
    }

    return std::make_pair(true, current_field_types);
  };

  auto initial_accumulator = [this](const Root& root) {
    // Leaf ports appear in callee ports. This only applies to caller ports.
    mt_assert(!root.is_leaf_port());

    DexType* MT_NULLABLE root_type = nullptr;
    if (root.is_argument()) {
      root_type = method_->parameter_type(root.parameter_position());
    } else if (root.is_return()) {
      root_type = method_->return_type();
    }

    if (root_type == nullptr) {
      // This can happen when there is an invalid model, e.g. model defined
      // on an argument that does not exist, usually from a model-generator.
      // Returning an empty list of types will collapse all paths.
      ERROR(
          1,
          "Could not find root type for method `{}`, root: `{}`",
          show(method_),
          show(root));
      return FieldTypesAccumulator{};
    }

    return FieldTypesAccumulator{root_type};
  };

  generations_.collapse_invalid_paths<FieldTypesAccumulator>(
      is_valid, initial_accumulator);
  parameter_sources_.collapse_invalid_paths<FieldTypesAccumulator>(
      is_valid, initial_accumulator);
  sinks_.collapse_invalid_paths<FieldTypesAccumulator>(
      is_valid, initial_accumulator);
  propagations_.collapse_invalid_paths<FieldTypesAccumulator>(
      is_valid, initial_accumulator);
}

void Model::approximate(const FeatureMayAlwaysSet& widening_features) {
  const auto transform = [&widening_features](Taint& taint) {
    taint.add_inferred_features(widening_features);
    return taint;
  };
  generations_.limit_leaves(Heuristics::kModelTreeMaxLeaves, transform);
  parameter_sources_.limit_leaves(Heuristics::kModelTreeMaxLeaves, transform);
  sinks_.limit_leaves(Heuristics::kModelTreeMaxLeaves, transform);
  propagations_.limit_leaves(Heuristics::kModelTreeMaxLeaves);
}

bool Model::empty() const {
  return modes_.empty() && frozen_.empty() && generations_.is_bottom() &&
      parameter_sources_.is_bottom() && sinks_.is_bottom() &&
      call_effect_sources_.is_bottom() && call_effect_sinks_.is_bottom() &&
      propagations_.is_bottom() && global_sanitizers_.is_bottom() &&
      port_sanitizers_.is_bottom() && attach_to_sources_.is_bottom() &&
      attach_to_sinks_.is_bottom() && attach_to_propagations_.is_bottom() &&
      add_features_to_arguments_.is_bottom() && inline_as_.is_bottom() &&
      issues_.is_bottom();
}

void Model::add_mode(Model::Mode mode, Context& context) {
  mt_assert(mode != Model::Mode::OverrideDefault);

  modes_ |= mode;

  if (mode == Model::Mode::TaintInTaintOut ||
      (mode == Model::Mode::AddViaObscureFeature &&
       modes_.test(Model::Mode::TaintInTaintOut))) {
    add_taint_in_taint_out(context);
  }
  if (mode == Model::Mode::TaintInTaintThis ||
      (mode == Model::Mode::AddViaObscureFeature &&
       modes_.test(Model::Mode::TaintInTaintThis))) {
    add_taint_in_taint_this(context);
  }
}

void Model::add_taint_in_taint_out(Context& context) {
  modes_ |= Model::Mode::TaintInTaintOut;

  if (!method_ || method_->returns_void()) {
    return;
  }

  auto user_features = FeatureSet::bottom();
  if (modes_.test(Model::Mode::AddViaObscureFeature)) {
    user_features.add(context.features->get("via-obscure"));
    user_features.add(context.features->get("via-obscure-taint-in-taint-out"));
  }

  for (ParameterPosition parameter_position = 0;
       parameter_position < method_->number_of_parameters();
       parameter_position++) {
    add_propagation(PropagationConfig(
        /* input_path */ AccessPath(
            Root(Root::Kind::Argument, parameter_position)),
        /* kind */ context.kinds->local_return(),
        /* output_paths */
        PathTreeDomain{{Path{}, SingletonAbstractDomain()}},
        /* inferred_features */ FeatureMayAlwaysSet::bottom(),
        user_features));
  }
}

void Model::add_taint_in_taint_this(Context& context) {
  modes_ |= Model::Mode::TaintInTaintThis;

  if (!method_ || method_->is_static()) {
    return;
  }

  auto user_features = FeatureSet::bottom();
  if (modes_.test(Model::Mode::AddViaObscureFeature)) {
    user_features.add(context.features->get("via-obscure"));
  }

  for (ParameterPosition parameter_position = 1;
       parameter_position < method_->number_of_parameters();
       parameter_position++) {
    add_propagation(PropagationConfig(
        /* input_path */ AccessPath(
            Root(Root::Kind::Argument, parameter_position)),
        /* kind */ context.kinds->local_receiver(),
        /* output_paths */
        PathTreeDomain{{Path{}, SingletonAbstractDomain()}},
        /* inferred_features */ FeatureMayAlwaysSet::bottom(),
        user_features));
  }
}

void Model::add_generation(const AccessPath& port, TaintConfig source) {
  if (!check_taint_config_consistency(source, "source")) {
    return;
  }
  add_generation(port, Taint{std::move(source)});
}

void Model::add_inferred_generations(
    AccessPath port,
    Taint generations,
    const FeatureMayAlwaysSet& widening_features) {
  auto sanitized_generations = apply_source_sink_sanitizers(
      SanitizerKind::Sources, generations, port.root());
  if (!sanitized_generations.is_bottom()) {
    update_taint_tree(
        generations_,
        port,
        Heuristics::kGenerationMaxPortSize,
        sanitized_generations,
        widening_features);
  }
}

void Model::add_parameter_source(const AccessPath& port, TaintConfig source) {
  if (!check_taint_config_consistency(source, "source")) {
    return;
  }

  add_parameter_source(port, Taint{std::move(source)});
}

void Model::add_sink(const AccessPath& port, TaintConfig sink) {
  if (!check_taint_config_consistency(sink, "sink")) {
    return;
  }

  add_sink(port, Taint{std::move(sink)});
}

void Model::add_inferred_sinks(
    AccessPath port,
    Taint sinks,
    const FeatureMayAlwaysSet& widening_features) {
  auto sanitized_sinks =
      apply_source_sink_sanitizers(SanitizerKind::Sinks, sinks, port.root());
  if (!sanitized_sinks.is_bottom()) {
    update_taint_tree(
        sinks_,
        port,
        Heuristics::kSinkMaxPortSize,
        sanitized_sinks,
        widening_features);
  }
}

void Model::add_call_effect_source(CallEffect effect, TaintConfig source) {
  if (!check_taint_config_consistency(source, "effect source")) {
    return;
  }

  add_call_effect_source(effect, Taint{std::move(source)});
}

void Model::add_call_effect_sink(CallEffect effect, TaintConfig sink) {
  if (!check_taint_config_consistency(sink, "effect sink")) {
    return;
  }

  add_call_effect_sink(effect, Taint{std::move(sink)});
}

void Model::add_inferred_call_effect_sinks(CallEffect effect, Taint sinks) {
  add_call_effect_sink(effect, sinks);
}

void Model::add_propagation(PropagationConfig propagation) {
  if (!check_port_consistency(propagation.input_path()) ||
      !check_root_consistency(propagation.kind()->root())) {
    return;
  }

  add_propagation(propagation.input_path(), Taint::propagation(propagation));
}

void Model::add_inferred_propagation(
    PropagationConfig propagation,
    const FeatureMayAlwaysSet& widening_features) {
  if (has_global_propagation_sanitizer() ||
      !port_sanitizers_.get(propagation.input_path().root()).is_bottom()) {
    return;
  }

  update_taint_tree(
      propagations_,
      propagation.input_path(),
      Heuristics::kPropagationMaxInputPathSize,
      Taint::propagation(propagation),
      widening_features);
}

void Model::add_global_sanitizer(Sanitizer sanitizer) {
  global_sanitizers_.add(sanitizer);
}

void Model::add_port_sanitizers(SanitizerSet sanitizers, Root root) {
  if (!check_root_consistency(root)) {
    return;
  }

  port_sanitizers_.update(
      root, [&](const SanitizerSet& set) { return set.join(sanitizers); });
}

Taint Model::apply_source_sink_sanitizers(
    SanitizerKind kind,
    Taint taint,
    Root root) {
  mt_assert(kind != SanitizerKind::Propagations);
  for (const auto& sanitizer_set :
       {global_sanitizers_, port_sanitizers_.get(root)}) {
    for (const auto& sanitizer : sanitizer_set) {
      if (sanitizer.sanitizer_kind() == kind) {
        if (sanitizer.kinds().is_top()) {
          return Taint::bottom();
        }
        taint.transform_kind_with_features(
            [&sanitizer](const Kind* kind) -> std::vector<const Kind*> {
              if (sanitizer.kinds().contains(kind)) {
                return {};
              }
              return {kind};
            },
            /* add_features, never called */ nullptr);
      }
    }
  }
  return taint;
}

bool Model::has_global_propagation_sanitizer() {
  return global_sanitizers_.contains(
      Sanitizer(SanitizerKind::Propagations, KindSetAbstractDomain::top()));
}

void Model::add_attach_to_sources(Root root, FeatureSet features) {
  if (!check_root_consistency(root)) {
    return;
  }

  attach_to_sources_.update(
      root, [&](const FeatureSet& set) { return set.join(features); });
}

FeatureSet Model::attach_to_sources(Root root) const {
  return attach_to_sources_.get(root);
}

void Model::add_attach_to_sinks(Root root, FeatureSet features) {
  if (!check_root_consistency(root)) {
    return;
  }

  attach_to_sinks_.update(
      root, [&](const FeatureSet& set) { return set.join(features); });
}

FeatureSet Model::attach_to_sinks(Root root) const {
  return attach_to_sinks_.get(root);
}

void Model::add_attach_to_propagations(Root root, FeatureSet features) {
  if (!check_root_consistency(root)) {
    return;
  }

  attach_to_propagations_.update(
      root, [&](const FeatureSet& set) { return set.join(features); });
}

FeatureSet Model::attach_to_propagations(Root root) const {
  return attach_to_propagations_.get(root);
}

void Model::add_add_features_to_arguments(Root root, FeatureSet features) {
  if (!check_root_consistency(root)) {
    return;
  }

  add_features_to_arguments_.update(
      root, [&](const FeatureSet& set) { return set.join(features); });
}

bool Model::has_add_features_to_arguments() const {
  return !add_features_to_arguments_.is_bottom();
}

FeatureSet Model::add_features_to_arguments(Root root) const {
  return add_features_to_arguments_.get(root);
}

const AccessPathConstantDomain& Model::inline_as() const {
  return inline_as_;
}

void Model::set_inline_as(AccessPathConstantDomain inline_as) {
  if (!check_inline_as_consistency(inline_as)) {
    return;
  }

  inline_as_ = std::move(inline_as);
}

void Model::add_issue(Issue trace) {
  issues_.add(std::move(trace));
}

bool Model::override_default() const {
  return modes_.test(Model::Mode::OverrideDefault);
}

bool Model::skip_analysis() const {
  return modes_.test(Model::Mode::SkipAnalysis);
}

bool Model::add_via_obscure_feature() const {
  return modes_.test(Model::Mode::AddViaObscureFeature);
}

bool Model::is_taint_in_taint_out() const {
  return modes_.test(Model::Mode::TaintInTaintOut);
}

bool Model::is_taint_in_taint_this() const {
  return modes_.test(Model::Mode::TaintInTaintThis);
}

bool Model::no_join_virtual_overrides() const {
  return modes_.test(Model::Mode::NoJoinVirtualOverrides);
}

bool Model::no_collapse_on_propagation() const {
  return modes_.test(Model::Mode::NoCollapseOnPropagation);
}

bool Model::alias_memory_location_on_invoke() const {
  return modes_.test(Model::Mode::AliasMemoryLocationOnInvoke);
}

bool Model::strong_write_on_propagation() const {
  return modes_.test(Model::Mode::StrongWriteOnPropagation);
}

bool Model::is_frozen(Model::FreezeKind freeze_kind) const {
  return frozen_.test(freeze_kind);
}

bool Model::leq(const Model& other) const {
  return modes_.is_subset_of(other.modes_) &&
      frozen_.is_subset_of(other.frozen_) &&
      leq_frozen(
             generations_,
             other.generations_,
             is_frozen(Model::FreezeKind::Generations),
             other.is_frozen(Model::FreezeKind::Generations)) &&
      leq_frozen(
             parameter_sources_,
             other.parameter_sources_,
             is_frozen(Model::FreezeKind::ParameterSources),
             other.is_frozen(Model::FreezeKind::ParameterSources)) &&
      leq_frozen(
             sinks_,
             other.sinks_,
             is_frozen(Model::FreezeKind::Sinks),
             other.is_frozen(Model::FreezeKind::Sinks)) &&
      leq_frozen(
             propagations_,
             other.propagations_,
             is_frozen(Model::FreezeKind::Propagations),
             other.is_frozen(Model::FreezeKind::Propagations)) &&
      call_effect_sources_.leq(other.call_effect_sources_) &&
      call_effect_sinks_.leq(other.call_effect_sinks_) &&
      global_sanitizers_.leq(other.global_sanitizers_) &&
      port_sanitizers_.leq(other.port_sanitizers_) &&
      attach_to_sources_.leq(other.attach_to_sources_) &&
      attach_to_sinks_.leq(other.attach_to_sinks_) &&
      attach_to_propagations_.leq(other.attach_to_propagations_) &&
      add_features_to_arguments_.leq(other.add_features_to_arguments_) &&
      inline_as_.leq(other.inline_as_) && issues_.leq(other.issues_);
}

void Model::join_with(const Model& other) {
  if (this == &other) {
    return;
  }

  mt_if_expensive_assert(auto previous = *this);

  join_with_frozen(
      generations_,
      other.generations_,
      is_frozen(Model::FreezeKind::Generations),
      other.is_frozen(Model::FreezeKind::Generations));
  join_with_frozen(
      parameter_sources_,
      other.parameter_sources_,
      is_frozen(Model::FreezeKind::ParameterSources),
      other.is_frozen(Model::FreezeKind::ParameterSources));
  join_with_frozen(
      sinks_,
      other.sinks_,
      is_frozen(Model::FreezeKind::Sinks),
      other.is_frozen(Model::FreezeKind::Sinks));
  join_with_frozen(
      propagations_,
      other.propagations_,
      is_frozen(Model::FreezeKind::Propagations),
      other.is_frozen(Model::FreezeKind::Propagations));

  modes_ |= other.modes_;
  frozen_ |= other.frozen_;
  call_effect_sources_.join_with(other.call_effect_sources_);
  call_effect_sinks_.join_with(other.call_effect_sinks_);
  global_sanitizers_.join_with(other.global_sanitizers_);
  port_sanitizers_.join_with(other.port_sanitizers_);
  attach_to_sources_.join_with(other.attach_to_sources_);
  attach_to_sinks_.join_with(other.attach_to_sinks_);
  attach_to_propagations_.join_with(other.attach_to_propagations_);
  add_features_to_arguments_.join_with(other.add_features_to_arguments_);
  inline_as_.join_with(other.inline_as_);
  issues_.join_with(other.issues_);

  mt_expensive_assert(previous.leq(*this) && other.leq(*this));
}

Model Model::from_json(
    const Method* method,
    const Json::Value& value,
    Context& context) {
  JsonValidation::validate_object(value);

  Modes modes;
  for (auto mode_value :
       JsonValidation::null_or_array(value, /* field */ "modes")) {
    auto mode = string_to_model_mode(JsonValidation::string(mode_value));
    if (!mode) {
      throw JsonValidationError(value, /* field */ "modes", "valid mode");
    }
    modes.set(*mode, true);
  }

  Frozen frozen;
  for (auto freeze :
       JsonValidation::null_or_array(value, /* field */ "freeze")) {
    auto freeze_kind = string_to_freeze_kind(JsonValidation::string(freeze));
    if (!freeze_kind) {
      throw JsonValidationError(
          value, /* field */ "freeze", "valid freeze kind");
    }
    frozen.set(*freeze_kind, true);
  }

  Model model(method, context, modes, frozen);

  for (auto generation_value :
       JsonValidation::null_or_array(value, /* field */ "generations")) {
    auto port = AccessPath(Root(Root::Kind::Return));
    if (generation_value.isMember("port")) {
      JsonValidation::string(generation_value, /* field */ "port");
      port = AccessPath::from_json(generation_value["port"]);
    } else if (generation_value.isMember("caller_port")) {
      JsonValidation::string(generation_value, /* field */ "caller_port");
      port = AccessPath::from_json(generation_value["caller_port"]);
    }
    model.add_generation(
        port, TaintConfig::from_json(generation_value, context));
  }

  for (auto parameter_source_value :
       JsonValidation::null_or_array(value, /* field */ "parameter_sources")) {
    std::string port_field =
        parameter_source_value.isMember("port") ? "port" : "caller_port";
    JsonValidation::string(parameter_source_value, /* field */ port_field);
    auto port = AccessPath::from_json(parameter_source_value[port_field]);
    model.add_parameter_source(
        port, TaintConfig::from_json(parameter_source_value, context));
  }

  for (auto source_value :
       JsonValidation::null_or_array(value, /* field */ "sources")) {
    auto port = AccessPath(Root(Root::Kind::Return));
    if (source_value.isMember("port")) {
      JsonValidation::string(source_value, /* field */ "port");
      port = AccessPath::from_json(source_value["port"]);
    } else if (source_value.isMember("caller_port")) {
      JsonValidation::string(source_value, /* field */ "caller_port");
      port = AccessPath::from_json(source_value["caller_port"]);
    }
    auto source = TaintConfig::from_json(source_value, context);
    if (port.root().is_argument()) {
      model.add_parameter_source(port, source);
    } else {
      model.add_generation(port, source);
    }
  }

  for (auto sink_value :
       JsonValidation::null_or_array(value, /* field */ "sinks")) {
    std::string port_field =
        sink_value.isMember("port") ? "port" : "caller_port";
    JsonValidation::string(sink_value, /* field */ port_field);
    auto port = AccessPath::from_json(sink_value[port_field]);
    model.add_sink(port, TaintConfig::from_json(sink_value, context));
  }

  for (auto effect_source_value :
       JsonValidation::null_or_array(value, /* field */ "effect_sources")) {
    std::string effect_type_port =
        effect_source_value.isMember("port") ? "port" : "type";
    JsonValidation::string(effect_source_value, /* field */ effect_type_port);
    auto effect = CallEffect::from_json(effect_source_value[effect_type_port]);
    model.add_call_effect_source(
        effect, TaintConfig::from_json(effect_source_value, context));
  }

  for (auto effect_sink_value :
       JsonValidation::null_or_array(value, /* field */ "effect_sinks")) {
    std::string effect_type_port =
        effect_sink_value.isMember("port") ? "port" : "type";
    JsonValidation::string(effect_sink_value, /* field */ effect_type_port);
    auto effect = CallEffect::from_json(effect_sink_value[effect_type_port]);
    model.add_call_effect_sink(
        effect, TaintConfig::from_json(effect_sink_value, context));
  }

  for (auto propagation_value :
       JsonValidation::null_or_array(value, /* field */ "propagation")) {
    model.add_propagation(
        PropagationConfig::from_json(propagation_value, context));
  }

  for (auto sanitizer_value :
       JsonValidation::null_or_array(value, /* field */ "sanitizers")) {
    auto sanitizer = Sanitizer::from_json(sanitizer_value, context);
    if (!sanitizer_value.isMember("port")) {
      model.add_global_sanitizer(sanitizer);
    } else {
      auto root = Root::from_json(sanitizer_value["port"]);
      model.add_port_sanitizers(SanitizerSet(sanitizer), root);
    }
  }

  for (auto attach_to_sources_value :
       JsonValidation::null_or_array(value, /* field */ "attach_to_sources")) {
    JsonValidation::string(attach_to_sources_value, /* field */ "port");
    auto root = Root::from_json(attach_to_sources_value["port"]);
    JsonValidation::null_or_array(
        attach_to_sources_value, /* field */ "features");
    auto features =
        FeatureSet::from_json(attach_to_sources_value["features"], context);
    model.add_attach_to_sources(root, features);
  }

  for (auto attach_to_sinks_value :
       JsonValidation::null_or_array(value, /* field */ "attach_to_sinks")) {
    JsonValidation::string(attach_to_sinks_value, /* field */ "port");
    auto root = Root::from_json(attach_to_sinks_value["port"]);
    JsonValidation::null_or_array(
        attach_to_sinks_value, /* field */ "features");
    auto features =
        FeatureSet::from_json(attach_to_sinks_value["features"], context);
    model.add_attach_to_sinks(root, features);
  }

  for (auto attach_to_propagations_value : JsonValidation::null_or_array(
           value, /* field */ "attach_to_propagations")) {
    JsonValidation::string(attach_to_propagations_value, /* field */ "port");
    auto root = Root::from_json(attach_to_propagations_value["port"]);
    JsonValidation::null_or_array(
        attach_to_propagations_value, /* field */ "features");
    auto features = FeatureSet::from_json(
        attach_to_propagations_value["features"], context);
    model.add_attach_to_propagations(root, features);
  }

  for (auto add_features_to_arguments_value : JsonValidation::null_or_array(
           value, /* field */ "add_features_to_arguments")) {
    JsonValidation::string(add_features_to_arguments_value, /* field */ "port");
    auto root = Root::from_json(add_features_to_arguments_value["port"]);
    JsonValidation::null_or_array(
        add_features_to_arguments_value, /* field */ "features");
    auto features = FeatureSet::from_json(
        add_features_to_arguments_value["features"], context);
    model.add_add_features_to_arguments(root, features);
  }

  if (value.isMember("inline_as")) {
    JsonValidation::string(value, /* field */ "inline_as");
    model.set_inline_as(
        AccessPathConstantDomain(AccessPath::from_json(value["inline_as"])));
  }

  // We cannot parse issues for now.
  if (value.isMember("issues")) {
    throw JsonValidationError(
        value, /* field */ std::nullopt, /* expected */ "model without issues");
  }

  return model;
}

Json::Value Model::to_json() const {
  auto value = Json::Value(Json::objectValue);

  if (method_) {
    value["method"] = method_->to_json();
  }

  if (modes_) {
    auto modes = Json::Value(Json::arrayValue);
    for (auto mode : k_all_modes) {
      if (modes_.test(mode)) {
        modes.append(Json::Value(model_mode_to_string(mode)));
      }
    }
    value["modes"] = modes;
  }

  if (frozen_) {
    auto freeze = Json::Value(Json::arrayValue);
    for (auto freeze_kind : k_all_freeze_kinds) {
      if (frozen_.test(freeze_kind)) {
        freeze.append(Json::Value(model_freeze_kind_to_string(freeze_kind)));
      }
    }
    value["freeze"] = freeze;
  }

  if (!generations_.is_bottom()) {
    auto generations_value = Json::Value(Json::arrayValue);
    for (const auto& [port, generation_taint] : generations_.elements()) {
      auto generation_value = Json::Value(Json::objectValue);
      generation_value["port"] = port.to_json();
      generation_value["taint"] = generation_taint.to_json();
      generations_value.append(generation_value);
    }
    value["generations"] = generations_value;
  }

  if (!parameter_sources_.is_bottom()) {
    auto parameter_sources_value = Json::Value(Json::arrayValue);
    for (const auto& [port, parameter_source_taint] :
         parameter_sources_.elements()) {
      auto parameter_source_value = Json::Value(Json::objectValue);
      parameter_source_value["port"] = port.to_json();
      parameter_source_value["taint"] = parameter_source_taint.to_json();
      parameter_sources_value.append(parameter_source_value);
    }
    value["parameter_sources"] = parameter_sources_value;
  }

  if (!call_effect_sources_.is_bottom()) {
    auto effect_sources_value = Json::Value(Json::arrayValue);
    for (const auto& [effect, source_taint] : call_effect_sources_) {
      auto source_value = Json::Value(Json::objectValue);
      source_value["port"] = effect.to_json();
      source_value["taint"] = source_taint.to_json();
      effect_sources_value.append(source_value);
    }
    value["effect_sources"] = effect_sources_value;
  }

  if (!sinks_.is_bottom()) {
    auto sinks_value = Json::Value(Json::arrayValue);
    for (const auto& [port, sink_taint] : sinks_.elements()) {
      auto sink_value = Json::Value(Json::objectValue);
      sink_value["port"] = port.to_json();
      sink_value["taint"] = sink_taint.to_json();
      sinks_value.append(sink_value);
    }
    value["sinks"] = sinks_value;
  }

  if (!call_effect_sinks_.is_bottom()) {
    auto effect_sinks_value = Json::Value(Json::arrayValue);
    for (const auto& [effect, sink_taint] : call_effect_sinks_) {
      auto sink_value = Json::Value(Json::objectValue);
      sink_value["port"] = effect.to_json();
      sink_value["taint"] = sink_taint.to_json();
      effect_sinks_value.append(sink_value);
    }
    value["effect_sinks"] = effect_sinks_value;
  }

  if (!propagations_.is_bottom()) {
    auto propagations_value = Json::Value(Json::arrayValue);
    for (const auto& [input_path, propagation_taint] :
         propagations_.elements()) {
      auto propagation_value = Json::Value(Json::objectValue);
      propagation_value["input"] = input_path.to_json();
      propagation_value["output"] = propagation_taint.to_json();
      propagations_value.append(propagation_value);
    }
    value["propagation"] = propagations_value;
  }

  auto sanitizers_value = Json::Value(Json::arrayValue);
  for (const auto& sanitizer : global_sanitizers_) {
    if (!sanitizer.is_bottom()) {
      sanitizers_value.append(sanitizer.to_json());
    }
  }
  for (const auto& [root, sanitizers] : port_sanitizers_) {
    auto root_value = root.to_json();
    for (const auto& sanitizer : sanitizers) {
      if (!sanitizer.is_bottom()) {
        auto sanitizer_value = sanitizer.to_json();
        sanitizer_value["port"] = root_value;
        sanitizers_value.append(sanitizer_value);
      }
    }
  }
  if (!sanitizers_value.empty()) {
    value["sanitizers"] = sanitizers_value;
  }

  if (!attach_to_sources_.is_bottom()) {
    auto attach_to_sources_value = Json::Value(Json::arrayValue);
    for (const auto& [root, features] : attach_to_sources_) {
      auto attach_to_sources_root_value = Json::Value(Json::objectValue);
      attach_to_sources_root_value["port"] = root.to_json();
      attach_to_sources_root_value["features"] = features.to_json();
      attach_to_sources_value.append(attach_to_sources_root_value);
    }
    value["attach_to_sources"] = attach_to_sources_value;
  }

  if (!attach_to_sinks_.is_bottom()) {
    auto attach_to_sinks_value = Json::Value(Json::arrayValue);
    for (const auto& [root, features] : attach_to_sinks_) {
      auto attach_to_sinks_root_value = Json::Value(Json::objectValue);
      attach_to_sinks_root_value["port"] = root.to_json();
      attach_to_sinks_root_value["features"] = features.to_json();
      attach_to_sinks_value.append(attach_to_sinks_root_value);
    }
    value["attach_to_sinks"] = attach_to_sinks_value;
  }

  if (!attach_to_propagations_.is_bottom()) {
    auto attach_to_propagations_value = Json::Value(Json::arrayValue);
    for (const auto& [root, features] : attach_to_propagations_) {
      auto attach_to_propagations_root_value = Json::Value(Json::objectValue);
      attach_to_propagations_root_value["port"] = root.to_json();
      attach_to_propagations_root_value["features"] = features.to_json();
      attach_to_propagations_value.append(attach_to_propagations_root_value);
    }
    value["attach_to_propagations"] = attach_to_propagations_value;
  }

  if (!add_features_to_arguments_.is_bottom()) {
    auto add_features_to_arguments_value = Json::Value(Json::arrayValue);
    for (const auto& [root, features] : add_features_to_arguments_) {
      auto add_features_to_arguments_root_value =
          Json::Value(Json::objectValue);
      add_features_to_arguments_root_value["port"] = root.to_json();
      add_features_to_arguments_root_value["features"] = features.to_json();
      add_features_to_arguments_value.append(
          add_features_to_arguments_root_value);
    }
    value["add_features_to_arguments"] = add_features_to_arguments_value;
  }

  if (auto access_path = inline_as_.get_constant()) {
    value["inline_as"] = access_path->to_json();
  }

  if (!issues_.is_bottom()) {
    auto issues_value = Json::Value(Json::arrayValue);
    for (const auto& issue : issues_) {
      mt_assert(!issue.is_bottom());
      issues_value.append(issue.to_json());
    }
    value["issues"] = issues_value;
  }

  return value;
}

Json::Value Model::to_json(Context& context) const {
  auto value = to_json();

  if (method_) {
    const auto* position = context.positions->get(method_);
    value["position"] = position->to_json();
  }

  return value;
}

std::ostream& operator<<(std::ostream& out, const Model& model) {
  out << "\nModel(method=`" << show(model.method_) << "`";
  if (model.modes_) {
    out << ",\n  modes={";
    for (auto mode : k_all_modes) {
      if (model.modes_.test(mode)) {
        out << " " << model_mode_to_string(mode);
      }
    }
    out << "}";
  }
  if (model.frozen_) {
    out << ",\n  freeze={";
    for (auto freeze_kind : k_all_freeze_kinds) {
      if (model.frozen_.test(freeze_kind)) {
        out << " " << model_freeze_kind_to_string(freeze_kind);
      }
    }
    out << "}";
  }
  if (!model.generations_.is_bottom()) {
    out << ",\n  generations={\n";
    for (const auto& [port, generation_taint] : model.generations_.elements()) {
      for (const auto& generation : generation_taint.frames_iterator()) {
        out << "    " << port << ": " << generation << ",\n";
      }
    }
    out << "  }";
  }
  if (!model.parameter_sources_.is_bottom()) {
    out << ",\n  parameter_sources={\n";
    for (const auto& [port, parameter_source_taint] :
         model.parameter_sources_.elements()) {
      for (const auto& parameter_source :
           parameter_source_taint.frames_iterator()) {
        out << "    " << port << ": " << parameter_source << ",\n";
      }
    }
    out << "  }";
  }
  if (!model.sinks_.is_bottom()) {
    out << ",\n  sinks={\n";
    for (const auto& [port, sink_taint] : model.sinks_.elements()) {
      for (const auto& sink : sink_taint.frames_iterator()) {
        out << "    " << port << ": " << sink << ",\n";
      }
    }
    out << "  }";
  }
  if (!model.call_effect_sources_.is_bottom()) {
    out << ", \n call effect sources=" << model.call_effect_sources_;
  }
  if (!model.call_effect_sinks_.is_bottom()) {
    out << ", \n call effect sinks=" << model.call_effect_sinks_;
  }
  if (!model.propagations_.is_bottom()) {
    out << ",\n  propagation={\n";
    for (const auto& [input_path, propagation_taint] :
         model.propagations_.elements()) {
      for (const auto& output : propagation_taint.frames_iterator()) {
        out << "    " << input_path << ": " << output << ",\n";
      }
    }
    out << "  }";
  }
  if (!model.global_sanitizers_.is_bottom()) {
    out << ",\n  global_sanitizers=" << model.global_sanitizers_;
  }
  if (!model.port_sanitizers_.is_bottom()) {
    out << ",\n  port_sanitizers={\n";
    for (const auto& [root, sanitizers] : model.port_sanitizers_) {
      out << "   " << root << " -> " << sanitizers << ",\n";
    }
    out << "  }";
  }
  if (!model.attach_to_sources_.is_bottom()) {
    out << ",\n attach_to_sources={\n";
    for (const auto& [root, features] : model.attach_to_sources_) {
      out << "    " << root << " -> " << features << ",\n";
    }
    out << "  }";
  }
  if (!model.attach_to_sinks_.is_bottom()) {
    out << ",\n attach_to_sinks={\n";
    for (const auto& [root, features] : model.attach_to_sinks_) {
      out << "    " << root << " -> " << features << ",\n";
    }
    out << "  }";
  }
  if (!model.attach_to_propagations_.is_bottom()) {
    out << ",\n attach_to_propagations={\n";
    for (const auto& [root, features] : model.attach_to_propagations_) {
      out << "    " << root << " -> " << features << ",\n";
    }
    out << "  }";
  }
  if (!model.add_features_to_arguments_.is_bottom()) {
    out << ",\n add_features_to_arguments={\n";
    for (const auto& [root, features] : model.add_features_to_arguments_) {
      out << "    " << root << " -> " << features << ",\n";
    }
    out << "  }";
  }
  if (auto access_path = model.inline_as_.get_constant()) {
    out << ",\n  inline_as=" << *access_path;
  }
  if (!model.issues_.is_bottom()) {
    out << ",\n  issues={\n";
    for (const auto& issue : model.issues_) {
      out << "    " << issue << ",\n";
    }
    out << "  }";
  }
  return out << ")";
}

void Model::remove_kinds(const std::unordered_set<const Kind*>& to_remove) {
  auto drop_special_kinds =
      [&to_remove](const Kind* kind) -> std::vector<const Kind*> {
    if (to_remove.find(kind) != to_remove.end()) {
      return std::vector<const Kind*>();
    }
    return std::vector<const Kind*>{kind};
  };
  auto map = [&drop_special_kinds](Taint& taint) -> void {
    taint.transform_kind_with_features(
        drop_special_kinds, /* add_features, never called */ nullptr);
  };

  generations_.map(map);
  parameter_sources_.map(map);
  sinks_.map(map);
  call_effect_sources_.map(map);
  call_effect_sinks_.map(map);
}

void Model::update_taint_tree(
    TaintAccessPathTree& tree,
    AccessPath port,
    std::size_t truncation_amount,
    Taint new_taint,
    const FeatureMayAlwaysSet& widening_features) {
  if (!check_port_consistency(port)) {
    return;
  }

  if (port.path().size() > truncation_amount) {
    new_taint.add_inferred_features(widening_features);
  }
  port.truncate(truncation_amount);
  tree.write(port, std::move(new_taint), UpdateKind::Weak);
}

bool Model::check_root_consistency(Root root) const {
  switch (root.kind()) {
    case Root::Kind::Return: {
      if (method_ && method_->returns_void()) {
        ModelConsistencyError::raise(fmt::format(
            "Model for method `{}` contains a `Return` port but method returns void.",
            show(method_)));
        return false;
      }
      return true;
    }
    case Root::Kind::Argument: {
      auto position = root.parameter_position();
      if (method_ && position >= method_->number_of_parameters()) {
        ModelConsistencyError::raise(fmt::format(
            "Model for method `{}` contains a port on parameter {} but the method only has {} parameters.",
            show(method_),
            position,
            method_->number_of_parameters()));
        return false;
      }
      return true;
    }
    default: {
      ModelConsistencyError::raise(fmt::format(
          "Model for method `{}` contains an invalid port: `{}`",
          show(method_),
          show(root)));
      return false;
    }
  }
}

bool Model::check_port_consistency(const AccessPath& access_path) const {
  return check_root_consistency(access_path.root());
}

bool Model::check_parameter_source_port_consistency(
    const AccessPath& access_path) const {
  if (access_path.root().is_return()) {
    ModelConsistencyError::raise(fmt::format(
        "Model for method `{}` contains a parameter source with a `Return` port."
        " Use a generation instead.",
        show(method_)));
    return false;
  }

  return true;
}

bool Model::check_taint_config_consistency(
    const TaintConfig& config,
    std::string_view kind) const {
  if (config.kind() == nullptr) {
    ModelConsistencyError::raise(fmt::format(
        "Model for method `{}` contains a unknown kind for {}.",
        show(method_),
        kind));
    return false;
  }
  if (config.is_artificial_source()) {
    ModelConsistencyError::raise(fmt::format(
        "Model for method `{}` contains an artificial {}.",
        show(method_),
        kind));
    return false;
  }
  return true;
}

bool Model::check_taint_consistency(const Taint& taint, std::string_view kind)
    const {
  for (const auto& frame : taint.frames_iterator()) {
    if (method_ && (frame.origins().empty() && frame.field_origins().empty())) {
      ModelConsistencyError::raise(fmt::format(
          "Model for method `{}` contains a {} without origins.",
          show(method_),
          kind));
      return false;
    }
    if (frame.via_type_of_ports().is_value()) {
      for (const auto& root : frame.via_type_of_ports().elements()) {
        // Logs invalid ports specifed for via_type_of but does not prevent the
        // model from being created.
        check_root_consistency(root);
      }
    }
    if (frame.via_value_of_ports().is_value()) {
      for (const auto& root : frame.via_value_of_ports().elements()) {
        // Logs invalid ports specifed for via_value_of but does not prevent the
        // model from being created.
        check_root_consistency(root);
      }
    }
  }
  return true;
}

bool Model::check_inline_as_consistency(
    const AccessPathConstantDomain& inline_as) const {
  auto access_path = inline_as.get_constant();

  if (!access_path) {
    return true;
  }

  if (!access_path->root().is_argument()) {
    ModelConsistencyError::raise(fmt::format(
        "Model for method `{}` has an inline-as with a non-argument root.",
        show(method_)));

    return false;
  }

  return check_port_consistency(*access_path);
}

void Model::add_generation(AccessPath port, Taint source) {
  if (method_) {
    source.set_leaf_origins_if_empty(MethodSet{method_});
  }

  if (!check_port_consistency(port) ||
      !check_taint_consistency(source, "source")) {
    return;
  }

  if (port.path().size() > Heuristics::kGenerationMaxPortSize) {
    WARNING(
        1,
        "Truncating user-defined generation {} down to path length {} for method {}",
        port,
        Heuristics::kGenerationMaxPortSize,
        method_ ? method_->get_name() : "nullptr");
  }
  port.truncate(Heuristics::kGenerationMaxPortSize);
  generations_.write(port, std::move(source), UpdateKind::Weak);
}

void Model::add_parameter_source(AccessPath port, Taint source) {
  if (method_) {
    source.set_leaf_origins_if_empty(MethodSet{method_});
  }

  if (!check_port_consistency(port) ||
      !check_parameter_source_port_consistency(port) ||
      !check_taint_consistency(source, "source")) {
    return;
  }

  if (port.path().size() > Heuristics::kParameterSourceMaxPortSize) {
    WARNING(
        1,
        "Truncating user-defined parameter source {} down to path length {} for method {}",
        port,
        Heuristics::kParameterSourceMaxPortSize,
        method_ ? method_->get_name() : "nullptr");
  }
  port.truncate(Heuristics::kParameterSourceMaxPortSize);
  parameter_sources_.write(port, Taint{std::move(source)}, UpdateKind::Weak);
}

void Model::add_sink(AccessPath port, Taint sink) {
  if (method_) {
    sink.set_leaf_origins_if_empty(MethodSet{method_});
  }

  if (!check_port_consistency(port) || !check_taint_consistency(sink, "sink")) {
    return;
  }

  if (port.path().size() > Heuristics::kSinkMaxPortSize) {
    WARNING(
        1,
        "Truncating user-defined sink {} down to path length {} for method {}",
        port,
        Heuristics::kSinkMaxPortSize,
        method_ ? method_->get_name() : "nullptr");
  }
  port.truncate(Heuristics::kSinkMaxPortSize);
  sinks_.write(port, Taint{std::move(sink)}, UpdateKind::Weak);
}

void Model::add_call_effect_source(CallEffect effect, Taint source) {
  if (method_) {
    source.set_leaf_origins_if_empty(MethodSet{method_});
  }

  if (!check_taint_consistency(source, "effect source")) {
    return;
  }

  call_effect_sources_.write(effect, Taint{std::move(source)});
}

void Model::add_call_effect_sink(CallEffect effect, Taint sink) {
  if (method_) {
    sink.set_leaf_origins_if_empty(MethodSet{method_});
  }

  if (!check_taint_consistency(sink, "effect sink")) {
    return;
  }

  call_effect_sinks_.write(effect, Taint{std::move(sink)});
}

void Model::add_propagation(AccessPath input_path, Taint output) {
  if (!check_root_consistency(input_path.root())) {
    return;
  }

  if (input_path.path().size() > Heuristics::kPropagationMaxInputPathSize) {
    WARNING(
        1,
        "Truncating user-defined propagation {} down to path length {} for method {}",
        input_path.path(),
        Heuristics::kPropagationMaxInputPathSize,
        method_ ? method_->get_name() : "nullptr");
  }
  input_path.truncate(Heuristics::kPropagationMaxInputPathSize);
  propagations_.write(input_path, output, UpdateKind::Weak);
}

} // namespace marianatrench
