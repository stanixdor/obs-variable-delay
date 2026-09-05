#pragma once

#include <obs.h>

#include <cstdint>
#include <functional>
#include <memory>

namespace dynamic_delay {
class HoldMediaHub;
class OutputSession;
} // namespace dynamic_delay

namespace output_test {

void set_clock(uint64_t nanoseconds);
encoder_packet make_packet(obs_encoder_type type, int64_t dtsUsec, uint64_t payloadId, bool keyframe = false,
			   std::size_t track = 0, int64_t compositionUsec = 0, std::size_t declaredBytes = 64);
uint64_t payload_id(const encoder_packet &packet);
std::size_t live_payloads();
std::shared_ptr<dynamic_delay::HoldMediaHub> media_hub();
void reconnect(obs_output_t &output);
void activate_output(obs_output_t &output);
void pause_output(obs_output_t &output, bool paused);
void emit(obs_output_t &output, encoder_packet &packet, encoder_packet_time *timing = nullptr);

struct PipelineBehavior {
	bool startSucceeds = true;
	bool compatible = true;
	int constructed = 0;
	int destroyed = 0;
	int stopped = 0;
	bool paused = false;
	int pauseCalls = 0;
	std::function<void(dynamic_delay::OutputSession &)> onDestroy;
	std::function<void(dynamic_delay::OutputSession &)> onStop;
};
PipelineBehavior &pipeline_behavior();
void reset_pipeline_behavior();

} // namespace output_test
