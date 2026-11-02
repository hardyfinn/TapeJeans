// Tapejeans.cpp
// A clear-interface take on a tape and microsound module: every hardware
// button combination gets its own labelled button, and a display shows the
// reel, splices, the gene window and the playheads.
// The sound engine is in TapeEngine.hpp; this file connects it to the panel.
#include "plugin.hpp"
#include "TapeEngine.hpp"
#include "Wav.hpp"
#include <osdialog.h>
#include <atomic>
#include <thread>
#include <chrono>

using namespace tj;

static const float GAINS[4] = {0.708f, 1.f, 2.f, 4.f};  // -3, 0, +6, +12 dB
static const char* GAIN_NAMES[4] = {"-3 dB (hardware: blue)", "0 dB, modular level (hardware: green)", "+6 dB (hardware: orange)", "+12 dB (hardware: purple)"};

// ---- knob read-outs ----------------------------------------------------
struct SpeedQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		float r = speedFromKnob(getValue());
		if (std::fabs(r) < 1e-3f) return "stopped";
		float st = 12.f * std::log2(std::fabs(r));
		return string::f("%s x%.2f (%+.1f semitones)", r < 0 ? "reverse" : "forward", std::fabs(r), st);
	}
};

struct GeneQuantity : ParamQuantity {
	std::string getDisplayValueString() override {
		float v = getValue();
		if (v < 0.01f) return "whole splice";
		return string::f("%.0f%% (smaller = shorter genes)", v * 100.f);
	}
};

static std::string morphWords(float m) {
	if (m < MORPH_SEAMLESS - 0.01f) return "gaps between genes";
	if (m < MORPH_SEAMLESS + 0.01f) return "1/1 seamless";
	float d = 1.f + 3.f * (m - MORPH_SEAMLESS) / (1.f - MORPH_SEAMLESS);
	std::string s = string::f("%.1f genes overlap", d);
	if (d > 2.5f) s += ", panning";
	if (m > MORPH_PITCH) s += ", pitch layers";
	return s;
}

struct MorphQuantity : ParamQuantity {
	std::string getDisplayValueString() override { return morphWords(getValue()); }
};

struct Tapejeans : Module {
	enum ParamId {
		SOS_PARAM, GENE_PARAM, GENE_ATT_PARAM, SPEED_PARAM, SPEED_ATT_PARAM,
		MORPH_PARAM, SLIDE_PARAM, SLIDE_ATT_PARAM, ORGANIZE_PARAM, GAIN_PARAM,
		REC_BUTTON, RECNEW_BUTTON, SPLICE_BUTTON, NEXT_BUTTON,
		DELMARK_BUTTON, DELAUDIO_BUTTON, CLRMARKS_BUTTON, CLRREEL_BUTTON,
		PARAMS_LEN
	};
	enum InputId {
		IN_L_INPUT, IN_R_INPUT,
		SOS_INPUT, GENE_INPUT, SPEED_INPUT, MORPH_INPUT, SLIDE_INPUT, ORGANIZE_INPUT,
		CLOCK_INPUT, PLAY_INPUT, REC_INPUT, SPLICE_INPUT, SHIFT_INPUT,
		INPUTS_LEN
	};
	enum OutputId { OUT_L_OUTPUT, OUT_R_OUTPUT, EOSG_OUTPUT, CV_OUTPUT, OUTPUTS_LEN };
	enum LightId {
		REC_LIGHT, RECNEW_LIGHT, SPLICE_LIGHT, NEXT_LIGHT,
		DELMARK_LIGHT, DELAUDIO_LIGHT, CLRMARKS_LIGHT, CLRREEL_LIGHT,
		ENUMS(LEVEL_LIGHT, 2), EOSG_LIGHT,
		LIGHTS_LEN
	};
	enum SpeedMode { SPEED_CLASSIC, SPEED_VOCT_BI, SPEED_VOCT_POS };

	TapeEngine tape;
	int speedMode = SPEED_CLASSIC;       // vsop
	bool recGateNew = false;             // rsop: REC gate records a new splice
	bool keepWithPatch = true;
	int chordPreset = 0;

	dsp::SchmittTrigger clockTrig, playTrig, recTrig, spliceTrig, shiftTrig;
	dsp::BooleanTrigger btn[8];
	dsp::PulseGenerator eosgPulse, flash[8];
	dsp::ClockDivider ctrlDiv, lightDiv;
	float holdTime[2] = {0.f, 0.f};      // clear markers / clear reel hold
	bool holdDone[2] = {false, false};
	float level = 0.f;
	double recFlashPhase = 0;

	std::atomic<bool> pauseReq{false}, paused{false};

	// short messages for the display
	std::string message;
	float messageTime = 0.f;
	void say(const std::string& m) { message = m; messageTime = 2.f; }

	Tapejeans() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		configParam(SOS_PARAM, 0.f, 1.f, 1.f, "S.O.S. (sound on sound): input <-> reel", "% reel", 0.f, 100.f);
		configParam<GeneQuantity>(GENE_PARAM, 0.f, 1.f, 0.f, "Gene size");
		configParam(GENE_ATT_PARAM, -1.f, 1.f, 1.f, "Gene size CV attenuverter", "%", 0.f, 100.f);
		configParam<SpeedQuantity>(SPEED_PARAM, -1.f, 1.f, 0.5f, "Vari-speed");
		configParam(SPEED_ATT_PARAM, -1.f, 1.f, 1.f, "Vari-speed CV attenuverter", "%", 0.f, 100.f);
		configParam<MorphQuantity>(MORPH_PARAM, 0.f, 1.f, MORPH_SEAMLESS, "Morph");
		configParam(SLIDE_PARAM, 0.f, 1.f, 0.f, "Slide (where the gene starts in the splice)", "%", 0.f, 100.f);
		configParam(SLIDE_ATT_PARAM, -1.f, 1.f, 1.f, "Slide CV attenuverter", "%", 0.f, 100.f);
		configParam(ORGANIZE_PARAM, 0.f, 1.f, 0.f, "Organize (which splice plays next)", "%", 0.f, 100.f);
		configSwitch(GAIN_PARAM, 0.f, 3.f, 1.f, "Input gain", {GAIN_NAMES[0], GAIN_NAMES[1], GAIN_NAMES[2], GAIN_NAMES[3]});
		configButton(REC_BUTTON, "REC: record into the current splice (hardware: REC)");
		configButton(RECNEW_BUTTON, "REC NEW: record a new splice (hardware: hold REC, tap SPLICE)");
		configButton(SPLICE_BUTTON, "SPLICE: add a marker here (hardware: SPLICE)");
		configButton(NEXT_BUTTON, "NEXT: go to the next splice (hardware: SHIFT)");
		configButton(DELMARK_BUTTON, "DELETE MARKER: join this splice with the next (hardware: hold SHIFT, tap SPLICE)");
		configButton(DELAUDIO_BUTTON, "DELETE SPLICE AUDIO (hardware: hold SHIFT, tap REC)");
		configButton(CLRMARKS_BUTTON, "CLEAR ALL MARKERS, keep audio: hold 1 s (hardware: hold SHIFT + SPLICE 3 s)");
		configButton(CLRREEL_BUTTON, "CLEAR REEL: hold 1 s (hardware: hold SHIFT + REC 3 s)");
		configInput(IN_L_INPUT, "Audio left (mono)");
		configInput(IN_R_INPUT, "Audio right");
		configInput(SOS_INPUT, "S.O.S. CV (0-8 V, scales the knob)");
		configInput(GENE_INPUT, "Gene size CV (0-8 V)");
		configInput(SPEED_INPUT, "Vari-speed CV (+-4 V, or 1 V/oct in the 1 V/oct modes)");
		configInput(MORPH_INPUT, "Morph CV (0-5 V)");
		configInput(SLIDE_INPUT, "Slide CV (0-8 V)");
		configInput(ORGANIZE_INPUT, "Organize CV (0-5 V)");
		configInput(CLOCK_INPUT, "Clock (gene shift / time stretch, syncs recording)");
		configInput(PLAY_INPUT, "Play gate (normalled high)");
		configInput(REC_INPUT, "Record gate (toggles)");
		configInput(SPLICE_INPUT, "Splice gate (adds a marker)");
		configInput(SHIFT_INPUT, "Shift gate (next splice)");
		configOutput(OUT_L_OUTPUT, "Audio left");
		configOutput(OUT_R_OUTPUT, "Audio right");
		configOutput(EOSG_OUTPUT, "End of splice / gene");
		configOutput(CV_OUTPUT, "CV (envelope follower or gene ramp, 0-8 V)");
		configBypass(IN_L_INPUT, OUT_L_OUTPUT);
		configBypass(IN_R_INPUT, OUT_R_OUTPUT);
		ctrlDiv.setDivision(16);
		lightDiv.setDivision(64);
		tape.rng.seed(random::u64());
	}

	void onSampleRateChange(const SampleRateChangeEvent& e) override {
		tape.setSampleRate(e.sampleRate);
	}

	void onReset(const ResetEvent& e) override {
		Module::onReset(e);
		speedMode = SPEED_CLASSIC;
		recGateNew = false;
		setChordPreset(0);
		tape.playMode = TapeEngine::PLAY_CLASSIC;
		tape.clockMode = TapeEngine::CLOCK_HYBRID;
		tape.cvMode = TapeEngine::CV_ENVELOPE;
		tape.organizeImmediate = false;
		tape.smoothGene = false;
		tape.recordInputOnly = false;
	}

	void setChordPreset(int p) {
		static const float presets[5][3] = {{2.f, 3.f, 4.f}, {0.5f, 2.f, 4.f}, {1.5f, 2.f, 3.f}, {-1.f, 2.f, -2.f}, {1.f, 1.f, 1.f}};
		chordPreset = clamp(p, 0, 4);
		for (int i = 0; i < 3; i++) tape.chord[i] = presets[chordPreset][i];
	}

	float speedRate(float knob, float cv, float att) {
		switch (speedMode) {
			case SPEED_VOCT_BI:
				return speedFromKnob(knob) * std::pow(2.f, cv * att);
			case SPEED_VOCT_POS: {
				float u = (knob + 1.f) / 2.f;
				if (u < 0.02f) return 0.f;
				float semis = -26.f + 38.f * (u - 0.02f) / 0.98f;
				return std::pow(2.f, semis / 12.f + cv * att);
			}
			default:
				return speedFromKnob(clamp(knob + att * cv / 4.f, -1.f, 1.f));
		}
	}

	void readControls() {
		float sos = params[SOS_PARAM].getValue();
		if (inputs[SOS_INPUT].isConnected())
			sos *= clamp(inputs[SOS_INPUT].getVoltage() / 8.f, 0.f, 1.f);
		tape.sos = sos;
		tape.geneSize = clamp(params[GENE_PARAM].getValue() + params[GENE_ATT_PARAM].getValue() * inputs[GENE_INPUT].getVoltage() / 8.f, 0.f, 1.f);
		tape.speed = speedRate(params[SPEED_PARAM].getValue(), inputs[SPEED_INPUT].getVoltage(), params[SPEED_ATT_PARAM].getValue());
		tape.morph = clamp(params[MORPH_PARAM].getValue() + inputs[MORPH_INPUT].getVoltage() / 5.f, 0.f, 1.f);
		tape.slide = clamp(params[SLIDE_PARAM].getValue() + params[SLIDE_ATT_PARAM].getValue() * inputs[SLIDE_INPUT].getVoltage() / 8.f, 0.f, 1.f);
		tape.organize = clamp(params[ORGANIZE_PARAM].getValue() + inputs[ORGANIZE_INPUT].getVoltage() / 5.f, 0.f, 1.f);
		tape.clockConnected = inputs[CLOCK_INPUT].isConnected();
	}

	void doSplice() {
		if (tape.empty()) { say("Nothing to splice yet: record first"); return; }
		if (tape.numSplices >= MAX_SPLICES) { say("Reel is full of splices (300)"); return; }
		if (tape.addMarker(tape.markerPosition())) say(string::f("Marker added: now %d splices", tape.numSplices));
		else say("Marker too close to another one");
	}

	void process(const ProcessArgs& args) override {
		if (pauseReq) {
			paused = true;
			for (int o = 0; o < OUTPUTS_LEN; o++) outputs[o].setVoltage(0.f);
			return;
		}
		paused = false;

		if (ctrlDiv.process()) readControls();

		// buttons and gates
		bool b[8];
		for (int i = 0; i < 8; i++) b[i] = btn[i].process(params[REC_BUTTON + i].getValue() > 0.f);
		bool recGate = recTrig.process(inputs[REC_INPUT].getVoltage(), 0.1f, 2.5f);
		bool spliceGate = spliceTrig.process(inputs[SPLICE_INPUT].getVoltage(), 0.1f, 2.5f);
		bool shiftGate = shiftTrig.process(inputs[SHIFT_INPUT].getVoltage(), 0.1f, 2.5f);
		bool clockEdge = clockTrig.process(inputs[CLOCK_INPUT].getVoltage(), 0.1f, 2.5f);
		bool playRise = false;
		if (inputs[PLAY_INPUT].isConnected()) {
			playRise = playTrig.process(inputs[PLAY_INPUT].getVoltage(), 0.1f, 2.5f);
			tape.playHigh = playTrig.isHigh();
		}
		else tape.playHigh = true;

		if (b[0] || (recGate && !recGateNew)) {
			bool was = tape.rec != TapeEngine::REC_IDLE;
			tape.recButton();
			if (!was && tape.rec == TapeEngine::REC_ARMED_CURRENT) say("REC armed: starts on the next clock");
		}
		if (b[1] || (recGate && recGateNew)) tape.recNewButton();
		if (b[2] || spliceGate) doSplice();
		if (b[3] || shiftGate) tape.shiftSplice();
		if (b[4]) {
			if (tape.numSplices > 1) { tape.deleteMarker(); say("Marker deleted: splices joined"); }
			else say("Only one splice: no marker to delete");
		}
		if (b[5]) {
			if (!tape.empty()) { tape.deleteSpliceAudio(); say("Splice audio deleted"); }
		}
		for (int i = 0; i < 2; i++) {
			bool held = params[CLRMARKS_BUTTON + i].getValue() > 0.f;
			if (held) {
				holdTime[i] += args.sampleTime;
				if (holdTime[i] >= 1.f && !holdDone[i]) {
					holdDone[i] = true;
					if (i == 0) { tape.clearMarkers(); say("All markers cleared, audio kept"); }
					else { tape.clearReel(); say("Reel cleared"); }
				}
			}
			else { holdTime[i] = 0.f; holdDone[i] = false; }
		}
		for (int i = 0; i < 8; i++) if (b[i]) flash[i].trigger(0.15f);

		// audio
		float gain = GAINS[clamp((int) std::round(params[GAIN_PARAM].getValue()), 0, 3)];
		float inL = inputs[IN_L_INPUT].getVoltage() / 5.f * gain;
		float inR = (inputs[IN_R_INPUT].isConnected() ? inputs[IN_R_INPUT].getVoltage() : inputs[IN_L_INPUT].getVoltage()) / 5.f * gain;
		tape.process(inL, inR, clockEdge, playRise);

		outputs[OUT_L_OUTPUT].setVoltage(tape.outL * 5.f);
		outputs[OUT_R_OUTPUT].setVoltage(tape.outR * 5.f);
		if (tape.eosg) eosgPulse.trigger(0.01f);
		bool eo = eosgPulse.process(args.sampleTime);
		outputs[EOSG_OUTPUT].setVoltage(eo ? 10.f : 0.f);
		outputs[CV_OUTPUT].setVoltage(tape.cvOut);

		level = std::max(level * 0.9995f, std::max(std::fabs(inL), std::fabs(inR)));
		if (messageTime > 0.f) messageTime -= args.sampleTime;

		if (lightDiv.process()) {
			float dt = args.sampleTime * 64;
			recFlashPhase += dt * 4.0;
			bool blink = std::fmod(recFlashPhase, 1.0) < 0.5;
			TapeEngine::RecState r = tape.rec;
			lights[REC_LIGHT].setBrightness(r == TapeEngine::REC_CURRENT ? 1.f : r == TapeEngine::REC_ARMED_CURRENT ? (blink ? 1.f : 0.f) : 0.f);
			lights[RECNEW_LIGHT].setBrightness(r == TapeEngine::REC_NEW ? 1.f : r == TapeEngine::REC_ARMED_NEW ? (blink ? 1.f : 0.f) : 0.f);
			lights[SPLICE_LIGHT].setBrightness(flash[2].process(dt) ? 1.f : 0.f);
			lights[NEXT_LIGHT].setBrightness(tape.pending != tape.current ? (blink ? 1.f : 0.3f) : (flash[3].process(dt) ? 1.f : 0.f));
			lights[DELMARK_LIGHT].setBrightness(flash[4].process(dt) ? 1.f : 0.f);
			lights[DELAUDIO_LIGHT].setBrightness(flash[5].process(dt) ? 1.f : 0.f);
			lights[CLRMARKS_LIGHT].setBrightness(holdDone[0] ? 1.f : holdTime[0]);
			lights[CLRREEL_LIGHT].setBrightness(holdDone[1] ? 1.f : holdTime[1]);
			lights[LEVEL_LIGHT + 0].setBrightness(level > 0.05f && level <= 1.f ? 1.f : 0.f);
			lights[LEVEL_LIGHT + 1].setBrightness(level > 1.f ? 1.f : 0.f);
		}
		lights[EOSG_LIGHT].setBrightnessSmooth(eo ? 1.f : 0.f, args.sampleTime);
	}

	// ---- reel files ---------------------------------------------------------
	std::string loadReel(const std::string& path, bool live) {
		WavData wd;
		std::string err = readWav(path, wd, MAX_FRAMES);
		if (!err.empty()) return err;
		if (live) {
			pauseReq = true;
			for (int i = 0; i < 200 && !paused; i++)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		tape.setReel(wd.L, wd.R, wd.cues);
		pauseReq = false;
		return "";
	}

	std::string saveReel(const std::string& path) {
		return writeWav(path, tape.bufL.data(), tape.bufR.data(), tape.length, tape.markers, tape.numSplices);
	}

	std::string storagePath() { return system::join(getPatchStorageDirectory(), "reel.wav"); }

	void onAdd(const AddEvent& e) override {
		std::string p = storagePath();
		if (system::isFile(p)) loadReel(p, false);
	}

	void onSave(const SaveEvent& e) override {
		std::string dir = createPatchStorageDirectory();
		std::string p = system::join(dir, "reel.wav");
		if (!keepWithPatch || tape.length == 0) {
			if (system::isFile(p)) system::remove(p);
			return;
		}
		if (tape.dirty || !system::isFile(p)) {
			saveReel(p);
			tape.dirty = false;
		}
	}

	json_t* dataToJson() override {
		json_t* root = json_object();
		json_object_set_new(root, "speedMode", json_integer(speedMode));
		json_object_set_new(root, "recGateNew", json_boolean(recGateNew));
		json_object_set_new(root, "keepWithPatch", json_boolean(keepWithPatch));
		json_object_set_new(root, "chordPreset", json_integer(chordPreset));
		json_object_set_new(root, "playMode", json_integer(tape.playMode));
		json_object_set_new(root, "clockMode", json_integer(tape.clockMode));
		json_object_set_new(root, "cvMode", json_integer(tape.cvMode));
		json_object_set_new(root, "organizeImmediate", json_boolean(tape.organizeImmediate));
		json_object_set_new(root, "smoothGene", json_boolean(tape.smoothGene));
		json_object_set_new(root, "recordInputOnly", json_boolean(tape.recordInputOnly));
		json_object_set_new(root, "current", json_integer(tape.current));
		return root;
	}

	void dataFromJson(json_t* root) override {
		auto geti = [&](const char* k, int def) { json_t* j = json_object_get(root, k); return j ? (int) json_integer_value(j) : def; };
		auto getb = [&](const char* k, bool def) { json_t* j = json_object_get(root, k); return j ? json_boolean_value(j) : def; };
		speedMode = clamp(geti("speedMode", 0), 0, 2);
		recGateNew = getb("recGateNew", false);
		keepWithPatch = getb("keepWithPatch", true);
		setChordPreset(geti("chordPreset", 0));
		tape.playMode = (TapeEngine::PlayMode) clamp(geti("playMode", 0), 0, 2);
		tape.clockMode = (TapeEngine::ClockMode) clamp(geti("clockMode", 0), 0, 2);
		tape.cvMode = (TapeEngine::CvMode) clamp(geti("cvMode", 0), 0, 1);
		tape.organizeImmediate = getb("organizeImmediate", false);
		tape.smoothGene = getb("smoothGene", false);
		tape.recordInputOnly = getb("recordInputOnly", false);
	}
};

// ---- display ---------------------------------------------------------------
static const NVGcolor C_BG = nvgRGB(0x14, 0x14, 0x19);
static const NVGcolor C_INK = nvgRGB(0xEC, 0xE6, 0xDA);
static const NVGcolor C_SOFT = nvgRGB(0x8A, 0x84, 0x78);
static const NVGcolor C_CUR = nvgRGB(0xF0, 0x8A, 0x3E);
static const NVGcolor C_NEXT = nvgRGB(0x4C, 0xD6, 0xE0);
static const NVGcolor C_GENE = nvgRGB(0xF2, 0xD0, 0x50);
static const NVGcolor C_REC = nvgRGB(0xF0, 0x40, 0x40);
static const NVGcolor C_WAVE = nvgRGB(0xB8, 0xB0, 0xA0);

static std::shared_ptr<window::Font> displayFont() {
	return APP->window->loadFont(asset::system("res/fonts/ShareTechMono-Regular.ttf"));
}

static std::string fmtTime(double frames) {
	double s = frames / REEL_RATE;
	if (s < 1.0) return string::f("%.0f ms", s * 1000.0);
	if (s < 60.0) return string::f("%.2f s", s);
	return string::f("%d:%04.1f", (int) (s / 60), std::fmod(s, 60.0));
}

struct ReelDisplay : Widget {
	Tapejeans* module = nullptr;

	void rect(const DrawArgs& a, float x, float y, float w, float h, NVGcolor c) {
		nvgBeginPath(a.vg);
		nvgRect(a.vg, x, y, w, h);
		nvgFillColor(a.vg, c);
		nvgFill(a.vg);
	}
	void vline(const DrawArgs& a, float x, float y, float h, NVGcolor c, float wd) {
		nvgBeginPath(a.vg);
		nvgMoveTo(a.vg, x, y);
		nvgLineTo(a.vg, x, y + h);
		nvgStrokeColor(a.vg, c);
		nvgStrokeWidth(a.vg, wd);
		nvgStroke(a.vg);
	}
	void text(const DrawArgs& a, float x, float y, const std::string& s, NVGcolor c, int align, float size) {
		nvgFontSize(a.vg, size);
		nvgTextAlign(a.vg, align);
		nvgFillColor(a.vg, c);
		nvgText(a.vg, x, y, s.c_str(), NULL);
	}
	void dot(const DrawArgs& a, float x, float y, float r, NVGcolor c) {
		nvgBeginPath(a.vg);
		nvgCircle(a.vg, x, y, r);
		nvgFillColor(a.vg, c);
		nvgFill(a.vg);
	}

	// waveform of frames [f0, f1) into the box
	void wave(const DrawArgs& a, TapeEngine& e, double f0, double f1, float x, float y, float w, float h, NVGcolor c) {
		if (f1 <= f0) return;
		int cols = (int) w;
		nvgBeginPath(a.vg);
		for (int i = 0; i < cols; i++) {
			double s = f0 + (f1 - f0) * i / cols, t = f0 + (f1 - f0) * (i + 1) / cols;
			int b0 = (int) (s / PEAK_BLOCK), b1 = std::max(b0, (int) (t / PEAK_BLOCK));
			float m = 0.f;
			for (int b = b0; b <= b1 && b < NUM_PEAK_BLOCKS; b++) m = std::max(m, e.peaks[b]);
			float hh = std::min(1.f, m) * h * 0.5f;
			nvgRect(a.vg, x + i, y + h / 2 - hh, 1.f, std::max(0.6f, 2 * hh));
		}
		nvgFillColor(a.vg, c);
		nvgFill(a.vg);
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		if (layer != 1) { Widget::drawLayer(args, layer); return; }
		float W = box.size.x, H = box.size.y;
		nvgBeginPath(args.vg);
		nvgRoundedRect(args.vg, 0, 0, W, H, mm2px(1.2f));
		nvgFillColor(args.vg, C_BG);
		nvgFill(args.vg);
		std::shared_ptr<window::Font> font = displayFont();
		if (font && font->handle >= 0) nvgFontFaceId(args.vg, font->handle);
		float pad = mm2px(1.6f);
		float fs = mm2px(2.5f), fsSmall = mm2px(2.1f);

		if (!module) {
			text(args, W / 2, H / 2, "TAPEJEANS  reel / splice / gene", C_INK, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, fs);
			return;
		}
		TapeEngine& e = module->tape;
		int n = e.numSplices, len = e.length;
		int cur = std::min(e.current, std::max(0, n - 1)), pend = std::min(e.pending, std::max(0, n - 1));

		// ---- header ----
		float hy = pad + fs * 0.5f;
		if (n > 0 && len > 0) {
			text(args, pad, hy, string::f("SPLICE %d of %d", cur + 1, n), C_CUR, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, fs);
			text(args, pad + mm2px(25.f), hy, fmtTime(e.spliceLen(cur)), C_SOFT, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, fsSmall);
			if (pend != cur)
				text(args, pad + mm2px(38.f), hy, string::f("NEXT: %d (at gene end)", pend + 1), C_NEXT, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, fsSmall);
		}
		else text(args, pad, hy, "EMPTY REEL", C_SOFT, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, fs);

		std::string right;
		NVGcolor rc = C_SOFT;
		switch (e.rec) {
			case TapeEngine::REC_CURRENT:
				right = e.recordInputOnly ? string::f("REC splice %d (input only)", cur + 1)
				        : string::f("REC splice %d  keep %d%% / input %d%%", cur + 1, (int) std::round(e.sos * 100), (int) std::round((1 - e.sos) * 100));
				rc = C_REC; break;
			case TapeEngine::REC_NEW:
				right = "REC NEW splice  " + fmtTime(len - e.markers[std::max(0, n - 1)]);
				rc = C_REC; break;
			case TapeEngine::REC_ARMED_CURRENT: case TapeEngine::REC_ARMED_NEW:
				right = "REC armed: waiting for clock"; rc = C_REC; break;
			default:
				right = "reel " + fmtTime(len) + " of 2:54";
		}
		if (module->messageTime > 0.f) { right = module->message; rc = C_INK; }
		for (int i = 0; i < 2; i++) {
			if (module->holdTime[i] > 0.f && !module->holdDone[i]) {
				right = string::f("keep holding to %s... %d%%", i ? "clear the reel" : "clear markers", (int) (module->holdTime[i] * 100));
				rc = C_REC;
			}
		}
		text(args, W - pad, hy, right, rc, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE, fsSmall);

		// ---- whole reel strip ----
		float rx = pad, rw = W - 2 * pad;
		float ry = pad + fs + mm2px(1.2f), rh = mm2px(7.f);
		text(args, rx, ry - mm2px(0.2f), "", C_SOFT, NVG_ALIGN_LEFT, fsSmall);
		rect(args, rx, ry, rw, rh, nvgRGB(0x1E, 0x1E, 0x25));
		float zy = ry + rh + mm2px(4.2f), zh = mm2px(11.f);
		if (len > 0 && n > 0) {
			double fpp = (double) len / rw;  // frames per pixel
			for (int i = 0; i < n; i++) {
				float x0 = rx + e.spliceStart(i) / fpp, x1 = rx + e.spliceEnd(i) / fpp;
				NVGcolor c = i == cur ? nvgTransRGBA(C_CUR, 70) : (i % 2 ? nvgRGB(0x24, 0x24, 0x2C) : nvgRGB(0x1E, 0x1E, 0x25));
				rect(args, x0, ry, std::max(1.f, x1 - x0), rh, c);
				if (i == pend && pend != cur) {
					nvgBeginPath(args.vg);
					nvgRect(args.vg, x0 + 0.5f, ry + 0.5f, std::max(1.f, x1 - x0 - 1), rh - 1);
					nvgStrokeColor(args.vg, C_NEXT);
					nvgStrokeWidth(args.vg, 1.f);
					nvgStroke(args.vg);
				}
			}
			wave(args, e, 0, len, rx, ry, rw, rh, C_WAVE);
			for (int i = 1; i < n; i++) vline(args, rx + e.markers[i] / fpp, ry, rh, C_INK, 1.f);
			// splice numbers if there's room
			for (int i = 0; i < n; i++) {
				float x0 = rx + e.spliceStart(i) / fpp, x1 = rx + e.spliceEnd(i) / fpp;
				if (x1 - x0 > mm2px(3.f))
					text(args, x0 + mm2px(0.5f), ry + mm2px(0.4f), string::f("%d", i + 1), i == cur ? C_CUR : C_SOFT, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, mm2px(1.8f));
			}
			for (int v = 0; v < NUM_VOICES; v++)
				if (e.voices[v].active) vline(args, rx + e.voices[v].absPos / fpp, ry, rh, nvgRGBA(255, 255, 255, 200), 1.f);
			if (e.rec == TapeEngine::REC_CURRENT || e.rec == TapeEngine::REC_NEW)
				vline(args, rx + e.recHead / fpp, ry - 2, rh + 4, C_REC, 1.5f);

			// connector lines from the current splice to the zoom strip
			float cx0 = rx + e.spliceStart(cur) / fpp, cx1 = rx + e.spliceEnd(cur) / fpp;
			nvgBeginPath(args.vg);
			nvgMoveTo(args.vg, cx0, ry + rh);
			nvgLineTo(args.vg, rx, zy);
			nvgMoveTo(args.vg, cx1, ry + rh);
			nvgLineTo(args.vg, rx + rw, zy);
			nvgStrokeColor(args.vg, nvgTransRGBA(C_CUR, 90));
			nvgStrokeWidth(args.vg, 0.8f);
			nvgStroke(args.vg);
		}
		else {
			text(args, rx + rw / 2, ry + rh / 2, "press REC NEW to record, or right-click > Load reel", C_SOFT, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE, fsSmall);
		}

		// ---- current splice zoom ----
		rect(args, rx, zy, rw, zh, nvgRGB(0x1E, 0x1E, 0x25));
		if (len > 0 && n > 0) {
			int s0 = e.spliceStart(cur), sl = e.spliceLen(cur);
			if (sl > 0) {
				double fpp = (double) sl / rw;
				// gene window (may wrap round the end of the splice)
				double g0 = e.geneStartShown, gl = std::min<double>(e.geneMatShown, sl);
				auto geneRect = [&](double a, double b) {
					rect(args, rx + a / fpp, zy, std::max(1.0, (b - a) / fpp), zh, nvgTransRGBA(C_GENE, 45));
				};
				if (g0 + gl <= sl) geneRect(g0, g0 + gl);
				else { geneRect(g0, sl); geneRect(0, g0 + gl - sl); }
				wave(args, e, s0, s0 + sl, rx, zy, rw, zh, C_WAVE);
				vline(args, rx + g0 / fpp, zy, zh, C_GENE, 1.5f);  // slide point
				for (int v = 0; v < NUM_VOICES; v++) {
					const TapeEngine::Voice& vo = e.voices[v];
					if (!vo.active || vo.spliceStart != s0) continue;
					float x = rx + (vo.absPos - s0) / fpp;
					vline(args, x, zy, zh, nvgRGBA(255, 255, 255, vo.killing ? 80 : 220), 1.2f);
				}
				if (e.rec == TapeEngine::REC_CURRENT && e.recStart == s0)
					vline(args, rx + (e.recHead - s0) / fpp, zy - 2, zh + 4, C_REC, 1.5f);
				text(args, rx + mm2px(0.6f), zy + mm2px(0.4f), "SPLICE " + std::to_string(cur + 1), C_CUR, NVG_ALIGN_LEFT | NVG_ALIGN_TOP, mm2px(1.9f));
				text(args, rx + rw - mm2px(0.6f), zy + mm2px(0.4f), "yellow = gene window, white = playheads", C_SOFT, NVG_ALIGN_RIGHT | NVG_ALIGN_TOP, mm2px(1.8f));
			}
		}
		text(args, rx, zy - mm2px(0.6f), "whole reel", C_SOFT, NVG_ALIGN_LEFT | NVG_ALIGN_BOTTOM, mm2px(1.8f));

		// ---- footer: what the hardware's lights would say ----
		float fy = H - pad - fs * 0.5f;
		float colW = rw / 4.f;
		// speed
		float sp = e.speed;
		NVGcolor spC;
		std::string spS;
		if (std::fabs(sp) < 1e-3f) { spC = nvgRGB(0xE8, 0x40, 0x40); spS = "STOPPED"; }
		else {
			float a = std::fabs(sp);
			spC = std::fabs(a - 1.f) < 0.03f ? nvgRGB(0x50, 0xD0, 0x60) : a > 1.9f ? nvgRGB(0x8C, 0xD8, 0xFF) : (a > 0.48f && a < 0.52f) ? nvgRGB(0xF8, 0xB8, 0x90) : C_INK;
			spS = string::f("%s %+.1f st", sp < 0 ? "<REV" : "FWD>", 12.f * std::log2(a));
		}
		dot(args, rx + mm2px(1.f), fy, mm2px(0.9f), spC);
		text(args, rx + mm2px(2.6f), fy, spS, C_INK, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, fsSmall);
		// gene
		std::string gS = "GENE " + (e.geneSize < 0.01f ? std::string("whole splice") : fmtTime(e.geneMatShown));
		dot(args, rx + colW + mm2px(1.f), fy, mm2px(0.9f), C_GENE);
		text(args, rx + colW + mm2px(2.6f), fy, gS, C_INK, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, fsSmall);
		// morph
		NVGcolor mC = e.stretchActive ? nvgRGB(0x50, 0x90, 0xFF) : std::fabs(e.morph - MORPH_SEAMLESS) < 0.01f ? nvgRGB(0xF0, 0xB0, 0x30) : nvgRGB(0xE8, 0x40, 0x40);
		std::string mS;
		if (e.morph < MORPH_SEAMLESS - 0.01f) mS = "MORPH gaps";
		else if (e.morph < MORPH_SEAMLESS + 0.01f) mS = "MORPH 1/1";
		else mS = string::f("MORPH x%.1f%s%s", e.density, e.density > 2.5f ? " pan" : "", e.morph > MORPH_PITCH ? " +pitch" : "");
		dot(args, rx + 2 * colW + mm2px(1.f), fy, mm2px(0.9f), mC);
		text(args, rx + 2 * colW + mm2px(2.6f), fy, mS, C_INK, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, fsSmall);
		// clock / play
		std::string cS;
		if (!e.clockConnected) cS = "CLK free";
		else cS = e.stretchActive ? "CLK time stretch" : "CLK gene shift";
		if (!e.playHigh) cS += " | PLAY off";
		text(args, rx + 3 * colW + mm2px(0.6f), fy, cS, C_INK, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE, fsSmall);

		Widget::drawLayer(args, layer);
	}
};

// ---- panel -----------------------------------------------------------------
struct TapejeansWidget : ModuleWidget {
	TapejeansWidget(Tapejeans* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/Tapejeans.svg")));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, 0)));
		addChild(createWidget<ScrewSilver>(Vec(RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));
		addChild(createWidget<ScrewSilver>(Vec(box.size.x - 2 * RACK_GRID_WIDTH, RACK_GRID_HEIGHT - RACK_GRID_WIDTH)));

		// Positions in mm, matching res/make_panel.py
		ReelDisplay* d = createWidget<ReelDisplay>(mm2px(Vec(5.0, 12.5)));
		d->box.size = mm2px(Vec(142.4, 36.0));
		d->module = module;
		addChild(d);

		const float colX[6] = {14.0, 38.5, 63.0, 89.4, 113.9, 138.4};
		const float knobY = 62.0, attY = 77.2, cvY = 86.0;
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(colX[0], knobY)), module, Tapejeans::SOS_PARAM));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(colX[1], knobY)), module, Tapejeans::GENE_PARAM));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(colX[2], knobY)), module, Tapejeans::SPEED_PARAM));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(colX[3], knobY)), module, Tapejeans::MORPH_PARAM));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(colX[4], knobY)), module, Tapejeans::SLIDE_PARAM));
		addParam(createParamCentered<RoundLargeBlackKnob>(mm2px(Vec(colX[5], knobY)), module, Tapejeans::ORGANIZE_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(colX[1], attY)), module, Tapejeans::GENE_ATT_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(colX[2], attY)), module, Tapejeans::SPEED_ATT_PARAM));
		addParam(createParamCentered<Trimpot>(mm2px(Vec(colX[4], attY)), module, Tapejeans::SLIDE_ATT_PARAM));
		const int cvIn[6] = {Tapejeans::SOS_INPUT, Tapejeans::GENE_INPUT, Tapejeans::SPEED_INPUT, Tapejeans::MORPH_INPUT, Tapejeans::SLIDE_INPUT, Tapejeans::ORGANIZE_INPUT};
		for (int i = 0; i < 6; i++)
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(colX[i], cvY)), module, cvIn[i]));

		// buttons
		const float btnY = 98.0;
		for (int i = 0; i < 8; i++) {
			float x = 9.0 + 16.6 * i;
			if (i < 2 || i == 7)
				addParam(createLightParamCentered<VCVLightBezel<RedLight>>(mm2px(Vec(x, btnY)), module, Tapejeans::REC_BUTTON + i, Tapejeans::REC_LIGHT + i));
			else if (i < 4)
				addParam(createLightParamCentered<VCVLightBezel<WhiteLight>>(mm2px(Vec(x, btnY)), module, Tapejeans::REC_BUTTON + i, Tapejeans::REC_LIGHT + i));
			else
				addParam(createLightParamCentered<VCVLightBezel<YellowLight>>(mm2px(Vec(x, btnY)), module, Tapejeans::REC_BUTTON + i, Tapejeans::REC_LIGHT + i));
		}
		addParam(createParamCentered<RoundSmallBlackKnob>(mm2px(Vec(141.8, btnY)), module, Tapejeans::GAIN_PARAM));
		addChild(createLightCentered<SmallLight<GreenRedLight>>(mm2px(Vec(147.0, btnY - 4.5)), module, Tapejeans::LEVEL_LIGHT));

		// jacks
		const float jackY = 118.0;
		const int ins[7] = {Tapejeans::IN_L_INPUT, Tapejeans::IN_R_INPUT, Tapejeans::CLOCK_INPUT, Tapejeans::PLAY_INPUT, Tapejeans::REC_INPUT, Tapejeans::SPLICE_INPUT, Tapejeans::SHIFT_INPUT};
		for (int i = 0; i < 7; i++)
			addInput(createInputCentered<PJ301MPort>(mm2px(Vec(9.0 + 11.6 * i, jackY)), module, ins[i]));
		const int outs[4] = {Tapejeans::OUT_L_OUTPUT, Tapejeans::OUT_R_OUTPUT, Tapejeans::EOSG_OUTPUT, Tapejeans::CV_OUTPUT};
		for (int i = 0; i < 4; i++)
			addOutput(createOutputCentered<PJ301MPort>(mm2px(Vec(97.0 + 13.5 * i, jackY)), module, outs[i]));
		addChild(createLightCentered<SmallLight<YellowLight>>(mm2px(Vec(127.5, jackY - 5.6)), module, Tapejeans::EOSG_LIGHT));
	}

	void appendContextMenu(Menu* menu) override {
		Tapejeans* m = getModule<Tapejeans>();
		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuItem("Load reel (.wav)...", "", [=]() {
			osdialog_filters* filters = osdialog_filters_parse("WAV:wav,WAV");
			char* path = osdialog_file(OSDIALOG_OPEN, NULL, NULL, filters);
			osdialog_filters_free(filters);
			if (!path) return;
			std::string err = m->loadReel(path, true);
			std::free(path);
			if (!err.empty()) osdialog_message(OSDIALOG_WARNING, OSDIALOG_OK, err.c_str());
			else m->say(string::f("Reel loaded: %d splices", m->tape.numSplices));
		}));
		menu->addChild(createMenuItem("Save reel as .wav...", "", [=]() {
			if (m->tape.length == 0) { m->say("The reel is empty"); return; }
			osdialog_filters* filters = osdialog_filters_parse("WAV:wav");
			char* path = osdialog_file(OSDIALOG_SAVE, NULL, "mg1.wav", filters);
			osdialog_filters_free(filters);
			if (!path) return;
			std::string p = path;
			std::free(path);
			if (system::getExtension(p) != ".wav") p += ".wav";
			std::string err = m->saveReel(p);
			if (!err.empty()) osdialog_message(OSDIALOG_WARNING, OSDIALOG_OK, err.c_str());
			else m->say("Reel saved");
		}));
		menu->addChild(createBoolPtrMenuItem("Keep the reel with the patch", "", &m->keepWithPatch));

		menu->addChild(new MenuSeparator);
		menu->addChild(createMenuLabel("Options (the hardware's options.txt)"));
		menu->addChild(createIndexPtrSubmenuItem("Vari-speed (vsop)",
			{"Classic: CV +-4 V sweeps the knob", "1 V/oct, both directions", "1 V/oct, forward only"}, &m->speedMode));
		menu->addChild(createIndexSubmenuItem("Recording source (inop)",
			{"S.O.S. mix of input and reel", "Input only (acts like a delay)"},
			[=]() { return m->tape.recordInputOnly ? 1 : 0; },
			[=](int i) { m->tape.recordInputOnly = i == 1; }));
		menu->addChild(createIndexSubmenuItem("Organize (omod)",
			{"Changes at the end of the gene", "Changes immediately"},
			[=]() { return m->tape.organizeImmediate ? 1 : 0; },
			[=](int i) { m->tape.organizeImmediate = i == 1; }));
		menu->addChild(createIndexSubmenuItem("Gene window (gnsm)",
			{"Classic (short fades)", "Smooth (soft window)"},
			[=]() { return m->tape.smoothGene ? 1 : 0; },
			[=](int i) { m->tape.smoothGene = i == 1; }));
		menu->addChild(createIndexSubmenuItem("PLAY input (pmod)",
			{"Classic: checked at each gene end", "Momentary: plays only while high", "Trigger loop: each trigger restarts"},
			[=]() { return (int) m->tape.playMode; },
			[=](int i) { m->tape.playMode = (TapeEngine::PlayMode) i; }));
		menu->addChild(createIndexSubmenuItem("Clock (ckop)",
			{"Hybrid: gene shift, time stretch above Morph 2/1", "Gene shift only", "Time stretch only"},
			[=]() { return (int) m->tape.clockMode; },
			[=](int i) { m->tape.clockMode = (TapeEngine::ClockMode) i; }));
		menu->addChild(createIndexSubmenuItem("CV output (cvop)",
			{"Envelope follower", "Gene ramp"},
			[=]() { return (int) m->tape.cvMode; },
			[=](int i) { m->tape.cvMode = (TapeEngine::CvMode) i; }));
		menu->addChild(createIndexSubmenuItem("Morph pitch layers (mcr1-3)",
			{"Octaves and fifths (x2, x3, x4)", "Octaves (x0.5, x2, x4)", "Fifths (x1.5, x2, x3)", "Reverse layers (-1, x2, -2)", "None (unison)"},
			[=]() { return m->chordPreset; },
			[=](int i) { m->setChordPreset(i); }));
		menu->addChild(createIndexSubmenuItem("REC gate input (rsop)",
			{"Records into the current splice", "Records a new splice"},
			[=]() { return m->recGateNew ? 1 : 0; },
			[=](int i) { m->recGateNew = i == 1; }));
	}
};

Model* modelTapejeans = createModel<Tapejeans, TapejeansWidget>("Tapejeans");
