/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <Show.h>

#include <mariana-trench/Assert.h>
#include <mariana-trench/CallPositionFrames.h>
#include <mariana-trench/Features.h>
#include <mariana-trench/JsonValidation.h>
#include <mariana-trench/Log.h>

namespace marianatrench {

namespace {

void materialize_via_type_of_ports(
    const Method* callee,
    Context& context,
    const Frame& frame,
    const std::vector<const DexType * MT_NULLABLE>& source_register_types,
    std::vector<const Feature*>& via_type_of_features_added,
    FeatureMayAlwaysSet& inferred_features) {
  if (!frame.via_type_of_ports().is_value() ||
      frame.via_type_of_ports().elements().empty()) {
    return;
  }

  // Materialize via_type_of_ports into features and add them to the inferred
  // features
  for (const auto& port : frame.via_type_of_ports().elements()) {
    if (!port.is_argument() ||
        port.parameter_position() >= source_register_types.size()) {
      ERROR(
          1,
          "Invalid port {} provided for via_type_of ports of method {}.{}",
          port,
          callee->get_class()->str(),
          callee->get_name());
      continue;
    }
    const auto* feature = context.features->get_via_type_of_feature(
        source_register_types[port.parameter_position()]);
    via_type_of_features_added.push_back(feature);
    inferred_features.add_always(feature);
  }
}

void materialize_via_value_of_ports(
    const Method* callee,
    Context& context,
    const Frame& frame,
    const std::vector<std::optional<std::string>>& source_constant_arguments,
    FeatureMayAlwaysSet& inferred_features) {
  if (!frame.via_value_of_ports().is_value() ||
      frame.via_value_of_ports().elements().empty()) {
    return;
  }

  // Materialize via_value_of_ports into features and add them to the inferred
  // features
  for (const auto& port : frame.via_value_of_ports().elements()) {
    if (!port.is_argument() ||
        port.parameter_position() >= source_constant_arguments.size()) {
      ERROR(
          1,
          "Invalid port {} provided for via_value_of ports of method {}.{}",
          port,
          callee->get_class()->str(),
          callee->get_name());
      continue;
    }
    const auto* feature = context.features->get_via_value_of_feature(
        source_constant_arguments[port.parameter_position()]);
    inferred_features.add_always(feature);
  }
}

} // namespace

CallPositionFrames::CallPositionFrames(std::initializer_list<Frame> frames)
    : position_(nullptr) {
  for (const auto& frame : frames) {
    add(frame);
  }
}

void CallPositionFrames::add(const Frame& frame) {
  if (position_ == nullptr) {
    position_ = frame.call_position();
  } else {
    mt_assert(position_ == frame.call_position());
  }

  frames_.update(frame.kind(), [&](const Frames& old_frames) {
    auto new_frames = old_frames;
    new_frames.add(frame);
    return new_frames;
  });
}

bool CallPositionFrames::leq(const CallPositionFrames& other) const {
  mt_assert(is_bottom() || other.is_bottom() || position_ == other.position());
  return frames_.leq(other.frames_);
}

bool CallPositionFrames::equals(const CallPositionFrames& other) const {
  mt_assert(is_bottom() || other.is_bottom() || position_ == other.position());
  return frames_.equals(other.frames_);
}

void CallPositionFrames::join_with(const CallPositionFrames& other) {
  mt_if_expensive_assert(auto previous = *this);

  if (is_bottom()) {
    mt_assert(position_ == nullptr);
    position_ = other.position();
  }
  mt_assert(other.is_bottom() || position_ == other.position());

  frames_.join_with(other.frames_);

  mt_expensive_assert(previous.leq(*this) && other.leq(*this));
}

void CallPositionFrames::widen_with(const CallPositionFrames& other) {
  mt_if_expensive_assert(auto previous = *this);

  if (is_bottom()) {
    mt_assert(position_ == nullptr);
    position_ = other.position();
  }
  mt_assert(other.is_bottom() || position_ == other.position());

  frames_.widen_with(other.frames_);

  mt_expensive_assert(previous.leq(*this) && other.leq(*this));
}

void CallPositionFrames::meet_with(const CallPositionFrames& other) {
  if (is_bottom()) {
    mt_assert(position_ == nullptr);
    position_ = other.position();
  }
  mt_assert(other.is_bottom() || position_ == other.position());

  frames_.meet_with(other.frames_);
}

void CallPositionFrames::narrow_with(const CallPositionFrames& other) {
  if (is_bottom()) {
    mt_assert(position_ == nullptr);
    position_ = other.position();
  }
  mt_assert(other.is_bottom() || position_ == other.position());

  frames_.narrow_with(other.frames_);
}

void CallPositionFrames::difference_with(const CallPositionFrames& other) {
  if (is_bottom()) {
    mt_assert(position_ == nullptr);
    position_ = other.position();
  }
  mt_assert(other.is_bottom() || position_ == other.position());

  frames_.difference_like_operation(
      other.frames_, [](const Frames& frames_left, const Frames& frames_right) {
        auto frames_copy = frames_left;
        frames_copy.difference_with(frames_right);
        return frames_copy;
      });
}

void CallPositionFrames::map(const std::function<void(Frame&)>& f) {
  frames_.map([&](const Frames& frames) {
    auto new_frames = frames;
    new_frames.map(f);
    return new_frames;
  });
}

void CallPositionFrames::add_inferred_features(
    const FeatureMayAlwaysSet& features) {
  if (features.empty()) {
    return;
  }

  map([&features](Frame& frame) { frame.add_inferred_features(features); });
}

LocalPositionSet CallPositionFrames::local_positions() const {
  // Ideally this can be stored within `CallPositionFrames` instead of `Frame`.
  // Local positions should be the same for a given (callee, call_position).
  auto result = LocalPositionSet::bottom();
  for (const auto& [_, frames] : frames_.bindings()) {
    for (const auto& frame : frames) {
      result.join_with(frame.local_positions());
    }
  }
  return result;
}

void CallPositionFrames::add_local_position(const Position* position) {
  map([position](Frame& frame) { frame.add_local_position(position); });
}

void CallPositionFrames::set_local_positions(
    const LocalPositionSet& positions) {
  map([&positions](Frame& frame) { frame.set_local_positions(positions); });
}

void CallPositionFrames::add_inferred_features_and_local_position(
    const FeatureMayAlwaysSet& features,
    const Position* MT_NULLABLE position) {
  if (features.empty() && position == nullptr) {
    return;
  }

  map([&features, position](Frame& frame) {
    if (!features.empty()) {
      frame.add_inferred_features(features);
    }
    if (position != nullptr) {
      frame.add_local_position(position);
    }
  });
}

CallPositionFrames CallPositionFrames::propagate(
    const Method* callee,
    const AccessPath& callee_port,
    const Position* call_position,
    int maximum_source_sink_distance,
    Context& context,
    const std::vector<const DexType * MT_NULLABLE>& source_register_types,
    const std::vector<std::optional<std::string>>& source_constant_arguments)
    const {
  if (is_bottom()) {
    return CallPositionFrames::bottom();
  }

  CallPositionFrames result;
  auto partitioned_by_kind = partition_map<const Kind*>(
      [](const Frame& frame) { return frame.kind(); });

  for (const auto& [kind, frames] : partitioned_by_kind) {
    std::vector<std::reference_wrapper<const Frame>> non_crtex_frames;
    std::vector<std::reference_wrapper<const Frame>> crtex_frames;
    for (const auto& frame : frames) {
      if (frame.get().is_crtex_producer_declaration()) {
        crtex_frames.push_back(frame);
      } else {
        non_crtex_frames.push_back(frame);
      }
    }

    result.join_with(propagate_crtex_frames(
        callee,
        callee_port,
        call_position,
        maximum_source_sink_distance,
        context,
        source_register_types,
        crtex_frames));

    // Non-CRTEX frames can be joined into the same callee
    std::vector<const Feature*> via_type_of_features_added;
    auto non_crtex_frame = propagate_frames(
        callee,
        callee_port,
        call_position,
        maximum_source_sink_distance,
        context,
        source_register_types,
        source_constant_arguments,
        non_crtex_frames,
        via_type_of_features_added);
    if (!non_crtex_frame.is_bottom()) {
      result.add(non_crtex_frame);
    }
  }

  return result;
}

CallPositionFrames CallPositionFrames::attach_position(
    const Position* position) const {
  FramesByKind result;

  // NOTE: This method does more than update the position in frames. It
  // functions similarly to `propagate`. Frame features are propagated here.
  for (const auto& [_, frames] : frames_.bindings()) {
    for (const auto& frame : frames) {
      if (!frame.is_leaf()) {
        continue;
      }

      // Canonical names should theoretically be instantiated here the way
      // they are instantiated in `propagate`, but there is currently no
      // scenario that requires this. If a templated name does get configured,
      // the name will be instantiated when this frame gets propagated.
      result.update(frame.kind(), [&](const Frames& frames) {
        auto new_frames = frames;
        new_frames.add(Frame(
            frame.kind(),
            frame.callee_port(),
            /* callee */ nullptr,
            /* field_callee */ nullptr,
            /* call_position */ position,
            /* distance */ 0,
            frame.origins(),
            frame.field_origins(),
            frame.features(),
            // Since CallPositionFrames::attach_position is used (only) for
            // parameter_sinks and return sources which may be included in an
            // issue as a leaf, we need to make sure that those leaf frames in
            // issues contain the user_features as being locally inferred.
            /* locally_inferred_features */
            frame.user_features().is_bottom()
                ? FeatureMayAlwaysSet::bottom()
                : FeatureMayAlwaysSet::make_always(frame.user_features()),
            /* user_features */ FeatureSet::bottom(),
            /* via_type_of_ports */ {},
            /* via_value_of_ports */ {},
            frame.local_positions(),
            frame.canonical_names()));
        return new_frames;
      });
    }
  }

  return CallPositionFrames(position, result);
}

CallPositionFrames CallPositionFrames::transform_kind_with_features(
    const std::function<std::vector<const Kind*>(const Kind*)>& transform_kind,
    const std::function<FeatureMayAlwaysSet(const Kind*)>& add_features) const {
  FramesByKind new_frames_by_kind;
  for (const auto& [old_kind, frames] : frames_.bindings()) {
    auto new_kinds = transform_kind(old_kind);
    if (new_kinds.empty()) {
      continue;
    } else if (new_kinds.size() == 1 && new_kinds.front() == old_kind) {
      new_frames_by_kind.set(old_kind, frames); // no transformation
    } else {
      for (const auto* new_kind : new_kinds) {
        // Even if new_kind == old_kind for some new_kind, perform map_frame_set
        // because a transformation occurred.
        Frames new_frames;
        auto features_to_add = add_features(new_kind);
        for (const auto& frame : frames) {
          auto new_frame = frame.with_kind(new_kind);
          new_frame.add_inferred_features(features_to_add);
          new_frames.add(new_frame);
        }
        new_frames_by_kind.update(new_kind, [&](const Frames& existing) {
          return existing.join(new_frames);
        });
      }
    }
  }
  return CallPositionFrames(position_, new_frames_by_kind);
}

void CallPositionFrames::append_callee_port(
    Path::Element path_element,
    const std::function<bool(const Kind*)>& filter) {
  // TODO (T91357916): GroupHashedSetAbstractDomain could be more efficient.
  // It supports in-place modifying of the elements as long as the key does
  // not change. Without that, we need to create a new `FramesByKind` object.
  FramesByKind new_frames;
  for (const auto& [kind, frames] : frames_.bindings()) {
    if (filter(kind)) {
      auto frames_copy = frames;
      frames_copy.map(
          [&](Frame& frame) { frame.callee_port_append(path_element); });
      new_frames.set(kind, frames_copy);
    } else {
      new_frames.set(kind, frames); // no changes
    }
  }

  frames_ = new_frames;
}

void CallPositionFrames::filter_invalid_frames(
    const std::function<bool(const Method*, const AccessPath&, const Kind*)>&
        is_valid) {
  FramesByKind new_frames;
  for (const auto& [kind, frames] : frames_.bindings()) {
    auto frames_copy = frames;
    frames_copy.filter([&](const Frame& frame) {
      return is_valid(frame.callee(), frame.callee_port(), frame.kind());
    });
    new_frames.set(kind, frames_copy);
  }

  frames_ = new_frames;
}

bool CallPositionFrames::contains_kind(const Kind* kind) const {
  for (const auto& [actual_kind, _] : frames_.bindings()) {
    if (actual_kind == kind) {
      return true;
    }
  }
  return false;
}

std::ostream& operator<<(std::ostream& out, const CallPositionFrames& frames) {
  if (frames.is_top()) {
    return out << "T";
  } else {
    out << "[";
    for (const auto& [kind, frames] : frames.frames_.bindings()) {
      out << "FrameByKind(kind=" << show(kind) << ", frames=" << frames << "),";
    }
    return out << "]";
  }
}

Frame CallPositionFrames::propagate_frames(
    const Method* callee,
    const AccessPath& callee_port,
    const Position* call_position,
    int maximum_source_sink_distance,
    Context& context,
    const std::vector<const DexType * MT_NULLABLE>& source_register_types,
    const std::vector<std::optional<std::string>>& source_constant_arguments,
    std::vector<std::reference_wrapper<const Frame>> frames,
    std::vector<const Feature*>& via_type_of_features_added) const {
  if (frames.size() == 0) {
    return Frame::bottom();
  }

  const auto* kind = frames.begin()->get().kind();
  int distance = std::numeric_limits<int>::max();
  auto origins = MethodSet::bottom();
  auto field_origins = FieldSet::bottom();
  auto inferred_features = FeatureMayAlwaysSet::bottom();

  for (const Frame& frame : frames) {
    // Only frames sharing the same kind can be propagated this way.
    mt_assert(frame.kind() == kind);

    if (frame.distance() >= maximum_source_sink_distance) {
      continue;
    }

    distance = std::min(distance, frame.distance() + 1);
    origins.join_with(frame.origins());
    field_origins.join_with(frame.field_origins());

    // Note: This merges user features with existing inferred features.
    inferred_features.join_with(frame.features());

    materialize_via_type_of_ports(
        callee,
        context,
        frame,
        source_register_types,
        via_type_of_features_added,
        inferred_features);

    materialize_via_value_of_ports(
        callee, context, frame, source_constant_arguments, inferred_features);
  }

  if (distance == std::numeric_limits<int>::max()) {
    return Frame::bottom();
  }

  mt_assert(distance <= maximum_source_sink_distance);
  return Frame(
      kind,
      callee_port,
      callee,
      /* field_callee */ nullptr, // Since propagate is only called at method
                                  // callsites and not field accesses
      call_position,
      distance,
      std::move(origins),
      std::move(field_origins),
      std::move(inferred_features),
      /* locally_inferred_features */ FeatureMayAlwaysSet::bottom(),
      /* user_features */ FeatureSet::bottom(),
      /* via_type_of_ports */ {},
      /* via_value_of_ports */ {},
      /* local_positions */ {},
      /* canonical_names */ {});
}

CallPositionFrames CallPositionFrames::propagate_crtex_frames(
    const Method* callee,
    const AccessPath& callee_port,
    const Position* call_position,
    int maximum_source_sink_distance,
    Context& context,
    const std::vector<const DexType * MT_NULLABLE>& source_register_types,
    std::vector<std::reference_wrapper<const Frame>> frames) const {
  if (frames.size() == 0) {
    return CallPositionFrames::bottom();
  }

  CallPositionFrames result;
  const auto* kind = frames.begin()->get().kind();

  for (const Frame& frame : frames) {
    // Only frames sharing the same kind can be propagated this way.
    mt_assert(frame.kind() == kind);

    std::vector<const Feature*> via_type_of_features_added;
    auto propagated = propagate_frames(
        callee,
        callee_port,
        call_position,
        maximum_source_sink_distance,
        context,
        source_register_types,
        {}, // TODO: Support via-value-of for crtex frames
        {std::cref(frame)},
        via_type_of_features_added);

    if (propagated.is_bottom()) {
      continue;
    }

    auto canonical_names = frame.canonical_names();
    if (!canonical_names.is_value() || canonical_names.elements().empty()) {
      WARNING(
          2,
          "Encountered crtex frame without canonical names. Frame: `{}`",
          frame);
      continue;
    }

    CanonicalNameSetAbstractDomain instantiated_names;
    for (const auto& canonical_name : canonical_names.elements()) {
      auto instantiated_name = canonical_name.instantiate(
          propagated.callee(), via_type_of_features_added);
      if (!instantiated_name) {
        continue;
      }
      instantiated_names.add(*instantiated_name);
    }

    auto canonical_callee_port =
        propagated.callee_port().canonicalize_for_method(propagated.callee());

    // All fields should be propagated like other frames, except the crtex
    // fields. Ideally, origins should contain the canonical names as well,
    // but canonical names are strings and cannot be stored in MethodSet.
    // Frame is not propagated if none of the canonical names instantiated
    // successfully.
    if (instantiated_names.is_value() &&
        !instantiated_names.elements().empty()) {
      result.add(Frame(
          kind,
          canonical_callee_port,
          propagated.callee(),
          propagated.field_callee(),
          propagated.call_position(),
          /* distance (always leaves for crtex frames) */ 0,
          propagated.origins(),
          propagated.field_origins(),
          propagated.inferred_features(),
          propagated.locally_inferred_features(),
          propagated.user_features(),
          propagated.via_type_of_ports(),
          propagated.via_value_of_ports(),
          propagated.local_positions(),
          /* canonical_names */ instantiated_names));
    }
  }

  return result;
}

} // namespace marianatrench
