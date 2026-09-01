#include "packet_delay.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

namespace obs_delay::core {
namespace {

template<typename Integer>
void saturating_add(Integer &value, Integer increment) noexcept
{
	const auto maximum = std::numeric_limits<Integer>::max();
	value = increment > maximum - value ? maximum : value + increment;
}

[[nodiscard]] bool checked_add_size(std::size_t left, std::size_t right,
	std::size_t &result) noexcept
{
	if (right > std::numeric_limits<std::size_t>::max() - left)
		return false;
	result = left + right;
	return true;
}

[[nodiscard]] Duration elapsed(TimePoint now, TimePoint then) noexcept
{
	if (now <= then)
		return Duration{0};
	const auto now_count = now.count();
	const auto then_count = then.count();
	const auto maximum = std::numeric_limits<Duration::rep>::max();
	if (then_count < 0 && now_count > maximum + then_count)
		return Duration{maximum};
	return Duration{now_count - then_count};
}

[[nodiscard]] bool due(TimePoint now, TimePoint received_at, Duration delay) noexcept
{
	return now >= received_at && elapsed(now, received_at) >= delay;
}

[[nodiscard]] std::optional<long double> normalized_timestamp(std::int64_t timestamp,
	const TimeBase &time_base) noexcept
{
	if (!time_base.valid())
		return std::nullopt;
	const long double result =
		static_cast<long double>(timestamp) /
		static_cast<long double>(time_base.denominator);
	if (!std::isfinite(result))
		return std::nullopt;
	return result;
}

[[nodiscard]] std::optional<long double> normalized_dts(const Packet &packet) noexcept
{
	return normalized_timestamp(packet.metadata.dts, packet.metadata.time_base);
}

[[nodiscard]] StreamKey timeline_key(StreamKey stream) noexcept
{
	stream.generation = 0;
	return stream;
}

[[nodiscard]] std::uint64_t magnitude(std::int64_t value) noexcept
{
	return value >= 0 ? static_cast<std::uint64_t>(value)
		: static_cast<std::uint64_t>(-(value + 1)) + std::uint64_t{1};
}

struct UInt192 {
	std::array<std::uint32_t, 6> limbs{};
};

[[nodiscard]] bool multiply(UInt192 &value, std::uint64_t factor) noexcept
{
	const UInt192 source = value;
	UInt192 result;
	const std::array<std::uint32_t, 2> factor_limbs{
		static_cast<std::uint32_t>(factor),
		static_cast<std::uint32_t>(factor >> 32U),
	};
	for (std::size_t factor_index = 0; factor_index < factor_limbs.size();
		++factor_index) {
		std::uint64_t carry = 0;
		for (std::size_t source_index = 0;
			source_index + factor_index < result.limbs.size(); ++source_index) {
			const auto destination = source_index + factor_index;
			const std::uint64_t current =
				static_cast<std::uint64_t>(source.limbs[source_index]) *
					factor_limbs[factor_index] +
				result.limbs[destination] + carry;
			result.limbs[destination] = static_cast<std::uint32_t>(current);
			carry = current >> 32U;
		}
		if (carry != 0)
			return false;
	}
	value = result;
	return true;
}

[[nodiscard]] UInt192 product(std::uint64_t first, std::uint64_t second,
	std::uint64_t third) noexcept
{
	UInt192 result;
	result.limbs[0] = 1;
	const bool representable = multiply(result, first) && multiply(result, second) &&
		multiply(result, third);
	(void)representable;
	return result;
}

[[nodiscard]] int compare_unsigned(const UInt192 &left,
	const UInt192 &right) noexcept
{
	for (std::size_t index = left.limbs.size(); index-- > 0;) {
		if (left.limbs[index] < right.limbs[index])
			return -1;
		if (left.limbs[index] > right.limbs[index])
			return 1;
	}
	return 0;
}

// Exact comparison of timestamp / denominator. Six 32-bit limbs leave ample
// headroom for the signed 64-bit cross products.
[[nodiscard]] int compare_timestamps(std::int64_t left, const TimeBase &left_base,
	std::int64_t right, const TimeBase &right_base) noexcept
{
	if (left < 0 && right >= 0)
		return -1;
	if (left >= 0 && right < 0)
		return 1;
	const auto left_product = product(magnitude(left),
		std::uint64_t{1},
		static_cast<std::uint64_t>(right_base.denominator));
	const auto right_product = product(magnitude(right),
		std::uint64_t{1},
		static_cast<std::uint64_t>(left_base.denominator));
	const int comparison = compare_unsigned(left_product, right_product);
	return left < 0 ? -comparison : comparison;
}

[[nodiscard]] bool add_timestamp(std::int64_t value, std::int64_t offset,
	std::int64_t &result) noexcept
{
	if (offset > 0 && value > std::numeric_limits<std::int64_t>::max() - offset)
		return false;
	if (offset < 0 && value < std::numeric_limits<std::int64_t>::min() - offset)
		return false;
	result = value + offset;
	return true;
}

[[nodiscard]] std::optional<std::int64_t> delay_ticks(Duration delay,
	const TimeBase &time_base) noexcept
{
	if (delay.count() < 0 || !time_base.valid())
		return std::nullopt;
	const long double raw =
		(static_cast<long double>(delay.count()) *
			static_cast<long double>(time_base.denominator)) /
		1'000.0L;
	const long double rounded = std::round(raw);
	const long double upper_exclusive = std::ldexp(1.0L, 63);
	if (!std::isfinite(rounded) || rounded < 0.0L ||
		rounded >= upper_exclusive)
		return std::nullopt;
	return static_cast<std::int64_t>(rounded);
}

[[nodiscard]] std::optional<std::int64_t> duration_ticks_ceil(Duration duration,
	const TimeBase &time_base) noexcept
{
	if (duration.count() < 0 || !time_base.valid())
		return std::nullopt;
	if (duration.count() == 0)
		return std::int64_t{0};
	const long double raw =
		(static_cast<long double>(duration.count()) *
			static_cast<long double>(time_base.denominator)) /
		1'000.0L;
	const long double rounded = std::ceil(std::nextafter(raw,
		std::numeric_limits<long double>::infinity()));
	const long double upper_exclusive = std::ldexp(1.0L, 63);
	if (!std::isfinite(rounded) || rounded < 0.0L ||
		rounded >= upper_exclusive)
		return std::nullopt;
	return static_cast<std::int64_t>(rounded);
}

[[nodiscard]] bool retimestamp(Packet &packet, std::int64_t offset) noexcept
{
	std::int64_t pts = 0;
	std::int64_t dts = 0;
	if (!add_timestamp(packet.metadata.pts, offset, pts) ||
		!add_timestamp(packet.metadata.dts, offset, dts))
		return false;
	packet.metadata.pts = pts;
	packet.metadata.dts = dts;
	return true;
}

} // namespace

std::string_view to_string(DelayState state) noexcept
{
	switch (state) {
	case DelayState::Bypass:
		return "Bypass";
	case DelayState::Preparing:
		return "Preparing";
	case DelayState::Filling:
		return "Filling";
	case DelayState::Delayed:
		return "Delayed";
	case DelayState::Returning:
		return "Returning";
	case DelayState::Error:
		return "Error";
	}
	return "Unknown";
}

std::size_t StreamKeyHash::operator()(const StreamKey &key) const noexcept
{
	std::size_t seed = static_cast<std::size_t>(key.kind);
	seed ^= static_cast<std::size_t>(key.track) + 0x9e3779b9U + (seed << 6U) +
		(seed >> 2U);
	const auto generation = static_cast<std::size_t>(key.generation ^ (key.generation >> 32U));
	seed ^= generation + 0x9e3779b9U + (seed << 6U) + (seed >> 2U);
	return seed;
}

SharedPayload::SharedPayload(std::shared_ptr<const void> owner, const std::byte *data,
	std::size_t size, std::size_t retained_size) noexcept
	: owner_(std::move(owner)), data_(data), size_(size), retained_size_(retained_size)
{
}

SharedPayload::SharedPayload(SharedPayload &&other) noexcept
	: owner_(std::move(other.owner_)), data_(std::exchange(other.data_, nullptr)),
	  size_(std::exchange(other.size_, 0)),
	  retained_size_(std::exchange(other.retained_size_, 0))
{
}

SharedPayload &SharedPayload::operator=(SharedPayload &&other) noexcept
{
	if (this == &other)
		return *this;
	owner_ = std::move(other.owner_);
	data_ = std::exchange(other.data_, nullptr);
	size_ = std::exchange(other.size_, 0);
	retained_size_ = std::exchange(other.retained_size_, 0);
	return *this;
}

SharedPayload SharedPayload::copy(std::span<const std::byte> bytes)
{
	auto storage = std::make_shared<const std::vector<std::byte>>(bytes.begin(), bytes.end());
	return from_vector(std::move(storage));
}

SharedPayload SharedPayload::from_vector(
	std::shared_ptr<const std::vector<std::byte>> bytes)
{
	if (!bytes)
		throw std::invalid_argument("payload vector owner cannot be null");
	const auto *data = bytes->empty() ? nullptr : bytes->data();
	const auto size = bytes->size();
	const auto retained = bytes->capacity();
	return SharedPayload{std::move(bytes), data, size, retained};
}

SharedPayload SharedPayload::alias(std::shared_ptr<const void> owner,
	const std::byte *data, std::size_t size, std::size_t retained_size)
{
	if (size != 0 && (!owner || data == nullptr))
		throw std::invalid_argument("non-empty aliased payload needs an owner and data");
	if (retained_size == 0)
		retained_size = size;
	if (retained_size < size)
		throw std::invalid_argument("retained payload size cannot be smaller than its view");
	return SharedPayload{std::move(owner), size == 0 ? nullptr : data, size,
		retained_size};
}

std::size_t estimate_payload_bytes(std::uint64_t total_bits_per_second,
	Duration delay) noexcept
{
	if (delay.count() <= 0 || total_bits_per_second == 0)
		return 0;
	const long double bytes =
		(static_cast<long double>(total_bits_per_second) *
			static_cast<long double>(delay.count())) /
		8'000.0L;
	const auto maximum = static_cast<long double>(std::numeric_limits<std::size_t>::max());
	if (!std::isfinite(bytes) || bytes >= maximum)
		return std::numeric_limits<std::size_t>::max();
	return static_cast<std::size_t>(std::ceil(bytes));
}

PacketDelay::PacketDelay(DelayConfig config) : config_(config)
{
	if (config_.max_delay.count() < 0 || config_.retention_headroom.count() < 0 ||
		config_.reorder_window.count() < 0 || config_.stream_stall_timeout.count() < 0 ||
		config_.max_stream_gap.count() < 0)
		throw std::invalid_argument("delay durations cannot be negative");
	if (config_.max_delay.count() >
		std::numeric_limits<Duration::rep>::max() - config_.retention_headroom.count())
		throw std::invalid_argument("delay retention window overflows");
	if (config_.max_buffered_packets == 0)
		throw std::invalid_argument("max_buffered_packets must be finite and non-zero");
	if (config_.max_streams == 0)
		throw std::invalid_argument("max_streams must be finite and non-zero");
	if (config_.metadata_bytes_per_packet == 0)
		throw std::invalid_argument("metadata_bytes_per_packet must be non-zero");
	for (const auto &stream : config_.required_streams) {
		if (!required_streams_.insert(stream).second)
			throw std::invalid_argument("required stream roster contains duplicates");
		known_timelines_.insert(timeline_key(stream));
	}
	if (known_timelines_.size() > config_.max_streams)
		throw std::invalid_argument("required stream roster exceeds max_streams");
}

RequestResult PacketDelay::request_activate(Duration delay, TimePoint now)
{
	std::scoped_lock lock{mutex_};
	if (!observe_time_locked(now))
		return RequestResult::ClockRegression;
	if (!valid_delay_locked(delay))
		return RequestResult::InvalidDelay;
	if (state_ == DelayState::Preparing && !preparing_change_)
		return delay == pending_target_delay_ ? RequestResult::NoOp
			: RequestResult::InvalidState;
	if (state_ != DelayState::Bypass)
		return RequestResult::InvalidState;

	clear_all_locked();
	target_delay_ = delay;
	pending_target_delay_ = delay;
	previous_target_delay_ = Duration{0};
	error_message_.clear();
	state_ = DelayState::Preparing;
	return RequestResult::Accepted;
}

RequestResult PacketDelay::begin_filling(TimePoint now)
{
	std::scoped_lock lock{mutex_};
	if (!observe_time_locked(now))
		return RequestResult::ClockRegression;
	if (state_ == DelayState::Filling)
		return RequestResult::NoOp;
	if (state_ != DelayState::Preparing)
		return RequestResult::InvalidState;

	if (preparing_change_) {
		target_delay_ = pending_target_delay_;
		preparing_change_ = false;
	} else {
		clear_delay_queues_locked();
		target_delay_ = pending_target_delay_;
		change_origin_delayed_ = false;
	}
	filling_started_at_ = now;
	state_ = DelayState::Filling;
	return RequestResult::Accepted;
}

RequestResult PacketDelay::request_change_delay(Duration delay, TimePoint now)
{
	std::scoped_lock lock{mutex_};
	if (!observe_time_locked(now))
		return RequestResult::ClockRegression;
	if (!valid_delay_locked(delay))
		return RequestResult::InvalidDelay;

	switch (state_) {
	case DelayState::Bypass:
		clear_all_locked();
		target_delay_ = delay;
		pending_target_delay_ = delay;
		state_ = DelayState::Preparing;
		return RequestResult::Accepted;
	case DelayState::Preparing:
		if (delay == reported_target_locked())
			return RequestResult::NoOp;
		pending_target_delay_ = delay;
		if (!preparing_change_)
			target_delay_ = delay;
		return RequestResult::Accepted;
	case DelayState::Filling:
		if (delay == target_delay_)
			return RequestResult::NoOp;
		if (!change_origin_delayed_)
			previous_target_delay_ = target_delay_;
		target_delay_ = delay;
		pending_target_delay_ = delay;
		return RequestResult::Accepted;
	case DelayState::Delayed:
		if (delay == target_delay_)
			return RequestResult::NoOp;
		previous_target_delay_ = target_delay_;
		pending_target_delay_ = delay;
		preparing_change_ = true;
		change_origin_delayed_ = true;
		state_ = DelayState::Preparing;
		return RequestResult::Accepted;
	case DelayState::Returning:
	case DelayState::Error:
		return RequestResult::InvalidState;
	}
	return RequestResult::InvalidState;
}

bool PacketDelay::ready_for_delayed(TimePoint now) const
{
	std::scoped_lock lock{mutex_};
	return state_ == DelayState::Filling && cut_plan_locked(now).has_value();
}

RequestResult PacketDelay::commit_delayed(TimePoint now)
{
	std::scoped_lock lock{mutex_};
	if (!observe_time_locked(now))
		return RequestResult::ClockRegression;
	if (state_ != DelayState::Filling)
		return RequestResult::InvalidState;
	const auto plan = cut_plan_locked(now);
	if (!plan)
		return RequestResult::NotReady;
	try {
		apply_cut_plan_locked(*plan);
		if (!begin_timestamp_epoch_locked()) {
			enter_error_locked("cannot create a monotonic timestamp epoch");
			return RequestResult::InvalidState;
		}
	} catch (...) {
		enter_error_locked("allocation failed while committing delayed output");
		return RequestResult::InvalidState;
	}
	active_streams_ = required_streams_;
	for (const auto &[stream, queue] : queues_) {
		if (!queue.empty())
			active_streams_.insert(stream);
	}
	filling_started_at_.reset();
	preparing_change_ = false;
	change_origin_delayed_ = false;
	pending_target_delay_ = target_delay_;
	state_ = DelayState::Delayed;
	return RequestResult::Accepted;
}

RequestResult PacketDelay::request_cancel(TimePoint now)
{
	std::scoped_lock lock{mutex_};
	if (!observe_time_locked(now))
		return RequestResult::ClockRegression;
	if (state_ == DelayState::Bypass || state_ == DelayState::Returning)
		return RequestResult::NoOp;
	if (state_ == DelayState::Preparing && preparing_change_) {
		pending_target_delay_ = target_delay_;
		preparing_change_ = false;
		change_origin_delayed_ = false;
		state_ = DelayState::Delayed;
		return RequestResult::Accepted;
	}
	if (state_ == DelayState::Filling && change_origin_delayed_) {
		target_delay_ = previous_target_delay_;
		pending_target_delay_ = previous_target_delay_;
		preparing_change_ = false;
		change_origin_delayed_ = false;
		filling_started_at_.reset();
		state_ = DelayState::Delayed;
		return RequestResult::Accepted;
	}
	if (state_ != DelayState::Preparing && state_ != DelayState::Filling)
		return RequestResult::InvalidState;
	start_return_locked(change_origin_delayed_, now);
	return RequestResult::Accepted;
}

RequestResult PacketDelay::request_deactivate(TimePoint now)
{
	std::scoped_lock lock{mutex_};
	if (!observe_time_locked(now))
		return RequestResult::ClockRegression;
	if (state_ == DelayState::Bypass || state_ == DelayState::Returning)
		return RequestResult::NoOp;
	if (state_ == DelayState::Error)
		return RequestResult::InvalidState;
	const bool was_delayed = state_ == DelayState::Delayed ||
		(state_ == DelayState::Preparing && preparing_change_) || change_origin_delayed_;
	start_return_locked(was_delayed, now);
	return RequestResult::Accepted;
}

bool PacketDelay::ready_for_return() const
{
	std::scoped_lock lock{mutex_};
	const auto now = last_observed_at_.value_or(
		return_requested_at_.value_or(TimePoint{0}));
	return state_ == DelayState::Returning && return_ready_locked(now);
}

bool PacketDelay::ready_for_return(TimePoint now) const
{
	std::scoped_lock lock{mutex_};
	return state_ == DelayState::Returning && return_ready_locked(now);
}

RequestResult PacketDelay::commit_return(TimePoint now)
{
	std::scoped_lock lock{mutex_};
	if (!observe_time_locked(now))
		return RequestResult::ClockRegression;
	if (state_ != DelayState::Returning)
		return RequestResult::InvalidState;
	if (return_committed_)
		return RequestResult::NoOp;
	if (!return_ready_locked(now))
		return RequestResult::NotReady;
	try {
		if (!build_return_pending_locked()) {
			enter_error_locked("cannot create a monotonic live timestamp epoch");
			return RequestResult::InvalidState;
		}
	} catch (...) {
		enter_error_locked("allocation failed while committing live output");
		return RequestResult::InvalidState;
	}
	return_committed_ = true;
	return_from_delayed_ = false;
	return RequestResult::Accepted;
}

RequestResult PacketDelay::finish_return(TimePoint now)
{
	std::scoped_lock lock{mutex_};
	if (!observe_time_locked(now))
		return RequestResult::ClockRegression;
	if (state_ == DelayState::Bypass)
		return RequestResult::NoOp;
	if (state_ != DelayState::Returning)
		return RequestResult::InvalidState;
	if (!return_committed_ || !return_pending_.empty())
		return RequestResult::NotReady;
	clear_all_locked();
	target_delay_ = Duration{0};
	pending_target_delay_ = Duration{0};
	previous_target_delay_ = Duration{0};
	state_ = DelayState::Bypass;
	return RequestResult::Accepted;
}

void PacketDelay::report_error(std::string message)
{
	std::scoped_lock lock{mutex_};
	enter_error_locked(std::move(message));
}

RequestResult PacketDelay::recover(TimePoint now)
{
	std::scoped_lock lock{mutex_};
	if (!observe_time_locked(now))
		return RequestResult::ClockRegression;
	if (state_ != DelayState::Error)
		return RequestResult::NoOp;
	clear_all_locked();
	target_delay_ = Duration{0};
	pending_target_delay_ = Duration{0};
	previous_target_delay_ = Duration{0};
	error_message_.clear();
	state_ = DelayState::Bypass;
	return RequestResult::Accepted;
}

std::vector<OutputPacket> PacketDelay::push(Packet packet, TimePoint now)
{
	std::scoped_lock lock{mutex_};
	const auto sequence = next_sequence_++;
	if (!observe_time_locked(now)) {
		(void)retimestamp_live_locked(packet);
		saturating_add(bypassed_packets_, std::uint64_t{1});
		return {{std::move(packet), now, now, sequence, false}};
	}

	auto fail_open_current = [&]() {
		if (!retimestamp_live_locked(packet))
			enter_error_locked("cannot maintain monotonic fail-open timestamps");
		saturating_add(bypassed_packets_, std::uint64_t{1});
		return std::vector<OutputPacket>{{std::move(packet), now, now, sequence, false}};
	};

	switch (state_) {
	case DelayState::Bypass:
	case DelayState::Error:
		return fail_open_current();
	case DelayState::Preparing:
		if (!preparing_change_)
			return fail_open_current();
		if (!enqueue_locked(packet, now, sequence))
			return fail_open_current();
		prune_locked(now);
		return drain_ready_locked(now);
	case DelayState::Filling:
		if (!enqueue_locked(packet, now, sequence))
			return fail_open_current();
		prune_locked(now);
		return {};
	case DelayState::Delayed:
		if (!enqueue_locked(packet, now, sequence))
			return fail_open_current();
		{
			auto output = drain_ready_locked(now);
			if (state_ == DelayState::Delayed)
				prune_locked(now);
			return output;
		}
	case DelayState::Returning:
		if (return_committed_) {
			auto output = drain_return_pending_locked(now);
			if (!retimestamp_live_locked(packet))
				enter_error_locked("cannot maintain monotonic live timestamps");
			output.push_back({std::move(packet), now, now, sequence, false});
			saturating_add(bypassed_packets_, std::uint64_t{1});
			return output;
		}
		if (!enqueue_locked(packet, now, sequence))
			return fail_open_current();
		observe_return_keyframe_locked(queues_.at(packet.metadata.stream).back());
		if (return_from_delayed_)
			return drain_ready_locked(now);
		return {};
	}
	return {};
}

std::vector<OutputPacket> PacketDelay::poll(TimePoint now)
{
	std::scoped_lock lock{mutex_};
	if (!observe_time_locked(now))
		return {};
	switch (state_) {
	case DelayState::Preparing:
		if (preparing_change_)
			return drain_ready_locked(now);
		return {};
	case DelayState::Filling:
		prune_locked(now);
		return {};
	case DelayState::Delayed: {
		auto output = drain_ready_locked(now);
		if (state_ == DelayState::Delayed)
			prune_locked(now);
		return output;
	}
	case DelayState::Returning:
		if (return_committed_)
			return drain_return_pending_locked(now);
		if (return_from_delayed_)
			return drain_ready_locked(now);
		return {};
	case DelayState::Bypass:
	case DelayState::Error:
		return {};
	}
	return {};
}

void PacketDelay::prune(TimePoint now)
{
	std::scoped_lock lock{mutex_};
	if (observe_time_locked(now))
		prune_locked(now);
}

bool PacketDelay::observe_external_output(PacketMetadata metadata)
{
	std::scoped_lock lock{mutex_};
	const auto timeline = timeline_key(metadata.stream);
	if (!known_timelines_.contains(timeline)) {
		if (known_timelines_.size() >= config_.max_streams) {
			enter_error_locked("maximum number of stream timelines exceeded");
			return false;
		}
		known_timelines_.insert(timeline);
	}
	if (note_output_timestamp_locked(metadata))
		return true;
	enter_error_locked("external output timestamps are invalid or regress DTS");
	return false;
}

DelayState PacketDelay::state() const
{
	std::scoped_lock lock{mutex_};
	return state_;
}

Duration PacketDelay::target_delay() const
{
	std::scoped_lock lock{mutex_};
	return reported_target_locked();
}

std::string PacketDelay::error_message() const
{
	std::scoped_lock lock{mutex_};
	return error_message_;
}

DelayMetrics PacketDelay::metrics(TimePoint now) const
{
	std::scoped_lock lock{mutex_};
	return {
		state_,
		reported_target_locked(),
		fill_progress_locked(now),
		state_ == DelayState::Filling && cut_plan_locked(now).has_value(),
		state_ == DelayState::Returning && return_ready_locked(now),
		buffered_packets_,
		buffered_payload_bytes_,
		retained_bytes_,
		active_streams_.size(),
		bypassed_packets_,
		emitted_delayed_packets_,
		pruned_retention_packets_,
		rejected_capacity_packets_,
		pruned_payload_bytes_,
	};
}

std::vector<StreamMetrics> PacketDelay::stream_metrics(TimePoint now) const
{
	std::scoped_lock lock{mutex_};
	std::unordered_map<StreamKey, StreamMetrics, StreamKeyHash> metrics;
	for (const auto &[stream, queue] : queues_) {
		if (queue.empty())
			continue;
		auto &item = metrics[stream];
		item.stream = stream;
		item.buffered_packets = queue.size();
		for (const auto &queued : queue) {
			saturating_add(item.buffered_payload_bytes,
				queued.packet.payload.retained_size());
			saturating_add(item.retained_bytes, queued.retained_bytes);
		}
		item.buffered_span = elapsed(queue.back().received_at, queue.front().received_at);
		item.oldest_packet_age = elapsed(now, queue.front().received_at);
	}
	for (const auto &queued : return_pending_) {
		auto &item = metrics[queued.packet.metadata.stream];
		item.stream = queued.packet.metadata.stream;
		saturating_add(item.buffered_packets, std::size_t{1});
		saturating_add(item.buffered_payload_bytes,
			queued.packet.payload.retained_size());
		saturating_add(item.retained_bytes, queued.retained_bytes);
		item.oldest_packet_age = std::max(item.oldest_packet_age,
			elapsed(now, queued.received_at));
	}
	std::vector<StreamMetrics> result;
	result.reserve(metrics.size());
	for (auto &[stream, item] : metrics) {
		(void)stream;
		result.push_back(std::move(item));
	}
	std::sort(result.begin(), result.end(), [](const auto &left, const auto &right) {
		return left.stream < right.stream;
	});
	return result;
}

bool PacketDelay::observe_time_locked(TimePoint now)
{
	if (last_observed_at_ && now < *last_observed_at_) {
		enter_error_locked("monotonic clock moved backwards");
		return false;
	}
	last_observed_at_ = now;
	return true;
}

void PacketDelay::enter_error_locked(std::string message)
{
	clear_all_locked();
	error_message_ = message.empty() ? "unspecified packet delay error" : std::move(message);
	state_ = DelayState::Error;
}

void PacketDelay::clear_all_locked() noexcept
{
	queues_.clear();
	return_pending_.clear();
	buffered_packets_ = 0;
	buffered_payload_bytes_ = 0;
	retained_bytes_ = 0;
	active_streams_.clear();
	latest_dts_.clear();
	last_stream_seen_.clear();
	filling_started_at_.reset();
	reset_transition_locked();
}

void PacketDelay::clear_delay_queues_locked() noexcept
{
	queues_.clear();
	return_pending_.clear();
	buffered_packets_ = 0;
	buffered_payload_bytes_ = 0;
	retained_bytes_ = 0;
	active_streams_.clear();
	latest_dts_.clear();
	last_stream_seen_.clear();
}

void PacketDelay::reset_transition_locked() noexcept
{
	preparing_change_ = false;
	change_origin_delayed_ = false;
	return_from_delayed_ = false;
	return_committed_ = false;
	return_keyframes_.clear();
	return_required_video_.clear();
	return_requested_at_.reset();
}

bool PacketDelay::valid_delay_locked(Duration delay) const noexcept
{
	return delay.count() >= 0 && delay <= config_.max_delay;
}

Duration PacketDelay::reported_target_locked() const noexcept
{
	return state_ == DelayState::Preparing && preparing_change_
		? pending_target_delay_
		: target_delay_;
}

bool PacketDelay::prepare_stream_locked(const StreamKey &stream)
{
	const auto timeline = timeline_key(stream);
	if (!known_timelines_.contains(timeline)) {
		if (known_timelines_.size() >= config_.max_streams) {
			enter_error_locked("maximum number of stream timelines exceeded");
			return false;
		}
		known_timelines_.insert(timeline);
	}
	for (const auto &[queued_stream, queue] : queues_) {
		if (queued_stream != stream && timeline_key(queued_stream) == timeline &&
			!queue.empty()) {
			enter_error_locked("stream generation changed while packets were buffered");
			return false;
		}
	}
	const auto replaced = [&](const StreamKey &candidate) {
		return candidate != stream && timeline_key(candidate) == timeline;
	};
	std::erase_if(active_streams_, replaced);
	std::erase_if(latest_dts_, [&](const auto &item) { return replaced(item.first); });
	std::erase_if(last_stream_seen_,
		[&](const auto &item) { return replaced(item.first); });
	std::erase_if(timestamp_offsets_,
		[&](const auto &item) { return replaced(item.first); });
	const bool return_lane_required = std::erase_if(return_required_video_,
		replaced) != 0;
	std::erase_if(return_keyframes_,
		[&](const auto &item) { return replaced(item.first); });
	if (return_lane_required && stream.kind == StreamKind::Video)
		return_required_video_.insert(stream);
	return true;
}

bool PacketDelay::enqueue_locked(Packet &packet, TimePoint now, std::uint64_t sequence)
{
	if (!prepare_stream_locked(packet.metadata.stream))
		return false;
	if (!normalized_dts(packet)) {
		enter_error_locked("invalid packet time base");
		return false;
	}
	const auto stream = packet.metadata.stream;
	const bool output_epoch_active = state_ == DelayState::Delayed ||
		state_ == DelayState::Returning ||
		(state_ == DelayState::Preparing && preparing_change_);
	if (output_epoch_active && !timestamp_offsets_.contains(stream)) {
		if (!create_timestamp_offset_locked(packet, target_delay_)) {
			enter_error_locked("cannot create timestamp offset for a new stream");
			return false;
		}
	}
	if (output_epoch_active) {
		const auto timeline = timeline_key(stream);
		last_output_dts_.try_emplace(timeline,
			TimestampHorizon{0, TimeBase{0, 0}, 0.0L});
		max_output_pts_.try_emplace(timeline,
			TimestampHorizon{0, TimeBase{0, 0}, 0.0L});
	}
	const auto packet_dts = ordering_dts_locked(packet);
	if (!packet_dts) {
		enter_error_locked("timestamp overflow while ordering a packet");
		return false;
	}

	// Once Returning has selected live keyframes, every candidate packet is a
	// protected transition cursor. Capacity is still enforced transactionally,
	// but time pruning must not invalidate the pending cut.
	if (state_ != DelayState::Returning)
		prune_locked(now);
	const auto payload_size = packet.payload.retained_size();
	std::size_t packet_retained = 0;
	std::size_t next_payload = 0;
	std::size_t next_retained = 0;
	const bool arithmetic_ok =
		checked_add_size(payload_size, config_.metadata_bytes_per_packet, packet_retained) &&
		checked_add_size(buffered_payload_bytes_, payload_size, next_payload) &&
		checked_add_size(retained_bytes_, packet_retained, next_retained);
	const bool exceeds_payload = config_.max_payload_bytes != 0 &&
		next_payload > config_.max_payload_bytes;
	const bool exceeds_retained = config_.max_retained_bytes != 0 &&
		next_retained > config_.max_retained_bytes;
	const bool exceeds_packets = buffered_packets_ >= config_.max_buffered_packets;
	if (!arithmetic_ok || exceeds_payload || exceeds_retained || exceeds_packets) {
		saturating_add(rejected_capacity_packets_, std::uint64_t{1});
		enter_error_locked("packet buffer capacity exceeded without a GOP-safe eviction");
		return false;
	}

	auto &queue = queues_[packet.metadata.stream];
	queue.push_back({std::move(packet), now, sequence, packet_retained});
	++buffered_packets_;
	buffered_payload_bytes_ = next_payload;
	retained_bytes_ = next_retained;
	auto [latest, inserted] = latest_dts_.try_emplace(queue.back().packet.metadata.stream,
		*packet_dts);
	if (!inserted && compare_timestamps(packet_dts->value, packet_dts->time_base,
		latest->second.value, latest->second.time_base) > 0)
		latest->second = *packet_dts;
	last_stream_seen_[queue.back().packet.metadata.stream] = now;
	if (state_ == DelayState::Delayed || state_ == DelayState::Returning ||
		(state_ == DelayState::Preparing && preparing_change_))
		active_streams_.insert(queue.back().packet.metadata.stream);
	return true;
}

bool PacketDelay::create_timestamp_offset_locked(const Packet &packet,
	Duration base_delay)
{
	const auto stream = packet.metadata.stream;
	if (timestamp_offsets_.contains(stream))
		return true;
	const auto base_offset = delay_ticks(base_delay, packet.metadata.time_base);
	if (!base_offset)
		return false;
	std::int64_t offset = *base_offset;
	std::int64_t shifted_dts = 0;
	if (!add_timestamp(packet.metadata.dts, offset, shifted_dts))
		return false;

	const auto raise_to_horizon = [&](std::int64_t timestamp,
		const TimestampHorizon &horizon, bool strict) {
		const auto &time_base = packet.metadata.time_base;
		for (int attempt = 0; attempt < 8; ++attempt) {
			std::int64_t shifted = 0;
			if (!add_timestamp(timestamp, offset, shifted))
				return false;
			const int comparison = compare_timestamps(shifted, time_base,
				horizon.value, horizon.time_base);
			if (comparison > 0 || (!strict && comparison == 0))
				return true;
			const auto shifted_seconds = normalized_timestamp(shifted, time_base);
			if (!shifted_seconds)
				return false;
			const auto target = strict ? std::nextafter(horizon.normalized,
				std::numeric_limits<long double>::infinity())
				: horizon.normalized;
			long double missing_ticks = std::ceil(
				(target - *shifted_seconds) *
				static_cast<long double>(time_base.denominator));
			missing_ticks = std::max(missing_ticks, 1.0L);
			const long double upper_exclusive = std::ldexp(1.0L, 63);
			if (!std::isfinite(missing_ticks) || missing_ticks < 0.0L ||
				missing_ticks >= upper_exclusive)
				return false;
			if (!add_timestamp(offset,
				static_cast<std::int64_t>(missing_ticks), offset))
				return false;
		}
		return false;
	};

	const auto last_stream_dts = last_output_dts_.find(timeline_key(stream));
	if (last_stream_dts != last_output_dts_.end() &&
		last_stream_dts->second.time_base.valid() &&
		!raise_to_horizon(packet.metadata.dts, last_stream_dts->second, true))
		return false;
	if (last_emitted_dts_ &&
		!raise_to_horizon(packet.metadata.dts, *last_emitted_dts_, false))
		return false;
	const auto last_lane_pts = max_output_pts_.find(timeline_key(stream));
	if (last_lane_pts != max_output_pts_.end() &&
		last_lane_pts->second.time_base.valid() &&
		!raise_to_horizon(packet.metadata.pts, last_lane_pts->second, true))
		return false;
	std::int64_t shifted_pts = 0;
	if (!add_timestamp(packet.metadata.dts, offset, shifted_dts) ||
		!add_timestamp(packet.metadata.pts, offset, shifted_pts))
		return false;
	timestamp_offsets_.emplace(stream, offset);
	return true;
}

bool PacketDelay::unify_timestamp_offsets_locked(
	const std::vector<const Packet *> &epoch_fronts)
{
	std::optional<TimestampHorizon> common_shift;
	for (const auto *packet : epoch_fronts) {
		if (!packet)
			return false;
		const auto offset = timestamp_offsets_.find(packet->metadata.stream);
		if (offset == timestamp_offsets_.end())
			return false;
		const auto normalized = normalized_timestamp(offset->second,
			packet->metadata.time_base);
		if (!normalized)
			return false;
		const TimestampHorizon shift{offset->second, packet->metadata.time_base,
			*normalized};
		if (!common_shift || compare_timestamps(shift.value, shift.time_base,
			common_shift->value, common_shift->time_base) > 0)
			common_shift = shift;
	}
	if (!common_shift)
		return true;

	for (const auto *packet : epoch_fronts) {
		const auto &time_base = packet->metadata.time_base;
		long double raw = common_shift->normalized *
			static_cast<long double>(time_base.denominator);
		long double rounded = std::ceil(raw);
		const long double upper_exclusive = std::ldexp(1.0L, 63);
		if (!std::isfinite(rounded) || rounded < 0.0L ||
			rounded >= upper_exclusive)
			return false;
		auto ticks = static_cast<std::int64_t>(rounded);
		if (compare_timestamps(ticks, time_base, common_shift->value,
			common_shift->time_base) < 0) {
			raw = std::nextafter(common_shift->normalized,
				std::numeric_limits<long double>::infinity()) *
				static_cast<long double>(time_base.denominator);
			rounded = std::ceil(raw);
			if (!std::isfinite(rounded) || rounded < 0.0L ||
				rounded >= upper_exclusive)
				return false;
			ticks = static_cast<std::int64_t>(rounded);
			if (compare_timestamps(ticks, time_base, common_shift->value,
				common_shift->time_base) < 0)
				return false;
		}
		auto offset = timestamp_offsets_.find(packet->metadata.stream);
		if (offset == timestamp_offsets_.end())
			return false;
		offset->second = std::max(offset->second, ticks);
		std::int64_t shifted_pts = 0;
		std::int64_t shifted_dts = 0;
		if (!add_timestamp(packet->metadata.pts, offset->second, shifted_pts) ||
			!add_timestamp(packet->metadata.dts, offset->second, shifted_dts))
			return false;
	}
	return true;
}

bool PacketDelay::begin_timestamp_epoch_locked()
{
	timestamp_offsets_.clear();
	std::vector<const Packet *> epoch_fronts;
	epoch_fronts.reserve(queues_.size());
	for (const auto &[stream, queue] : queues_) {
		(void)stream;
		if (!queue.empty()) {
			if (!create_timestamp_offset_locked(queue.front().packet, target_delay_))
				return false;
			epoch_fronts.push_back(&queue.front().packet);
		}
	}
	if (!unify_timestamp_offsets_locked(epoch_fronts))
		return false;
	for (const auto *packet : epoch_fronts) {
		const auto timeline = timeline_key(packet->metadata.stream);
		last_output_dts_.try_emplace(timeline,
			TimestampHorizon{0, TimeBase{0, 0}, 0.0L});
		max_output_pts_.try_emplace(timeline,
			TimestampHorizon{0, TimeBase{0, 0}, 0.0L});
	}

	latest_dts_.clear();
	for (const auto &[stream, queue] : queues_) {
		if (queue.empty())
			continue;
		const auto latest = ordering_dts_locked(queue.back().packet);
		if (!latest)
			return false;
		latest_dts_.emplace(stream, *latest);
	}
	return true;
}

bool PacketDelay::retimestamp_live_locked(Packet &packet)
{
	if (!prepare_stream_locked(packet.metadata.stream))
		return false;
	if (!create_timestamp_offset_locked(packet, Duration{0}))
		return false;
	const auto offset = timestamp_offsets_.find(packet.metadata.stream);
	return offset != timestamp_offsets_.end() &&
		retimestamp(packet, offset->second) &&
		note_output_timestamp_locked(packet.metadata);
}

bool PacketDelay::note_output_timestamp_locked(const PacketMetadata &metadata)
{
	const auto output_dts = normalized_timestamp(metadata.dts, metadata.time_base);
	const auto output_pts = normalized_timestamp(metadata.pts, metadata.time_base);
	if (!output_dts || !output_pts)
		return false;
	if (last_emitted_dts_) {
		const bool regressed = compare_timestamps(metadata.dts, metadata.time_base,
			last_emitted_dts_->value, last_emitted_dts_->time_base) < 0;
		if (regressed)
			return false;
	}
	last_emitted_dts_ = TimestampHorizon{metadata.dts, metadata.time_base,
		*output_dts};
	last_output_dts_[timeline_key(metadata.stream)] = TimestampHorizon{
		metadata.dts, metadata.time_base, *output_dts};
	auto [maximum_pts, inserted] = max_output_pts_.try_emplace(
		timeline_key(metadata.stream), TimestampHorizon{metadata.pts,
			metadata.time_base, *output_pts});
	if (!inserted && maximum_pts->second.time_base.valid()) {
		const bool is_larger = compare_timestamps(metadata.pts,
			metadata.time_base, maximum_pts->second.value,
			maximum_pts->second.time_base) > 0;
		if (is_larger)
			maximum_pts->second = TimestampHorizon{metadata.pts,
				metadata.time_base, *output_pts};
	}
	if (!maximum_pts->second.time_base.valid())
		maximum_pts->second = TimestampHorizon{metadata.pts,
			metadata.time_base, *output_pts};
	return true;
}

std::optional<PacketDelay::TimestampHorizon> PacketDelay::ordering_dts_locked(
	const Packet &packet) const noexcept
{
	const auto offset = timestamp_offsets_.find(packet.metadata.stream);
	const auto ticks = offset == timestamp_offsets_.end() ? std::int64_t{0}
		: offset->second;
	std::int64_t shifted = 0;
	if (!add_timestamp(packet.metadata.dts, ticks, shifted) ||
		!packet.metadata.time_base.valid())
		return std::nullopt;
	const auto normalized = normalized_timestamp(shifted, packet.metadata.time_base);
	return normalized ? std::optional<TimestampHorizon>{TimestampHorizon{
		shifted, packet.metadata.time_base, *normalized}} : std::nullopt;
}

std::vector<OutputPacket> PacketDelay::drain_ready_locked(TimePoint now)
{
	std::vector<OutputPacket> output;
	std::size_t due_packets = 0;
	for (const auto &[stream, queue] : queues_) {
		(void)stream;
		for (const auto &queued : queue) {
			if (!due(now, queued.received_at, target_delay_))
				break;
			saturating_add(due_packets, std::size_t{1});
		}
	}
	output.reserve(due_packets);
	for (;;) {
		std::optional<TimestampHorizon> watermark;
		for (const auto &stream : active_streams_) {
			if (stream_is_stalled_locked(stream, now))
				continue;
			const auto latest = latest_dts_.find(stream);
			if (latest == latest_dts_.end())
				return output;
			if (!watermark || compare_timestamps(latest->second.value,
				latest->second.time_base, watermark->value,
				watermark->time_base) < 0)
				watermark = latest->second;
		}

		auto candidate_queue = queues_.end();
		TimestampHorizon candidate_dts{};
		for (auto iterator = queues_.begin(); iterator != queues_.end(); ++iterator) {
			if (iterator->second.empty() ||
				!due(now, iterator->second.front().received_at, target_delay_))
				continue;
			const auto dts = ordering_dts_locked(iterator->second.front().packet);
			if (!dts) {
				enter_error_locked("invalid queued packet time base");
				return output;
			}
			const int comparison = candidate_queue == queues_.end() ? -1
				: compare_timestamps(dts->value, dts->time_base,
					candidate_dts.value, candidate_dts.time_base);
			if (candidate_queue == queues_.end() || comparison < 0 ||
				(comparison == 0 && iterator->second.front().sequence <
					candidate_queue->second.front().sequence)) {
				candidate_queue = iterator;
				candidate_dts = *dts;
			}
		}
		if (candidate_queue == queues_.end())
			break;

		if (state_ == DelayState::Returning && !return_committed_) {
			const auto stream = candidate_queue->first;
			if (!config_.require_video_keyframe && return_requested_at_ &&
				candidate_queue->second.front().received_at >= *return_requested_at_)
				break;
			if (stream.kind == StreamKind::Video) {
				const auto cut = return_keyframes_.find(stream);
				if (cut != return_keyframes_.end() &&
					candidate_queue->second.front().sequence >= cut->second) {
					if (return_ready_locked(now))
						break;
					// Another video lane has not reached a usable keyframe yet.
					// Keep the delayed branch moving and wait for this lane's
					// next keyframe rather than freezing every stream globally.
					return_keyframes_.erase(cut);
				}
			}
			if (stream.kind == StreamKind::Audio && return_ready_locked(now)) {
				std::optional<TimePoint> live_cut;
				for (const auto &[video_stream, sequence] : return_keyframes_) {
					const auto video_queue = queues_.find(video_stream);
					if (video_queue == queues_.end())
						continue;
					const auto keyframe = std::find_if(video_queue->second.begin(),
						video_queue->second.end(), [sequence](const auto &item) {
							return item.sequence == sequence;
						});
					if (keyframe != video_queue->second.end())
						live_cut = !live_cut ? keyframe->received_at
							: std::min(*live_cut, keyframe->received_at);
				}
				if (!live_cut)
					live_cut = return_requested_at_;
				if (live_cut && candidate_queue->second.front().received_at >= *live_cut)
					break;
			}
		}

		if (watermark) {
			const auto reorder_ticks = duration_ticks_ceil(config_.reorder_window,
				candidate_dts.time_base);
			std::int64_t guarded_candidate = 0;
			if (!reorder_ticks || !add_timestamp(candidate_dts.value,
				*reorder_ticks, guarded_candidate)) {
				enter_error_locked("reorder window cannot be represented safely");
				return output;
			}
			if (compare_timestamps(guarded_candidate, candidate_dts.time_base,
				watermark->value, watermark->time_base) > 0)
				break;
		}

		auto queued = std::move(candidate_queue->second.front());
		candidate_queue->second.pop_front();
		if (candidate_queue->second.empty())
			queues_.erase(candidate_queue);
		--buffered_packets_;
		buffered_payload_bytes_ -= queued.packet.payload.retained_size();
		retained_bytes_ -= queued.retained_bytes;
		const auto offset = timestamp_offsets_.find(queued.packet.metadata.stream);
		if (offset == timestamp_offsets_.end() ||
			!retimestamp(queued.packet, offset->second)) {
			enter_error_locked("timestamp overflow while retimestamping");
			return output;
		}
		if (!note_output_timestamp_locked(queued.packet.metadata)) {
			enter_error_locked("late packet would regress global output DTS");
			return output;
		}
		output.push_back({std::move(queued.packet), queued.received_at, now,
			queued.sequence, true});
		saturating_add(emitted_delayed_packets_, std::uint64_t{1});
	}
	return output;
}

std::vector<OutputPacket> PacketDelay::drain_return_pending_locked(TimePoint now)
{
	std::vector<OutputPacket> output;
	output.reserve(return_pending_.size());
	while (!return_pending_.empty()) {
		auto queued = std::move(return_pending_.front());
		return_pending_.pop_front();
		--buffered_packets_;
		buffered_payload_bytes_ -= queued.packet.payload.retained_size();
		retained_bytes_ -= queued.retained_bytes;
		const auto offset = timestamp_offsets_.find(queued.packet.metadata.stream);
		if (offset == timestamp_offsets_.end() ||
			!retimestamp(queued.packet, offset->second) ||
			!note_output_timestamp_locked(queued.packet.metadata)) {
			enter_error_locked("live return packet would regress output timestamps");
			return output;
		}
		output.push_back({std::move(queued.packet), queued.received_at, now,
			queued.sequence, false});
	}
	return output;
}

void PacketDelay::prune_locked(TimePoint now)
{
	const Duration retention_target = state_ == DelayState::Filling &&
		change_origin_delayed_ ? std::max(target_delay_, previous_target_delay_)
		: target_delay_;
	const Duration retention = retention_target + config_.retention_headroom;
	for (auto &[stream, queue] : queues_) {
		if (queue.size() < 2)
			continue;
		std::optional<std::uint64_t> keep;
		for (const auto &packet : queue) {
			if (!due(now, packet.received_at, retention))
				break;
			if (stream.kind != StreamKind::Video || !config_.require_video_keyframe ||
				packet.packet.metadata.keyframe)
				keep = packet.sequence;
		}
		if (keep && queue.front().sequence != *keep)
			prune_prefix_locked(queue, *keep);
	}
	for (auto iterator = queues_.begin(); iterator != queues_.end();) {
		if (iterator->second.empty())
			iterator = queues_.erase(iterator);
		else
			++iterator;
	}
}

void PacketDelay::prune_prefix_locked(PacketQueue &queue,
	std::uint64_t keep_sequence)
{
	while (!queue.empty() && queue.front().sequence < keep_sequence) {
		const auto payload = queue.front().packet.payload.retained_size();
		const auto retained = queue.front().retained_bytes;
		queue.pop_front();
		--buffered_packets_;
		buffered_payload_bytes_ -= payload;
		retained_bytes_ -= retained;
		saturating_add(pruned_retention_packets_, std::uint64_t{1});
		saturating_add(pruned_payload_bytes_, static_cast<std::uint64_t>(payload));
	}
}

std::optional<PacketDelay::CutPlan> PacketDelay::cut_plan_locked(TimePoint now) const
{
	if (queues_.empty())
		return std::nullopt;
	for (const auto &required : required_streams_) {
		const auto queue = queues_.find(required);
		if (queue == queues_.end() || queue->second.empty())
			return std::nullopt;
	}
	CutPlan plan;
	for (const auto &[stream, queue] : queues_) {
		const bool required = required_streams_.empty() ||
			required_streams_.contains(stream);
		if (config_.max_stream_gap.count() != 0 &&
			elapsed(now, queue.back().received_at) > config_.max_stream_gap) {
			if (required)
				return std::nullopt;
			continue;
		}
		auto candidate = queue.end();
		for (auto iterator = queue.rbegin(); iterator != queue.rend(); ++iterator) {
			if (!due(now, iterator->received_at, target_delay_))
				continue;
			if (stream.kind == StreamKind::Video && config_.require_video_keyframe &&
				!iterator->packet.metadata.keyframe)
				continue;
			const auto possible = std::prev(iterator.base());
			if (target_delay_.count() != 0 &&
				elapsed(queue.back().received_at, possible->received_at) < target_delay_)
				continue;
			bool continuous = true;
			if (config_.max_stream_gap.count() != 0) {
				auto previous = possible;
				for (auto current = std::next(possible); current != queue.end(); ++current) {
					if (elapsed(current->received_at, previous->received_at) >
						config_.max_stream_gap) {
						continuous = false;
						break;
					}
					previous = current;
				}
			}
			if (!continuous)
				continue;
			candidate = possible;
			break;
		}
		if (candidate == queue.end()) {
			if (required)
				return std::nullopt;
			continue;
		}
		plan.start_sequence.emplace(stream, candidate->sequence);
	}
	if (plan.start_sequence.empty())
		return std::nullopt;
	return plan;
}

void PacketDelay::apply_cut_plan_locked(const CutPlan &plan)
{
	for (auto iterator = queues_.begin(); iterator != queues_.end();) {
		const auto start = plan.start_sequence.find(iterator->first);
		if (start == plan.start_sequence.end()) {
			while (!iterator->second.empty()) {
				const auto payload =
					iterator->second.front().packet.payload.retained_size();
				const auto retained = iterator->second.front().retained_bytes;
				iterator->second.pop_front();
				--buffered_packets_;
				buffered_payload_bytes_ -= payload;
				retained_bytes_ -= retained;
				saturating_add(pruned_retention_packets_, std::uint64_t{1});
				saturating_add(pruned_payload_bytes_,
					static_cast<std::uint64_t>(payload));
			}
			iterator = queues_.erase(iterator);
			continue;
		}
		prune_prefix_locked(iterator->second, start->second);
		++iterator;
	}
}

void PacketDelay::start_return_locked(bool from_delayed, TimePoint now)
{
	state_ = DelayState::Returning;
	return_from_delayed_ = from_delayed;
	return_committed_ = false;
	return_requested_at_ = now;
	return_keyframes_.clear();
	return_required_video_.clear();
	if (!active_streams_.empty()) {
		for (const auto &stream : active_streams_) {
			if (stream.kind == StreamKind::Video && config_.require_video_keyframe)
				return_required_video_.insert(stream);
		}
	} else {
		for (const auto &[stream, queue] : queues_) {
			(void)queue;
			if (stream.kind == StreamKind::Video && config_.require_video_keyframe)
				return_required_video_.insert(stream);
		}
	}
	preparing_change_ = false;
	change_origin_delayed_ = false;
}

void PacketDelay::observe_return_keyframe_locked(const QueuedPacket &packet)
{
	const auto stream = packet.packet.metadata.stream;
	if (stream.kind != StreamKind::Video || !config_.require_video_keyframe ||
		!return_requested_at_ || packet.received_at < *return_requested_at_)
		return;
	if (packet.packet.metadata.keyframe)
		return_keyframes_.try_emplace(stream, packet.sequence);
}

bool PacketDelay::return_ready_locked(TimePoint now) const noexcept
{
	if (return_committed_)
		return true;
	for (const auto &stream : return_required_video_) {
		if (!return_keyframes_.contains(stream) &&
			!stream_is_stalled_locked(stream, now))
			return false;
	}
	return true;
}

bool PacketDelay::build_return_pending_locked()
{
	std::optional<TimePoint> live_cut;
	for (const auto &[stream, sequence] : return_keyframes_) {
		const auto queue = queues_.find(stream);
		if (queue == queues_.end())
			continue;
		const auto packet = std::find_if(queue->second.begin(), queue->second.end(),
			[sequence](const auto &item) { return item.sequence == sequence; });
		if (packet != queue->second.end())
			live_cut = !live_cut ? packet->received_at
				: std::min(*live_cut, packet->received_at);
	}
	if (!live_cut)
		live_cut = return_requested_at_;

	std::vector<QueuedPacket> pending;
	pending.reserve(buffered_packets_);
	for (const auto &[stream, queue] : queues_) {
		std::optional<std::uint64_t> video_start;
		if (stream.kind == StreamKind::Video) {
			const auto start = return_keyframes_.find(stream);
			if (start != return_keyframes_.end())
				video_start = start->second;
		}
		for (const auto &packet : queue) {
			const bool keep_video = stream.kind == StreamKind::Video &&
				((config_.require_video_keyframe && video_start &&
					packet.sequence >= *video_start) ||
				(!config_.require_video_keyframe && live_cut &&
					packet.received_at >= *live_cut));
			const bool keep_audio = stream.kind == StreamKind::Audio && live_cut &&
				packet.received_at >= *live_cut;
			if (keep_video || keep_audio)
				pending.push_back(packet);
		}
	}
	timestamp_offsets_.clear();
	StreamSet initialized_streams;
	std::vector<const Packet *> epoch_fronts;
	epoch_fronts.reserve(queues_.size());
	for (const auto &packet : pending) {
		if (initialized_streams.insert(packet.packet.metadata.stream).second) {
			if (!create_timestamp_offset_locked(packet.packet, Duration{0}))
				return false;
			epoch_fronts.push_back(&packet.packet);
		}
	}
	if (!unify_timestamp_offsets_locked(epoch_fronts))
		return false;
	for (const auto *packet : epoch_fronts) {
		const auto timeline = timeline_key(packet->metadata.stream);
		last_output_dts_.try_emplace(timeline,
			TimestampHorizon{0, TimeBase{0, 0}, 0.0L});
		max_output_pts_.try_emplace(timeline,
			TimestampHorizon{0, TimeBase{0, 0}, 0.0L});
	}
	for (const auto &packet : pending) {
		if (!ordering_dts_locked(packet.packet))
			return false;
	}
	std::sort(pending.begin(), pending.end(), [this](const auto &left,
		const auto &right) {
		const auto left_dts = *ordering_dts_locked(left.packet);
		const auto right_dts = *ordering_dts_locked(right.packet);
		const int comparison = compare_timestamps(left_dts.value,
			left_dts.time_base, right_dts.value, right_dts.time_base);
		if (comparison != 0)
			return comparison < 0;
		return left.sequence < right.sequence;
	});
	std::deque<QueuedPacket> next_return_pending;
	std::size_t next_packets = 0;
	std::size_t next_payload = 0;
	std::size_t next_retained = 0;
	for (auto &packet : pending) {
		saturating_add(next_packets, std::size_t{1});
		saturating_add(next_payload,
			packet.packet.payload.retained_size());
		saturating_add(next_retained, packet.retained_bytes);
		next_return_pending.push_back(std::move(packet));
	}
	queues_.clear();
	return_pending_.swap(next_return_pending);
	buffered_packets_ = next_packets;
	buffered_payload_bytes_ = next_payload;
	retained_bytes_ = next_retained;
	active_streams_.clear();
	latest_dts_.clear();
	last_stream_seen_.clear();
	return true;
}

double PacketDelay::fill_progress_locked(TimePoint now) const
{
	if (state_ == DelayState::Delayed)
		return 1.0;
	if (state_ != DelayState::Filling || queues_.empty())
		return 0.0;
	if (cut_plan_locked(now))
		return 1.0;
	if (target_delay_.count() == 0)
		return 0.0;
	double progress = 1.0;
	const auto accumulate = [&](const PacketQueue &queue) {
		if (queue.empty())
			return false;
		const auto age = elapsed(now, queue.front().received_at);
		progress = std::min(progress, static_cast<double>(age.count()) /
			static_cast<double>(target_delay_.count()));
		return true;
	};
	if (!required_streams_.empty()) {
		for (const auto &stream : required_streams_) {
			const auto queue = queues_.find(stream);
			if (queue == queues_.end() || !accumulate(queue->second))
				return 0.0;
		}
	} else {
		for (const auto &[stream, queue] : queues_) {
			(void)stream;
			if (!accumulate(queue))
				return 0.0;
		}
	}
	return std::clamp(progress, 0.0, std::nextafter(1.0, 0.0));
}

bool PacketDelay::stream_is_stalled_locked(const StreamKey &stream,
	TimePoint now) const noexcept
{
	if (config_.stream_stall_timeout.count() == 0)
		return false;
	const auto seen = last_stream_seen_.find(stream);
	return seen != last_stream_seen_.end() &&
		elapsed(now, seen->second) > config_.stream_stall_timeout;
}

} // namespace obs_delay::core
