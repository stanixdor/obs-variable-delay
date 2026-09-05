#include "hold-audio-tap.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>

namespace dynamic_delay {
namespace {

constexpr const char *AudioTapSourceId = "obs_dynamic_delay_hold_audio_tap";

} // namespace

struct HoldAudioTap::SharedState {
	SharedState(obs_source_t *scene, HoldAudioConfig selection)
		: holdScene(obs_source_get_ref(scene)),
		  audioSource(obs_source_get_ref(
			  selection.mode == HoldAudioMode::DedicatedSource ? selection.dedicatedSource : scene)),
		  config(selection),
		  fifo(selection.mode == HoldAudioMode::Silence ? nullptr : std::make_unique<Fifo>())
	{
	}

	~SharedState()
	{
		obs_source_release(audioSource);
		obs_source_release(holdScene);
	}

	obs_source_t *holdScene = nullptr;
	obs_source_t *audioSource = nullptr;
	HoldAudioConfig config;
	// Allocate before either audio clock starts. Silence never needs the
	// roughly 3 MiB PCM ring; a running ring stays alive after force_silence()
	// until both clocks stop so neither callback can race its destruction.
	std::unique_ptr<Fifo> fifo;
	core::PcmWindowCursor cursor;
	uint64_t lastPushedTimestampNs = 0;
	uint64_t lastMainClockTimestampNs = 0;
	uint32_t lastMainClockFrames = 0;
	std::atomic_bool forceSilence{false};
};

struct HoldAudioTap::SourceContext {
	std::shared_ptr<SharedState> state;
	obs_source_t *wrapper = nullptr;
};

HoldAudioTap::HoldAudioTap(obs_source_t *holdScene, HoldAudioConfig config)
	: state_(std::make_shared<SharedState>(holdScene, config))
{
}

HoldAudioTap::~HoldAudioTap()
{
	stop();
}

HoldAudioMode HoldAudioTap::mode() const noexcept
{
	return state_ ? state_->config.mode : HoldAudioMode::Silence;
}

bool HoldAudioTap::connect_main_clock(audio_t *audio, const std::size_t mixerIndex, std::string &error)
{
	if (mainAudioClock_)
		return true;
	if (!audio || !state_ || mixerIndex >= MAX_AUDIO_MIXES) {
		error = "OBS main audio clock is unavailable.";
		return false;
	}
	if (!audio_output_connect(audio, mixerIndex, nullptr, &HoldAudioTap::main_clock_output, state_.get())) {
		error = "Could not observe the main OBS audio clock.";
		return false;
	}
	mainAudioClock_ = audio;
	mainClockMixer_ = mixerIndex;
	return true;
}

bool HoldAudioTap::force_silence() noexcept
{
	return state_ && !state_->forceSilence.exchange(true, std::memory_order_acq_rel);
}

bool HoldAudioTap::silence_forced() const noexcept
{
	return state_ && state_->forceSilence.load(std::memory_order_acquire);
}

obs_source_t *HoldAudioTap::active_audio_child() const noexcept
{
	return state_ ? state_->audioSource : nullptr;
}

const char *HoldAudioTap::source_name(void *)
{
	return "Dynamic Delay private hold audio tap";
}

void *HoldAudioTap::source_create(obs_data_t *settings, obs_source_t *source)
{
	if (!settings || !source)
		return nullptr;
	auto *self =
		reinterpret_cast<HoldAudioTap *>(static_cast<uintptr_t>(obs_data_get_int(settings, "tap_pointer")));
	if (!self || !self->state_ || !self->state_->holdScene)
		return nullptr;
	if (self->state_->config.mode == HoldAudioMode::DedicatedSource && !self->state_->audioSource)
		return nullptr;
	auto *context = new SourceContext;
	context->state = self->state_;
	context->wrapper = source;
	return context;
}

void HoldAudioTap::source_destroy(void *data)
{
	delete static_cast<SourceContext *>(data);
}

uint32_t HoldAudioTap::source_width(void *data)
{
	auto *context = static_cast<SourceContext *>(data);
	return context && context->state->holdScene ? obs_source_get_width(context->state->holdScene) : 0;
}

uint32_t HoldAudioTap::source_height(void *data)
{
	auto *context = static_cast<SourceContext *>(data);
	return context && context->state->holdScene ? obs_source_get_height(context->state->holdScene) : 0;
}

void HoldAudioTap::source_video_render(void *data, gs_effect_t *)
{
	auto *context = static_cast<SourceContext *>(data);
	if (context && context->state->holdScene)
		obs_source_video_render(context->state->holdScene);
}

enum gs_color_space HoldAudioTap::source_video_color_space(void *data, const std::size_t count,
							   const enum gs_color_space *preferredSpaces)
{
	auto *context = static_cast<SourceContext *>(data);
	return context && context->state->holdScene
		       ? obs_source_get_color_space(context->state->holdScene, count, preferredSpaces)
		       : GS_CS_SRGB;
}

void HoldAudioTap::source_enum_active(void *data, obs_source_enum_proc_t callback, void *param)
{
	auto *context = static_cast<SourceContext *>(data);
	if (!context || !callback || !context->wrapper)
		return;
	if (context->state->forceSilence.load(std::memory_order_acquire))
		return;
	if ((context->state->config.mode == HoldAudioMode::SceneMix ||
	     context->state->config.mode == HoldAudioMode::ReservedTrack) &&
	    context->state->holdScene)
		callback(context->wrapper, context->state->holdScene, param);
	else if (context->state->config.mode == HoldAudioMode::DedicatedSource && context->state->audioSource)
		callback(context->wrapper, context->state->audioSource, param);
}

void HoldAudioTap::push(SharedState &state, const obs_source_audio_mix &mix, const uint64_t timestampNs,
			const uint32_t mixers, const std::size_t channels, const uint32_t sampleRate) noexcept
{
	// libobs may render the same future-dated source block over several ticks
	// while a positive sync offset catches up.  Queue each timestamp once so
	// the private clock emits silence instead of repeating PCM.
	if (!state.fifo || timestampNs == 0 || timestampNs <= state.lastPushedTimestampNs ||
	    state.lastMainClockTimestampNs == 0 || state.lastMainClockFrames == 0 ||
	    state.forceSilence.load(std::memory_order_acquire))
		return;
	const uint64_t mainWindowTimestamp =
		state.lastMainClockTimestampNs +
		static_cast<uint64_t>(state.lastMainClockFrames) * 1'000'000'000ULL / sampleRate;
	core::PcmAudioBlock *block = state.fifo->begin_push();
	if (!block)
		return;

	const std::size_t copyChannels = std::min(channels, core::PcmAudioBlock::MaxChannels);
	block->timestampNs = timestampNs;
	const bool sourceAfterWindow = timestampNs >= mainWindowTimestamp;
	const uint64_t deltaNs = sourceAfterWindow ? timestampNs - mainWindowTimestamp
						   : mainWindowTimestamp - timestampNs;
	const uint64_t deltaFrames = (deltaNs / 1'000'000'000ULL) * sampleRate +
				     ((deltaNs % 1'000'000'000ULL) * sampleRate + 500'000'000ULL) / 1'000'000'000ULL;
	if (deltaFrames > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
		block->alignmentFrames = sourceAfterWindow ? std::numeric_limits<int64_t>::max()
							   : std::numeric_limits<int64_t>::min();
	else
		block->alignmentFrames = sourceAfterWindow ? static_cast<int64_t>(deltaFrames)
							   : -static_cast<int64_t>(deltaFrames);
	block->sampleRate = sampleRate;
	block->channels = static_cast<uint32_t>(copyChannels);
	const uint32_t modeMixers = state.config.mode == HoldAudioMode::ReservedTrack
					    ? mixers & (1U << state.config.reservedMixerIndex)
					    : mixers;
	block->mixerMask = modeMixers & ((1U << core::PcmAudioBlock::MixerCount) - 1U);
	for (std::size_t mixer = 0; mixer < core::PcmAudioBlock::MixerCount; ++mixer) {
		if ((block->mixerMask & (1U << mixer)) == 0)
			continue;
		for (std::size_t channel = 0; channel < copyChannels; ++channel) {
			const float *input = mix.output[mixer].data[channel];
			float *destination = block->plane(mixer, channel);
			if (input)
				std::memcpy(destination, input, core::PcmAudioBlock::Frames * sizeof(float));
			else
				std::memset(destination, 0, core::PcmAudioBlock::Frames * sizeof(float));
		}
	}
	state.fifo->commit_push();
	state.lastPushedTimestampNs = timestampNs;
}

void HoldAudioTap::main_clock_output(void *param, std::size_t, audio_data *data)
{
	auto *state = static_cast<SharedState *>(param);
	if (!state || !data)
		return;
	state->lastMainClockTimestampNs = data->timestamp;
	state->lastMainClockFrames = data->frames;
}

bool HoldAudioTap::source_audio_render(void *data, uint64_t *timestampNs, obs_source_audio_mix *output,
				       const uint32_t mixers, const std::size_t channels, const std::size_t sampleRate)
{
	auto *context = static_cast<SourceContext *>(data);
	if (!context || !timestampNs || !output)
		return false;
	SharedState &state = *context->state;
	if ((state.config.mode != HoldAudioMode::SceneMix && state.config.mode != HoldAudioMode::DedicatedSource &&
	     state.config.mode != HoldAudioMode::ReservedTrack) ||
	    !state.audioSource || state.forceSilence.load(std::memory_order_acquire) ||
	    obs_source_audio_pending(state.audioSource))
		return false;

	const uint64_t sourceTimestamp = obs_source_get_audio_timestamp(state.audioSource);
	if (sourceTimestamp == 0)
		return false;

	obs_source_audio_mix childMix{};
	obs_source_get_audio_mix(state.audioSource, &childMix);
	// This private composite is only an audio-render dependency in an AUX
	// view, never a Program mix root. Its already-zeroed output is unused;
	// the private encoder receives child PCM exclusively through this ring.
	push(state, childMix, sourceTimestamp, mixers, channels, static_cast<uint32_t>(sampleRate));
	*timestampNs = sourceTimestamp;
	return true;
}

void HoldAudioTap::register_source_type()
{
	static std::once_flag once;
	std::call_once(once, [] {
		obs_source_info info{};
		info.id = AudioTapSourceId;
		info.type = OBS_SOURCE_TYPE_INPUT;
		info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_COMPOSITE |
				    OBS_SOURCE_DO_NOT_DUPLICATE | OBS_SOURCE_CAP_DISABLED | OBS_SOURCE_SRGB;
		info.get_name = &HoldAudioTap::source_name;
		info.create = &HoldAudioTap::source_create;
		info.destroy = &HoldAudioTap::source_destroy;
		info.get_width = &HoldAudioTap::source_width;
		info.get_height = &HoldAudioTap::source_height;
		info.video_render = &HoldAudioTap::source_video_render;
		info.video_get_color_space = &HoldAudioTap::source_video_color_space;
		info.enum_active_sources = &HoldAudioTap::source_enum_active;
		info.enum_all_sources = &HoldAudioTap::source_enum_active;
		info.audio_render = &HoldAudioTap::source_audio_render;
		obs_register_source(&info);
	});
}

bool HoldAudioTap::create(std::string &error)
{
	if (wrapperSource_)
		return true;
	if (!state_ || !state_->holdScene ||
	    (state_->config.mode == HoldAudioMode::DedicatedSource && !state_->audioSource)) {
		error = "The hold audio tap has no valid source.";
		return false;
	}
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "tap_pointer", static_cast<long long>(reinterpret_cast<uintptr_t>(this)));
	std::ostringstream name;
	name << "dynamic-delay-private-audio-tap-" << this;
	obs_source_t *created = obs_source_create_private(AudioTapSourceId, name.str().c_str(), settings);
	obs_data_release(settings);
	if (!created) {
		error = "Could not create the private hold audio tap source.";
		return false;
	}
	wrapperSource_ = created;
	return true;
}

void HoldAudioTap::stop()
{
	if (mainAudioClock_ && state_)
		audio_output_disconnect(mainAudioClock_, mainClockMixer_, &HoldAudioTap::main_clock_output,
					state_.get());
	mainAudioClock_ = nullptr;
	mainClockMixer_ = 0;
	if (wrapperSource_)
		obs_source_release(wrapperSource_);
	wrapperSource_ = nullptr;
}

bool HoldAudioTap::pull(const uint64_t callbackStartNs, uint64_t *newTimestampNs, const uint32_t activeMixers,
			audio_output_data *mixes, const std::size_t channels, const uint32_t sampleRate) noexcept
{
	if (!mixes)
		return false;
	if (newTimestampNs) {
		const uint64_t bridgeLatencyNs = sampleRate == 0 ? 0
								 : static_cast<uint64_t>(core::PcmAudioBlock::Frames) *
									   1'000'000'000ULL / sampleRate;
		*newTimestampNs = callbackStartNs > bridgeLatencyNs ? callbackStartNs - bridgeLatencyNs
								    : callbackStartNs;
	}
	const std::size_t copyChannels = std::min(channels, core::PcmAudioBlock::MaxChannels);
	// audio_output's input callback receives buffers cleared by libobs on
	// every quantum, including inactive mixers. Unwritten spans stay silent.
	if (!state_ || !state_->fifo || state_->forceSilence.load(std::memory_order_acquire)) {
		return true;
	}
	state_->cursor.consume(
		*state_->fifo, sampleRate, core::PcmAudioBlock::Frames,
		[copyChannels](const core::PcmAudioBlock &block) { return block.channels == copyChannels; },
		[&](const core::PcmAudioBlock &block, const std::size_t sourceOffset,
		    const std::size_t destinationOffset, const std::size_t frames) {
			const uint32_t availableMixers = activeMixers & block.mixerMask;
			for (std::size_t mixer = 0; mixer < core::PcmAudioBlock::MixerCount; ++mixer) {
				if ((availableMixers & (1U << mixer)) == 0)
					continue;
				for (std::size_t channel = 0; channel < copyChannels; ++channel) {
					if (float *destination = mixes[mixer].data[channel])
						std::memcpy(destination + destinationOffset,
							    block.plane(mixer, channel) + sourceOffset,
							    frames * sizeof(float));
				}
			}
		});

	return true;
}

} // namespace dynamic_delay
