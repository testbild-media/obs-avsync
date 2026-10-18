// Replays frames recorded by the plugin (AVSYNC_RECORD) through the FlashDetector.
// Usage: avsync-replay <file> [period_ms]
#include "../src/video-detector.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace avs;

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: avsync-replay <file>\n");
		return 1;
	}
	FILE *f = fopen(argv[1], "rb");
	if (!f) {
		fprintf(stderr, "cannot open %s\n", argv[1]);
		return 1;
	}
	FlashDetector det;
	det.setCallback([](const VideoDetection &d) {
		const double tMs = (double)d.timeNs / 1e6;
		printf("  pulse t=%.3f frac=%+.2f ms sigma=%.1f amp=%.1f n=%d marker=%d\n", tMs / 1000.0,
		       tMs - 1000.0 * std::round(tMs / 1000.0), d.sigmaMs, d.amplitude, d.samples, d.marker);
	});
	std::vector<uint8_t> buf;
	int frame = 0, lastChanges = 0;
	bool lastLocked = false;
	uint64_t firstTs = 0;
	while (true) {
		uint32_t hdr[4];
		uint64_t ts;
		if (fread(hdr, sizeof(hdr), 1, f) != 1 || fread(&ts, sizeof(ts), 1, f) != 1)
			break;
		if (hdr[0] != 0x46535641u)
			break;
		const uint32_t w = hdr[1], h = hdr[2];
		buf.resize((size_t)w * h * 4);
		if (fread(buf.data(), 1, buf.size(), f) != buf.size())
			break;
		if (!firstTs)
			firstTs = ts;
		det.pushFrame(buf.data(), w, h, w * 4, ts);
		FlashDetectorStatus st = det.status();
		if (st.locked != lastLocked || st.roiChanges != lastChanges) {
			int cells = 0;
			for (uint8_t c : st.cellMask)
				cells += c;
			printf("frame %d t=%.3f locked=%d roiChanges=%d cells=%d bbox=(%.2f,%.2f)-(%.2f,%.2f)\n", frame,
			       (ts - firstTs) / 1e9, st.locked, st.roiChanges, cells, st.bboxX0, st.bboxY0, st.bboxX1,
			       st.bboxY1);
			lastLocked = st.locked;
			lastChanges = st.roiChanges;
		}
		frame++;
	}
	FlashDetectorStatus st = det.status();
	printf("%d frames, fps=%.1f, signal=%.1f baseline=%.1f amp=%.1f\n", frame, st.frameRate, st.signal, st.baseline,
	       st.amplitude);
	return 0;
}
