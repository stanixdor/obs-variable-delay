#pragma once

#include "hold-audio-tap.hpp"
#include "obs-packet.hpp"

#include <obs.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dynamic_delay {

class OutputSession;
class HoldMediaHub;

class HoldPipeline {
public:
	HoldPipeline(OutputSession &owner, obs_output_t *primaryOutput, std::shared_ptr<HoldMediaHub> mediaHub);
	// Compatibility path for a single output.  New callers should create one
	// HoldMediaHub per activation and share it across every OutputSession.
	HoldPipeline(OutputSession &owner, obs_output_t *primaryOutput, obs_source_t *scene);
	~HoldPipeline();

	HoldPipeline(const HoldPipeline &) = delete;
	HoldPipeline &operator=(const HoldPipeline &) = delete;

	bool start(std::string &error);
	void stop();
	[[nodiscard]] bool compatible_with_primary(std::string &error) const;

	void receive(encoder_packet *packet);

	static void register_output_type();

private:
	bool clone_encoders(std::string &error);
	bool create_capture_output(std::string &error);
	obs_encoder_t *clone_video_encoder(obs_encoder_t *original, std::string &error);
	obs_encoder_t *clone_audio_encoder(obs_encoder_t *original, std::size_t index, std::string &error);
	static bool same_extradata(obs_encoder_t *first, obs_encoder_t *second);

	OutputSession &owner_;
	obs_output_t *primaryOutput_ = nullptr;
	std::shared_ptr<HoldMediaHub> mediaHub_;
	obs_source_t *scene_ = nullptr;
	obs_output_t *captureOutput_ = nullptr;
	obs_encoder_t *videoEncoder_ = nullptr;
	std::array<obs_encoder_t *, MAX_OUTPUT_AUDIO_ENCODERS> audioEncoders_{};
	bool started_ = false;
};

} // namespace dynamic_delay
