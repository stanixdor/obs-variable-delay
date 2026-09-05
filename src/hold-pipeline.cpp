#include "hold-pipeline.hpp"

#include "hold-media-hub.hpp"
#include "output-session.hpp"
#include "plugin-support.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <sstream>
#include <utility>

namespace dynamic_delay {

// Complete primary encoder layouts share one collector and pause state.
// Partial A/V matches stay separate to preserve audio pairing and pauses.
class SharedHoldEncoding final {
public:
	SharedHoldEncoding(std::shared_ptr<HoldMediaHub> hub, obs_output_t *primaryOutput);
	~SharedHoldEncoding();

	bool matches(obs_output_t *primaryOutput) const noexcept;
	bool start(std::string &error);
	bool set_paused(bool paused);
	bool compatible_with(obs_output_t *primaryOutput, std::string &error) const;
	void subscribe(OutputSession &owner);
	void unsubscribe(OutputSession &owner);
	void receive(encoder_packet *packet);

private:
	obs_encoder_t *clone_video_encoder(std::string &error);
	obs_encoder_t *clone_audio_encoder(std::size_t index, std::string &error);
	bool create_capture_output(std::string &error);
	static bool same_extradata(obs_encoder_t *first, obs_encoder_t *second);

	std::shared_ptr<HoldMediaHub> mediaHub_;
	obs_encoder_t *originalVideo_ = nullptr;
	std::array<obs_encoder_t *, MAX_OUTPUT_AUDIO_ENCODERS> originalAudio_{};
	obs_encoder_t *videoEncoder_ = nullptr;
	std::array<obs_encoder_t *, MAX_OUTPUT_AUDIO_ENCODERS> audioEncoders_{};
	obs_output_t *captureOutput_ = nullptr;
	std::mutex subscribersMutex_;
	std::vector<OutputSession *> subscribers_;
	bool started_ = false;
};

namespace {

constexpr const char *CaptureOutputId = "obs_dynamic_delay_capture_output";

struct CaptureSink {
	SharedHoldEncoding *encoding = nullptr;
	obs_output_t *output = nullptr;
};

const char *capture_name(void *)
{
	return "Dynamic Delay internal packet collector";
}

void *capture_create(obs_data_t *settings, obs_output_t *output)
{
	auto *sink = new CaptureSink;
	sink->encoding = reinterpret_cast<SharedHoldEncoding *>(
		static_cast<uintptr_t>(obs_data_get_int(settings, "encoding_pointer")));
	sink->output = output;
	return sink;
}

void capture_destroy(void *data)
{
	delete static_cast<CaptureSink *>(data);
}

bool capture_start(void *data)
{
	auto *sink = static_cast<CaptureSink *>(data);
	if (!sink || !sink->output)
		return false;
	if (!obs_output_can_begin_data_capture(sink->output, 0)) {
		obs_output_set_last_error(sink->output, "The internal hold output cannot begin data capture.");
		obs_log(LOG_ERROR, "Internal hold output cannot begin data capture");
		return false;
	}
	if (!obs_output_initialize_encoders(sink->output, 0)) {
		const char *lastError = obs_output_get_last_error(sink->output);
		if (!lastError || !*lastError) {
			obs_output_set_last_error(sink->output, "The internal hold encoders could not be initialized.");
			lastError = obs_output_get_last_error(sink->output);
		}
		obs_log(LOG_ERROR, "Internal hold encoder initialization failed: %s",
			lastError && *lastError ? lastError : "unknown encoder error");
		return false;
	}
	if (!obs_output_begin_data_capture(sink->output, 0)) {
		obs_output_set_last_error(sink->output, "The internal hold output could not start data capture.");
		obs_log(LOG_ERROR, "Internal hold output could not start data capture");
		return false;
	}
	return true;
}

void capture_stop(void *data, uint64_t)
{
	auto *sink = static_cast<CaptureSink *>(data);
	if (sink && sink->output)
		obs_output_end_data_capture(sink->output);
}

void capture_packet(void *data, encoder_packet *packet)
{
	auto *sink = static_cast<CaptureSink *>(data);
	if (sink && sink->encoding)
		sink->encoding->receive(packet);
}

} // namespace

SharedHoldEncoding::SharedHoldEncoding(std::shared_ptr<HoldMediaHub> hub, obs_output_t *primaryOutput)
	: mediaHub_(std::move(hub)),
	  originalVideo_(obs_encoder_get_ref(obs_output_get_video_encoder(primaryOutput)))
{
	for (std::size_t index = 0; index < originalAudio_.size(); ++index)
		originalAudio_[index] = obs_encoder_get_ref(obs_output_get_audio_encoder(primaryOutput, index));
}

SharedHoldEncoding::~SharedHoldEncoding()
{
	// Output destruction joins encoder callbacks before any clones,
	// subscriber storage, or media context can be released.
	if (captureOutput_) {
		if (obs_output_active(captureOutput_))
			obs_output_stop(captureOutput_);
		obs_output_release(captureOutput_);
	}
	for (obs_encoder_t *encoder : audioEncoders_)
		obs_encoder_release(encoder);
	obs_encoder_release(videoEncoder_);
	for (obs_encoder_t *encoder : originalAudio_)
		obs_encoder_release(encoder);
	obs_encoder_release(originalVideo_);
}

bool SharedHoldEncoding::matches(obs_output_t *primaryOutput) const noexcept
{
	if (started_ && (!captureOutput_ || !obs_output_active(captureOutput_)))
		return false;
	if (originalVideo_ != obs_output_get_video_encoder(primaryOutput))
		return false;
	for (std::size_t index = 0; index < originalAudio_.size(); ++index) {
		if (originalAudio_[index] != obs_output_get_audio_encoder(primaryOutput, index))
			return false;
	}
	return true;
}

void SharedHoldEncoding::subscribe(OutputSession &owner)
{
	std::scoped_lock lock(subscribersMutex_);
	if (std::find(subscribers_.begin(), subscribers_.end(), &owner) == subscribers_.end())
		subscribers_.push_back(&owner);
}

void SharedHoldEncoding::unsubscribe(OutputSession &owner)
{
	// A session may die immediately after this returns. Waiting for current
	// delivery prevents a callback-local raw pointer outliving its owner.
	std::scoped_lock lock(subscribersMutex_);
	std::erase(subscribers_, &owner);
}

void SharedHoldEncoding::receive(encoder_packet *packet)
{
	std::scoped_lock lock(subscribersMutex_);
	for (OutputSession *owner : subscribers_)
		owner->receive_hold_packet(packet);
}

obs_encoder_t *SharedHoldEncoding::clone_video_encoder(std::string &error)
{
	if (!originalVideo_) {
		error = "The primary output has no video encoder.";
		return nullptr;
	}
	obs_data_t *settings = obs_encoder_get_settings(originalVideo_);
	std::ostringstream name;
	name << "dynamic-delay-hold-video-" << this;
	obs_encoder_t *clone =
		obs_video_encoder_create(obs_encoder_get_id(originalVideo_), name.str().c_str(), settings, nullptr);
	obs_data_release(settings);
	if (!clone) {
		error = std::string("Could not clone video encoder: ") + obs_encoder_get_id(originalVideo_);
		return nullptr;
	}
	obs_encoder_set_video(clone, mediaHub_->video());
	if (obs_encoder_scaling_enabled(originalVideo_))
		obs_encoder_set_scaled_size(clone, obs_encoder_get_width(originalVideo_),
					    obs_encoder_get_height(originalVideo_));
	obs_encoder_set_gpu_scale_type(clone, obs_encoder_get_scale_type(originalVideo_));
	obs_encoder_set_frame_rate_divisor(clone, obs_encoder_get_frame_rate_divisor(originalVideo_));
	obs_encoder_set_preferred_video_format(clone, obs_encoder_get_preferred_video_format(originalVideo_));
	obs_encoder_set_preferred_color_space(clone, obs_encoder_get_preferred_color_space(originalVideo_));
	obs_encoder_set_preferred_range(clone, obs_encoder_get_preferred_range(originalVideo_));
	return clone;
}

obs_encoder_t *SharedHoldEncoding::clone_audio_encoder(const std::size_t index, std::string &error)
{
	obs_encoder_t *original = originalAudio_[index];
	obs_data_t *settings = obs_encoder_get_settings(original);
	std::ostringstream name;
	name << "dynamic-delay-hold-audio-" << index << '-' << this;
	obs_encoder_t *clone = obs_audio_encoder_create(obs_encoder_get_id(original), name.str().c_str(), settings,
							mediaHub_->encoder_mixer_index(original), nullptr);
	obs_data_release(settings);
	if (!clone) {
		error = std::string("Could not clone audio encoder: ") + obs_encoder_get_id(original);
		return nullptr;
	}
	obs_encoder_set_audio(clone, mediaHub_->audio());
	return clone;
}

bool SharedHoldEncoding::create_capture_output(std::string &error)
{
	obs_data_t *settings = obs_data_create();
	obs_data_set_int(settings, "encoding_pointer", static_cast<long long>(reinterpret_cast<uintptr_t>(this)));
	std::ostringstream name;
	name << "dynamic-delay-collector-" << this;
	captureOutput_ = obs_output_create(CaptureOutputId, name.str().c_str(), settings, nullptr);
	obs_data_release(settings);
	if (!captureOutput_) {
		error = "Could not create the internal packet collector.";
		return false;
	}
	obs_output_set_video_encoder(captureOutput_, videoEncoder_);
	for (std::size_t index = 0; index < audioEncoders_.size(); ++index) {
		if (audioEncoders_[index])
			obs_output_set_audio_encoder(captureOutput_, audioEncoders_[index], index);
	}
	return true;
}

bool SharedHoldEncoding::start(std::string &error)
{
	if (started_) {
		if (captureOutput_ && obs_output_active(captureOutput_))
			return true;
		error = "The shared hold encoder is no longer active.";
		return false;
	}
	videoEncoder_ = clone_video_encoder(error);
	if (!videoEncoder_)
		return false;
	for (std::size_t index = 0; index < originalAudio_.size(); ++index) {
		if (!originalAudio_[index])
			continue;
		audioEncoders_[index] = clone_audio_encoder(index, error);
		if (!audioEncoders_[index])
			return false;
	}
	if (!create_capture_output(error))
		return false;
	if (!obs_output_start(captureOutput_)) {
		const char *lastError = obs_output_get_last_error(captureOutput_);
		error = lastError && *lastError ? lastError : "The hold-scene encoder could not start.";
		return false;
	}
	started_ = true;
	return true;
}

bool SharedHoldEncoding::set_paused(const bool paused)
{
	return captureOutput_ && started_ && obs_output_pause(captureOutput_, paused);
}

bool SharedHoldEncoding::same_extradata(obs_encoder_t *first, obs_encoder_t *second)
{
	uint8_t *firstData = nullptr;
	uint8_t *secondData = nullptr;
	size_t firstSize = 0;
	size_t secondSize = 0;
	const bool firstOk = obs_encoder_get_extra_data(first, &firstData, &firstSize);
	const bool secondOk = obs_encoder_get_extra_data(second, &secondData, &secondSize);
	if (firstOk != secondOk || firstSize != secondSize)
		return false;
	return !firstOk || firstSize == 0 || std::memcmp(firstData, secondData, firstSize) == 0;
}

bool SharedHoldEncoding::compatible_with(obs_output_t *primaryOutput, std::string &error) const
{
	obs_encoder_t *primaryVideo = obs_output_get_video_encoder(primaryOutput);
	if (!primaryVideo || !videoEncoder_ ||
	    std::strcmp(obs_encoder_get_codec(primaryVideo), obs_encoder_get_codec(videoEncoder_)) != 0 ||
	    !same_extradata(primaryVideo, videoEncoder_)) {
		error = "The hold video encoder produced incompatible codec headers.";
		return false;
	}
	for (std::size_t index = 0; index < audioEncoders_.size(); ++index) {
		obs_encoder_t *primaryAudio = obs_output_get_audio_encoder(primaryOutput, index);
		if (!!primaryAudio != !!audioEncoders_[index]) {
			error = "The hold audio track layout does not match the output.";
			return false;
		}
		if (primaryAudio && (std::strcmp(obs_encoder_get_codec(primaryAudio),
						 obs_encoder_get_codec(audioEncoders_[index])) != 0 ||
				     !same_extradata(primaryAudio, audioEncoders_[index]))) {
			error = "A hold-scene audio encoder produced incompatible codec headers.";
			return false;
		}
	}
	return true;
}

std::shared_ptr<SharedHoldEncoding> HoldMediaHub::acquire_encoding(obs_output_t *primaryOutput, std::string &error)
{
	if (!started_ || !primaryOutput) {
		error = "The shared hold media hub is not active.";
		return {};
	}
	// Controller-thread lifecycle only. Encoder callbacks never touch this
	// weak cache, and the last subscriber releases the entire encoding.
	std::erase_if(encodings_, [](const auto &entry) { return entry.expired(); });
	for (const auto &entry : encodings_) {
		if (auto encoding = entry.lock(); encoding && encoding->matches(primaryOutput))
			return encoding;
	}
	auto encoding = std::make_shared<SharedHoldEncoding>(shared_from_this(), primaryOutput);
	encodings_.push_back(encoding);
	return encoding;
}

HoldPipeline::HoldPipeline(OutputSession &owner, obs_output_t *primaryOutput, obs_source_t *scene)
	: owner_(owner),
	  primaryOutput_(obs_output_get_ref(primaryOutput)),
	  scene_(obs_source_get_ref(scene))
{
}

HoldPipeline::HoldPipeline(OutputSession &owner, obs_output_t *primaryOutput, std::shared_ptr<HoldMediaHub> mediaHub)
	: owner_(owner),
	  primaryOutput_(obs_output_get_ref(primaryOutput)),
	  mediaHub_(std::move(mediaHub))
{
}

HoldPipeline::~HoldPipeline()
{
	stop();
	obs_output_release(primaryOutput_);
	obs_source_release(scene_);
}

void HoldPipeline::register_output_type()
{
	static std::once_flag once;
	std::call_once(once, [] {
		HoldMediaHub::register_source_type();
		obs_output_info info{};
		info.id = CaptureOutputId;
		info.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED | OBS_OUTPUT_MULTI_TRACK_AUDIO | OBS_OUTPUT_CAN_PAUSE;
		info.get_name = capture_name;
		info.create = capture_create;
		info.destroy = capture_destroy;
		info.start = capture_start;
		info.stop = capture_stop;
		info.encoded_packet = capture_packet;
		info.encoded_video_codecs = "h264;hevc;av1;vp8;vp9";
		info.encoded_audio_codecs = "aac;opus;pcm_s16le;pcm_s24le;pcm_s32le;flac";
		obs_register_output(&info);
	});
}

bool HoldPipeline::start(std::string &error)
{
	if (started_)
		return true;
	if (!mediaHub_) {
		mediaHub_ = HoldMediaHub::create(scene_, {}, error);
		if (!mediaHub_)
			return false;
	}
	encoding_ = mediaHub_->acquire_encoding(primaryOutput_, error);
	if (!encoding_)
		return false;
	encoding_->subscribe(owner_);
	if (!encoding_->start(error)) {
		stop();
		return false;
	}
	started_ = true;
	return true;
}

void HoldPipeline::stop()
{
	if (encoding_)
		encoding_->unsubscribe(owner_);
	encoding_.reset();
	mediaHub_.reset();
	started_ = false;
}

bool HoldPipeline::set_paused(const bool paused)
{
	return encoding_ && encoding_->set_paused(paused);
}

bool HoldPipeline::compatible_with_primary(std::string &error) const
{
	return encoding_ && encoding_->compatible_with(primaryOutput_, error);
}

void HoldPipeline::receive(encoder_packet *packet)
{
	owner_.receive_hold_packet(packet);
}

} // namespace dynamic_delay
