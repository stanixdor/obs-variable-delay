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
class SharedHoldEncoding;

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
	bool set_paused(bool paused);
	[[nodiscard]] bool compatible_with_primary(std::string &error) const;

	void receive(encoder_packet *packet);

	static void register_output_type();

private:
	OutputSession &owner_;
	obs_output_t *primaryOutput_ = nullptr;
	std::shared_ptr<HoldMediaHub> mediaHub_;
	obs_source_t *scene_ = nullptr;
	std::shared_ptr<SharedHoldEncoding> encoding_;
	bool started_ = false;
};

} // namespace dynamic_delay
