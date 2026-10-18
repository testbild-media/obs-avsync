// Detects the AV-Sync light pulses in a stream of small RGBA frames.
//
// 1. The frame is reduced to a grid of luma cells.
// 2. Auto-ROI: every cell's luma is folded over the event period; cells whose folded
//    profile shows a strong pulse belong to the phone screen.
// 3. The mean luma of the ROI is tracked; each pulse is reduced to its centroid
//    (centre of mass in time), which is independent of the camera's frame phase.
#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <vector>

namespace avs {

struct VideoDetection {
	uint64_t timeNs = 0;   // pulse centre in the caller's timestamp domain
	bool marker = false;   // wide pulse
	double sigmaMs = 0.0;  // temporal spread of the pulse
	double amplitude = 0.0; // luma levels (0..255)
	int samples = 0;       // distinct frames that contributed
};

struct FlashDetectorConfig {
	bool autoRoi = true;
	// manual ROI, normalised 0..1
	double roiX = 0.25, roiY = 0.25, roiW = 0.5, roiH = 0.5;
	double minContrast = 12.0; // luma levels
	double periodMs = 1000.0;
};

struct FlashDetectorStatus {
	bool locked = false;
	int gridW = 0, gridH = 0;
	std::vector<uint8_t> cellMask; // 1 = cell belongs to ROI
	double bboxX0 = 0, bboxY0 = 0, bboxX1 = 0, bboxY1 = 0; // normalised
	double signal = 0, baseline = 0, amplitude = 0;
	double frameRate = 0; // distinct frames per second
	bool inPulse = false;
	int roiChanges = 0;   // how often the ROI was (re)selected
};

class FlashDetector {
public:
	using Callback = std::function<void(const VideoDetection &)>;

	FlashDetector();

	void setConfig(const FlashDetectorConfig &cfg);
	const FlashDetectorConfig &config() const { return cfg_; }
	void setCallback(Callback cb) { callback_ = std::move(cb); }

	// rgba: 8 bit RGBA rows, timeNs: time at which this frame appears.
	void pushFrame(const uint8_t *rgba, uint32_t width, uint32_t height, uint32_t linesize, uint64_t timeNs);

	// Feed an already computed ROI signal (used by tests and by sources without frames).
	void pushSignal(double value, uint64_t timeNs);

	FlashDetectorStatus status() const { return status_; }
	void reset();

private:
	struct Sample {
		double tMs;
		double v;
	};

	void resizeGrid(int gw, int gh);
	void updateFold(double tMs);
	void updateRoi();
	void resetPulseState();
	void finalizePulse();

	FlashDetectorConfig cfg_;
	Callback callback_;

	// grid
	int gridW_ = 0, gridH_ = 0;
	uint32_t srcW_ = 0, srcH_ = 0;
	std::vector<float> cells_, prevCells_;
	bool havePrev_ = false;

	// fold profiles: per cell kBins values
	static constexpr int kBins = 50;
	std::vector<float> fold_;
	std::vector<uint8_t> foldHas_;
	int framesSinceRoi_ = 0;
	double foldStartMs_ = 0.0;
	double lastFoldMs_ = 0.0;
	std::vector<uint8_t> roi_;
	bool locked_ = false;

	// pulse tracking
	std::deque<Sample> history_;
	bool inPulse_ = false;
	std::vector<Sample> pulse_;
	double pulsePeak_ = 0.0;
	double pulseStartMs_ = 0.0;
	int belowCount_ = 0;
	double baseline_ = 0.0;
	double noise_ = 0.0;
	double ampRef_ = 0.0;
	double lastValue_ = 0.0;
	bool haveLast_ = false;
	std::deque<double> sigmaHistory_; // recent pulse widths for relative marker detection
	double ignorePulsesBeforeMs_ = 0.0;
	bool prevAbove_ = true;

	// frame rate estimate
	double lastFrameMs_ = 0.0;
	double frameIntervalMs_ = 0.0;

	FlashDetectorStatus status_;
};

} // namespace avs
