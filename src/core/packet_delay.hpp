#pragma once

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace obs_delay::core {

using TimePoint = std::chrono::milliseconds;
using Duration = std::chrono::milliseconds;

enum class DelayState {
	Bypass,
	Preparing,
	Filling,
	Delayed,
	Returning,
	Error,
};

[[nodiscard]] std::string_view to_string(DelayState state) noexcept;

enum class StreamKind : std::uint8_t {
	Video,
	Audio,
};

struct StreamKey {
	StreamKind kind{StreamKind::Video};
	std::uint32_t track{0};
	// Must change whenever an encoder/timebase/codec epoch is replaced.
	std::uint64_t generation{0};

	friend constexpr bool operator==(const StreamKey &, const StreamKey &) = default;
	friend constexpr auto operator<=>(const StreamKey &, const StreamKey &) = default;
};

struct StreamKeyHash {
	[[nodiscard]] std::size_t operator()(const StreamKey &key) const noexcept;
};

// OBS timestamps are expressed in ticks of 1 / denominator seconds.
// numerator is retained for format identity/frame-step metadata, but does not
// scale pts/dts values.
struct TimeBase {
	std::int64_t numerator{1};
	std::int64_t denominator{1'000};

	[[nodiscard]] constexpr bool valid() const noexcept
	{
		return numerator > 0 && denominator > 0;
	}

	friend constexpr bool operator==(const TimeBase &, const TimeBase &) = default;
};

// A read-only payload view with shared ownership. alias() allows an adapter to
// retain an externally allocated encoded packet without copying its bytes.
class SharedPayload final {
public:
	SharedPayload() noexcept = default;
	SharedPayload(const SharedPayload &) = default;
	SharedPayload &operator=(const SharedPayload &) = default;
	SharedPayload(SharedPayload &&other) noexcept;
	SharedPayload &operator=(SharedPayload &&other) noexcept;

	[[nodiscard]] static SharedPayload copy(std::span<const std::byte> bytes);
	[[nodiscard]] static SharedPayload from_vector(
		std::shared_ptr<const std::vector<std::byte>> bytes);
	[[nodiscard]] static SharedPayload alias(std::shared_ptr<const void> owner,
		const std::byte *data, std::size_t size,
		std::size_t retained_size = 0);

	[[nodiscard]] const std::byte *data() const noexcept { return data_; }
	[[nodiscard]] std::size_t size() const noexcept { return size_; }
	[[nodiscard]] std::size_t retained_size() const noexcept { return retained_size_; }
	[[nodiscard]] bool empty() const noexcept { return size_ == 0; }
	[[nodiscard]] const std::shared_ptr<const void> &owner() const noexcept { return owner_; }

private:
	SharedPayload(std::shared_ptr<const void> owner, const std::byte *data,
		std::size_t size, std::size_t retained_size) noexcept;

	std::shared_ptr<const void> owner_{};
	const std::byte *data_{nullptr};
	std::size_t size_{0};
	std::size_t retained_size_{0};
};

struct PacketMetadata {
	StreamKey stream{};
	std::int64_t pts{0};
	std::int64_t dts{0};
	std::int64_t duration{0};
	TimeBase time_base{};
	bool keyframe{false};
	std::uint32_t flags{0};
};

struct Packet {
	PacketMetadata metadata{};
	SharedPayload payload{};
};

struct OutputPacket {
	Packet packet{};
	TimePoint received_at{};
	TimePoint released_at{};
	std::uint64_t sequence{0};
	bool delayed{false};
};

enum class RequestResult {
	Accepted,
	NoOp,
	InvalidState,
	InvalidDelay,
	NotReady,
	ClockRegression,
};

struct DelayConfig {
	Duration max_delay{std::chrono::minutes{10}};
	Duration retention_headroom{std::chrono::seconds{2}};

	// All zero-valued byte limits mean unlimited. max_buffered_packets is kept
	// finite by default so metadata-only packets cannot grow memory without bound.
	std::size_t max_payload_bytes{0};
	std::size_t max_buffered_packets{200'000};
	std::size_t max_streams{64};
	std::size_t metadata_bytes_per_packet{128};
	std::size_t max_retained_bytes{512U * 1024U * 1024U};

	// A zero reorder window assumes the adapter supplies interleaved input. Even
	// then the persistent per-stream watermark prevents one lane overtaking a
	// temporarily empty lane. A stale lane is ignored after this timeout for
	// interleave and live-return readiness; zero waits indefinitely.
	Duration reorder_window{0};
	Duration stream_stall_timeout{std::chrono::seconds{2}};
	// Zero disables gap validation. Otherwise every adjacent packet in the
	// selected fill window must arrive within this bound.
	Duration max_stream_gap{std::chrono::seconds{1}};
	bool require_video_keyframe{true};
	// When non-empty, every listed stream is mandatory for fill readiness and
	// for the persistent interleave watermark. With an empty roster, streams are
	// inferred from those observed during Filling.
	std::vector<StreamKey> required_streams{};
};

struct StreamMetrics {
	StreamKey stream{};
	std::size_t buffered_packets{0};
	std::size_t buffered_payload_bytes{0};
	std::size_t retained_bytes{0};
	Duration buffered_span{0};
	Duration oldest_packet_age{0};
};

struct DelayMetrics {
	DelayState state{DelayState::Bypass};
	Duration target_delay{0};
	double fill_progress{0.0};
	bool ready_for_delayed{false};
	bool ready_for_return{false};
	std::size_t buffered_packets{0};
	std::size_t buffered_payload_bytes{0};
	std::size_t retained_bytes{0};
	std::size_t active_streams{0};
	std::uint64_t bypassed_packets{0};
	std::uint64_t emitted_delayed_packets{0};
	std::uint64_t pruned_retention_packets{0};
	std::uint64_t rejected_capacity_packets{0};
	std::uint64_t pruned_payload_bytes{0};
};

// Payload-only estimate (allocator/container overhead is represented separately
// by DelayConfig::metadata_bytes_per_packet). Saturates instead of overflowing.
[[nodiscard]] std::size_t estimate_payload_bytes(std::uint64_t total_bits_per_second,
	Duration delay) noexcept;

class PacketDelay final {
public:
	explicit PacketDelay(DelayConfig config = {});
	~PacketDelay() = default;

	PacketDelay(const PacketDelay &) = delete;
	PacketDelay &operator=(const PacketDelay &) = delete;
	PacketDelay(PacketDelay &&) = delete;
	PacketDelay &operator=(PacketDelay &&) = delete;

	// Initial activation is two-phase. The host first requests activation, puts
	// its cover/filler on air, then confirms that with begin_filling().
	[[nodiscard]] RequestResult request_activate(Duration delay, TimePoint now);
	[[nodiscard]] RequestResult begin_filling(TimePoint now);

	// An active target can be changed without discarding buffered history. From
	// Delayed this returns to Preparing while the old delayed branch continues;
	// begin_filling() commits the new target after the cover is on air.
	[[nodiscard]] RequestResult request_change_delay(Duration delay, TimePoint now);

	// Filling never changes state or releases packets merely because a timer
	// expired. The host observes ready_for_delayed(), performs its transition,
	// then explicitly commits the cutover.
	[[nodiscard]] bool ready_for_delayed(TimePoint now) const;
	[[nodiscard]] RequestResult commit_delayed(TimePoint now);

	[[nodiscard]] RequestResult request_cancel(TimePoint now);
	[[nodiscard]] RequestResult request_deactivate(TimePoint now);

	// Returning keeps the delayed branch flowing until every video stream active
	// at the request has a still-buffered keyframe (or reached the configured
	// stall timeout). Early keyframes are consumed and replaced by a later one so
	// one lane never freezes all others. New optional streams do not hold the
	// transaction open. The host then commits the clean live cut.
	[[nodiscard]] bool ready_for_return() const;
	[[nodiscard]] bool ready_for_return(TimePoint now) const;
	[[nodiscard]] RequestResult commit_return(TimePoint now);
	[[nodiscard]] RequestResult finish_return(TimePoint now);

	// Error is fail-open: queues are discarded and subsequent packets pass live.
	void report_error(std::string message);
	[[nodiscard]] RequestResult recover(TimePoint now);

	[[nodiscard]] std::vector<OutputPacket> push(Packet packet, TimePoint now);
	[[nodiscard]] std::vector<OutputPacket> poll(TimePoint now);
	void prune(TimePoint now);
	// Records a final packet emitted by a host-owned path (for example the cover
	// encoder used during Filling). Reporting every such packet lets the next
	// core-managed epoch clear both the prior DTS and PTS reorder horizon.
	[[nodiscard]] bool observe_external_output(PacketMetadata metadata);

	[[nodiscard]] DelayState state() const;
	[[nodiscard]] Duration target_delay() const;
	[[nodiscard]] std::string error_message() const;
	[[nodiscard]] DelayMetrics metrics(TimePoint now) const;
	[[nodiscard]] std::vector<StreamMetrics> stream_metrics(TimePoint now) const;

private:
	struct QueuedPacket {
		Packet packet{};
		TimePoint received_at{};
		std::uint64_t sequence{0};
		std::size_t retained_bytes{0};
	};

	struct CutPlan {
		std::unordered_map<StreamKey, std::uint64_t, StreamKeyHash> start_sequence{};
	};
	struct TimestampHorizon {
		std::int64_t value{0};
		TimeBase time_base{};
		long double normalized{0.0L};
	};

	using PacketQueue = std::deque<QueuedPacket>;
	using QueueMap = std::unordered_map<StreamKey, PacketQueue, StreamKeyHash>;
	using StreamSet = std::unordered_set<StreamKey, StreamKeyHash>;
	using SeenMap = std::unordered_map<StreamKey, TimePoint, StreamKeyHash>;
	using SequenceMap = std::unordered_map<StreamKey, std::uint64_t, StreamKeyHash>;
	using TimestampMap = std::unordered_map<StreamKey, std::int64_t, StreamKeyHash>;
	using HorizonMap =
		std::unordered_map<StreamKey, TimestampHorizon, StreamKeyHash>;

	[[nodiscard]] bool observe_time_locked(TimePoint now);
	void enter_error_locked(std::string message);
	void clear_all_locked() noexcept;
	void clear_delay_queues_locked() noexcept;
	void reset_transition_locked() noexcept;
	[[nodiscard]] bool valid_delay_locked(Duration delay) const noexcept;
	[[nodiscard]] Duration reported_target_locked() const noexcept;

	[[nodiscard]] bool enqueue_locked(Packet &packet, TimePoint now,
		std::uint64_t sequence);
	[[nodiscard]] bool prepare_stream_locked(const StreamKey &stream);
	[[nodiscard]] bool create_timestamp_offset_locked(const Packet &packet,
		Duration base_delay);
	[[nodiscard]] bool unify_timestamp_offsets_locked(
		const std::vector<const Packet *> &epoch_fronts);
	[[nodiscard]] bool begin_timestamp_epoch_locked();
	[[nodiscard]] bool retimestamp_live_locked(Packet &packet);
	[[nodiscard]] bool note_output_timestamp_locked(const PacketMetadata &metadata);
	[[nodiscard]] std::optional<TimestampHorizon> ordering_dts_locked(
		const Packet &packet) const noexcept;
	[[nodiscard]] std::vector<OutputPacket> drain_ready_locked(TimePoint now);
	[[nodiscard]] std::vector<OutputPacket> drain_return_pending_locked(TimePoint now);
	void prune_locked(TimePoint now);
	void prune_prefix_locked(PacketQueue &queue, std::uint64_t keep_sequence);

	[[nodiscard]] std::optional<CutPlan> cut_plan_locked(TimePoint now) const;
	void apply_cut_plan_locked(const CutPlan &plan);
	void start_return_locked(bool from_delayed, TimePoint now);
	void observe_return_keyframe_locked(const QueuedPacket &packet);
	[[nodiscard]] bool return_ready_locked(TimePoint now) const noexcept;
	[[nodiscard]] bool build_return_pending_locked();

	[[nodiscard]] double fill_progress_locked(TimePoint now) const;
	[[nodiscard]] bool stream_is_stalled_locked(const StreamKey &stream,
		TimePoint now) const noexcept;

	DelayConfig config_{};
	mutable std::mutex mutex_{};
	DelayState state_{DelayState::Bypass};
	Duration target_delay_{0};
	Duration pending_target_delay_{0};
	Duration previous_target_delay_{0};
	std::optional<TimePoint> filling_started_at_{};
	std::optional<TimePoint> last_observed_at_{};
	std::string error_message_{};

	QueueMap queues_{};
	std::deque<QueuedPacket> return_pending_{};
	StreamSet required_streams_{};
	StreamSet known_timelines_{};
	StreamSet active_streams_{};
	HorizonMap latest_dts_{};
	SeenMap last_stream_seen_{};
	TimestampMap timestamp_offsets_{};
	HorizonMap last_output_dts_{};
	HorizonMap max_output_pts_{};
	SequenceMap return_keyframes_{};
	StreamSet return_required_video_{};
	std::optional<TimePoint> return_requested_at_{};
	std::optional<TimestampHorizon> last_emitted_dts_{};

	bool preparing_change_{false};
	bool change_origin_delayed_{false};
	bool return_from_delayed_{false};
	bool return_committed_{false};

	std::size_t buffered_packets_{0};
	std::size_t buffered_payload_bytes_{0};
	std::size_t retained_bytes_{0};
	std::uint64_t next_sequence_{0};
	std::uint64_t bypassed_packets_{0};
	std::uint64_t emitted_delayed_packets_{0};
	std::uint64_t pruned_retention_packets_{0};
	std::uint64_t rejected_capacity_packets_{0};
	std::uint64_t pruned_payload_bytes_{0};
};

} // namespace obs_delay::core
