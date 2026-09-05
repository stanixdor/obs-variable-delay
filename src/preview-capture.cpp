#include "preview-capture.hpp"

#include <graphics/vec4.h>
#include <util/platform.h>

#include <algorithm>
#include <iterator>

namespace dynamic_delay {

PreviewCapture::~PreviewCapture()
{
	stop();
}

bool PreviewCapture::start()
{
	if (connected_)
		return true;
	obs_video_info info{};
	if (!obs_get_video_info(&info))
		return false;
	captureStartedNs_ = os_gettime_ns();
	obs_add_main_rendered_callback(&PreviewCapture::main_rendered, this);
	connected_ = true;
	return true;
}

void PreviewCapture::stop()
{
	if (connected_)
		obs_remove_main_rendered_callback(&PreviewCapture::main_rendered, this);
	connected_ = false;
	// Removal waits for in-flight rendering. Never enter graphics while
	// holding OBS's draw-callback mutex or our history mutex.
	if (render_ || staging_) {
		obs_enter_graphics();
		gs_texrender_destroy(render_);
		gs_stagesurface_destroy(staging_);
		obs_leave_graphics();
	}
	render_ = nullptr;
	staging_ = nullptr;
	cadence_ = {};
	pendingFrameNs_ = 0;
	captureStartedNs_ = 0;
	std::scoped_lock lock(framesMutex_);
	frames_.clear();
	protectedTimestampNs_ = 0;
	holdPinTimestampNs_ = 0;
	suspended_.store(false, std::memory_order_relaxed);
}

void PreviewCapture::set_history_seconds(const uint32_t seconds) noexcept
{
	historySeconds_.store(std::clamp(seconds, 3U, 3603U), std::memory_order_relaxed);
}

void PreviewCapture::set_playback_state(const bool paused, const bool emittingHold, const bool emittingDelayed,
					const uint64_t emittedTimestampNs, const uint64_t bufferStartTimestampNs)
{
	std::scoped_lock lock(framesMutex_);
	if (emittingHold) {
		// A pause while filling must also preserve frames that have not yet
		// reached the output. Keep that finite window until the delayed splice.
		if ((paused || holdPinTimestampNs_ != 0) && bufferStartTimestampNs != 0)
			holdPinTimestampNs_ = bufferStartTimestampNs;
		protectedTimestampNs_ = holdPinTimestampNs_;
	} else {
		holdPinTimestampNs_ = 0;
		protectedTimestampNs_ = emittingDelayed ? emittedTimestampNs : 0;
	}
	suspended_.store(paused, std::memory_order_release);
}

PreviewCapture::Frame PreviewCapture::frame_at(const uint64_t timestampNs) const
{
	std::scoped_lock lock(framesMutex_);
	const auto after = std::upper_bound(frames_.begin(), frames_.end(), timestampNs,
					    [](const uint64_t timestamp, const Frame &frame) {
						    return timestamp < frame.capturedAtNs;
					    });
	return after == frames_.begin() ? Frame{} : *std::prev(after);
}

void PreviewCapture::main_rendered(void *param)
{
	if (param)
		static_cast<PreviewCapture *>(param)->capture_frame();
}

void PreviewCapture::capture_frame()
{
	const uint64_t timestampNs = obs_get_video_frame_time();
	gs_texture_t *mainTexture = obs_get_main_texture();
	if (!mainTexture || !cadence_.begin_frame(timestampNs))
		return;
	// Map on the next video frame, after the preceding thumbnail transfer.
	// Sampling occurs BEFORE readback, never at output resolution/frequency.
	read_pending_frame();
	if (suspended_.load(std::memory_order_acquire))
		return;
	if (!cadence_.sample_due(timestampNs))
		return;
	cadence_.sampled(timestampNs);
	if (!render_)
		render_ = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	if (!staging_)
		staging_ = gs_stagesurface_create(Width, Height, GS_BGRA);
	if (!render_ || !staging_)
		return;
	gs_texrender_reset(render_);
	if (!gs_texrender_begin_with_color_space(render_, Width, Height, GS_CS_SRGB))
		return;
	vec4 background;
	vec4_set(&background, 0.0F, 0.0F, 0.0F, 1.0F);
	gs_clear(GS_CLEAR_COLOR, &background, 1.0F, 0);
	gs_ortho(0.0F, static_cast<float>(gs_texture_get_width(mainTexture)), 0.0F,
		 static_cast<float>(gs_texture_get_height(mainTexture)), -100.0F, 100.0F);
	// OBS handles SDR/HDR conversion. Sources/scenes are not rendered again.
	obs_render_main_texture();
	gs_texrender_end(render_);
	gs_stage_texture(staging_, gs_texrender_get_texture(render_));
	pendingFrameNs_ = timestampNs;
}

void PreviewCapture::read_pending_frame()
{
	if (pendingFrameNs_ == 0 || !staging_)
		return;
	uint8_t *pixels = nullptr;
	uint32_t stride = 0;
	if (!gs_stagesurface_map(staging_, &pixels, &stride)) {
		pendingFrameNs_ = 0;
		return;
	}
	QImage wrapped(pixels, Width, Height, static_cast<qsizetype>(stride), QImage::Format_ARGB32);
	Frame saved{pendingFrameNs_, wrapped.convertToFormat(QImage::Format_RGB16)};
	gs_stagesurface_unmap(staging_);
	pendingFrameNs_ = 0;
	if (saved.image.isNull())
		return;
	const uint32_t historySeconds = historySeconds_.load(std::memory_order_relaxed);
	const uint64_t keepNs = static_cast<uint64_t>(historySeconds) * 1'000'000'000ULL;
	std::scoped_lock lock(framesMutex_);
	// Do not mix clock epochs or binary-search an unsorted history.
	if (!frames_.empty() && saved.capturedAtNs <= frames_.back().capturedAtNs)
		frames_.clear();
	uint64_t cutoff = saved.capturedAtNs > keepNs ? saved.capturedAtNs - keepNs : 0;
	if (protectedTimestampNs_ != 0)
		cutoff = std::min(cutoff, protectedTimestampNs_);
	// Retain the nearest sample at/before the cutoff, including when a long
	// recording pause leaves a large wall-clock hole in the finite history.
	while (frames_.size() > 1 && frames_[1].capturedAtNs <= cutoff)
		frames_.pop_front();
	const auto maxFrames = static_cast<std::size_t>(historySeconds) * 2U + 4U;
	if (frames_.size() >= maxFrames) {
		// A stalled encoder must never make the preview grow unbounded. Keep
		// the still-required history rather than overwriting the paused frame.
		if (protectedTimestampNs_ != 0)
			return;
		frames_.pop_front();
	}
	frames_.push_back(std::move(saved));
}

} // namespace dynamic_delay
