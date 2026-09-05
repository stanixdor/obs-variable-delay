#pragma once

#include "core/preview-timing.hpp"

#include <obs.h>
#include <QImage>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>

namespace dynamic_delay {

// Independent of the dock: hiding it retains history without repainting.
// No raw-output consumer or extra encoder is used.
class PreviewCapture final {
public:
	static constexpr uint32_t Width = 320;
	static constexpr uint32_t Height = 180;
	struct Frame {
		uint64_t capturedAtNs = 0;
		QImage image;
	};

	PreviewCapture() = default;
	~PreviewCapture();
	PreviewCapture(const PreviewCapture &) = delete;
	PreviewCapture &operator=(const PreviewCapture &) = delete;
	bool start();
	void stop();
	void set_history_seconds(uint32_t seconds) noexcept;
	void set_playback_state(bool paused, bool emittingHold, bool emittingDelayed, uint64_t emittedTimestampNs,
				uint64_t bufferStartTimestampNs = 0);
	[[nodiscard]] Frame frame_at(uint64_t timestampNs) const;
	[[nodiscard]] uint64_t started_at_ns() const noexcept { return captureStartedNs_; }

private:
	static void main_rendered(void *param);
	void capture_frame();
	void read_pending_frame();
	mutable std::mutex framesMutex_;
	std::deque<Frame> frames_;
	std::atomic<uint32_t> historySeconds_{33};
	std::atomic_bool suspended_{false};
	uint64_t protectedTimestampNs_ = 0;
	uint64_t holdPinTimestampNs_ = 0;
	gs_texrender_t *render_ = nullptr;
	gs_stagesurf_t *staging_ = nullptr;
	core::PreviewCadence cadence_;
	uint64_t pendingFrameNs_ = 0;
	uint64_t captureStartedNs_ = 0;
	bool connected_ = false;
};

} // namespace dynamic_delay
