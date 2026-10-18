#include "frame-grabber.hpp"

#include <algorithm>
#include <cmath>

namespace avs {

void FrameGrabber::release()
{
	gs_texrender_destroy(mid_);
	gs_texrender_destroy(small_);
	mid_ = small_ = nullptr;
	for (int i = 0; i < kStages; i++) {
		gs_stagesurface_destroy(stage_[i]);
		stage_[i] = nullptr;
		stageValid_[i] = false;
	}
	smallW_ = smallH_ = 0;
}

void FrameGrabber::capture(uint32_t cx, uint32_t cy, uint64_t timeNs, const std::function<void()> &drawContent)
{
	if (!cx || !cy)
		return;

	// intermediate size: at most 2x downscale per step keeps bilinear sampling honest
	const uint32_t midW = std::min<uint32_t>(cx, kTargetWidth * 4);
	const uint32_t midH = std::max<uint32_t>(8, (uint32_t)std::lround((double)cy * midW / cx));
	uint32_t w, h;
	if (cx >= cy) {
		w = kTargetWidth;
		h = std::max<uint32_t>(8, (uint32_t)std::lround((double)cy * kTargetWidth / cx));
	} else {
		h = kTargetWidth;
		w = std::max<uint32_t>(8, (uint32_t)std::lround((double)cx * kTargetWidth / cy));
	}
	w &= ~3u;
	h &= ~3u;

	if (!mid_)
		mid_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (!small_)
		small_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	if (w != smallW_ || h != smallH_) {
		for (int i = 0; i < kStages; i++) {
			gs_stagesurface_destroy(stage_[i]);
			stage_[i] = gs_stagesurface_create(w, h, GS_RGBA);
			stageValid_[i] = false;
		}
		smallW_ = w;
		smallH_ = h;
	}

	const bool prevSrgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	bool ok = false;
	gs_texrender_reset(mid_);
	if (gs_texrender_begin(mid_, midW, midH)) {
		struct vec4 clear;
		vec4_zero(&clear);
		gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
		gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
		drawContent();
		gs_texrender_end(mid_);
		ok = true;
	}

	if (ok) {
		ok = false;
		gs_texture_t *midTex = gs_texrender_get_texture(mid_);
		gs_texrender_reset(small_);
		if (midTex && gs_texrender_begin(small_, w, h)) {
			gs_ortho(0.0f, (float)midW, 0.0f, (float)midH, -100.0f, 100.0f);
			gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_BILINEAR_LOWRES);
			gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), midTex);
			gs_eparam_t *mul = gs_effect_get_param_by_name(effect, "multiplier");
			if (mul)
				gs_effect_set_float(mul, 1.0f);
			while (gs_effect_loop(effect, "Draw"))
				gs_draw_sprite(midTex, 0, midW, midH);
			gs_texrender_end(small_);
			ok = true;
		}
	}

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(prevSrgb);

	if (ok) {
		gs_stage_texture(stage_[next_], gs_texrender_get_texture(small_));
		stageTime_[next_] = timeNs;
		stageValid_[next_] = true;
		next_ = (next_ + 1) % kStages;
	}
}

void FrameGrabber::collect(const FrameCallback &cb)
{
	// next_ is the oldest staged surface (captured kStages-1 frames ago)
	const int idx = next_;
	if (!stageValid_[idx] || !stage_[idx])
		return;
	uint8_t *data = nullptr;
	uint32_t linesize = 0;
	if (gs_stagesurface_map(stage_[idx], &data, &linesize)) {
		cb(data, smallW_, smallH_, linesize, stageTime_[idx]);
		gs_stagesurface_unmap(stage_[idx]);
	}
	stageValid_[idx] = false;
}

} // namespace avs
