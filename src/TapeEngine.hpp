// TapeEngine.hpp
// The sound engine of Tapejeans: a reel of stereo audio cut into splices,
// played back as "genes" (short windows) that can repeat, overlap and shift.
// Behaviour follows the Make Noise Morphagene manual. No VCV Rack dependencies,
// so test/engine_test.cpp can check it offline.
//
// Words used here (the same as on the hardware):
//   reel   - the whole recording (up to 2.9 minutes, stored at 48 kHz)
//   splice - a section of the reel between two markers
//   gene   - the playback window inside a splice. Gene Size sets its length,
//            Slide sets where it starts. Morph sets how genes repeat/overlap.
#pragma once
#include <vector>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>

namespace tj {

static const int REEL_RATE = 48000;
static const int MAX_FRAMES = 174 * REEL_RATE;  // 2.9 minutes, like the hardware
static const int MAX_SPLICES = 300;
static const int NUM_VOICES = 4;                // up to 4 overlapping genes
static const int PEAK_BLOCK = 256;              // frames per waveform-overview block
static const int NUM_PEAK_BLOCKS = MAX_FRAMES / PEAK_BLOCK + 1;
static const int MIN_SPLICE = 48;               // 1 ms: shortest allowed splice

// Morph knob landmarks (0..1). Below MORPH_SEAMLESS there are gaps between
// genes; at it genes join seamlessly (1/1); above it they overlap, up to 4.
static const float MORPH_SEAMLESS = 0.2f;
static const float MORPH_PITCH = 0.85f;         // pitch layers start here (~2:30)

// Small, fast random generator (xorshift)
struct Rng {
	uint64_t s = 0x9E3779B97F4A7C15ULL;
	void seed(uint64_t v) { s = v ? v : 1; }
	uint64_t next() { s ^= s << 13; s ^= s >> 7; s ^= s << 17; return s; }
	float uniform() { return (next() >> 40) * (1.f / 16777216.f); }
};

inline double wrapPos(double x, double len) {
	if (len <= 0) return 0;
	x = std::fmod(x, len);
	if (x < 0) x += len;
	return x;
}

// Vari-Speed knob (-1..1, noon = 0) to playback rate, like the hardware:
// 9:30 / 2:30 (|x| = 0.5) is original speed, full turn is +12 semitones,
// near the centre it goes down to -26 semitones and then stops.
inline float speedFromKnob(float x) {
	float a = std::fabs(x);
	float sign = x < 0 ? -1.f : 1.f;
	float rate;
	const float lowest = std::pow(2.f, -26.f / 12.f);
	if (a >= 0.5f)
		rate = std::pow(2.f, 24.f * (std::min(a, 1.f) - 0.5f) / 12.f);
	else if (a >= 0.05f)
		rate = std::pow(2.f, -26.f * (0.5f - a) / 0.45f / 12.f);
	else
		rate = lowest * a / 0.05f;
	return sign * rate;
}

struct TapeEngine {
	enum PlayMode { PLAY_CLASSIC, PLAY_MOMENTARY, PLAY_TRIGGER };
	enum ClockMode { CLOCK_HYBRID, CLOCK_SHIFT, CLOCK_STRETCH };
	enum CvMode { CV_ENVELOPE, CV_RAMP };
	enum RecState { REC_IDLE, REC_ARMED_CURRENT, REC_ARMED_NEW, REC_CURRENT, REC_NEW };

	// ---- the reel -------------------------------------------------------
	std::vector<float> bufL, bufR;   // MAX_FRAMES each, allocated once
	int length = 0;                  // frames in use
	int markers[MAX_SPLICES];        // splice start frames, markers[0] = 0
	int numSplices = 0;
	std::vector<float> peaks;        // max |sample| per PEAK_BLOCK frames (display)

	// ---- options (the hardware's options.txt) ---------------------------
	PlayMode playMode = PLAY_CLASSIC;       // pmod
	ClockMode clockMode = CLOCK_HYBRID;     // ckop
	CvMode cvMode = CV_ENVELOPE;            // cvop
	bool organizeImmediate = false;         // omod
	bool smoothGene = false;                // gnsm
	bool recordInputOnly = false;           // inop
	float chord[3] = {2.f, 3.f, 4.f};       // mcr1..mcr3: pitch layers at high Morph

	// ---- controls, set by the module every block ------------------------
	float sos = 1.f;        // 0 = hear/record only input, 1 = only the reel
	float geneSize = 0.f;   // 0 = whole splice, 1 = tiny
	float speed = 1.f;      // playback rate, negative = reverse
	float morph = MORPH_SEAMLESS;
	float slide = 0.f;      // 0..1 through the splice
	float organize = 0.f;   // 0..1 picks the splice
	bool playHigh = true;   // PLAY gate (normalled high)
	bool clockConnected = false;

	// ---- outputs, read by the module after process() --------------------
	float outL = 0.f, outR = 0.f;
	float cvOut = 0.f;      // 0..8 V
	bool eosg = false;      // end of splice / gene: true for one sample
	float playL = 0.f, playR = 0.f; // reel playback alone, before the S.O.S. mix

	// ---- state ----------------------------------------------------------
	struct Voice {
		bool active = false;
		int spliceStart = 0, spliceLen = 0;
		double scan = 0;       // material offset at spawn (Gene Shift / stretch)
		double rel = 0;        // material travelled since the gene started
		double age = 0, life = 1, fade = 1; // in engine samples
		float ratio = 1.f;     // pitch layer
		float panL = 1.f, panR = 1.f, gain = 1.f;
		bool smooth = false;
		float kill = 1.f;      // fades to 0 when the voice is cut short
		bool killing = false;
		double absPos = 0;     // last read position (for display)
	};
	Voice voices[NUM_VOICES];
	int current = 0;        // splice playing
	int pending = 0;        // splice that plays at the next gene end
	float lastOrganize = -1.f;
	double untilOnset = 0;  // engine samples until the next gene starts
	double scan = 0;        // gene shift / time-stretch offset inside the splice
	double slideSmooth = 0;
	int spawnCount = 0;
	int newestVoice = -1;
	float stopGain = 1.f;
	float env = 0.f;
	double sampleRate = 48000;
	Rng rng;

	// clock
	double sinceClock = 1e9, clockPeriod = 0;

	// recording
	RecState rec = REC_IDLE;
	double recHead = 0;     // reel frames
	int recStart = 0, recLen = 0;
	float prevInL = 0.f, prevInR = 0.f;
	bool recordedIntoEmpty = false;
	bool dirty = false;     // reel changed since last save

	// display helpers
	double geneMatShown = 0, geneStartShown = 0;
	int voicesActive = 0;
	float density = 1.f;
	bool stretchActive = false;

	TapeEngine() {
		bufL.assign(MAX_FRAMES, 0.f);
		bufR.assign(MAX_FRAMES, 0.f);
		peaks.assign(NUM_PEAK_BLOCKS, 0.f);
		markers[0] = 0;
	}

	// ---- splice helpers -------------------------------------------------
	int spliceStart(int i) const { return markers[i]; }
	int spliceEnd(int i) const { return i + 1 < numSplices ? markers[i + 1] : length; }
	int spliceLen(int i) const { return spliceEnd(i) - spliceStart(i); }
	bool empty() const { return numSplices == 0 || length < MIN_SPLICE; }

	double rateBase() const { return REEL_RATE / sampleRate; }

	// Gene length in reel frames for a splice of len frames
	double geneMaterial(int len) const {
		const double minFrames = 0.002 * REEL_RATE;
		if (len <= minFrames) return len;
		return len * std::pow(minFrames / len, (double) geneSize);
	}

	// Morph -> how many genes overlap (1 below the seamless point)
	float morphDensity() const {
		if (morph <= MORPH_SEAMLESS) return 1.f;
		return 1.f + 3.f * (morph - MORPH_SEAMLESS) / (1.f - MORPH_SEAMLESS);
	}

	bool inStretchMode() const {
		if (!clockConnected) return false;
		if (clockMode == CLOCK_STRETCH) return true;
		if (clockMode == CLOCK_SHIFT) return false;
		return morphDensity() > 2.f;
	}

	// Gene duration and spacing in engine samples
	void geneTiming(int len, double& dur, double& interval) const {
		double mat = geneMaterial(len);
		double rb = rateBase();
		double sp = std::max(std::fabs(speed), 1e-3f);
		if (geneSize < 0.01f)
			dur = mat / (sp * rb);          // whole splice: loop at playback speed
		else
			dur = mat / rb;                  // a gene is a fixed length in time
		dur = std::max(dur, 16.0);
		if (morph < MORPH_SEAMLESS)
			interval = dur * (1.0 + 3.0 * (1.0 - morph / MORPH_SEAMLESS));
		else
			interval = dur / morphDensity();
	}

	void setSampleRate(double sr) { sampleRate = sr; }

	// ---- peaks (waveform overview) --------------------------------------
	void recomputePeaks(int from, int to) {
		int b0 = std::max(0, from / PEAK_BLOCK), b1 = std::min(NUM_PEAK_BLOCKS - 1, to / PEAK_BLOCK);
		for (int b = b0; b <= b1; b++) {
			float m = 0.f;
			int s = b * PEAK_BLOCK, e = std::min(length, s + PEAK_BLOCK);
			for (int f = s; f < e; f++)
				m = std::max(m, std::max(std::fabs(bufL[f]), std::fabs(bufR[f])));
			peaks[b] = m;
		}
	}

	// ---- playback -------------------------------------------------------
	void killVoices() {
		for (Voice& v : voices)
			if (v.active) v.killing = true;
	}

	// Start again from the Slide point (Play retrigger, splice edits)
	void restart() {
		killVoices();
		scan = 0;
		untilOnset = 0;
	}

	void spawnVoice() {
		if (empty() || current >= numSplices) return;
		int slot = -1;
		for (int i = 0; i < NUM_VOICES; i++)
			if (!voices[i].active) { slot = i; break; }
		if (slot < 0) {  // steal the oldest
			double oldest = -1;
			for (int i = 0; i < NUM_VOICES; i++)
				if (voices[i].age > oldest) { oldest = voices[i].age; slot = i; }
		}
		Voice& v = voices[slot];
		int len = spliceLen(current);
		if (len < 1) return;
		double dur, interval;
		geneTiming(len, dur, interval);
		v = Voice();
		v.active = true;
		v.spliceStart = spliceStart(current);
		v.spliceLen = len;
		v.scan = scan;
		v.smooth = smoothGene;
		double mat = geneMaterial(len);
		// reverse genes play their window from the end backwards
		v.rel = speed < 0 ? mat : 0;
		if (smoothGene) {
			v.life = dur;
			v.fade = dur / 2;
		} else {
			v.fade = std::min(0.004 * sampleRate, dur / 4);
			v.life = dur + v.fade;   // overlap the next gene by one fade
		}
		v.fade = std::max(v.fade, 1.0);
		// pitch layers at high Morph
		int layer = spawnCount % 4;
		v.ratio = 1.f;
		if (morph > MORPH_PITCH && layer > 0) {
			float p = (morph - MORPH_PITCH) / (1.f - MORPH_PITCH);
			if (rng.uniform() < p) v.ratio = chord[layer - 1];
		}
		// panning once there are more than 2.5 overlapping genes
		float d = morphDensity();
		float spread = clampf((d - 2.5f) / 1.0f, 0.f, 1.f);
		float pan = spread * (rng.uniform() * 2.f - 1.f);
		v.panL = std::min(1.f, 1.f - pan);
		v.panR = std::min(1.f, 1.f + pan);
		v.gain = 1.f / std::sqrt(std::max(1.0, dur / interval));
		spawnCount++;
		newestVoice = slot;
	}

	static float clampf(float x, float a, float b) { return std::max(a, std::min(b, x)); }

	// Cubic (Hermite) read at a fractional position inside a splice
	inline float readAt(const std::vector<float>& buf, int start, int len, double pos) const {
		int i = (int) std::floor(pos);
		float t = (float) (pos - i);
		auto at = [&](int k) {
			k %= len;
			if (k < 0) k += len;
			return buf[start + k];
		};
		float y0 = at(i - 1), y1 = at(i), y2 = at(i + 1), y3 = at(i + 2);
		float c1 = 0.5f * (y2 - y0);
		float c2 = y0 - 2.5f * y1 + 2.f * y2 - 0.5f * y3;
		float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
		return ((c3 * t + c2) * t + c1) * t + y1;
	}

	void onsetTick() {
		eosg = true;
		if (!organizeImmediate && pending != current && pending < numSplices) {
			current = pending;
			scan = 0;
		}
		bool allowed = playMode == PLAY_CLASSIC ? playHigh : playMode == PLAY_MOMENTARY ? playHigh : true;
		if (allowed) spawnVoice();
	}

	// ---- organize / shift -----------------------------------------------
	void updateOrganize() {
		if (numSplices <= 0) return;
		if (lastOrganize < 0 || std::fabs(organize - lastOrganize) > 0.002f) {
			// splices are spread evenly across the knob, whatever their length
			int idx = std::min(numSplices - 1, (int) (organize * numSplices));
			if (lastOrganize >= 0 || numSplices > 0) {
				if (idx != pending) setPending(idx);
			}
			lastOrganize = organize;
		}
	}

	void setPending(int idx) {
		pending = std::max(0, std::min(numSplices - 1, idx));
		if (organizeImmediate && pending != current) {
			current = pending;
			restart();
		}
	}

	void shiftSplice() {   // SHIFT button / gate: next splice
		if (numSplices <= 0) return;
		setPending((pending + 1) % numSplices);
	}

	void playRetrigger() { restart(); }

	// ---- splice editing -------------------------------------------------
	// Position (absolute frame) where a marker would go now
	int markerPosition() const {
		if (rec == REC_NEW) return (int) recHead;
		if (newestVoice >= 0 && voices[newestVoice].active)
			return (int) voices[newestVoice].absPos;
		return spliceStart(std::min(current, std::max(0, numSplices - 1)));
	}

	bool addMarker(int pos) {
		if (numSplices <= 0 || numSplices >= MAX_SPLICES) return false;
		if (pos < MIN_SPLICE || pos > length - (rec == REC_NEW ? 0 : MIN_SPLICE)) return false;
		int j = 1;
		while (j < numSplices && markers[j] < pos) j++;
		if (std::abs(markers[j - 1] - pos) < MIN_SPLICE) return false;
		if (j < numSplices && std::abs(markers[j] - pos) < MIN_SPLICE) return false;
		for (int k = numSplices; k > j; k--) markers[k] = markers[k - 1];
		markers[j] = pos;
		numSplices++;
		if (current >= j) current++;
		if (pending >= j) pending++;
		dirty = true;
		return true;
	}

	void splice() { addMarker(markerPosition()); }

	// SHIFT + SPLICE on the hardware: merge the current splice with the next
	void deleteMarker() {
		stopRecording();
		if (numSplices <= 1) return;
		int idx = current + 1 < numSplices ? current + 1 : current;
		for (int k = idx; k < numSplices - 1; k++) markers[k] = markers[k + 1];
		numSplices--;
		if (idx == current) current--;
		if (pending >= idx) pending = std::max(0, pending - 1);
		pending = std::min(pending, numSplices - 1);
		current = std::min(current, numSplices - 1);
		dirty = true;
		restart();
	}

	// Hold SHIFT + SPLICE 3 s: remove every marker, keep the audio
	void clearMarkers() {
		stopRecording();
		if (numSplices <= 0) return;
		numSplices = 1;
		markers[0] = 0;
		current = pending = 0;
		dirty = true;
		restart();
	}

	// SHIFT + REC: delete the current splice's audio
	void deleteSpliceAudio() {
		stopRecording();
		if (numSplices <= 0) return;
		if (numSplices == 1) { clearReel(); return; }
		int s = spliceStart(current), e = spliceEnd(current), n = e - s;
		std::memmove(&bufL[s], &bufL[e], sizeof(float) * (length - e));
		std::memmove(&bufR[s], &bufR[e], sizeof(float) * (length - e));
		length -= n;
		for (int k = current; k < numSplices - 1; k++) markers[k] = markers[k + 1] - n;
		numSplices--;
		if (numSplices > 0) markers[0] = 0;
		current = std::min(current, numSplices - 1);
		pending = current;
		recomputePeaks(s, std::max(s, length + n));
		dirty = true;
		restart();
	}

	// Hold SHIFT + REC 3 s: erase everything
	void clearReel() {
		stopRecording();
		length = 0;
		numSplices = 0;
		current = pending = 0;
		std::fill(peaks.begin(), peaks.end(), 0.f);
		dirty = true;
		for (Voice& v : voices) v.active = false;
	}

	// Replace the reel (used when loading a file). Not called from the audio
	// thread while it runs; the module pauses processing first.
	void setReel(const std::vector<float>& L, const std::vector<float>& R, const std::vector<int>& cues) {
		rec = REC_IDLE;
		length = (int) std::min<size_t>(L.size(), MAX_FRAMES);
		std::copy(L.begin(), L.begin() + length, bufL.begin());
		std::copy(R.begin(), R.begin() + length, bufR.begin());
		numSplices = length >= MIN_SPLICE ? 1 : 0;
		markers[0] = 0;
		std::vector<int> c(cues);
		std::sort(c.begin(), c.end());
		for (int p : c) {
			if (numSplices >= MAX_SPLICES) break;
			if (p - markers[numSplices - 1] >= MIN_SPLICE && length - p >= MIN_SPLICE)
				markers[numSplices++] = p;
		}
		std::fill(peaks.begin(), peaks.end(), 0.f);
		recomputePeaks(0, length);
		current = pending = 0;
		lastOrganize = -1.f;
		for (Voice& v : voices) v.active = false;
		scan = 0;
		untilOnset = 0;
		dirty = false;
	}

	// ---- recording ------------------------------------------------------
	// REC button / gate: record into the current splice (Time Lag Accumulation)
	void recButton() {
		if (rec == REC_CURRENT || rec == REC_NEW || rec == REC_ARMED_CURRENT || rec == REC_ARMED_NEW) {
			if (clockConnected && (rec == REC_CURRENT || rec == REC_NEW))
				stopArmed = true;   // stop on the next clock
			else
				stopRecording();
			return;
		}
		if (clockConnected) rec = REC_ARMED_CURRENT;
		else startRecording(false);
	}

	// REC NEW (hold REC + press SPLICE on the hardware): record a new splice
	void recNewButton() {
		if (rec != REC_IDLE) { recButton(); return; }
		if (clockConnected) rec = REC_ARMED_NEW;
		else startRecording(true);
	}

	bool stopArmed = false;

	void startRecording(bool asNew) {
		stopArmed = false;
		if (empty()) {
			asNew = true;
			length = 0;
			numSplices = 0;
		}
		if (asNew) {
			if (length >= MAX_FRAMES - MIN_SPLICE) { rec = REC_IDLE; return; }
			if (numSplices >= MAX_SPLICES) { rec = REC_IDLE; return; }
			recordedIntoEmpty = numSplices == 0;
			markers[numSplices] = length;
			numSplices++;
			recHead = length;
			rec = REC_NEW;
		}
		else {
			recStart = spliceStart(current);
			recLen = spliceLen(current);
			double p = recStart;
			if (newestVoice >= 0 && voices[newestVoice].active && voices[newestVoice].spliceStart == recStart)
				p = std::floor(voices[newestVoice].absPos);
			recHead = p;
			rec = REC_CURRENT;
		}
		dirty = true;
	}

	void stopRecording() {
		stopArmed = false;
		if (rec == REC_NEW) {
			int idx = numSplices - 1;
			if (length - markers[idx] < MIN_SPLICE) {
				// too short: drop it
				length = markers[idx];
				numSplices--;
			}
			else {
				current = pending = idx;
				lastOrganize = organize;  // don't let the knob jump straight away
				restart();
			}
			recordedIntoEmpty = false;
		}
		if (rec == REC_ARMED_CURRENT || rec == REC_ARMED_NEW || rec == REC_CURRENT || rec == REC_NEW)
			rec = REC_IDLE;
	}

	void writeFrame(int f, float l, float r) {
		bufL[f] = l;
		bufR[f] = r;
		int b = f / PEAK_BLOCK;
		float a = std::max(std::fabs(l), std::fabs(r));
		if (f % PEAK_BLOCK == 0) peaks[b] = a;
		else peaks[b] = std::max(peaks[b], a);
	}

	void recordSample(float inL, float inR, float pl, float pr) {
		// what gets written: the S.O.S. mix (or input only)
		float wl, wr;
		if (recordInputOnly || recordedIntoEmpty) { wl = inL; wr = inR; }
		else {
			wl = inL * (1.f - sos) + pl * sos;
			wr = inR * (1.f - sos) + pr * sos;
		}
		wl = clampf(wl, -2.f, 2.f);
		wr = clampf(wr, -2.f, 2.f);
		double rb = rateBase();
		double h0 = recHead, h1 = recHead + rb;
		for (double fd = std::ceil(h0); fd < h1; fd += 1.0) {
			float t = (float) ((fd - h0) / rb);
			float l = prevInL + (wl - prevInL) * t;
			float r = prevInR + (wr - prevInR) * t;
			if (rec == REC_NEW) {
				int f = (int) fd;
				if (f >= MAX_FRAMES) { stopRecording(); break; }
				writeFrame(f, l, r);
				length = std::max(length, f + 1);
			}
			else {
				if (recLen <= 0) break;
				int f = recStart + (int) wrapPos(fd - recStart, recLen);
				writeFrame(f, l, r);
			}
		}
		recHead = h1;
		if (rec == REC_CURRENT && recLen > 0)
			recHead = recStart + wrapPos(recHead - recStart, recLen);
		prevInL = wl;
		prevInR = wr;
	}

	// ---- main process ---------------------------------------------------
	// inL/inR are already scaled (1.0 = 5 V) and gained. clockEdge/playRise
	// are rising edges detected by the module.
	void process(float inL, float inR, bool clockEdge, bool playRise) {
		eosg = false;
		double rb = rateBase();

		// clock
		sinceClock += 1;
		if (clockEdge) {
			if (sinceClock < 3.5 * sampleRate) clockPeriod = sinceClock;
			else clockPeriod = 0;
			sinceClock = 0;
			if (rec == REC_ARMED_CURRENT) startRecording(false);
			else if (rec == REC_ARMED_NEW) startRecording(true);
			else if (stopArmed) stopRecording();
		}

		updateOrganize();

		// Play input
		if (playRise) restart();
		if (playMode == PLAY_MOMENTARY && !playHigh) killVoices();

		// stopped at Vari-Speed noon
		bool stopped = std::fabs(speed) < 1e-3f;
		stopGain += ((stopped ? 0.f : 1.f) - stopGain) * (float) (1.0 / (0.005 * sampleRate));

		// slide is smoothed so moving it doesn't click
		slideSmooth += (slide - slideSmooth) * (1.0 / (0.02 * sampleRate));

		int len = (!empty() && current < numSplices) ? spliceLen(current) : 0;
		double mat = len > 0 ? geneMaterial(len) : 0;
		density = morphDensity();
		stretchActive = inStretchMode();

		if (len > 0 && !stopped) {
			double dur, interval;
			geneTiming(len, dur, interval);
			if (untilOnset > interval) untilOnset = interval;
			if (clockConnected && clockEdge && !stretchActive) {
				// Gene Shift: each clock jumps to the next gene now
				scan = wrapPos(scan + mat, len);
				onsetTick();
				untilOnset = interval;
			}
			else {
				untilOnset -= 1;
				if (untilOnset <= 0) {
					onsetTick();
					untilOnset += interval;
					if (untilOnset <= 0) untilOnset = interval;
				}
			}
			// Time Stretch: the clock sets how fast we move through the splice
			if (stretchActive && clockPeriod > 0 && sinceClock < 3.5 * sampleRate)
				scan = wrapPos(scan + mat / clockPeriod, len);
			geneMatShown = mat;
			geneStartShown = wrapPos(slideSmooth * len + scan, len);
		}
		if (organizeImmediate && pending != current && pending < numSplices) {
			current = pending;
			restart();
		}

		// voices
		float pl = 0.f, pr = 0.f;
		int nActive = 0;
		for (int i = 0; i < NUM_VOICES; i++) {
			Voice& v = voices[i];
			if (!v.active) continue;
			if (v.spliceStart + v.spliceLen > length || v.spliceLen < 1) { v.active = false; continue; }
			nActive++;
			double pos = wrapPos(slideSmooth * v.spliceLen + v.scan + v.rel, v.spliceLen);
			v.absPos = v.spliceStart + pos;
			float sl = readAt(bufL, v.spliceStart, v.spliceLen, pos);
			float sr = readAt(bufR, v.spliceStart, v.spliceLen, pos);
			// window
			float w;
			if (v.smooth) {
				double ph = v.age / v.life;
				w = (float) (0.5 - 0.5 * std::cos(2 * M_PI * std::min(1.0, ph)));
			}
			else {
				double a = v.age, rem = v.life - v.age;
				w = (float) std::min(1.0, std::min(a / v.fade, rem / v.fade));
				if (w < 0) w = 0;
			}
			if (v.killing) {
				v.kill -= (float) (1.0 / (0.004 * sampleRate));
				if (v.kill <= 0) { v.active = false; continue; }
			}
			float g = w * v.kill * v.gain;
			pl += sl * g * v.panL;
			pr += sr * g * v.panR;
			if (!stopped) {
				v.rel += speed * v.ratio * rb;
				v.age += 1;
				if (v.age >= v.life) v.active = false;
			}
		}
		voicesActive = nActive;
		pl *= stopGain;
		pr *= stopGain;
		playL = pl;
		playR = pr;

		// recording
		if (rec == REC_CURRENT || rec == REC_NEW)
			recordSample(inL, inR, pl, pr);

		// what you hear: S.O.S. mix of input and reel
		outL = inL * (1.f - sos) + pl * sos;
		outR = inR * (1.f - sos) + pr * sos;

		// CV out
		if (cvMode == CV_ENVELOPE) {
			float a = 0.5f * (std::fabs(outL) + std::fabs(outR));
			float coef = a > env ? (float) (1.0 / (0.005 * sampleRate)) : (float) (1.0 / (0.12 * sampleRate));
			env += (a - env) * coef;
			cvOut = clampf(env * 8.f, 0.f, 8.f);
		}
		else {
			cvOut = 0.f;
			if (newestVoice >= 0 && voices[newestVoice].active)
				cvOut = clampf((float) (voices[newestVoice].age / voices[newestVoice].life) * 8.f, 0.f, 8.f);
		}
	}
};

} // namespace tj
