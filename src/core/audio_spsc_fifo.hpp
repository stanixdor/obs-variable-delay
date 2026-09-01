#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

namespace dynamic_delay::core {

/**
 * One libobs audio render quantum.  The storage is deliberately fixed so the
 * OBS audio thread never allocates, locks, or performs an unbounded copy.
 */
struct PcmAudioBlock {
	static constexpr std::size_t MixerCount = 6;
	static constexpr std::size_t MaxChannels = 8;
	static constexpr std::size_t Frames = 1024;
	static constexpr std::size_t SampleCount = MixerCount * MaxChannels * Frames;

	uint64_t timestampNs = 0;
	// Source start relative to the main OBS audio window, converted once to
	// frames by the producer.  This bridges the main and private audio clock
	// domains without comparing their absolute nanosecond origins.
	int64_t alignmentFrames = 0;
	uint32_t sampleRate = 0;
	uint32_t channels = 0;
	uint32_t mixerMask = 0;
	std::array<float, SampleCount> samples{};

	[[nodiscard]] float *plane(const std::size_t mixer, const std::size_t channel) noexcept
	{
		return samples.data() + ((mixer * MaxChannels + channel) * Frames);
	}

	[[nodiscard]] const float *plane(const std::size_t mixer, const std::size_t channel) const noexcept
	{
		return samples.data() + ((mixer * MaxChannels + channel) * Frames);
	}
};

namespace detail {

inline constexpr std::size_t CacheLineSize = 64;
static_assert(sizeof(std::atomic<uint64_t>) * 2 <= CacheLineSize);

// Keep cache-line padding explicit: producer and consumer sequences remain on
// separate lines, and MSVC does not need to add warning-worthy tail padding.
struct alignas(CacheLineSize) CacheLineSequence {
	std::atomic<uint64_t> value{0};
	std::array<std::byte, CacheLineSize - sizeof(std::atomic<uint64_t>)> padding{};
};

struct alignas(CacheLineSize) CacheLineCounters {
	std::atomic<uint64_t> dropped{0};
	std::atomic<uint64_t> underruns{0};
	std::array<std::byte, CacheLineSize - (sizeof(std::atomic<uint64_t>) * 2)> padding{};
};

static_assert(sizeof(CacheLineSequence) == CacheLineSize);
static_assert(alignof(CacheLineSequence) == CacheLineSize);
static_assert(sizeof(CacheLineCounters) == CacheLineSize);
static_assert(alignof(CacheLineCounters) == CacheLineSize);

} // namespace detail

/**
 * Bounded single-producer/single-consumer ring used between libobs's main
 * mixer and the private hold encoder.  A slot stays owned by its caller until
 * commit_* is invoked.  Only the producer may call begin/commit_push and only
 * the consumer may call begin/commit_pop.
 */
template<std::size_t Capacity = 16> class PcmSpscFifo {
public:
	static_assert(Capacity >= 2);

	[[nodiscard]] PcmAudioBlock *begin_push() noexcept
	{
		const uint64_t write = writeSequence_.value.load(std::memory_order_relaxed);
		const uint64_t read = readSequence_.value.load(std::memory_order_acquire);
		if (write - read >= Capacity) {
			counters_.dropped.fetch_add(1, std::memory_order_relaxed);
			return nullptr;
		}
		return &slots_[static_cast<std::size_t>(write % Capacity)];
	}

	void commit_push() noexcept { writeSequence_.value.fetch_add(1, std::memory_order_release); }

	[[nodiscard]] const PcmAudioBlock *begin_pop() noexcept
	{
		const uint64_t read = readSequence_.value.load(std::memory_order_relaxed);
		const uint64_t write = writeSequence_.value.load(std::memory_order_acquire);
		if (read == write) {
			counters_.underruns.fetch_add(1, std::memory_order_relaxed);
			return nullptr;
		}
		return &slots_[static_cast<std::size_t>(read % Capacity)];
	}

	void commit_pop() noexcept { readSequence_.value.fetch_add(1, std::memory_order_release); }

	[[nodiscard]] std::size_t size() const noexcept
	{
		const uint64_t write = writeSequence_.value.load(std::memory_order_acquire);
		const uint64_t read = readSequence_.value.load(std::memory_order_acquire);
		return static_cast<std::size_t>(write - read);
	}

	[[nodiscard]] uint64_t dropped() const noexcept { return counters_.dropped.load(std::memory_order_relaxed); }
	[[nodiscard]] uint64_t underruns() const noexcept
	{
		return counters_.underruns.load(std::memory_order_relaxed);
	}

	void reset() noexcept
	{
		// reset() is a lifecycle operation: producer and consumer must already
		// be stopped.
		readSequence_.value.store(0, std::memory_order_relaxed);
		writeSequence_.value.store(0, std::memory_order_relaxed);
		counters_.dropped.store(0, std::memory_order_relaxed);
		counters_.underruns.store(0, std::memory_order_relaxed);
	}

private:
	std::array<PcmAudioBlock, Capacity> slots_{};
	detail::CacheLineSequence writeSequence_{};
	detail::CacheLineSequence readSequence_{};
	detail::CacheLineCounters counters_{};
};

/**
 * Consumer-side cursor which aligns timestamped FIFO blocks to one private
 * audio-output window.  A FIFO slot may remain consumer-owned across calls
 * when the two audio clocks have different phases.
 */
class PcmWindowCursor {
public:
	template<std::size_t Capacity, typename Compatible, typename Copy>
	void consume(PcmSpscFifo<Capacity> &fifo, const uint32_t sampleRate, const std::size_t outputFrames,
		     Compatible &&compatible, Copy &&copy) noexcept
	{
		if (sampleRate == 0 || outputFrames == 0)
			return;

		std::size_t destinationCursor = 0;
		while (destinationCursor < outputFrames) {
			// Keep one future quantum available.  A negative timestamp
			// correction can then consume the tail of this block and the head
			// of the next without creating a periodic gap or retaining a
			// permanent phase error between equal-rate clocks.  Positive
			// alignment is the exception: its silence countdown itself gives
			// the producer time to provide lookahead, so waiting here would
			// apply that offset twice.
			if (!primed_) {
				const std::size_t buffered = fifo.size();
				if (buffered == 0)
					break;
				if (buffered < 2) {
					const PcmAudioBlock *candidate = fifo.begin_pop();
					if (!candidate || candidate->alignmentFrames <= 0)
						break;
				}
				primed_ = true;
			}
			const PcmAudioBlock *block = fifo.begin_pop();
			if (!block) {
				primed_ = false;
				// A large negative sync offset can span several FIFO
				// quanta.  Keep the remaining skip debt across producer gaps;
				// reloading each block's absolute alignment would otherwise
				// restart the correction forever.
				if (!(alignmentPending_ && pendingAlignmentFrames_ < 0))
					reset_alignment();
				break;
			}
			if (block->timestampNs == 0 || block->sampleRate != sampleRate || !compatible(*block)) {
				fifo.commit_pop();
				sourceOffsetFrames_ = 0;
				primed_ = false;
				reset_alignment();
				continue;
			}

			if (!currentBlockAligned_) {
				const bool contiguous = haveContinuity_ &&
							timestamps_contiguous(expectedNextBlockTimestampNs_,
									      block->timestampNs, sampleRate);
				if (!alignmentPending_ && !contiguous) {
					pendingAlignmentFrames_ =
						haveContinuity_ && expectedNextBlockTimestampNs_ != 0
							? timestamp_delta_frames(expectedNextBlockTimestampNs_,
										 block->timestampNs, sampleRate)
							: block->alignmentFrames;
					alignmentPending_ = true;
					haveContinuity_ = false;
				}

				if (alignmentPending_ && pendingAlignmentFrames_ > 0) {
					const std::size_t silence = static_cast<std::size_t>(std::min<int64_t>(
						pendingAlignmentFrames_,
						static_cast<int64_t>(outputFrames - destinationCursor)));
					destinationCursor += silence;
					pendingAlignmentFrames_ -= static_cast<int64_t>(silence);
					if (destinationCursor == outputFrames)
						break;
				}

				if (alignmentPending_ && pendingAlignmentFrames_ < 0) {
					const uint64_t requestedSkip = negative_magnitude(pendingAlignmentFrames_);
					const std::size_t skip = static_cast<std::size_t>(std::min<uint64_t>(
						requestedSkip, PcmAudioBlock::Frames - sourceOffsetFrames_));
					sourceOffsetFrames_ += skip;
					pendingAlignmentFrames_ += static_cast<int64_t>(skip);
					if (sourceOffsetFrames_ == PcmAudioBlock::Frames) {
						finish_block(fifo, *block, sampleRate);
						continue;
					}
				}

				if (!alignmentPending_ || pendingAlignmentFrames_ == 0) {
					alignmentPending_ = false;
					pendingAlignmentFrames_ = 0;
					currentBlockAligned_ = true;
				}
			}

			if (!currentBlockAligned_)
				continue;
			const std::size_t frames =
				std::min(PcmAudioBlock::Frames - sourceOffsetFrames_, outputFrames - destinationCursor);
			if (frames == 0)
				break;
			copy(*block, sourceOffsetFrames_, destinationCursor, frames);
			sourceOffsetFrames_ += frames;
			destinationCursor += frames;
			if (sourceOffsetFrames_ == PcmAudioBlock::Frames)
				finish_block(fifo, *block, sampleRate);
		}
	}

	void reset() noexcept
	{
		sourceOffsetFrames_ = 0;
		primed_ = false;
		reset_alignment();
	}
	[[nodiscard]] std::size_t source_offset_frames() const noexcept { return sourceOffsetFrames_; }

private:
	template<std::size_t Capacity>
	void finish_block(PcmSpscFifo<Capacity> &fifo, const PcmAudioBlock &block, const uint32_t sampleRate) noexcept
	{
		expectedNextBlockTimestampNs_ =
			saturating_add_ns(block.timestampNs, frames_to_ns(PcmAudioBlock::Frames, sampleRate));
		haveContinuity_ = true;
		fifo.commit_pop();
		sourceOffsetFrames_ = 0;
		currentBlockAligned_ = false;
		if (alignmentPending_ && pendingAlignmentFrames_ == 0)
			alignmentPending_ = false;
	}

	void reset_alignment() noexcept
	{
		expectedNextBlockTimestampNs_ = 0;
		pendingAlignmentFrames_ = 0;
		currentBlockAligned_ = false;
		alignmentPending_ = false;
		haveContinuity_ = false;
	}

	[[nodiscard]] static uint64_t saturating_add_ns(const uint64_t value, const uint64_t increment) noexcept
	{
		return increment > std::numeric_limits<uint64_t>::max() - value ? std::numeric_limits<uint64_t>::max()
										: value + increment;
	}

	[[nodiscard]] static uint64_t frames_to_ns(const std::size_t frames, const uint32_t sampleRate) noexcept
	{
		if (sampleRate == 0)
			return 0;
		return static_cast<uint64_t>(frames / sampleRate) * 1'000'000'000ULL +
		       static_cast<uint64_t>(frames % sampleRate) * 1'000'000'000ULL / sampleRate;
	}

	[[nodiscard]] static uint64_t frames_for_ns_nearest(const uint64_t durationNs,
							    const uint32_t sampleRate) noexcept
	{
		const uint64_t whole = durationNs / 1'000'000'000ULL;
		const uint64_t remainder = durationNs % 1'000'000'000ULL;
		if (whole > std::numeric_limits<uint64_t>::max() / sampleRate)
			return std::numeric_limits<uint64_t>::max();
		const uint64_t fractional = (remainder * sampleRate + 500'000'000ULL) / 1'000'000'000ULL;
		const uint64_t base = whole * sampleRate;
		return fractional > std::numeric_limits<uint64_t>::max() - base ? std::numeric_limits<uint64_t>::max()
										: base + fractional;
	}

	[[nodiscard]] static int64_t timestamp_delta_frames(const uint64_t expected, const uint64_t actual,
							    const uint32_t sampleRate) noexcept
	{
		const bool positive = actual >= expected;
		const uint64_t magnitude =
			frames_for_ns_nearest(positive ? actual - expected : expected - actual, sampleRate);
		if (magnitude > static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
			return positive ? std::numeric_limits<int64_t>::max() : std::numeric_limits<int64_t>::min();
		return positive ? static_cast<int64_t>(magnitude) : -static_cast<int64_t>(magnitude);
	}

	[[nodiscard]] static bool timestamps_contiguous(const uint64_t expected, const uint64_t actual,
							const uint32_t sampleRate) noexcept
	{
		if (expected == 0)
			return false;
		const uint64_t difference = expected > actual ? expected - actual : actual - expected;
		return difference < std::max<uint64_t>(1, frames_to_ns(1, sampleRate));
	}

	[[nodiscard]] static uint64_t negative_magnitude(const int64_t value) noexcept
	{
		return value == std::numeric_limits<int64_t>::min()
			       ? static_cast<uint64_t>(std::numeric_limits<int64_t>::max()) + 1ULL
			       : static_cast<uint64_t>(-value);
	}

	std::size_t sourceOffsetFrames_ = 0;
	uint64_t expectedNextBlockTimestampNs_ = 0;
	int64_t pendingAlignmentFrames_ = 0;
	bool currentBlockAligned_ = false;
	bool alignmentPending_ = false;
	bool haveContinuity_ = false;
	bool primed_ = false;
};

} // namespace dynamic_delay::core
