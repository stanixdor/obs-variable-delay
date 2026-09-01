#include "core/audio_spsc_fifo.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <exception>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

using dynamic_delay::core::PcmAudioBlock;
using dynamic_delay::core::PcmSpscFifo;
using dynamic_delay::core::PcmWindowCursor;

namespace {

class TestFailure final : public std::runtime_error {
public:
	using std::runtime_error::runtime_error;
};

#define CHECK(expression)                                                                    \
	do {                                                                                       \
		if (!(expression)) {                                                                    \
			std::ostringstream message;                                                           \
			message << __FILE__ << ':' << __LINE__ << ": CHECK(" #expression ") failed";       \
			throw TestFailure{message.str()};                                                     \
		}                                                                                        \
	} while (false)

void bounded_fifo_wraps_without_overwriting_unread_data()
{
	PcmSpscFifo<3> fifo;
	for (uint64_t value = 1; value <= 3; ++value) {
		auto *block = fifo.begin_push();
		CHECK(block != nullptr);
		block->timestampNs = value;
		block->plane(0, 0)[0] = static_cast<float>(value);
		fifo.commit_push();
	}
	CHECK(fifo.begin_push() == nullptr);
	CHECK(fifo.dropped() == 1);
	CHECK(fifo.size() == 3);

	for (uint64_t expected = 1; expected <= 2; ++expected) {
		const auto *block = fifo.begin_pop();
		CHECK(block != nullptr);
		CHECK(block->timestampNs == expected);
		CHECK(block->plane(0, 0)[0] == static_cast<float>(expected));
		fifo.commit_pop();
	}

	for (uint64_t value = 4; value <= 5; ++value) {
		auto *block = fifo.begin_push();
		CHECK(block != nullptr);
		block->timestampNs = value;
		fifo.commit_push();
	}
	for (uint64_t expected = 3; expected <= 5; ++expected) {
		const auto *block = fifo.begin_pop();
		CHECK(block != nullptr);
		CHECK(block->timestampNs == expected);
		fifo.commit_pop();
	}
	CHECK(fifo.begin_pop() == nullptr);
	CHECK(fifo.underruns() == 1);
}

void fifo_is_spsc_safe_under_sustained_contention()
{
	constexpr uint64_t Count = 20'000;
	PcmSpscFifo<4> fifo;
	std::atomic_bool failed{false};

	std::thread producer([&] {
		for (uint64_t value = 1; value <= Count; ++value) {
			auto *block = fifo.begin_push();
			while (!block) {
				std::this_thread::yield();
				block = fifo.begin_push();
			}
			block->timestampNs = value;
			block->plane(0, 0)[0] = static_cast<float>(value);
			fifo.commit_push();
		}
	});

	std::thread consumer([&] {
		for (uint64_t expected = 1; expected <= Count; ++expected) {
			const auto *block = fifo.begin_pop();
			while (!block) {
				std::this_thread::yield();
				block = fifo.begin_pop();
			}
			if (block->timestampNs != expected || block->plane(0, 0)[0] != static_cast<float>(expected))
				failed.store(true, std::memory_order_relaxed);
			fifo.commit_pop();
		}
	});

	producer.join();
	consumer.join();
	CHECK(!failed.load(std::memory_order_relaxed));
	CHECK(fifo.size() == 0);
}

void timestamp_window_preserves_phase_and_positive_sync_offset()
{
	constexpr uint32_t SampleRate = 1'000;
	constexpr uint64_t NsPerFrame = 1'000'000;
	constexpr uint64_t WindowStart = 1'000'000'000;
	PcmSpscFifo<4> fifo;
	PcmWindowCursor cursor;

	auto enqueue = [&](const uint64_t timestamp, const float base) {
		auto *block = fifo.begin_push();
		CHECK(block != nullptr);
		block->timestampNs = timestamp;
		block->alignmentFrames = 10;
		block->sampleRate = SampleRate;
		block->channels = 1;
		block->mixerMask = 1;
		for (std::size_t frame = 0; frame < PcmAudioBlock::Frames; ++frame)
			block->plane(0, 0)[frame] = base + static_cast<float>(frame);
		fifo.commit_push();
	};

	enqueue(WindowStart + 10 * NsPerFrame, 1.0F);
	enqueue(WindowStart + (PcmAudioBlock::Frames + 10) * NsPerFrame, 2'001.0F);
	std::array<float, PcmAudioBlock::Frames> first{};
	cursor.consume(
		fifo, SampleRate, first.size(), [](const PcmAudioBlock &) { return true; },
		[&](const PcmAudioBlock &block, const std::size_t sourceOffset, const std::size_t destinationOffset,
		    const std::size_t frames) {
			std::copy_n(block.plane(0, 0) + sourceOffset, frames, first.data() + destinationOffset);
		});
	for (std::size_t frame = 0; frame < 10; ++frame)
		CHECK(first[frame] == 0.0F);
	CHECK(first[10] == 1.0F);
	CHECK(first.back() == 1'014.0F);
	CHECK(cursor.source_offset_frames() == 1'014);
	CHECK(fifo.size() == 2);

	std::array<float, PcmAudioBlock::Frames> second{};
	cursor.consume(
		fifo, SampleRate, second.size(), [](const PcmAudioBlock &) { return true; },
		[&](const PcmAudioBlock &block, const std::size_t sourceOffset, const std::size_t destinationOffset,
		    const std::size_t frames) {
			std::copy_n(block.plane(0, 0) + sourceOffset, frames, second.data() + destinationOffset);
		});
	CHECK(second[0] == 1'015.0F);
	CHECK(second[9] == 1'024.0F);
	CHECK(second[10] == 2'001.0F);
	CHECK(second.back() == 3'014.0F);
	CHECK(cursor.source_offset_frames() == 1'014);
	CHECK(fifo.size() == 1);
}

void positive_alignment_is_retained_while_the_window_stays_silent()
{
	constexpr uint32_t SampleRate = 1'000;
	PcmSpscFifo<2> fifo;
	auto *block = fifo.begin_push();
	CHECK(block != nullptr);
	block->timestampNs = 5'000'000'000;
	block->alignmentFrames = static_cast<int64_t>(PcmAudioBlock::Frames * 2);
	block->sampleRate = SampleRate;
	block->channels = 1;
	fifo.commit_push();
	block = fifo.begin_push();
	CHECK(block != nullptr);
	block->timestampNs = 5'000'000'000 + PcmAudioBlock::Frames * 1'000'000ULL;
	block->alignmentFrames = static_cast<int64_t>(PcmAudioBlock::Frames * 2);
	block->sampleRate = SampleRate;
	block->channels = 1;
	fifo.commit_push();

	PcmWindowCursor cursor;
	std::size_t copied = 0;
	cursor.consume(
		fifo, SampleRate, PcmAudioBlock::Frames, [](const PcmAudioBlock &) { return true; },
		[&](const PcmAudioBlock &, std::size_t, std::size_t, const std::size_t frames) { copied += frames; });
	CHECK(copied == 0);
	CHECK(fifo.size() == 2);
	CHECK(cursor.source_offset_frames() == 0);
}

void positive_alignment_countdown_does_not_double_preroll()
{
	constexpr uint32_t SampleRate = 48'000;
	constexpr int64_t Alignment = static_cast<int64_t>(PcmAudioBlock::Frames * 2);
	PcmSpscFifo<4> fifo;
	PcmWindowCursor cursor;

	auto enqueue = [&](const std::size_t blockIndex) {
		auto *block = fifo.begin_push();
		CHECK(block != nullptr);
		block->timestampNs = 10'000'000'000ULL + static_cast<uint64_t>(blockIndex * PcmAudioBlock::Frames) *
								 1'000'000'000ULL / SampleRate;
		block->alignmentFrames = Alignment;
		block->sampleRate = SampleRate;
		block->channels = 1;
		block->mixerMask = 1;
		for (std::size_t frame = 0; frame < PcmAudioBlock::Frames; ++frame)
			block->plane(0, 0)[frame] = static_cast<float>(blockIndex * PcmAudioBlock::Frames + frame + 1);
		fifo.commit_push();
	};
	auto consume = [&] {
		std::array<float, PcmAudioBlock::Frames> output{};
		cursor.consume(
			fifo, SampleRate, output.size(), [](const PcmAudioBlock &) { return true; },
			[&](const PcmAudioBlock &source, const std::size_t sourceOffset,
			    const std::size_t destinationOffset, const std::size_t frames) {
				std::copy_n(source.plane(0, 0) + sourceOffset, frames,
					    output.data() + destinationOffset);
			});
		return output;
	};

	enqueue(0);
	for (int window = 0; window < 2; ++window) {
		const auto silence = consume();
		CHECK(std::all_of(silence.begin(), silence.end(), [](const float sample) { return sample == 0.0F; }));
		CHECK(fifo.size() == 1);
	}

	// B0 was intentionally de-duplicated while future-dated.  Once B1
	// arrives, B0 must play immediately instead of paying the +2Q offset a
	// second time for FIFO priming.
	enqueue(1);
	const auto firstAudio = consume();
	for (std::size_t frame = 0; frame < firstAudio.size(); ++frame)
		CHECK(firstAudio[frame] == static_cast<float>(frame + 1));
}

void continuous_48khz_stream_never_drops_or_repeats_samples()
{
	constexpr uint32_t SampleRate = 48'000;
	constexpr std::size_t Windows = 400;
	constexpr uint64_t TimestampBase = 20'000'000'000ULL;
	for (const int64_t initialAlignment : {int64_t{0}, int64_t{1}, int64_t{48}}) {
		PcmSpscFifo<4> fifo;
		PcmWindowCursor cursor;
		uint64_t nextSample = 1;
		std::size_t initialSilence = static_cast<std::size_t>(initialAlignment);

		auto enqueue = [&](const std::size_t blockIndex) {
			auto *block = fifo.begin_push();
			CHECK(block != nullptr);
			const uint64_t firstFrame = static_cast<uint64_t>(blockIndex) * PcmAudioBlock::Frames;
			block->timestampNs = TimestampBase + firstFrame * 1'000'000'000ULL / SampleRate;
			block->alignmentFrames = initialAlignment;
			block->sampleRate = SampleRate;
			block->channels = 1;
			block->mixerMask = 1;
			for (std::size_t frame = 0; frame < PcmAudioBlock::Frames; ++frame)
				block->plane(0, 0)[frame] = static_cast<float>(firstFrame + frame + 1);
			fifo.commit_push();
		};
		enqueue(0);
		enqueue(1);
		for (std::size_t window = 0; window < Windows; ++window) {
			std::array<float, PcmAudioBlock::Frames> output{};
			cursor.consume(
				fifo, SampleRate, output.size(), [](const PcmAudioBlock &) { return true; },
				[&](const PcmAudioBlock &source, const std::size_t sourceOffset,
				    const std::size_t destinationOffset, const std::size_t frames) {
					std::copy_n(source.plane(0, 0) + sourceOffset, frames,
						    output.data() + destinationOffset);
				});

			for (const float sample : output) {
				if (initialSilence != 0) {
					CHECK(sample == 0.0F);
					--initialSilence;
					continue;
				}
				CHECK(sample == static_cast<float>(nextSample));
				++nextSample;
			}
			if (window + 2 < Windows)
				enqueue(window + 2);
		}
		CHECK(nextSample == Windows * PcmAudioBlock::Frames - static_cast<std::size_t>(initialAlignment) + 1);
	}
}

void timestamp_discontinuities_realign_once_without_periodic_drift()
{
	constexpr uint32_t SampleRate = 48'000;
	constexpr uint64_t TimestampBase = 30'000'000'000ULL;
	constexpr std::size_t BlockFrames = PcmAudioBlock::Frames;

	auto run = [](const int64_t discontinuityFrames) {
		PcmSpscFifo<4> fifo;
		PcmWindowCursor cursor;
		uint64_t sourceSequence = 1;
		uint64_t timestamp = TimestampBase;

		auto enqueue = [&](const uint64_t blockTimestamp) {
			auto *block = fifo.begin_push();
			CHECK(block != nullptr);
			block->timestampNs = blockTimestamp;
			block->alignmentFrames = 0;
			block->sampleRate = SampleRate;
			block->channels = 1;
			block->mixerMask = 1;
			for (std::size_t frame = 0; frame < BlockFrames; ++frame)
				block->plane(0, 0)[frame] = static_cast<float>(sourceSequence++);
			fifo.commit_push();
		};

		auto consume = [&] {
			std::array<float, BlockFrames> output{};
			cursor.consume(
				fifo, SampleRate, output.size(), [](const PcmAudioBlock &) { return true; },
				[&](const PcmAudioBlock &source, const std::size_t sourceOffset,
				    const std::size_t destinationOffset, const std::size_t frames) {
					std::copy_n(source.plane(0, 0) + sourceOffset, frames,
						    output.data() + destinationOffset);
				});
			return output;
		};

		enqueue(timestamp);
		timestamp += BlockFrames * 1'000'000'000ULL / SampleRate;
		const uint64_t magnitude =
			static_cast<uint64_t>(discontinuityFrames < 0 ? -discontinuityFrames : discontinuityFrames);
		const uint64_t discontinuityNs = magnitude * 1'000'000'000ULL / SampleRate;
		timestamp = discontinuityFrames < 0 ? timestamp - discontinuityNs : timestamp + discontinuityNs;
		enqueue(timestamp);

		const auto first = consume();
		for (std::size_t frame = 0; frame < BlockFrames; ++frame)
			CHECK(first[frame] == static_cast<float>(frame + 1));

		timestamp += BlockFrames * 1'000'000'000ULL / SampleRate;
		enqueue(timestamp);
		const auto shifted = consume();

		if (discontinuityFrames > 0) {
			for (std::size_t frame = 0; frame < magnitude; ++frame)
				CHECK(shifted[frame] == 0.0F);
			for (std::size_t frame = magnitude; frame < BlockFrames; ++frame)
				CHECK(shifted[frame] == static_cast<float>(BlockFrames + frame - magnitude + 1));
		} else {
			for (std::size_t frame = 0; frame < BlockFrames; ++frame)
				CHECK(shifted[frame] == static_cast<float>(BlockFrames + magnitude + frame + 1));
		}

		// Once resynchronized, the cursor advances only by frame counts.  A
		// long 48 kHz run must not repeat the timestamp correction.
		uint64_t expected = discontinuityFrames > 0 ? 2 * BlockFrames - magnitude + 1
							    : 2 * BlockFrames + magnitude + 1;
		for (std::size_t blockIndex = 0; blockIndex < 200; ++blockIndex) {
			timestamp += BlockFrames * 1'000'000'000ULL / SampleRate;
			enqueue(timestamp);
			const auto output = consume();
			for (const float sample : output) {
				CHECK(sample == static_cast<float>(expected));
				++expected;
			}
		}
	};

	run(100);
	run(-100);
}

void one_block_at_a_time_recovers_negative_alignment_without_drift()
{
	constexpr uint32_t SampleRate = 48'000;
	constexpr uint64_t TimestampBase = 40'000'000'000ULL;
	constexpr int64_t Alignment = -100;
	PcmSpscFifo<4> fifo;
	PcmWindowCursor cursor;

	auto enqueue = [&](const std::size_t blockIndex) {
		auto *block = fifo.begin_push();
		CHECK(block != nullptr);
		const uint64_t firstFrame = static_cast<uint64_t>(blockIndex) * PcmAudioBlock::Frames;
		block->timestampNs = TimestampBase + firstFrame * 1'000'000'000ULL / SampleRate;
		block->alignmentFrames = Alignment;
		block->sampleRate = SampleRate;
		block->channels = 1;
		block->mixerMask = 1;
		for (std::size_t frame = 0; frame < PcmAudioBlock::Frames; ++frame)
			block->plane(0, 0)[frame] = static_cast<float>(firstFrame + frame + 1);
		fifo.commit_push();
	};
	auto consume = [&] {
		std::array<float, PcmAudioBlock::Frames> output{};
		cursor.consume(
			fifo, SampleRate, output.size(), [](const PcmAudioBlock &) { return true; },
			[&](const PcmAudioBlock &source, const std::size_t sourceOffset,
			    const std::size_t destinationOffset, const std::size_t frames) {
				std::copy_n(source.plane(0, 0) + sourceOffset, frames,
					    output.data() + destinationOffset);
			});
		return output;
	};

	enqueue(0);
	const auto preroll = consume();
	CHECK(std::all_of(preroll.begin(), preroll.end(), [](const float sample) { return sample == 0.0F; }));
	CHECK(fifo.size() == 1);

	enqueue(1);
	uint64_t expected = 101;
	for (std::size_t window = 0; window < 200; ++window) {
		const auto output = consume();
		for (const float sample : output) {
			CHECK(sample == static_cast<float>(expected));
			++expected;
		}
		enqueue(window + 2);
	}
}

void large_negative_alignment_survives_fifo_gaps()
{
	constexpr uint32_t SampleRate = 48'000;
	constexpr int64_t Alignment = -static_cast<int64_t>(PcmAudioBlock::Frames * 3);
	PcmSpscFifo<4> fifo;
	PcmWindowCursor cursor;

	auto enqueue = [&](const std::size_t blockIndex) {
		auto *block = fifo.begin_push();
		CHECK(block != nullptr);
		const uint64_t firstFrame = static_cast<uint64_t>(blockIndex) * PcmAudioBlock::Frames;
		block->timestampNs = 50'000'000'000ULL + firstFrame * 1'000'000'000ULL / SampleRate;
		block->alignmentFrames = Alignment;
		block->sampleRate = SampleRate;
		block->channels = 1;
		block->mixerMask = 1;
		for (std::size_t frame = 0; frame < PcmAudioBlock::Frames; ++frame)
			block->plane(0, 0)[frame] = static_cast<float>(firstFrame + frame + 1);
		fifo.commit_push();
	};
	auto consume = [&] {
		std::array<float, PcmAudioBlock::Frames> output{};
		cursor.consume(
			fifo, SampleRate, output.size(), [](const PcmAudioBlock &) { return true; },
			[&](const PcmAudioBlock &source, const std::size_t sourceOffset,
			    const std::size_t destinationOffset, const std::size_t frames) {
				std::copy_n(source.plane(0, 0) + sourceOffset, frames,
					    output.data() + destinationOffset);
			});
		return output;
	};
	const auto allSilent = [](const auto &output) {
		return std::all_of(output.begin(), output.end(), [](const float sample) { return sample == 0.0F; });
	};

	enqueue(0);
	CHECK(allSilent(consume()));
	enqueue(1);
	CHECK(allSilent(consume()));
	CHECK(fifo.size() == 0);
	enqueue(2);
	CHECK(allSilent(consume()));
	CHECK(fifo.size() == 1);
	enqueue(3);
	const auto recovered = consume();
	for (std::size_t frame = 0; frame < recovered.size(); ++frame)
		CHECK(recovered[frame] == static_cast<float>(3 * PcmAudioBlock::Frames + frame + 1));
}

} // namespace

int main()
{
	try {
		bounded_fifo_wraps_without_overwriting_unread_data();
		fifo_is_spsc_safe_under_sustained_contention();
		timestamp_window_preserves_phase_and_positive_sync_offset();
		positive_alignment_is_retained_while_the_window_stays_silent();
		positive_alignment_countdown_does_not_double_preroll();
		continuous_48khz_stream_never_drops_or_repeats_samples();
		timestamp_discontinuities_realign_once_without_periodic_drift();
		one_block_at_a_time_recovers_negative_alignment_without_drift();
		large_negative_alignment_survives_fifo_gaps();
		std::cout << "audio_spsc_fifo_tests: all checks passed\n";
		return 0;
	} catch (const std::exception &error) {
		std::cerr << error.what() << '\n';
		return 1;
	}
}
