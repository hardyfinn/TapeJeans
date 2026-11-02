// Offline tests for the Tapejeans engine and WAV reader/writer.
//   c++ -std=c++11 -O2 -o test/engine_test test/engine_test.cpp && test/engine_test
#include "../src/TapeEngine.hpp"
#include "../src/Wav.hpp"
#include <cstdio>
#include <cstdlib>

using namespace tj;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

// Record `frames` engine samples of constant value v into a new splice
// (S.O.S. fully CCW so only the input is recorded, as on the hardware)
static void recordDC(TapeEngine& e, float v, int samples) {
	float sos = e.sos;
	e.sos = 0.f;
	e.recNewButton();
	for (int i = 0; i < samples; i++) e.process(v, v, false, false);
	e.recNewButton();  // stop
	e.sos = sos;
}

static double meanOut(TapeEngine& e, int samples, int* eosgCount = nullptr) {
	double s = 0;
	int n = 0;
	for (int i = 0; i < samples; i++) {
		e.process(0, 0, false, false);
		s += e.outL;
		if (e.eosg) n++;
	}
	if (eosgCount) *eosgCount = n;
	return s / samples;
}

int main() {
	TapeEngine* ep = new TapeEngine();
	TapeEngine& e = *ep;
	e.setSampleRate(48000);

	// 1. empty reel: silence, REC records a first splice
	CHECK(e.empty(), "starts empty");
	e.process(0.3f, 0.3f, false, false);
	CHECK(std::fabs(e.playL) < 1e-6, "empty reel plays nothing");
	recordDC(e, 0.5f, 48000);
	CHECK(e.numSplices == 1, "one splice after first recording, got %d", e.numSplices);
	CHECK(std::abs(e.length - 48000) <= 2, "1 s recorded, got %d frames", e.length);

	// 2. plays back what was recorded, one EOSG per splice at speed 1
	e.sos = 1.f;
	int eo = 0;
	double m = meanOut(e, 48000 * 2, &eo);
	CHECK(m > 0.4 && m < 0.55, "plays back the DC level, mean %.3f", m);
	CHECK(eo >= 2 && eo <= 3, "EOSG once per splice loop, got %d in 2 s", eo);

	// 3. second splice, Organize picks it at the end of the gene
	recordDC(e, -0.5f, 24000);
	CHECK(e.numSplices == 2, "two splices, got %d", e.numSplices);
	CHECK(e.current == 1, "new splice plays after recording");
	m = meanOut(e, 12000);
	CHECK(m < -0.3, "hears splice 2, mean %.3f", m);
	// Organize acts when the knob (or its CV) moves
	e.organize = 0.9f;
	meanOut(e, 100);
	e.organize = 0.f;
	m = meanOut(e, 48000 * 2);
	CHECK(e.current == 0, "organize 0 selects splice 1, current %d", e.current);
	m = meanOut(e, 24000);
	CHECK(m > 0.3, "hears splice 1 again, mean %.3f", m);

	// 4. Shift moves to the next splice (wrapping)
	e.organize = 0.f;
	e.shiftSplice();
	CHECK(e.pending == 1, "shift -> pending 1");
	meanOut(e, 48000 * 2);
	CHECK(e.current == 1, "shift applied at gene end");
	e.shiftSplice();
	CHECK(e.pending == 0, "shift wraps to 0");
	meanOut(e, 48000);

	// 5. Vari-Speed at noon stops playback
	e.speed = 0.f;
	meanOut(e, 4800);
	m = meanOut(e, 4800);
	CHECK(std::fabs(m) < 0.01, "speed 0 is silent, mean %.4f", m);
	e.speed = -1.f;
	meanOut(e, 4800);
	m = meanOut(e, 4800);
	CHECK(std::fabs(m) > 0.2, "reverse plays, mean %.3f", m);
	e.speed = 1.f;
	CHECK(std::fabs(speedFromKnob(0.5f) - 1.f) < 1e-4, "knob 2:30 = original speed");
	CHECK(std::fabs(speedFromKnob(1.f) - 2.f) < 1e-3, "full CW = +1 octave");
	CHECK(speedFromKnob(-0.5f) < 0, "CCW = reverse");
	CHECK(std::fabs(speedFromKnob(0.f)) < 1e-6, "noon = stop");

	// 6. Gene size + Morph: more genes per second
	e.current = e.pending = 0;
	e.lastOrganize = e.organize;
	e.geneSize = 0.5f;
	e.morph = MORPH_SEAMLESS;
	int e1 = 0, e2 = 0, e3 = 0;
	meanOut(e, 4800);
	meanOut(e, 48000, &e1);
	e.morph = 1.f;
	meanOut(e, 4800);
	meanOut(e, 48000, &e2);
	e.morph = 0.f;
	meanOut(e, 4800);
	meanOut(e, 48000, &e3);
	CHECK(e2 > e1 * 3, "Morph full overlaps ~4x: %d vs %d", e2, e1);
	CHECK(e3 < e1, "Morph CCW leaves gaps: %d vs %d", e3, e1);
	CHECK(e.geneMaterial(48000) < 48000 && e.geneMaterial(48000) > 96, "gene smaller than splice");
	e.morph = MORPH_SEAMLESS;
	e.geneSize = 0.f;

	// 7. Splice marker, delete marker, delete audio, clear
	int before = e.numSplices;
	CHECK(e.addMarker(10000), "marker added");
	CHECK(e.numSplices == before + 1 && e.markers[1] == 10000, "marker at 10000");
	CHECK(!e.addMarker(10010), "marker too close is refused");
	e.current = 0;
	e.deleteMarker();
	CHECK(e.numSplices == before, "delete marker merges");
	int len = e.length;
	e.current = 1;
	int n1 = e.spliceLen(1);
	e.deleteSpliceAudio();
	CHECK(e.length == len - n1 && e.numSplices == before - 1, "splice audio deleted: len %d", e.length);
	e.clearMarkers();
	CHECK(e.numSplices == 1, "clear markers");
	e.clearReel();
	CHECK(e.empty(), "clear reel");

	// 8. Time Lag Accumulation: S.O.S. CW keeps the loop, CCW replaces it
	recordDC(e, 0.5f, 24000);
	e.sos = 1.f;
	e.recButton();
	for (int i = 0; i < 48000; i++) e.process(-0.5f, -0.5f, false, false);
	e.recButton();
	m = meanOut(e, 24000);
	CHECK(m > 0.4, "SOS CW keeps the loop, mean %.3f", m);
	e.sos = 0.f;
	e.recButton();
	for (int i = 0; i < 30000; i++) e.process(-0.5f, -0.5f, false, false);
	e.recButton();
	e.sos = 1.f;
	meanOut(e, 24000);
	m = meanOut(e, 24000);
	CHECK(m < -0.4, "SOS CCW overwrites with input, mean %.3f", m);

	// 9. Clock: Gene Shift moves the window on each clock
	e.clearReel();
	e.recNewButton();  // empty reel: records the input whatever S.O.S. is
	for (int i = 0; i < 48000; i++) e.process(i < 24000 ? 0.5f : -0.5f, 0, false, false);
	e.recNewButton();
	e.clockConnected = true;
	e.geneSize = 0.3f;
	double g = e.geneMaterial(48000);
	double s0 = e.scan;
	e.process(0, 0, true, false);
	CHECK(std::fabs(e.scan - wrapPos(s0 + g, 48000)) < 1, "clock shifts by one gene");
	// record waits for the clock
	e.recButton();
	CHECK(e.rec == TapeEngine::REC_ARMED_CURRENT, "REC armed until clock");
	e.process(0, 0, true, false);
	CHECK(e.rec == TapeEngine::REC_CURRENT, "REC starts on clock");
	e.recButton();
	e.process(0, 0, true, false);
	CHECK(e.rec == TapeEngine::REC_IDLE, "REC stops on clock");
	e.clockConnected = false;
	e.geneSize = 0.f;

	// 10. 44.1 kHz: 1 s of recording is still 1 s of reel
	TapeEngine* e44 = new TapeEngine();
	e44->setSampleRate(44100);
	recordDC(*e44, 0.25f, 44100);
	CHECK(std::abs(e44->length - 48000) <= 3, "44.1k record length %d", e44->length);
	int eo44 = 0;
	meanOut(*e44, 44100 * 2, &eo44);
	CHECK(eo44 >= 2 && eo44 <= 3, "44.1k loops once a second, %d", eo44);

	// 11. Max reel length stops recording
	TapeEngine* big = new TapeEngine();
	big->setSampleRate(48000);
	big->recNewButton();
	for (int i = 0; i < MAX_FRAMES + 1000; i++) big->process(0.1f, 0.1f, false, false);
	CHECK(big->rec == TapeEngine::REC_IDLE && big->length == MAX_FRAMES, "auto-stop at 2.9 min, len %d", big->length);

	// 12. WAV round trip keeps audio and splice markers
	TapeEngine w;
	w.setSampleRate(48000);
	recordDC(w, 0.5f, 4800);
	recordDC(w, -0.25f, 4800);
	recordDC(w, 0.125f, 4800);
	std::string path = "/tmp/tapejeans_test.wav";
	std::string err = writeWav(path, w.bufL.data(), w.bufR.data(), w.length, w.markers, w.numSplices);
	CHECK(err.empty(), "write: %s", err.c_str());
	WavData wd;
	err = readWav(path, wd, MAX_FRAMES);
	CHECK(err.empty(), "read: %s", err.c_str());
	CHECK((int) wd.L.size() == w.length, "frames %d vs %d", (int) wd.L.size(), w.length);
	CHECK(wd.cues.size() == 2, "two cue markers, got %d", (int) wd.cues.size());
	TapeEngine w2;
	w2.setReel(wd.L, wd.R, wd.cues);
	CHECK(w2.numSplices == 3, "3 splices after load, got %d", w2.numSplices);
	CHECK(std::fabs(w2.bufL[w2.markers[1] + 100] + 0.25f) < 1e-6, "audio intact");

	delete ep; delete e44; delete big;
	if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
	std::printf("all engine tests passed\n");
	return 0;
}
