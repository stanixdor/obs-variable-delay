#pragma once

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

class HoldPipeline {
public:
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
	static bool audio_input(void *param, uint64_t startTs, uint64_t endTs, uint64_t *newTs, uint32_t activeMixers,
				audio_output_data *mixes);
	bool create_view(std::string &error);
	bool create_audio(std::string &error);
	bool clone_encoders(std::string &error);
	bool create_capture_output(std::string &error);
	obs_encoder_t *clone_video_encoder(obs_encoder_t *original, std::string &error);
	obs_encoder_t *clone_audio_encoder(obs_encoder_t *original, std::size_t index, std::string &error);
	static bool same_extradata(obs_encoder_t *first, obs_encoder_t *second);

	OutputSession &owner_;
	obs_output_t *primaryOutput_ = nullptr;
	obs_source_t *scene_ = nullptr;
	obs_view_t *view_ = nullptr;
	video_t *video_ = nullptr;
	audio_t *audio_ = nullptr;
	obs_output_t *captureOutput_ = nullptr;
	obs_encoder_t *videoEncoder_ = nullptr;
	std::array<obs_encoder_t *, MAX_OUTPUT_AUDIO_ENCODERS> audioEncoders_{};
	std::string audioName_;
	std::size_t audioChannels_ = 0;
	bool started_ = false;
	bool sceneActive_ = false;
};

} // namespace dynamic_delay
