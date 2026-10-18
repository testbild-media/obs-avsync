// Video filter "AV-Sync Video-Sensor": detects the phone's light pulses in a source.
#include "frame-grabber.hpp"
#include "plugin.hpp"
#include "video-detector.hpp"

#include <obs-module.h>

#include <QDir>
#include <QImage>
#include <QRegularExpression>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>

namespace avs {

namespace {

struct VideoSensor {
	obs_source_t *self = nullptr;
	int probeId = 0;
	std::string lastName;

	std::mutex cfgMutex;
	FlashDetectorConfig cfg;
	bool cfgDirty = true;
	bool overlay = false;

	// graphics thread only
	FrameGrabber grabber;
	FlashDetector detector;
	FlashDetectorStatus status;
	uint64_t lastCapture = 0;
	uint64_t lastDetectionNs = 0;
	int statusCounter = 0;
};

const char *getName(void *)
{
	return obs_module_text("VideoSensor");
}

void update(void *data, obs_data_t *settings)
{
	auto *s = static_cast<VideoSensor *>(data);
	std::lock_guard<std::mutex> lock(s->cfgMutex);
	s->cfg.autoRoi = obs_data_get_int(settings, "roi_mode") == 0;
	s->cfg.roiX = obs_data_get_double(settings, "roi_x") / 100.0;
	s->cfg.roiY = obs_data_get_double(settings, "roi_y") / 100.0;
	s->cfg.roiW = obs_data_get_double(settings, "roi_w") / 100.0;
	s->cfg.roiH = obs_data_get_double(settings, "roi_h") / 100.0;
	s->cfg.minContrast = obs_data_get_double(settings, "min_contrast");
	s->cfg.periodMs = Plugin::instance().engine().periodMs();
	s->overlay = obs_data_get_bool(settings, "overlay");
	s->cfgDirty = true;
}

void *create(obs_data_t *settings, obs_source_t *source)
{
	auto *s = new VideoSensor();
	s->self = source;
	s->probeId = Plugin::instance().engine().addProbe(Kind::Video, obs_module_text("VideoSensor"), "filter", "");
	s->detector.setCallback([s](const VideoDetection &d) {
		s->lastDetectionNs = d.timeNs;
		Plugin &p = Plugin::instance();
		p.engine().reportDetection(s->probeId, p.utcMs(d.timeNs), d.marker, d.amplitude);
	});
	update(s, settings);
	return s;
}

void destroy(void *data)
{
	auto *s = static_cast<VideoSensor *>(data);
	obs_enter_graphics();
	s->grabber.release();
	obs_leave_graphics();
	Plugin::instance().engine().removeProbe(s->probeId);
	delete s;
}

void getDefaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "roi_mode", 0);
	obs_data_set_default_double(settings, "roi_x", 25.0);
	obs_data_set_default_double(settings, "roi_y", 25.0);
	obs_data_set_default_double(settings, "roi_w", 50.0);
	obs_data_set_default_double(settings, "roi_h", 50.0);
	obs_data_set_default_double(settings, "min_contrast", 12.0);
	obs_data_set_default_bool(settings, "overlay", false);
}

bool roiModeChanged(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const bool manual = obs_data_get_int(settings, "roi_mode") == 1;
	for (const char *name : {"roi_x", "roi_y", "roi_w", "roi_h"})
		obs_property_set_visible(obs_properties_get(props, name), manual);
	return true;
}

obs_properties_t *getProperties(void *)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *mode = obs_properties_add_list(props, "roi_mode", obs_module_text("VideoSensor.RoiMode"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode, obs_module_text("VideoSensor.RoiAuto"), 0);
	obs_property_list_add_int(mode, obs_module_text("VideoSensor.RoiManual"), 1);
	obs_property_set_modified_callback(mode, roiModeChanged);

	struct {
		const char *key, *text;
	} roi[] = {{"roi_x", "VideoSensor.RoiX"},
		   {"roi_y", "VideoSensor.RoiY"},
		   {"roi_w", "VideoSensor.RoiW"},
		   {"roi_h", "VideoSensor.RoiH"}};
	for (const auto &r : roi) {
		obs_property_t *p = obs_properties_add_float_slider(props, r.key, obs_module_text(r.text), 0.0, 100.0, 0.5);
		obs_property_float_set_suffix(p, " %");
	}

	obs_property_t *contrast = obs_properties_add_float_slider(
		props, "min_contrast", obs_module_text("VideoSensor.MinContrast"), 3.0, 80.0, 1.0);
	obs_property_set_long_description(contrast, obs_module_text("VideoSensor.MinContrast.Description"));

	obs_property_t *overlay = obs_properties_add_bool(props, "overlay", obs_module_text("VideoSensor.Overlay"));
	obs_property_set_long_description(overlay, obs_module_text("VideoSensor.Overlay.Description"));

	obs_properties_add_text(props, "info", obs_module_text("VideoSensor.Info"), OBS_TEXT_INFO);
	return props;
}

void videoTick(void *data, float)
{
	auto *s = static_cast<VideoSensor *>(data);
	obs_source_t *parent = obs_filter_get_parent(s->self);
	if (!parent)
		return;
	const char *name = obs_source_get_name(parent);
	if (name && s->lastName != name) {
		s->lastName = name;
		Plugin::instance().engine().setProbeName(s->probeId, name);
		Plugin::instance().engine().setProbeSource(s->probeId, obs_source_get_uuid(parent));
	}
}

void drawRect(float x, float y, float w, float h, float thickness)
{
	auto bar = [](float bx, float by, float bw, float bh) {
		gs_matrix_push();
		gs_matrix_translate3f(bx, by, 0.0f);
		gs_draw_sprite(nullptr, 0, (uint32_t)std::max(1.0f, bw), (uint32_t)std::max(1.0f, bh));
		gs_matrix_pop();
	};
	bar(x, y, w, thickness);
	bar(x, y + h - thickness, w, thickness);
	bar(x, y, thickness, h);
	bar(x + w - thickness, y, thickness, h);
}

void drawOverlay(VideoSensor *s, uint32_t cx, uint32_t cy)
{
	const FlashDetectorStatus &st = s->status;
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	gs_eparam_t *color = gs_effect_get_param_by_name(solid, "color");
	struct vec4 c;
	const bool recent = s->lastDetectionNs && obs_get_video_frame_time() - s->lastDetectionNs < 400000000ULL;
	if (!st.locked)
		vec4_set(&c, 1.0f, 0.6f, 0.1f, 1.0f); // orange: searching
	else if (recent)
		vec4_set(&c, 0.2f, 0.6f, 1.0f, 1.0f); // blue: pulse detected
	else
		vec4_set(&c, 0.2f, 0.9f, 0.4f, 1.0f); // green: locked
	gs_effect_set_vec4(color, &c);

	const float t = std::max(2.0f, (float)cx / 300.0f);
	gs_blend_state_push();
	gs_reset_blend_state();
	while (gs_effect_loop(solid, "Solid")) {
		if (st.locked && st.bboxX1 > st.bboxX0) {
			drawRect((float)(st.bboxX0 * cx), (float)(st.bboxY0 * cy), (float)((st.bboxX1 - st.bboxX0) * cx),
				 (float)((st.bboxY1 - st.bboxY0) * cy), t);
		} else {
			drawRect(t, t, (float)cx - 2 * t, (float)cy - 2 * t, t);
		}
	}
	gs_blend_state_pop();
}

// test hook: AVSYNC_DEBUG_DIR=<folder> stores every 120th analysed frame as PNG
void debugDump(const std::string &name, const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t linesize)
{
	static const char *dir = getenv("AVSYNC_DEBUG_DIR");
	static std::atomic<int> counter{0};
	if (!dir || ++counter % 120 != 0)
		return;
	QImage img(rgba, (int)w, (int)h, (int)linesize, QImage::Format_RGBA8888);
	QString file = QDir(QString::fromLocal8Bit(dir)).filePath(QString::fromStdString(name).replace(QRegularExpression("[^A-Za-z0-9]"), "_") + ".png");
	img.copy().save(file);
}

// test hook: AVSYNC_RECORD=<file> appends the first 1200 analysed frames (raw RGBA) for
// offline replay with avsync-replay
void debugRecord(const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t linesize, uint64_t ts)
{
	static const char *path = getenv("AVSYNC_RECORD");
	static FILE *f = nullptr;
	static int frames = 0;
	if (!path || frames >= 1200)
		return;
	if (!f)
		f = fopen(path, "wb");
	if (!f)
		return;
	const uint32_t hdr[4] = {0x46535641u /* AVSF */, w, h, 0};
	fwrite(hdr, sizeof(hdr), 1, f);
	fwrite(&ts, sizeof(ts), 1, f);
	for (uint32_t y = 0; y < h; y++)
		fwrite(rgba + (size_t)y * linesize, 1, (size_t)w * 4, f);
	if (++frames >= 1200)
		fclose(f);
}

void videoRender(void *data, gs_effect_t *)
{
	auto *s = static_cast<VideoSensor *>(data);
	obs_source_t *target = obs_filter_get_target(s->self);
	obs_source_t *parent = obs_filter_get_parent(s->self);
	if (!target || !parent) {
		obs_source_skip_video_filter(s->self);
		return;
	}
	const uint32_t cx = obs_source_get_base_width(target);
	const uint32_t cy = obs_source_get_base_height(target);

	obs_source_skip_video_filter(s->self);
	if (!cx || !cy)
		return;

	bool overlay;
	{
		std::lock_guard<std::mutex> lock(s->cfgMutex);
		if (s->cfgDirty) {
			s->detector.setConfig(s->cfg);
			s->cfgDirty = false;
		}
		overlay = s->overlay;
	}

	// The filter can be rendered several times per frame (preview, program, multiview).
	const uint64_t t = obs_get_video_frame_time();
	if (t != s->lastCapture) {
		s->lastCapture = t;
		s->grabber.capture(cx, cy, t, [s] { obs_source_skip_video_filter(s->self); });
		s->grabber.collect([s](const uint8_t *rgba, uint32_t w, uint32_t h, uint32_t ls, uint64_t ts) {
			s->detector.pushFrame(rgba, w, h, ls, ts);
			debugDump(s->lastName, rgba, w, h, ls);
			debugRecord(rgba, w, h, ls, ts);
		});
		s->status = s->detector.status();

		if (++s->statusCounter >= 30) {
			s->statusCounter = 0;
			ProbeSignal sig;
			sig.ok = s->status.locked;
			sig.level = s->status.amplitude;
			sig.text = s->status.locked ? obs_module_text("Signal.DisplayFound")
						    : obs_module_text("Signal.SearchingDisplay");
			Plugin::instance().engine().setProbeSignal(s->probeId, sig);
		}
	}

	if (overlay)
		drawOverlay(s, cx, cy);
}

void filterAdd(void *data, obs_source_t *parent)
{
	auto *s = static_cast<VideoSensor *>(data);
	if (!parent)
		return;
	const char *name = obs_source_get_name(parent);
	s->lastName = name ? name : "";
	Plugin::instance().engine().setProbeName(s->probeId, s->lastName);
	Plugin::instance().engine().setProbeSource(s->probeId, obs_source_get_uuid(parent));
}

} // namespace

void registerVideoSensor()
{
	obs_source_info info = {};
	info.id = "avsync_video_sensor";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name = getName;
	info.create = create;
	info.destroy = destroy;
	info.update = update;
	info.get_defaults = getDefaults;
	info.get_properties = getProperties;
	info.filter_add = filterAdd;
	info.video_tick = videoTick;
	info.video_render = videoRender;
	obs_register_source(&info);
}

} // namespace avs
