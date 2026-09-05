#include "output-session-test-support.hpp"

#include "output-session.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using dynamic_delay::DelayState;
using dynamic_delay::OutputSession;

namespace {

#define CHECK(expression)                                                                                                \
	do {                                                                                                            \
		if (!(expression)) {                                                                                    \
			std::ostringstream message;                                                                     \
			message << __FILE__ << ':' << __LINE__ << ": CHECK(" #expression ") failed";                       \
			throw std::runtime_error(message.str());                                                         \
		}                                                                                                       \
	} while (false)

constexpr uint64_t HoldId = 1'000'000;
constexpr uint64_t PrimaryId = 10'000;

struct Sample {
	uint64_t id = 0;
	int64_t dts = 0;
	int64_t pts = 0;
	int64_t dtsUsec = 0;
	int64_t systemDtsUsec = 0;
	std::size_t track = 0;
	bool keyframe = false;
	obs_encoder_t *encoder = nullptr;
};

class Fixture {
public:
	explicit Fixture(const std::size_t tracks = 1) : audioTracks(tracks)
	{
		output_test::set_clock(1'000'000'000ULL);
		output.video[0] = &video;
		for (std::size_t index = 0; index < audioTracks; ++index) {
			audio[index].type = OBS_ENCODER_AUDIO;
			audio[index].id = "fake-aac";
			audio[index].settings.bitrate = 160;
			output.audio[index] = &audio[index];
		}
		session = std::make_unique<OutputSession>(&output, "Test recording", [this] { ++notifications; });
		std::string error;
		CHECK(session->attach(error));
		CHECK(output.refs == 2);
	}

	~Fixture() { session.reset(); }

	void clock(const int64_t milliseconds)
	{
		CHECK(milliseconds >= 0);
		output_test::set_clock(1'000'000'000ULL + static_cast<uint64_t>(milliseconds) * 1'000'000ULL);
	}

	void activate(const uint32_t seconds = 1)
	{
		std::string error;
		CHECK(session->request_delay(seconds, output_test::media_hub(), error));
		CHECK(session->state() == DelayState::Preparing);
	}

	Sample primary(const int64_t milliseconds, const obs_encoder_type kind = OBS_ENCODER_VIDEO,
		       const bool keyframe = false, const std::size_t track = 0, const int64_t compositionUsec = 0,
		       const std::size_t bytes = 64, const int64_t timestampOverride = -1,
		       const uint64_t exactCaptureNs = 0)
	{
		clock(milliseconds);
		const int64_t timestamp = timestampOverride >= 0 ? timestampOverride : milliseconds * 1'000;
		auto packet = output_test::make_packet(kind, timestamp, PrimaryId + static_cast<uint64_t>(milliseconds),
						       keyframe, track, compositionUsec, bytes);
		if (kind == OBS_ENCODER_VIDEO)
			packet.timebase_num = videoTimebaseNumerator;
		encoder_packet_time timing{};
		timing.cts = exactCaptureNs;
		output_test::emit(output, packet, exactCaptureNs ? &timing : nullptr);
		Sample sample{output_test::payload_id(packet),
			      packet.dts,
			      packet.pts,
			      packet.dts_usec,
			      packet.sys_dts_usec,
			      packet.track_idx,
			      packet.keyframe,
			      packet.encoder};
		obs_encoder_packet_release(&packet);
		return sample;
	}

	void hold(const int64_t milliseconds, const obs_encoder_type kind = OBS_ENCODER_VIDEO,
		  const bool keyframe = false, const std::size_t track = 0, const int64_t compositionUsec = 0,
		  const std::size_t bytes = 64)
	{
		clock(milliseconds);
		auto packet = output_test::make_packet(kind, milliseconds * 1'000,
						       HoldId + static_cast<uint64_t>(milliseconds), keyframe, track,
						       compositionUsec, bytes);
		if (kind == OBS_ENCODER_VIDEO)
			packet.timebase_num = videoTimebaseNumerator;
		session->receive_hold_packet(&packet);
		obs_encoder_packet_release(&packet);
	}

	void hold_tick(const int64_t milliseconds, const bool keyframe = false)
	{
		hold(milliseconds, OBS_ENCODER_VIDEO, keyframe);
		for (std::size_t track = 0; track < audioTracks; ++track)
			hold(milliseconds, OBS_ENCODER_AUDIO, false, track);
	}

	std::vector<Sample> primary_tick(const int64_t milliseconds, const bool keyframe = false)
	{
		std::vector<Sample> result;
		result.push_back(primary(milliseconds, OBS_ENCODER_VIDEO, keyframe));
		for (std::size_t track = 0; track < audioTracks; ++track)
			result.push_back(primary(milliseconds, OBS_ENCODER_AUDIO, false, track));
		return result;
	}

	void prepare(const int64_t finalLiveCompositionUsec = 0)
	{
		activate();
		for (int64_t time = 0; time < 500; time += 50) {
			if (time == 450 && finalLiveCompositionUsec != 0) {
				(void)primary(time, OBS_ENCODER_VIDEO, false, 0, finalLiveCompositionUsec);
				for (std::size_t track = 0; track < audioTracks; ++track)
					(void)primary(time, OBS_ENCODER_AUDIO, false, track);
			} else {
				(void)primary_tick(time, time == 0);
			}
			hold_tick(time, time == 0);
		}
		hold_tick(500);
		const auto cut = primary_tick(500, true);
		CHECK(session->state() == DelayState::Filling);
		CHECK(cut[0].id == HoldId);
		CHECK(cut[0].keyframe);
	}

	void fill_until(const int64_t end)
	{
		for (int64_t time = 550; time <= end; time += 50) {
			hold_tick(time, time % 500 == 0);
			(void)primary_tick(time, time % 500 == 0);
		}
	}

	obs_encoder_t video;
	std::array<obs_encoder_t, MAX_OUTPUT_AUDIO_ENCODERS> audio;
	obs_output_t output;
	std::unique_ptr<OutputSession> session;
	std::size_t audioTracks;
	int32_t videoTimebaseNumerator = 50'000;
	std::atomic_int notifications{0};
};

void validates_real_output_layout()
{
	Fixture fixture;
	std::string error;
	fixture.output.flags |= OBS_OUTPUT_MULTI_TRACK_VIDEO;
	CHECK(fixture.session->attach(error)); // Hybrid MP4 capability, one active video lane.
	fixture.output.video[1] = &fixture.video;
	CHECK(!fixture.session->attach(error));
	fixture.output.video[1] = nullptr;
	fixture.output.nativeDelay = 30;
	CHECK(!fixture.session->attach(error));
	fixture.output.nativeDelay = 0;
	fixture.output.flags = OBS_OUTPUT_AV;
	CHECK(!fixture.session->attach(error));
	fixture.output.flags = OBS_OUTPUT_ENCODED | OBS_OUTPUT_AV;
	fixture.output.audio[0] = nullptr;
	CHECK(!fixture.session->attach(error));
}

void bypass_preserves_carrier_and_does_not_retain_packets()
{
	Fixture fixture;
	for (int64_t time = 0; time < 3'000; time += 50) {
		const auto emitted = fixture.primary(time, OBS_ENCODER_VIDEO, true, 0, 100'000);
		CHECK(emitted.id == PrimaryId + static_cast<uint64_t>(time));
		CHECK(emitted.dts == time * 1'000);
		CHECK(emitted.pts - emitted.dts == 100'000);
		CHECK(emitted.encoder == &fixture.video);
		CHECK(output_test::live_payloads() == 0);
	}
	CHECK(fixture.session->snapshot().bufferedBytes == 0);
}

void actual_callback_fills_then_releases_delayed_video_and_tracks()
{
	Fixture fixture{2};
	fixture.prepare();
	fixture.fill_until(1'450);
	CHECK(fixture.session->state() == DelayState::Filling);
	CHECK(fixture.session->snapshot().progress < 1.0);
	fixture.hold_tick(1'500, true);
	const auto splice = fixture.primary_tick(1'500, true);
	CHECK(fixture.session->state() == DelayState::Delayed);
	CHECK(splice[0].id == PrimaryId + 500);
	CHECK(splice[0].keyframe);
	CHECK(splice[0].encoder == &fixture.video);
	for (std::size_t track = 0; track < fixture.audioTracks; ++track) {
		CHECK(splice[track + 1].id == PrimaryId + 500);
		CHECK(splice[track + 1].track == track);
		CHECK(splice[track + 1].encoder == &fixture.audio[track]);
	}
	fixture.session->maintenance();
	CHECK(output_test::pipeline_behavior().stopped == 1);
	CHECK(output_test::pipeline_behavior().destroyed == 1);
	const auto continuation = fixture.primary_tick(1'550);
	CHECK(continuation[0].id == PrimaryId + 550);
	CHECK(continuation[0].systemDtsUsec == 2'550'000);
	CHECK(fixture.session->snapshot().progress == 1.0);
}

void cancellation_preparing_releases_encoder_outside_session_lock()
{
	Fixture fixture;
	fixture.activate();
	fixture.hold_tick(0, true);
	fixture.session->request_bypass();
	CHECK(fixture.session->state() == DelayState::Bypass);
	output_test::pipeline_behavior().onDestroy = [](OutputSession &owner) {
		(void)owner.snapshot();
	};
	output_test::pipeline_behavior().onStop = [](OutputSession &owner) {
		(void)owner.snapshot();
	};
	fixture.session->maintenance();
	CHECK(fixture.session->snapshot().bufferedBytes == 0);
	CHECK(output_test::live_payloads() == 0);
	CHECK(output_test::pipeline_behavior().destroyed == 1);
}

void cancellation_filling_holds_until_clean_live_keyframe()
{
	Fixture fixture;
	fixture.prepare();
	fixture.fill_until(750);
	fixture.session->request_bypass();
	CHECK(fixture.session->state() == DelayState::ReturningLive);
	fixture.hold_tick(800);
	const auto waiting = fixture.primary_tick(800);
	CHECK(waiting[0].id >= HoldId);
	const auto live = fixture.primary_tick(850, true);
	CHECK(fixture.session->state() == DelayState::Bypass);
	CHECK(live[0].id == PrimaryId + 850);
	CHECK(live[0].keyframe);
	CHECK(live[0].dtsUsec >= waiting[0].dtsUsec);
	fixture.session->maintenance();
	CHECK(fixture.session->snapshot().bufferedBytes == 0);
	CHECK(output_test::live_payloads() == 0);
}

void immediate_cancel_rearm_does_not_destroy_pipeline_under_callback_mutex()
{
	Fixture fixture;
	fixture.activate();
	fixture.hold_tick(0, true);
	fixture.session->request_bypass();
	// No maintenance tick between cancel and activate: this is the old
	// destructor-under-mutex deadlock. A callback may inspect the session while
	// the real HoldPipeline destructor is stopping/joining that callback.
	output_test::pipeline_behavior().onDestroy = [](OutputSession &owner) {
		(void)owner.snapshot();
	};
	fixture.activate();
	CHECK(output_test::pipeline_behavior().constructed == 2);
	CHECK(output_test::pipeline_behavior().destroyed == 1);
	CHECK(fixture.session->state() == DelayState::Preparing);
	fixture.session->request_bypass();
	fixture.session->maintenance();
	CHECK(output_test::pipeline_behavior().destroyed == 2);
}

void reconnect_clears_previous_timeline_before_either_first_packet_kind()
{
	for (const bool audioFirst : {false, true}) {
		Fixture fixture{2};
		fixture.prepare(250'000);
		fixture.fill_until(1'500);
		fixture.session->maintenance();
		fixture.session->request_bypass();
		const auto oldLive = fixture.primary(1'600, OBS_ENCODER_VIDEO, true);
		CHECK(oldLive.dtsUsec > 1'600'000); // Ensure the previous epoch really had a bridge.
		fixture.session->maintenance();
		output_test::reconnect(fixture.output);
		CHECK(!fixture.session->take_rearm_request());
		output_test::activate_output(fixture.output);
		CHECK(fixture.session->take_rearm_request());
		CHECK(!fixture.session->take_rearm_request());
		CHECK(fixture.session->state() == DelayState::Bypass);
		if (audioFirst) {
			auto priming = output_test::make_packet(OBS_ENCODER_AUDIO, -21'333, 777, false, 1);
			output_test::emit(fixture.output, priming);
			CHECK(priming.dts_usec == -21'333);
			CHECK(priming.dts == -21'333);
			obs_encoder_packet_release(&priming);
			const auto first = fixture.primary(2'000, OBS_ENCODER_AUDIO, false, 1, 0, 64, 0);
			CHECK(first.dtsUsec == 0);
			CHECK(first.pts == 0);
		}
		const auto video = fixture.primary(2'000, OBS_ENCODER_VIDEO, true, 0, 100'000, 64, 0);
		CHECK(video.dtsUsec == 0);
		CHECK(video.dts == 0);
		CHECK(video.pts == 100'000);
		CHECK(video.id == PrimaryId + 2'000);
		const auto audio = fixture.primary(2'050, OBS_ENCODER_AUDIO, false, 0, 0, 64, 50'000);
		CHECK(audio.dtsUsec == 50'000);
		CHECK(audio.pts == 50'000);
		fixture.session->maintenance();
		CHECK(output_test::live_payloads() == 0);
	}
}

void pause_during_fill_preserves_progress_until_media_clock_resumes()
{
	Fixture fixture;
	fixture.output.flags |= OBS_OUTPUT_CAN_PAUSE;
	fixture.prepare();
	fixture.fill_until(750);
	const auto before = fixture.session->snapshot();
	output_test::pause_output(fixture.output, true);
	fixture.session->maintenance();
	CHECK(output_test::pipeline_behavior().paused);
	CHECK(fixture.session->snapshot().paused);
	fixture.clock(30'750);
	fixture.session->maintenance();
	CHECK(fixture.session->state() == DelayState::Filling);
	CHECK(fixture.session->snapshot().progress == before.progress);
	CHECK(fixture.session->snapshot().bufferedBytes == before.bufferedBytes);
	output_test::pause_output(fixture.output, false);
	fixture.session->maintenance();
	CHECK(!output_test::pipeline_behavior().paused);
	CHECK(!fixture.session->snapshot().paused);
	for (int64_t mediaTime = 800; mediaTime <= 1'500; mediaTime += 50) {
		const auto wallTime = 30'000 + mediaTime;
		fixture.hold_tick(wallTime, mediaTime % 500 == 0);
		(void)fixture.primary(wallTime, OBS_ENCODER_VIDEO, mediaTime % 500 == 0, 0, 0, 64, mediaTime * 1'000);
		(void)fixture.primary(wallTime, OBS_ENCODER_AUDIO, false, 0, 0, 64, mediaTime * 1'000);
	}
	CHECK(fixture.session->state() == DelayState::Delayed);
}

void snapshot_and_control_are_safe_during_packet_delivery()
{
	Fixture fixture;
	fixture.prepare();
	fixture.fill_until(1'500);
	fixture.session->maintenance();
	std::atomic_bool started{false};
	std::atomic_bool done{false};
	std::exception_ptr failure;
	std::thread producer([&] {
		try {
			started.store(true, std::memory_order_release);
			for (int64_t time = 1'550; time < 10'000; time += 50)
				(void)fixture.primary_tick(time, time % 500 == 0);
		} catch (...) {
			failure = std::current_exception();
		}
		done.store(true, std::memory_order_release);
	});
	while (!started.load(std::memory_order_acquire))
		std::this_thread::yield();
	fixture.session->request_bypass();
	while (!done.load(std::memory_order_acquire)) {
		(void)fixture.session->snapshot();
		std::this_thread::yield();
	}
	producer.join();
	if (failure)
		std::rethrow_exception(failure);
	(void)fixture.primary_tick(10'000, true);
	fixture.session->maintenance();
	CHECK(fixture.session->state() == DelayState::Bypass);
	CHECK(output_test::live_payloads() == 0);
}

void delayed_return_uses_live_keyframe_and_preserves_b_frame_offset()
{
	Fixture fixture;
	fixture.prepare();
	fixture.fill_until(1'500);
	fixture.session->maintenance();
	fixture.session->request_bypass();
	const auto waiting = fixture.primary(1'550, OBS_ENCODER_VIDEO, false, 0, 150'000);
	CHECK(waiting.id == PrimaryId + 550);
	const auto live = fixture.primary(1'600, OBS_ENCODER_VIDEO, true, 0, 150'000);
	CHECK(fixture.session->state() == DelayState::Bypass);
	CHECK(live.id == PrimaryId + 1'600);
	CHECK(live.pts - live.dts == 150'000);
	CHECK(live.dtsUsec >= waiting.dtsUsec);
	fixture.session->maintenance();
	CHECK(output_test::live_payloads() == 0);
}

void hold_underrun_uses_safe_fallback_then_returns_live()
{
	Fixture fixture;
	fixture.prepare();
	// Stop supplying the auxiliary encoder while primary output continues.
	for (int64_t time = 550; time <= 1'050; time += 50)
		(void)fixture.primary_tick(time);
	CHECK(fixture.session->state() == DelayState::ReturningLive);
	const auto fallback = fixture.primary(1'100);
	CHECK(fallback.id >= HoldId);
	CHECK(fallback.keyframe);
	const auto live = fixture.primary(1'150, OBS_ENCODER_VIDEO, true);
	CHECK(live.id == PrimaryId + 1'150);
	CHECK(fixture.session->state() == DelayState::Error);
	fixture.session->maintenance();
	CHECK(output_test::live_payloads() == 0);
}

void missing_video_carriers_recover_av_alignment_at_a_buffered_keyframe()
{
	Fixture fixture{2};
	fixture.prepare();
	fixture.fill_until(1'500);
	fixture.session->maintenance();
	uint64_t previousVideo = PrimaryId + 500;
	bool sawRecoveryKeyframe = false;
	int64_t maxPresentedPts = 0;
	const auto composition = [](const int64_t sourceTime) {
		if (sourceTime <= 1'500)
			return int64_t{0};
		return sourceTime % 150 == 0 ? int64_t{100'000}
					     : (sourceTime % 150 == 50 ? int64_t{0} : int64_t{-50'000});
	};
	for (int64_t time = 1'550; time <= 5'000; time += 50) {
		Sample video;
		const bool missingVideo = time <= 2'500;
		if (!missingVideo) {
			video = fixture.primary(time, OBS_ENCODER_VIDEO, time % 500 == 0, 0, composition(time));
			CHECK(video.id >= previousVideo);
			CHECK(video.pts - video.dts == composition(static_cast<int64_t>(video.id - PrimaryId)));
			if (video.id > previousVideo + 50 && video.keyframe) {
				CHECK(video.pts > maxPresentedPts);
				sawRecoveryKeyframe = true;
			}
			maxPresentedPts = std::max(maxPresentedPts, video.pts);
			previousVideo = video.id;
		}
		for (std::size_t track = 0; track < fixture.audioTracks; ++track) {
			const auto audio = fixture.primary(time, OBS_ENCODER_AUDIO, false, track);
			if (time >= 3'500) {
				const auto difference = static_cast<int64_t>(audio.id) - static_cast<int64_t>(video.id);
				CHECK(difference >= -100 && difference <= 100);
			}
		}
		CHECK(fixture.session->state() == DelayState::Delayed);
	}
	CHECK(sawRecoveryKeyframe);
	const double actualAge = static_cast<double>(5'000 - static_cast<int64_t>(previousVideo - PrimaryId)) / 1000.0;
	CHECK(fixture.session->snapshot().effectiveSeconds == actualAge);
}

void arrival_jitter_without_media_gaps_keeps_every_delayed_packet()
{
	Fixture fixture;
	fixture.prepare();
	fixture.fill_until(1'500);
	fixture.session->maintenance();
	for (int64_t mediaTime = 1'550; mediaTime <= 3'000; mediaTime += 50) {
		const auto wallTime = mediaTime + 2'000;
		const auto video =
			fixture.primary(wallTime, OBS_ENCODER_VIDEO, mediaTime % 500 == 0, 0, 0, 64, mediaTime * 1'000);
		(void)fixture.primary(wallTime, OBS_ENCODER_AUDIO, false, 0, 0, 64, mediaTime * 1'000);
		// Payload identifiers before the jitter still encode their original
		// wall-clock time. The old encoded backlog must not be skipped merely
		// because callback delivery was late.
		if (mediaTime <= 2'500)
			CHECK(video.id == PrimaryId + static_cast<uint64_t>(mediaTime - 1'000));
		CHECK(fixture.session->state() == DelayState::Delayed);
	}
}

void missing_one_audio_track_recovers_without_persistent_av_skew()
{
	Fixture fixture{2};
	fixture.prepare();
	fixture.fill_until(1'500);
	fixture.session->maintenance();
	for (int64_t time = 1'550; time <= 5'000; time += 50) {
		const auto video = fixture.primary(time, OBS_ENCODER_VIDEO, time % 500 == 0);
		const auto firstAudio = fixture.primary(time, OBS_ENCODER_AUDIO, false, 0);
		if (time > 2'500) {
			const auto resumedAudio = fixture.primary(time, OBS_ENCODER_AUDIO, false, 1);
			if (time >= 3'500) {
				const auto firstSkew =
					static_cast<int64_t>(firstAudio.id) - static_cast<int64_t>(video.id);
				const auto resumedSkew =
					static_cast<int64_t>(resumedAudio.id) - static_cast<int64_t>(video.id);
				CHECK(firstSkew >= -100 && firstSkew <= 100);
				CHECK(resumedSkew >= -100 && resumedSkew <= 100);
			}
		}
		CHECK(fixture.session->state() == DelayState::Delayed);
	}
}

void isolated_video_losses_during_fill_do_not_accumulate_av_skew()
{
	Fixture fixture{2};
	fixture.prepare();
	for (int64_t time = 550; time <= 1'500; time += 50) {
		fixture.hold_tick(time, time % 500 == 0);
		// A single missing frame creates a two-step source delta. Repeating
		// those small holes must not survive as cumulative skew after filling.
		const bool missingVideo = time <= 1'050 && time % 100 == 50;
		if (!missingVideo)
			(void)fixture.primary(time, OBS_ENCODER_VIDEO, time % 500 == 0);
		for (std::size_t track = 0; track < fixture.audioTracks; ++track)
			(void)fixture.primary(time, OBS_ENCODER_AUDIO, false, track);
	}
	CHECK(fixture.session->state() == DelayState::Delayed);
	fixture.session->maintenance();
	for (int64_t time = 1'550; time <= 5'000; time += 50) {
		const auto emitted = fixture.primary_tick(time, time % 500 == 0);
		if (time >= 3'000) {
			for (std::size_t track = 0; track < fixture.audioTracks; ++track) {
				const auto skew = static_cast<int64_t>(emitted[track + 1].id) -
						  static_cast<int64_t>(emitted[0].id);
				CHECK(skew >= -100 && skew <= 100);
			}
		}
		CHECK(fixture.session->state() == DelayState::Delayed);
	}
}

void audio_track_hole_during_fill_is_recovered_after_activation()
{
	Fixture fixture{2};
	fixture.prepare();
	for (int64_t time = 550; time <= 1'500; time += 50) {
		fixture.hold_tick(time, time % 500 == 0);
		(void)fixture.primary(time, OBS_ENCODER_VIDEO, time % 500 == 0);
		(void)fixture.primary(time, OBS_ENCODER_AUDIO, false, 0);
		if (time > 1'050)
			(void)fixture.primary(time, OBS_ENCODER_AUDIO, false, 1);
	}
	CHECK(fixture.session->state() == DelayState::Delayed);
	fixture.session->maintenance();
	// All carriers now have perfect cadence: recovery must remember the
	// source gap buffered during Filling, not wait for a new missing packet.
	for (int64_t time = 1'550; time <= 5'000; time += 50) {
		const auto emitted = fixture.primary_tick(time, time % 500 == 0);
		if (time >= 3'000) {
			for (std::size_t track = 0; track < fixture.audioTracks; ++track) {
				const auto skew = static_cast<int64_t>(emitted[track + 1].id) -
						  static_cast<int64_t>(emitted[0].id);
				CHECK(skew >= -100 && skew <= 100);
			}
		}
		CHECK(fixture.session->state() == DelayState::Delayed);
	}
}

void preview_timestamp_uses_composition_time_of_emitted_payload()
{
	Fixture fixture;
	fixture.prepare();
	fixture.fill_until(1'500);
	fixture.session->maintenance();
	constexpr uint64_t ExactCapture = 2'500'000'123ULL;
	(void)fixture.primary(1'550, OBS_ENCODER_VIDEO, false, 0, 100'000, 64, -1, ExactCapture);
	(void)fixture.primary(1'550, OBS_ENCODER_AUDIO);
	for (int64_t time = 1'600; time <= 2'500; time += 50)
		(void)fixture.primary_tick(time, time % 500 == 0);
	const auto emitted = fixture.primary(2'550);
	CHECK(emitted.id == PrimaryId + 1'550);
	CHECK(emitted.pts - emitted.dts == 100'000);
	CHECK(fixture.session->snapshot().emittedVideoTimestampNs == ExactCapture);
}

void divided_video_rate_uses_packet_increment_for_b_frame_bridge_and_gap_recovery()
{
	Fixture fixture{2};
	// OBS has already multiplied timebase_num by the encoder FPS divisor:
	// 20 fps / 2 means 100,000 denominator ticks per actual video frame.
	fixture.videoTimebaseNumerator = 100'000;
	fixture.activate();
	for (int64_t time = 0; time < 500; time += 50) {
		if (time % 100 == 0) {
			(void)fixture.primary(time, OBS_ENCODER_VIDEO, time == 0, 0, time == 400 ? 300'000 : 0);
			fixture.hold(time, OBS_ENCODER_VIDEO, time == 0);
		}
		for (std::size_t track = 0; track < fixture.audioTracks; ++track) {
			(void)fixture.primary(time, OBS_ENCODER_AUDIO, false, track);
			fixture.hold(time, OBS_ENCODER_AUDIO, false, track);
		}
	}
	fixture.hold_tick(500);
	const auto splice = fixture.primary_tick(500, true);
	CHECK(fixture.session->state() == DelayState::Filling);
	CHECK(splice[0].id == HoldId);
	// Previous maximum video PTS is 400+300 ms. The new GOP must begin
	// exactly one actual 100 ms video step later, not 50 ms or double 100 ms.
	CHECK(splice[0].dtsUsec == 800'000);
	CHECK(splice[0].pts == 800'000);
	for (int64_t time = 550; time <= 1'500; time += 50) {
		if (time % 100 == 0) {
			fixture.hold(time, OBS_ENCODER_VIDEO, time % 500 == 0);
			if (time != 900 && time != 1'100 && time != 1'200)
				(void)fixture.primary(time, OBS_ENCODER_VIDEO, time % 500 == 0);
		}
		for (std::size_t track = 0; track < fixture.audioTracks; ++track) {
			fixture.hold(time, OBS_ENCODER_AUDIO, false, track);
			(void)fixture.primary(time, OBS_ENCODER_AUDIO, false, track);
		}
	}
	CHECK(fixture.session->state() == DelayState::Delayed);
	fixture.session->maintenance();
	Sample video;
	for (int64_t time = 1'550; time <= 5'000; time += 50) {
		if (time % 100 == 0)
			video = fixture.primary(time, OBS_ENCODER_VIDEO, time % 500 == 0);
		for (std::size_t track = 0; track < fixture.audioTracks; ++track) {
			const auto audio = fixture.primary(time, OBS_ENCODER_AUDIO, false, track);
			if (time >= 3'000 && time % 100 == 0) {
				const auto skew = static_cast<int64_t>(audio.id) - static_cast<int64_t>(video.id);
				CHECK(skew >= -200 && skew <= 200);
			}
		}
		CHECK(fixture.session->state() == DelayState::Delayed);
	}
}

void primary_capacity_limit_never_emits_an_unsafe_mid_gop_live_packet()
{
	Fixture fixture;
	fixture.prepare();
	constexpr std::size_t TooLarge = std::size_t{4} * 1024U * 1024U * 1024U + 1;
	fixture.hold_tick(550);
	const auto atLimit = fixture.primary(550, OBS_ENCODER_VIDEO, false, 0, 0, TooLarge);
	CHECK(fixture.session->state() == DelayState::ReturningLive);
	CHECK(atLimit.id >= HoldId);
	CHECK(fixture.session->snapshot().bufferedBytes < TooLarge);
	const auto live = fixture.primary(600, OBS_ENCODER_VIDEO, true);
	CHECK(live.id == PrimaryId + 600);
	CHECK(live.keyframe);
	fixture.session->maintenance();
	CHECK(output_test::live_payloads() == 0);
}

void encoder_start_and_format_failures_keep_original_output_live()
{
	Fixture fixture;
	output_test::pipeline_behavior().startSucceeds = false;
	std::string error;
	CHECK(!fixture.session->request_delay(1, output_test::media_hub(), error));
	CHECK(fixture.session->state() == DelayState::Error);
	CHECK(!error.empty());
	CHECK(fixture.primary(0).id == PrimaryId);
	fixture.session->maintenance();
	output_test::pipeline_behavior().startSucceeds = true;
	output_test::pipeline_behavior().compatible = false;
	fixture.activate();
	fixture.hold_tick(50, true);
	CHECK(fixture.session->state() == DelayState::Error);
	CHECK(fixture.primary(100).id == PrimaryId + 100);
	fixture.session->maintenance();
	CHECK(output_test::live_payloads() == 0);
}

void session_destruction_detaches_callbacks_and_releases_references()
{
	Fixture fixture;
	fixture.prepare();
	CHECK(output_test::live_payloads() > 0);
	fixture.session.reset();
	CHECK(fixture.output.callback == nullptr);
	CHECK(fixture.output.signals.reconnect == nullptr);
	CHECK(fixture.output.signals.activate == nullptr);
	CHECK(fixture.output.signals.pause == nullptr);
	CHECK(fixture.output.signals.unpause == nullptr);
	CHECK(fixture.output.refs == 1);
	CHECK(output_test::live_payloads() == 0);
}

} // namespace

int main()
{
	const std::vector<std::pair<const char *, std::function<void()>>> cases{
		{"production output layout validation", validates_real_output_layout},
		{"production bypass ownership", bypass_preserves_carrier_and_does_not_retain_packets},
		{"production fill and two audio tracks", actual_callback_fills_then_releases_delayed_video_and_tracks},
		{"production prepare cancellation and teardown",
		 cancellation_preparing_releases_encoder_outside_session_lock},
		{"production fill cancellation", cancellation_filling_holds_until_clean_live_keyframe},
		{"production immediate cancel and rearm",
		 immediate_cancel_rearm_does_not_destroy_pipeline_under_callback_mutex},
		{"production reconnect audio-first and video-first",
		 reconnect_clears_previous_timeline_before_either_first_packet_kind},
		{"production paused fill and media-clock resume",
		 pause_during_fill_preserves_progress_until_media_clock_resumes},
		{"production concurrent callback and control", snapshot_and_control_are_safe_during_packet_delivery},
		{"production delayed live return and B frames",
		 delayed_return_uses_live_keyframe_and_preserves_b_frame_offset},
		{"production hold underrun", hold_underrun_uses_safe_fallback_then_returns_live},
		{"production missing video carrier A/V recovery",
		 missing_video_carriers_recover_av_alignment_at_a_buffered_keyframe},
		{"production arrival jitter preserves content",
		 arrival_jitter_without_media_gaps_keeps_every_delayed_packet},
		{"production missing audio track A/V recovery",
		 missing_one_audio_track_recovers_without_persistent_av_skew},
		{"production isolated video losses during fill",
		 isolated_video_losses_during_fill_do_not_accumulate_av_skew},
		{"production audio track hole during fill", audio_track_hole_during_fill_is_recovered_after_activation},
		{"production preview uses emitted composition timestamp",
		 preview_timestamp_uses_composition_time_of_emitted_payload},
		{"production divided video rate B-frame bridge and recovery",
		 divided_video_rate_uses_packet_increment_for_b_frame_bridge_and_gap_recovery},
		{"production primary RAM bound", primary_capacity_limit_never_emits_an_unsafe_mid_gop_live_packet},
		{"production encoder errors", encoder_start_and_format_failures_keep_original_output_live},
		{"production callback lifetime", session_destruction_detaches_callbacks_and_releases_references},
	};
	std::size_t passed = 0;
	for (const auto &[name, test] : cases) {
		output_test::reset_pipeline_behavior();
		try {
			CHECK(output_test::live_payloads() == 0);
			test();
			CHECK(output_test::live_payloads() == 0);
			++passed;
			std::cout << "[PASS] " << name << '\n';
		} catch (const std::exception &error) {
			std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
		}
	}
	std::cout << passed << '/' << cases.size() << " production OutputSession tests passed\n";
	return passed == cases.size() ? 0 : 1;
}
