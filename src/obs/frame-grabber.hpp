// Renders content into a small RGBA image on the GPU and reads it back asynchronously
// (2 frames later, so the render thread never stalls). Must be used on the graphics thread.
#pragma once

#include <obs-module.h>

#include <cstdint>
#include <functional>

namespace avs {

class FrameGrabber {
public:
	using FrameCallback = std::function<void(const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t linesize, uint64_t t)>;

	FrameGrabber() = default;
	FrameGrabber(const FrameGrabber &) = delete;
	FrameGrabber &operator=(const FrameGrabber &) = delete;

	// drawContent renders the content in a cx x cy coordinate space.
	void capture(uint32_t cx, uint32_t cy, uint64_t timeNs, const std::function<void()> &drawContent);
	// Delivers the oldest finished capture (if any).
	void collect(const FrameCallback &cb);
	// Frees GPU resources (graphics context must be entered).
	void release();

	static constexpr uint32_t kTargetWidth = 256;

private:
	gs_texrender_t *mid_ = nullptr;
	gs_texrender_t *small_ = nullptr;
	static constexpr int kStages = 3;
	gs_stagesurf_t *stage_[kStages] = {};
	uint64_t stageTime_[kStages] = {};
	bool stageValid_[kStages] = {};
	int next_ = 0;
	uint32_t smallW_ = 0, smallH_ = 0;
};

} // namespace avs
