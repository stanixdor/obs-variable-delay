#pragma once

#include "delay-types.hpp"
#include "obs-packet.hpp"

#include <obs.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

namespace dynamic_delay {

class HoldPipeline;
class HoldMediaHub;

class OutputSession {
public:
	using Notify = std::function<void()>;

	OutputSession(obs_output_t *output, std::string label, Notify notify);
	~OutputSession();

	OutputSession(const OutputSession &) = delete;
	OutputSession &operator=(const OutputSession &) = delete;

	bool attach(std::string &error);
	bool request_delay(uint32_t seconds, std::shared_ptr<HoldMediaHub> mediaHub, std::string &error);
	void request_bypass();
	void sync_pause_state();
	void maintenance();
	void receive_hold_packet(encoder_packet *packet);
	[[nodiscard]] bool take_rearm_request() noexcept
	{
		return reconnectRearmRequested_.exchange(false, std::memory_order_acq_rel);
	}

	[[nodiscard]] DelaySnapshot snapshot() const;
	[[nodiscard]] std::size_t estimated_bytes(uint32_t seconds, bool *available = nullptr) const;
	[[nodiscard]] DelayState state() const noexcept { return state_.load(std::memory_order_relaxed); }
	[[nodiscard]] bool is_bypass() const noexcept
	{
		const DelayState current = state();
		return current == DelayState::Bypass || current == DelayState::Error;
	}
	[[nodiscard]] obs_output_t *output() const noexcept { return output_; }
	[[nodiscard]] const std::string &label() const noexcept { return label_; }

private:
	static constexpr std::size_t LaneCount = 1 + MAX_OUTPUT_AUDIO_ENCODERS;
	using PacketQueue = std::deque<ObsPacket>;
	struct LaneFormat {
		bool seen = false;
		enum obs_encoder_type type = OBS_ENCODER_VIDEO;
		size_t track = 0;
		int32_t timebaseNum = 0;
		int32_t timebaseDen = 0;
	};

	static void packet_callback(obs_output_t *output, encoder_packet *packet, encoder_packet_time *packetTime,
				    void *param);
	static void reconnect_signal(void *param, calldata_t *data);
	static void activate_signal(void *param, calldata_t *data);
	static void pause_signal(void *param, calldata_t *data);
	void process_primary_packet(encoder_packet *packet, const encoder_packet_time *packetTime);
	bool buffer_primary(encoder_packet *packet, uint64_t nowNs, int64_t rawDtsUsec);
	bool replace_from(std::array<PacketQueue, LaneCount> &queues, std::array<ObsPacket, LaneCount> *fallbackPackets,
			  encoder_packet *carrier, bool holdQueue, bool *usedFallback = nullptr);
	bool replace_with_fallback(std::array<ObsPacket, LaneCount> &fallbackPackets, encoder_packet *carrier);
	void note_lane_format(std::array<LaneFormat, LaneCount> &formats, const encoder_packet &packet);
	[[nodiscard]] bool hold_preroll_ready_locked() const;
	[[nodiscard]] bool hold_formats_compatible_locked(bool &complete) const;
	[[nodiscard]] bool packets_compatible(const encoder_packet &carrier, const ObsPacket &replacement) const;
	void apply_timeline_offset_locked(encoder_packet &packet, int64_t offsetUsec);
	void begin_timeline_epoch_locked(encoder_packet &carrier, const ObsPacket *replacement);
	void note_emitted_video_locked(const encoder_packet &packet);
	void reset_timeline_locked();
	void note_primary_cadence_locked(const encoder_packet &packet, int64_t rawDtsUsec);
	void realign_primary_locked(encoder_packet *carrier, int64_t rawDtsUsec);
	void clear_primary_buffer();
	void clear_hold_buffer();
	void set_error_locked(std::string message);
	void return_live_with_error_locked(std::string message, bool fromFilling);
	void note_throughput(std::size_t bytes, uint64_t nowNs);
	[[nodiscard]] static std::size_t lane_for(const encoder_packet &packet) noexcept;
	[[nodiscard]] bool supported(std::string &error) const;
	[[nodiscard]] std::size_t configured_bitrate_bytes(uint32_t seconds, bool &available) const;

	obs_output_t *output_ = nullptr;
	std::string label_;
	Notify notify_;
	std::atomic<DelayState> state_{DelayState::Bypass};
	mutable std::mutex mutex_;
	std::array<PacketQueue, LaneCount> primaryQueues_;
	std::array<PacketQueue, LaneCount> holdQueues_;
	std::array<ObsPacket, LaneCount> primaryFallbackPackets_;
	std::array<ObsPacket, LaneCount> holdFallbackPackets_;
	std::array<LaneFormat, LaneCount> primaryFormats_{};
	std::array<LaneFormat, LaneCount> holdFormats_{};
	std::array<int64_t, LaneCount> lastPrimaryDts_{};
	std::array<int64_t, LaneCount> primaryStepUsec_{};
	std::array<int64_t, LaneCount> lastEmittedSourceDts_{};
	std::array<bool, LaneCount> havePrimaryDts_{};
	std::array<bool, LaneCount> haveEmittedSourceDts_{};
	std::unique_ptr<HoldPipeline> holdPipeline_;
	uint64_t fillStartNs_ = 0;
	int64_t fillStartDtsUsec_ = 0;
	int64_t latestPrimaryDtsUsec_ = 0;
	int64_t videoFrameDurationUsec_ = 16'667;
	int64_t maxEmittedVideoPtsUsec_ = 0;
	std::atomic<uint64_t> observedWindowStartNs_{0};
	std::atomic<uint64_t> observedBytes_{0};
	std::atomic<double> measuredBytesPerSecond_{0.0};
	std::size_t bufferedBytes_ = 0;
	std::size_t holdBufferedBytes_ = 0;
	uint32_t targetSeconds_ = 0;
	uint32_t activeAudioMask_ = 0;
	int64_t holdGopStartDtsUsec_ = 0;
	bool callbackAttached_ = false;
	bool reconnectSignalAttached_ = false;
	bool lifecycleSignalsAttached_ = false;
	bool paused_ = false;
	bool holdPauseStateKnown_ = false;
	bool holdPaused_ = false;
	bool realignmentPending_ = false;
	bool holdReady_ = false;
	bool returnFromFilling_ = false;
	bool drainPrimaryOnReturn_ = false;
	bool cleanupHoldRequested_ = false;
	bool haveEmittedVideoPts_ = false;
	bool errorReturnPending_ = false;
	std::atomic_bool reconnectRearmRequested_{false};
	std::atomic_bool reconnectResetPending_{false};
	std::atomic_bool pauseSyncRequested_{false};
	std::atomic<int64_t> timelineOffsetUsec_{0};
	double effectiveSeconds_ = 0.0;
	uint64_t currentVideoCaptureNs_ = 0;
	std::atomic<uint64_t> emittedVideoTimestampNs_{0};
	std::string detail_;
	std::string pendingErrorDetail_;
};

} // namespace dynamic_delay
