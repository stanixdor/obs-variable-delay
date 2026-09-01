#include "audio-preflight.hpp"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace dynamic_delay {
namespace {

constexpr std::size_t MaxTreeDepth = 64;
constexpr std::size_t MaxTreeVisits = 16'384;
constexpr const char *InternalCaptureOutputId = "obs_dynamic_delay_capture_output";
constexpr const char *InternalAudioTapSourceId = "obs_dynamic_delay_hold_audio_tap";

class SourceRef {
public:
	explicit SourceRef(obs_source_t *source = nullptr) : source_(source ? obs_source_get_ref(source) : nullptr) {}
	~SourceRef() { obs_source_release(source_); }

	SourceRef(const SourceRef &) = delete;
	SourceRef &operator=(const SourceRef &) = delete;

	SourceRef(SourceRef &&other) noexcept : source_(std::exchange(other.source_, nullptr)) {}
	SourceRef &operator=(SourceRef &&other) noexcept
	{
		if (this != &other) {
			obs_source_release(source_);
			source_ = std::exchange(other.source_, nullptr);
		}
		return *this;
	}

	[[nodiscard]] obs_source_t *get() const noexcept { return source_; }

private:
	obs_source_t *source_ = nullptr;
};

class OutputRef {
public:
	explicit OutputRef(obs_output_t *output = nullptr) : output_(output ? obs_output_get_ref(output) : nullptr) {}
	~OutputRef() { obs_output_release(output_); }

	OutputRef(const OutputRef &) = delete;
	OutputRef &operator=(const OutputRef &) = delete;

	OutputRef(OutputRef &&other) noexcept : output_(std::exchange(other.output_, nullptr)) {}
	OutputRef &operator=(OutputRef &&other) noexcept
	{
		if (this != &other) {
			obs_output_release(output_);
			output_ = std::exchange(other.output_, nullptr);
		}
		return *this;
	}

	[[nodiscard]] obs_output_t *get() const noexcept { return output_; }

private:
	obs_output_t *output_ = nullptr;
};

struct AudioLeaf {
	std::string name;
	uint32_t mixerMask = 0;
	std::size_t occurrences = 0;
};

struct AudioTree {
	std::unordered_map<obs_source_t *, AudioLeaf> leaves;
	std::vector<std::string> traversalConflicts;
	std::size_t visits = 0;
};

[[nodiscard]] std::string source_name(obs_source_t *source)
{
	const char *name = source ? obs_source_get_name(source) : nullptr;
	return name && *name ? name : "(unnamed source)";
}

[[nodiscard]] std::string output_name(obs_output_t *output)
{
	const char *name = output ? obs_output_get_name(output) : nullptr;
	return name && *name ? name : "(unnamed output)";
}

void append_unique(std::vector<std::string> &values, std::string value)
{
	if (value.empty())
		return;
	if (std::find(values.begin(), values.end(), value) == values.end())
		values.emplace_back(std::move(value));
}

void normalize_names(std::vector<std::string> &names)
{
	std::sort(names.begin(), names.end());
	names.erase(std::unique(names.begin(), names.end()), names.end());
}

[[nodiscard]] std::string format_names(const std::vector<std::string> &names, const std::size_t limit = 4)
{
	if (names.empty())
		return {};
	std::ostringstream text;
	const std::size_t shown = std::min(names.size(), limit);
	for (std::size_t index = 0; index < shown; ++index) {
		if (index != 0)
			text << ", ";
		text << names[index];
	}
	if (shown < names.size())
		text << " and " << (names.size() - shown) << " more";
	return text.str();
}

void collect_direct_children(obs_source_t *source, std::vector<SourceRef> &children)
{
	if (!source)
		return;

	obs_scene_t *scene = obs_scene_from_source(source);
	if (!scene)
		scene = obs_group_from_source(source);
	if (scene) {
		obs_scene_enum_items(
			scene,
			[](obs_scene_t *, obs_sceneitem_t *item, void *param) {
				auto &items = *static_cast<std::vector<SourceRef> *>(param);
				if (obs_source_t *child = obs_sceneitem_get_source(item)) {
					SourceRef reference(child);
					if (reference.get())
						items.emplace_back(std::move(reference));
				}
				return true;
			},
			&children);
		return;
	}

	obs_source_enum_active_sources(
		source,
		[](obs_source_t *, obs_source_t *child, void *param) {
			auto &items = *static_cast<std::vector<SourceRef> *>(param);
			SourceRef reference(child);
			if (reference.get())
				items.emplace_back(std::move(reference));
		},
		&children);
}

bool collect_audio_tree_impl(obs_source_t *source, AudioTree &tree, std::unordered_set<obs_source_t *> &path,
			     const std::size_t depth)
{
	if (!source || obs_source_removed(source))
		return false;
	if (depth >= MaxTreeDepth || path.contains(source) || tree.visits >= MaxTreeVisits) {
		append_unique(tree.traversalConflicts, source_name(source));
		return false;
	}
	++tree.visits;

	path.insert(source);
	std::vector<SourceRef> children;
	collect_direct_children(source, children);
	bool childHasAudio = false;
	for (const SourceRef &child : children)
		childHasAudio = collect_audio_tree_impl(child.get(), tree, path, depth + 1) || childHasAudio;
	path.erase(source);

	const bool audioCapable = (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) != 0;
	if (audioCapable && !childHasAudio) {
		auto [entry, inserted] = tree.leaves.try_emplace(source);
		if (inserted) {
			entry->second.name = source_name(source);
			entry->second.mixerMask = obs_source_get_audio_mixers(source);
		}
		++entry->second.occurrences;
		return true;
	}
	return childHasAudio;
}

[[nodiscard]] AudioTree collect_audio_tree(obs_source_t *root)
{
	AudioTree tree;
	SourceRef rootRef(root);
	if (!rootRef.get())
		return tree;
	std::unordered_set<obs_source_t *> path;
	collect_audio_tree_impl(rootRef.get(), tree, path, 0);
	return tree;
}

[[nodiscard]] bool valid_dedicated_source(obs_source_t *source)
{
	SourceRef reference(source);
	return reference.get() && !obs_source_removed(reference.get()) &&
	       (obs_source_get_output_flags(reference.get()) & OBS_SOURCE_AUDIO) != 0;
}

void collect_scene_mix_conflicts(obs_source_t *holdScene, const AudioTree &holdTree,
				 std::vector<std::string> &conflicts)
{
	for (const auto &[source, leaf] : holdTree.leaves) {
		(void)source;
		if (leaf.occurrences > 1)
			append_unique(conflicts, leaf.name);
	}
	for (const std::string &name : holdTree.traversalConflicts)
		append_unique(conflicts, name);

	struct SceneContext {
		obs_source_t *hold = nullptr;
		const AudioTree *holdTree = nullptr;
		std::vector<std::string> *conflicts = nullptr;
	} context{holdScene, &holdTree, &conflicts};

	obs_enum_canvases(
		[](void *param, obs_canvas_t *canvas) {
			auto &context = *static_cast<SceneContext *>(param);
			if (!canvas || obs_canvas_removed(canvas))
				return true;
			obs_canvas_enum_scenes(
				canvas,
				[](void *sceneParam, obs_source_t *sceneSource) {
					auto &context = *static_cast<SceneContext *>(sceneParam);
					if (!sceneSource || sceneSource == context.hold ||
					    obs_source_removed(sceneSource))
						return true;
					const AudioTree otherTree = collect_audio_tree(sceneSource);
					for (const auto &[source, leaf] : otherTree.leaves) {
						if (context.holdTree->leaves.contains(source))
							append_unique(*context.conflicts, leaf.name);
					}
					return true;
				},
				&context);

			// The selected hold scene itself is skipped above.  If any
			// Program canvas currently renders it, the private view would
			// introduce a duplicate audio tree.  Inspect every canvas root,
			// not only the legacy main-canvas channels.
			for (uint32_t channel = 0; channel < MAX_CHANNELS; ++channel) {
				obs_source_t *root = obs_canvas_get_channel(canvas, channel);
				if (!root)
					continue;
				const AudioTree activeTree = collect_audio_tree(root);
				obs_source_release(root);
				for (const auto &[source, leaf] : activeTree.leaves) {
					if (context.holdTree->leaves.contains(source))
						append_unique(*context.conflicts, leaf.name);
				}
			}
			return true;
		},
		&context);
	normalize_names(conflicts);
}

[[nodiscard]] bool dedicated_source_is_exclusive(obs_source_t *holdScene, obs_source_t *dedicatedSource,
						 std::vector<std::string> &conflicts)
{
	if (!valid_dedicated_source(dedicatedSource))
		return false;
	const AudioTree dedicatedTree = collect_audio_tree(dedicatedSource);
	collect_scene_mix_conflicts(holdScene, dedicatedTree, conflicts);
	normalize_names(conflicts);
	return conflicts.empty();
}

void add_output(std::vector<OutputRef> &outputs, std::unordered_set<obs_output_t *> &identities, obs_output_t *output)
{
	if (!output || identities.contains(output))
		return;
	OutputRef reference(output);
	if (!reference.get())
		return;
	identities.insert(output);
	outputs.emplace_back(std::move(reference));
}

[[nodiscard]] bool output_uses_mixer(obs_output_t *output, const uint32_t mixerIndex)
{
	if (!output || mixerIndex >= MAX_AUDIO_MIXES)
		return false;
	const uint32_t flags = obs_output_get_flags(output);
	if ((flags & OBS_OUTPUT_AUDIO) == 0)
		return false;

	if ((flags & OBS_OUTPUT_ENCODED) != 0) {
		for (std::size_t index = 0; index < MAX_OUTPUT_AUDIO_ENCODERS; ++index) {
			obs_encoder_t *encoder = obs_output_get_audio_encoder(output, index);
			if (!encoder || obs_encoder_get_type(encoder) != OBS_ENCODER_AUDIO)
				continue;
			// A private auxiliary media context does not consume a global OBS track.
			audio_t *audio = obs_encoder_audio(encoder);
			if (audio && audio != obs_get_audio())
				continue;
			if (obs_encoder_get_mixer_index(encoder) == mixerIndex)
				return true;
		}
		return false;
	}

	if (obs_output_audio(output) && obs_output_audio(output) != obs_get_audio())
		return false;
	if ((flags & OBS_OUTPUT_MULTI_TRACK) != 0)
		return (obs_output_get_mixers(output) & (std::size_t{1} << mixerIndex)) != 0;
	return obs_output_get_mixer(output) == mixerIndex;
}

void collect_output_conflicts(const std::vector<obs_output_t *> &primaryOutputs, const uint32_t mixerIndex,
			      std::vector<std::string> &conflicts)
{
	std::vector<OutputRef> outputs;
	std::unordered_set<obs_output_t *> identities;
	for (obs_output_t *output : primaryOutputs)
		add_output(outputs, identities, output);

	struct ActiveOutputContext {
		std::vector<OutputRef> *outputs = nullptr;
		std::unordered_set<obs_output_t *> *identities = nullptr;
	} context{&outputs, &identities};
	obs_enum_outputs(
		[](void *param, obs_output_t *output) {
			auto &context = *static_cast<ActiveOutputContext *>(param);
			const char *id = output ? obs_output_get_id(output) : nullptr;
			if (output && obs_output_active(output) &&
			    (!id || std::strcmp(id, InternalCaptureOutputId) != 0))
				add_output(*context.outputs, *context.identities, output);
			return true;
		},
		&context);

	for (const OutputRef &output : outputs) {
		if (output_uses_mixer(output.get(), mixerIndex))
			append_unique(conflicts, output_name(output.get()));
	}
}

void collect_external_source_conflicts(const AudioTree &holdTree, const uint32_t mixerIndex,
				       std::vector<std::string> &conflicts)
{
	struct SourceContext {
		const AudioTree *holdTree = nullptr;
		uint32_t bit = 0;
		std::vector<std::string> *conflicts = nullptr;
	} context{&holdTree, 1U << mixerIndex, &conflicts};

	obs_enum_all_sources(
		[](void *param, obs_source_t *source) {
			auto &context = *static_cast<SourceContext *>(param);
			const char *id = source ? obs_source_get_id(source) : nullptr;
			if (!source || obs_source_removed(source) || context.holdTree->leaves.contains(source) ||
			    (id && std::strcmp(id, InternalAudioTapSourceId) == 0) ||
			    (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) == 0)
				return true;
			if ((obs_source_get_audio_mixers(source) & context.bit) != 0)
				append_unique(*context.conflicts, source_name(source));
			return true;
		},
		&context);
}

[[nodiscard]] AudioPreflightResult make_silence_fallback(const HoldAudioMode requested, const uint32_t mixerIndex,
							 std::string message, std::vector<std::string> conflicts = {})
{
	normalize_names(conflicts);
	AudioPreflightResult result;
	result.requestedMode = requested;
	result.effectiveMode = HoldAudioMode::Silence;
	result.degraded = requested != HoldAudioMode::Silence;
	result.reservedMixerIndex = mixerIndex;
	result.message = std::move(message);
	result.conflictNames = std::move(conflicts);
	return result;
}

} // namespace

AudioPreflightResult preflight_hold_audio(const DelaySettings &settings, obs_source_t *holdScene,
					  obs_source_t *dedicatedSource,
					  const std::vector<obs_output_t *> &primaryOutputs)
{
	SourceRef holdReference(holdScene);
	SourceRef dedicatedReference(dedicatedSource);
	holdScene = holdReference.get();
	dedicatedSource = dedicatedReference.get();
	if (holdScene && obs_source_removed(holdScene))
		holdScene = nullptr;

	const uint32_t normalizedTrack =
		std::clamp(settings.reservedAudioTrack, 1U, static_cast<uint32_t>(MAX_AUDIO_MIXES));
	const uint32_t mixerIndex = normalizedTrack - 1U;
	AudioPreflightResult result;
	result.requestedMode = settings.holdAudioMode;
	result.effectiveMode = settings.holdAudioMode;
	result.reservedMixerIndex = mixerIndex;

	switch (settings.holdAudioMode) {
	case HoldAudioMode::SceneMix: {
		if (!holdScene) {
			std::vector<std::string> dedicatedConflicts;
			if (dedicated_source_is_exclusive(holdScene, dedicatedSource, dedicatedConflicts)) {
				result.effectiveMode = HoldAudioMode::DedicatedSource;
				result.degraded = true;
				result.message = "The hold scene is unavailable; using the dedicated audio source.";
				return result;
			}
			if (!dedicatedConflicts.empty())
				return make_silence_fallback(
					settings.holdAudioMode, mixerIndex,
					"The hold scene is unavailable and the dedicated source is not exclusive (" +
						format_names(dedicatedConflicts) + "); using silence.",
					std::move(dedicatedConflicts));
			return make_silence_fallback(settings.holdAudioMode, mixerIndex,
						     "The hold scene is unavailable; using silence.");
		}

		const AudioTree holdTree = collect_audio_tree(holdScene);
		collect_scene_mix_conflicts(holdScene, holdTree, result.conflictNames);
		if (result.conflictNames.empty()) {
			result.message =
				holdTree.leaves.empty()
					? "The hold scene has no audio leaves; its hold audio will be silent."
					: "The hold scene audio tree is isolated and can use the private scene mix.";
			return result;
		}

		const std::string names = format_names(result.conflictNames);
		std::vector<std::string> dedicatedConflicts;
		if (dedicated_source_is_exclusive(holdScene, dedicatedSource, dedicatedConflicts)) {
			result.effectiveMode = HoldAudioMode::DedicatedSource;
			result.degraded = true;
			result.message = "The scene mix references audio sources also used elsewhere, including hidden "
					 "scene items (" +
					 names + "); using the exclusive dedicated audio source.";
			return result;
		}
		if (!dedicatedConflicts.empty()) {
			for (const std::string &name : dedicatedConflicts)
				append_unique(result.conflictNames, name);
			normalize_names(result.conflictNames);
			return make_silence_fallback(settings.holdAudioMode, mixerIndex,
						     "The scene mix is shared (" + names +
							     ") and the dedicated source is not exclusive (" +
							     format_names(dedicatedConflicts) + "); using silence.",
						     std::move(result.conflictNames));
		}
		return make_silence_fallback(
			settings.holdAudioMode, mixerIndex,
			"The scene mix references audio sources also used elsewhere, including hidden "
			"scene items (" +
				names + "); no valid dedicated source is available, so hold audio will be silent.",
			std::move(result.conflictNames));
	}

	case HoldAudioMode::DedicatedSource: {
		if (!valid_dedicated_source(dedicatedSource))
			return make_silence_fallback(
				settings.holdAudioMode, mixerIndex,
				"The selected dedicated source is unavailable or has no audio; using silence.");
		std::vector<std::string> conflicts;
		if (!dedicated_source_is_exclusive(holdScene, dedicatedSource, conflicts))
			return make_silence_fallback(
				settings.holdAudioMode, mixerIndex,
				"The dedicated source is active or referenced outside the hold path (" +
					format_names(conflicts) + "); using silence to avoid changing Program audio.",
				std::move(conflicts));
		result.message = "The dedicated hold audio source is available and exclusive.";
		return result;
	}

	case HoldAudioMode::ReservedTrack: {
		if (settings.reservedAudioTrack < 1 || settings.reservedAudioTrack > MAX_AUDIO_MIXES)
			return make_silence_fallback(
				settings.holdAudioMode, mixerIndex,
				"The reserved OBS track is outside the valid 1-6 range; using silence.");
		if (!holdScene)
			return make_silence_fallback(
				settings.holdAudioMode, mixerIndex,
				"The hold scene is unavailable; reserved-track audio will be silent.");
		const AudioTree holdTree = collect_audio_tree(holdScene);
		std::vector<std::string> isolationConflicts;
		collect_scene_mix_conflicts(holdScene, holdTree, isolationConflicts);
		if (!isolationConflicts.empty())
			return make_silence_fallback(
				settings.holdAudioMode, mixerIndex,
				"The reserved-track hold scene references audio sources also used elsewhere, including "
				"hidden scene items (" +
					format_names(isolationConflicts) + "); using silence.",
				std::move(isolationConflicts));
		const uint32_t bit = 1U << mixerIndex;
		const bool holdSourceAssigned =
			std::any_of(holdTree.leaves.begin(), holdTree.leaves.end(),
				    [bit](const auto &entry) { return (entry.second.mixerMask & bit) != 0; });
		if (!holdSourceAssigned)
			return make_silence_fallback(
				settings.holdAudioMode, mixerIndex,
				"No audio leaf in the hold scene is assigned to reserved OBS track " +
					std::to_string(settings.reservedAudioTrack) + "; using silence.");

		std::vector<std::string> outputConflicts;
		collect_output_conflicts(primaryOutputs, mixerIndex, outputConflicts);
		std::vector<std::string> sourceConflicts;
		collect_external_source_conflicts(holdTree, mixerIndex, sourceConflicts);
		for (std::string &name : outputConflicts)
			append_unique(result.conflictNames, "Output: " + name);
		for (std::string &name : sourceConflicts)
			append_unique(result.conflictNames, "Source: " + name);
		normalize_names(result.conflictNames);
		if (!result.conflictNames.empty()) {
			const std::string names = format_names(result.conflictNames);
			return make_silence_fallback(settings.holdAudioMode, mixerIndex,
						     "OBS track " + std::to_string(settings.reservedAudioTrack) +
							     " is not exclusive to the hold scene (" + names +
							     "); using silence.",
						     std::move(result.conflictNames));
		}
		result.message = "OBS track " + std::to_string(settings.reservedAudioTrack) +
				 " is reserved exclusively for the hold scene.";
		return result;
	}

	case HoldAudioMode::Silence:
		result.message = "Hold audio is intentionally silent.";
		return result;
	}

	return make_silence_fallback(settings.holdAudioMode, mixerIndex,
				     "The requested hold audio mode is unknown; using silence.");
}

} // namespace dynamic_delay
