#include "plugin.hpp"
#include "header_regex.hpp"

#include "chowdsp_wdf/chowdsp_wdf.h"

float SEMITONE = (1.0/12.0);

namespace wdft = chowdsp::wdft;
struct RCLowpass {
	wdft::ResistorT<double> r1 { 100.0e3 };
	wdft::CapacitorT<double> c1 { .22e-6 };
	
	wdft::WDFSeriesT<double, decltype (r1), decltype (c1)> s1 { r1, c1 };
	wdft::IdealVoltageSourceT<double, decltype (s1)> vSource { s1 };
   
	double lastSample;

	void setRackParameters(float rMod, float cMod) {
		float newR = 100.0e3 + 99.9e3 * rMod;
		float newC = 220e-9 + 219.9e-9 * cMod;
		r1.setResistanceValue(newR);
		c1.setCapacitanceValue(newC);

	}

	bool prepared = false;
	void prepare (double sampleRate) {
		c1.prepare (sampleRate);
		prepared = true;
	}
	
	inline double processSample (double x) {
		vSource.setVoltage (x);

		vSource.incident (s1.reflected());
		s1.incident (vSource.reflected());

		lastSample = -1 * wdft::voltage<double> (c1);
		return lastSample;
	}
};

// PS16
class StepAttributes {
	unsigned short attributes;
	
	public:

	static const unsigned short ATT_ST_GATE = 0x01;
	static const unsigned short ATT_ST_ACCENT = 0x04;
	static const unsigned short ATT_ST_SLIDE = 0x08;
	static const unsigned short ATT_ST_TIED = 0x10;
	
	static const unsigned short ATT_ST_INIT =  ATT_ST_GATE;
	
	inline void clear() {attributes = 0u;}
	inline void init() {attributes = ATT_ST_INIT;}
	
	inline bool getGate() {return (attributes & ATT_ST_GATE) != 0;}
	inline bool getAccent() {return (attributes & ATT_ST_ACCENT) != 0;}
	inline bool getSlide() {return (attributes & ATT_ST_SLIDE) != 0;}
	inline bool getTie() {return (attributes & ATT_ST_TIED) != 0;}
	inline unsigned short getAttribute() {return attributes;}

	inline void setGate(bool gateState) {attributes &= ~ATT_ST_GATE; if (gateState) attributes |= ATT_ST_GATE;}
	inline void setAccent(bool accentState) {attributes &= ~ATT_ST_ACCENT; if (accentState) attributes |= ATT_ST_ACCENT;}
	inline void setSlide(bool slideState) {attributes &= ~ATT_ST_SLIDE; if (slideState) attributes |= ATT_ST_SLIDE;}
	inline void setTie(bool tiedState) {
		attributes &= ~ATT_ST_TIED; 
		if (tiedState) {
			attributes |= ATT_ST_TIED;
			attributes &= ~(ATT_ST_GATE | ATT_ST_ACCENT | ATT_ST_SLIDE);// clear other attributes if tied
		}
	}
	inline void setAttribute(unsigned short _attributes) {attributes = _attributes;}

	inline void toggleGate() {attributes ^= ATT_ST_GATE;}
	inline void toggleAccent() {attributes ^= ATT_ST_ACCENT;}
	inline void toggleSlide() {attributes ^= ATT_ST_SLIDE;}
};// class StepAttributes

struct ComposerSequence {
	std::string headerStr;
	std::string notesStr;
	std::string octaveStr;
	std::string slideAccentStr;
	std::string timeStr;
	bool dirty;
};

struct AcidComposer : Module {
	enum ParamId {
		RUN_PARAM,
		RESET_PARAM,
		RES_PARAM,
		CAP_PARAM,
		PARAMS_LEN
	};
	enum InputId {
		RESET_INPUT,
		CLOCK_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		CV_OUTPUT,
		GATE_OUTPUT,
		ACCENT_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightId {
		RUN_LIGHT,
		RESET_LIGHT,
		LIGHTS_LEN
	};

	ComposerSequence sequence;

	RCLowpass slideFilter;

	float currentCv;
	bool currentAccent;
	bool currentSlide;

	// PS16
	dsp::SchmittTrigger clockTrigger;
	dsp::SchmittTrigger runningTrigger;
	dsp::SchmittTrigger resetTrigger;
	bool running;
	int stepIndexRun;
	float notes[16][16]; // [-3.0 : 3.917]. First index is patten number, 2nd index is step
	float sharpflats[16][16]; // [-3.0 : 3.917]. First index is patten number, 2nd index is step
	float octaves[16][16];
	float transposes[16];
	char letters[16];
	uint8_t lengths[16];
	StepAttributes attributes[16][16]; // First index is patten number, 2nd index is step (see enum AttributeBitMasks for details)
	static constexpr float clockIgnoreOnResetDuration = 0.001f;// disable clock on powerup and reset for 1 ms (so that the first step plays)
	long clockIgnoreOnReset;
	float resetLight;

	// json
	bool resetOnRun;

	AcidComposer() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);
		// PS16
		configInput(CLOCK_INPUT, "Clock");
		configInput(RESET_INPUT, "Reset");

		configOutput(CV_OUTPUT, "CV");
		configOutput(GATE_OUTPUT, "Gate");
		configOutput(ACCENT_OUTPUT, "Accent");

		configParam(RUN_PARAM, 0.f, 1.f, 0.f, "Run");
		configParam(RESET_PARAM, 0.f, 1.f, 0.f, "Reset");
		configParam(RES_PARAM, -1.f, 1.f, 0.f, "Slide resistor");
		configParam(CAP_PARAM, -1.f, 1.f, 0.f, "Slide capacitor");

		clockIgnoreOnReset = (long) (clockIgnoreOnResetDuration * APP->engine->getSampleRate());
		attributes[0][0].setGate(true);
		attributes[0][4].setGate(true);
		attributes[0][8].setGate(true);
		attributes[0][12].setGate(true);

		sequence.headerStr = std::string("A 16 +0");
		sequence.notesStr = std::string(64, ' ');
		sequence.octaveStr = std::string(64, ' ');
		sequence.slideAccentStr = std::string(64, ' ');
		sequence.timeStr = std::string(64, ' ');
		parseSeq();
		sequence.dirty = false;

		resetOnRun = true;
	}

	float noteToCv(unsigned char ch) {
		float note;
		switch (ch) {
			case 'c':
			case 'C':
				note = 0.0;
				break;
			case 'd':
			case 'D':
				note = 2.0 / 12.0;
				break;
			case 'e':
			case 'E':
				note = 4.0 / 12.0;
				break;
			case 'f':
			case 'F':
				note = 5.0 / 12.0;
				break;
			case 'g':
			case 'G':
				note = 7.0 / 12.0;
				break;
			case 'a':
			case 'A':
				note = 9.0 / 12.0;
				break;
			case 'b':
			case 'B':
				note = 11.0 / 12.0;
				break;
			default:
				note = 0.0;
				break;
		}
		return note;
	}

	void clearSeq() {
		for (int i = 0; i < 16; ++i)
		{
			notes[0][i] = 0.0;
			octaves[0][i] = 0.0;
			attributes[0][i].clear();
		}
	}

	int parseSeq() {
		std::string letter;
		int length;
		int transpose;

		clearSeq();

		std::cmatch m;

		DEBUG("Parsing Header: %s Len:%zu", sequence.headerStr.c_str(), sequence.headerStr.size());
		// Header
		if (header_search(sequence.headerStr.c_str(), &m)) {
			if (m.size() > 0) {
				letter = m.str(1).substr(0,1);
				length = std::stoi(m.str(2));
				transpose = (m.str(3).size() ? std::stoi(m.str(3)) : 0);
			} else return -2; // Invalid header
		} else return -2; // Invalid header

		DEBUG("Parsing Notes: %s Len:%zu", sequence.notesStr.c_str(), sequence.notesStr.size());
		int step = 0;
		std::string line = sequence.notesStr;
		for (; step < length && step < (int)line.size(); ++step) {
			if (line.at(step * 2) != ' ') {
				float cv = noteToCv(line.at(step * 2));
				DEBUG("%d %f %c", step, cv, step);
				notes[0][step] = cv;
				if (line.at((step * 2) + 1) == '#') { notes[0][step] += SEMITONE; }
				if (line.at((step * 2) + 1) == 'b') { notes[0][step] -= SEMITONE; }
			} else {
				notes[0][step] = 0.0;
			}
		}

		DEBUG("Parsing Octaves (Up/Down): %s Len:%zu", sequence.octaveStr.c_str(), sequence.octaveStr.size());
		line = sequence.octaveStr;
		for (step = 0; step < length && step < (int)line.size(); ++step)
		{	
			if (line.at(step) == 'U' || line.at(step) == 'u' ) { octaves[0][step] = 1.0; }
			else if (line.at(step) == 'D' || line.at(step) == 'd' ) { octaves[0][step] = -1.0; }
			else { octaves[0][step] = 0.0; }
		}

		DEBUG("Parsing Slide/Accent: %s Len:%zu", sequence.slideAccentStr.c_str(), sequence.slideAccentStr.size());
		line = sequence.slideAccentStr;
		for (step = 0; step < length && step < (int)line.size(); ++step) {
			if (line.at(step * 2) != ' ') {
				if (line.at((step * 2) + 1) == 'S' || line.at((step * 2) + 1) == 's' ||
					line.at(step * 2) == 'S' || line.at(step * 2) == 's') {
					attributes[0][step].setSlide(true);
					// DEBUG("Slide %d %d", step, attributes[0][step].getSlide());
				}
				if (line.at((step * 2) + 1) == 'A' || line.at((step * 2) + 1) == 'a' ||
					line.at(step * 2) == 'A' || line.at(step * 2) == 'a') {
					attributes[0][step].setAccent(true);
					// DEBUG("Accent %d %d", step, attributes[0][step].getAccent());
				}
			}
		}
		
		DEBUG("Parsing Time: %s Len:%zu", sequence.timeStr.c_str(), sequence.timeStr.size());
		line = sequence.timeStr;
		for (step = 0; step < length && step < (int)line.size(); ++step)
		{	
			if (line.at(step) == 'O' || line.at(step) == 'o' ) { attributes[0][step].setGate(true); }
			else if (line.at(step) == '_') { attributes[0][step].setTie(true); }
			else if (line.at(step) == ' ' || line.at(step) == '-' ) { attributes[0][step].clear(); } // Rest, redundant but just as a security
			else {
				attributes[0][step].clear();
				return -4; // wrong time value
			}
		}
		DEBUG("Letter: %s", letter.c_str());
		letters[0] = letter.at(0);
		DEBUG("Length: %d", length);
		lengths[0] = length;
		DEBUG("Transpose: %d", transpose);
		transposes[0] = transpose * SEMITONE;
		for (int i = 0; i < length; ++i)
		{	
			unsigned char ud;
			if (octaves[0][i] == 1.0) ud = 'u';
			else if (octaves[0][i] == -1.0) ud = 'd';
			else ud = ' ';
			const char *time;
			if (attributes[0][i].getGate()) time = "Gate";
			else if (attributes[0][i].getTie()) time = "Tie";
			else if (attributes[0][i].getAttribute() == 0u) time = "Rest";
			else time = "";			
			DEBUG("%d|Sha/Fla:%0.2f|Cv:%0.2f(%0.2fHz|U/D:%c|Acc/Sli:%c%c|Time:%s)"
				, i, sharpflats[0][i], notes[0][i], dsp::FREQ_C4 * pow(2,notes[0][i]), ud,
				(attributes[0][i].getAccent() ? 'A' : ' '), (attributes[0][i].getSlide() ? 'S' : ' '), time);
		}
		return 0;
	}

	void initRun() { // run button activated or run edge in run input jack
		clockIgnoreOnReset = (long) (clockIgnoreOnResetDuration * APP->engine->getSampleRate());
		stepIndexRun = 0;
	}

	float oldResParam;
	float oldCapParam;
	void process(const ProcessArgs& args) override {

		if (!slideFilter.prepared) {
			slideFilter.prepare(args.sampleRate);
		}

		if (sequence.dirty) {
			DEBUG("Header: %s", sequence.headerStr.c_str());
			DEBUG("Notes: %s", sequence.notesStr.c_str());
			DEBUG("Octaves: %s", sequence.octaveStr.c_str());
			DEBUG("Slide/Accent: %s", sequence.slideAccentStr.c_str());
			DEBUG("Time: %s", sequence.timeStr.c_str());
			int parseErr = parseSeq();
			if (parseErr) {
				DEBUG("Parse error: %d", parseErr);
			}
			sequence.dirty = false;
		}

		// Run button
		if (runningTrigger.process(params[RUN_PARAM].getValue())) {
			running = !running;
			if (running) {
				stepIndexRun = 0;
				clockIgnoreOnReset = (long) (clockIgnoreOnResetDuration * APP->engine->getSampleRate());
				if (resetOnRun) {
					initRun();
				}
			}
		}

		//********** Clock and reset **********
		
		// Clock
		if (running && clockIgnoreOnReset == 0l) {
			if (clockTrigger.process(inputs[CLOCK_INPUT].getVoltage())) {
				stepIndexRun++;
				if (stepIndexRun >= 16) {
					stepIndexRun = 0;
				}
				// DEBUG("%d %d %f", stepIndexRun + 1, attributes[0][stepIndexRun].getGate(), notes[0][stepIndexRun]);
			}
		}
    
		// Reset
		if (resetTrigger.process(params[RESET_PARAM].getValue() + inputs[RESET_INPUT].getVoltage())) {
			initRun();
			resetLight = 1.0f;
			clockTrigger.reset();
		}

		if (params[RES_PARAM].getValue() != oldResParam || params[CAP_PARAM].getValue() != oldCapParam) {
			oldResParam = params[RES_PARAM].getValue();
			oldCapParam = params[CAP_PARAM].getValue();
			slideFilter.setRackParameters(oldResParam, oldCapParam);
		}
		
		// gate on duty cycle = 49.96% - 55.8% <- assume 50% and calculate based on received gate ON time?
		if (running) {
			// latch cv, accent and slide to gate
			if (attributes[0][stepIndexRun].getGate()) currentCv = notes[0][stepIndexRun] + octaves[0][stepIndexRun] + transposes[0];
			if (attributes[0][stepIndexRun].getGate()) currentAccent = attributes[0][stepIndexRun].getAccent();
			if (attributes[0][stepIndexRun].getGate()) currentSlide = attributes[0][stepIndexRun].getSlide();

			// check is upcoming step is tied or first step if current step is last of pattern
			bool previousIsGate = attributes[0][(stepIndexRun - 1 < 0 ? 16 : stepIndexRun - 1)].getGate();

			bool nextIsTie = attributes[0][(stepIndexRun + 1 >= 16 ? 0 : stepIndexRun + 1)].getTie();
			bool previousIsTie = attributes[0][(stepIndexRun - 1 < 0 ? 16 : stepIndexRun - 1)].getTie();

			bool nextIsSlide = attributes[0][(stepIndexRun + 1 >= 16 ? 0 : stepIndexRun + 1)].getSlide();
			bool previousIsSlide = attributes[0][(stepIndexRun - 1 < 0 ? 16 : stepIndexRun - 1)].getSlide();

			bool isTie = attributes[0][stepIndexRun].getTie();
			bool isGate = attributes[0][stepIndexRun].getGate();
			bool isSlide = attributes[0][stepIndexRun].getSlide();

			bool clock = inputs[CLOCK_INPUT].getVoltage() > 0.1;

			bool gate = false;
			// copy clock : normal gate, no upcoming slide or tie
			// or: end of a tie
			if ((isGate && (!nextIsTie || !nextIsSlide)) || (isTie && previousIsTie && !nextIsTie)) {
				gate = clock;
			}
			// stay high: gate with upcoming slide or tie
			// or: during a long tie or a long slide
			if ((isGate && (nextIsTie || nextIsSlide)) || (isTie && previousIsTie && nextIsTie) || (isSlide && previousIsSlide && nextIsSlide)) {
				gate = true;
			}
			if (isTie && previousIsGate) {
				// during the first tie after a gate, copy clock if it's just a 2 steps tie, or stay high if more tie upcoming
				gate = (nextIsTie || nextIsSlide) ? true : clock;
			}

			bool accent = currentAccent;
			slideFilter.processSample(currentCv);
			float cv = currentSlide ? slideFilter.lastSample : currentCv;

			// Outputs
			outputs[CV_OUTPUT].setVoltage( cv );
			outputs[GATE_OUTPUT].setVoltage( (gate && (clockIgnoreOnReset == 0)) ? 10.f : 0.f );// gate retriggering on reset
			outputs[ACCENT_OUTPUT].setVoltage(accent ? 10.f : 0.f);
		} else {
			outputs[CV_OUTPUT].setVoltage(0.f);
			outputs[GATE_OUTPUT].setVoltage(0.f);
		}

		// Run light
		lights[RUN_LIGHT].setBrightness(running ? 1.0f : 0.0f);
		lights[RESET_LIGHT].setBrightnessSmooth(resetLight, (float)(args.sampleTime));
		resetLight = 0.0f;

		if (clockIgnoreOnReset > 0l)
			clockIgnoreOnReset--;

	}

	void onReset() override {
		clockIgnoreOnReset = (long) (clockIgnoreOnResetDuration * APP->engine->getSampleRate()); // useful when Rack starts
	}

	json_t* dataToJson() override {
		json_t* rootJ = json_object();

		// sequence
		json_object_set_new(rootJ, "header", json_stringn(sequence.headerStr.c_str(), sequence.headerStr.size()));
		json_object_set_new(rootJ, "notes", json_stringn(sequence.notesStr.c_str(), sequence.notesStr.size()));
		json_object_set_new(rootJ, "octave", json_stringn(sequence.octaveStr.c_str(), sequence.octaveStr.size()));
		json_object_set_new(rootJ, "slideAccent", json_stringn(sequence.slideAccentStr.c_str(), sequence.slideAccentStr.size()));
		json_object_set_new(rootJ, "time", json_stringn(sequence.timeStr.c_str(), sequence.timeStr.size()));

		// resetOnRun
		json_object_set_new(rootJ, "resetOnRun", json_boolean(resetOnRun));

		json_object_set_new(rootJ, "running", json_boolean(running));

		return rootJ;
	}

	void dataFromJson(json_t* rootJ) override {

		json_t* headerJ = json_object_get(rootJ, "header");
		if (headerJ)
			sequence.headerStr = json_string_value(headerJ);

		json_t* notesJ = json_object_get(rootJ, "notes");
		if (notesJ)
			sequence.notesStr = json_string_value(notesJ);

		json_t* octaveJ = json_object_get(rootJ, "octave");
		if (octaveJ)
			sequence.octaveStr = json_string_value(octaveJ);

		json_t* slideAccentJ = json_object_get(rootJ, "slideAccent");
		if (slideAccentJ)
			sequence.slideAccentStr = json_string_value(slideAccentJ);

		json_t* timeJ = json_object_get(rootJ, "time");
		if (timeJ)
			sequence.timeStr = json_string_value(timeJ);

		if (headerJ || notesJ || octaveJ || slideAccentJ || timeJ) {
			sequence.dirty = true;
		}

		// resetOnRun
		json_t *resetOnRunJ = json_object_get(rootJ, "resetOnRun");
		if (resetOnRunJ)
			resetOnRun = json_is_true(resetOnRunJ);
		
		// running
		json_t *runningJ = json_object_get(rootJ, "running");
		if (runningJ)
			running = json_is_true(runningJ);
	}
};

struct ComposerTextField : LedDisplayTextField {

	AcidComposer* module;
	int steps = 16;
	int stepHighlight = -1;
	int gridModulo = 4;
	int bgSteps = 1;
	float dxScale = 1.0;
	float caretOffset = 0.0;
	float dx;
	float lh;
	float fontSize = 12;
	float textRadius = BND_TEXT_RADIUS * 0.5;
	float padDown = BND_TEXT_PAD_DOWN * 1.8;
	std::string allowedCharacters = "";

	bool initSizes = false;
	bool dirty = true;

	bool noHighlight = false;

	// Modified from blendish and nanoVG
	static void mybndCaretPosition(NVGcontext *ctx, float x, float y,
		float desc, float lineHeight, const char *caret, NVGtextRow *rows,int nrows,
		int *cr, float *cx, float *cy, int *cn) {
		static NVGglyphPosition glyphs[BND_MAX_GLYPHS];
		int r,nglyphs;
		for (r=0; r < nrows-1 && rows[r].end < caret; ++r);
		*cr = r;
		*cx = x;
		*cy = y-lineHeight-desc + r*lineHeight;
		if (nrows == 0) return;
		*cx = rows[r].minx;
		nglyphs = nvgTextGlyphPositions(
			ctx, x, y, rows[r].start, rows[r].end+1, glyphs, BND_MAX_GLYPHS);
		for (int i=0; i < nglyphs; ++i) {
			*cx=glyphs[i].x;
			*cn=i;
			if (glyphs[i].str == caret) break;
		}
	}

	void mybndIconLabelCaret(NVGcontext *ctx, float x, float y, float w, float h,
		int iconid, NVGcolor color, float fontsize, const char *label,
		NVGcolor caretcolor, int cbegin, int cend, int fonthandle, float dx, float dxScale, float caretOffset) {
		float pleft = textRadius;
		if (!label) return;
		if (iconid >= 0) {
			bndIcon(ctx,x+4,y+2,iconid);
			pleft += BND_ICON_SHEET_RES;
		}

		if (fonthandle < 0) return;

		x+=pleft;
		y+=BND_WIDGET_HEIGHT-padDown;

		nvgFontFaceId(ctx, fonthandle);
		nvgFontSize(ctx, fontsize);
		nvgTextAlign(ctx, NVG_ALIGN_LEFT|NVG_ALIGN_BASELINE);

		w -= textRadius+pleft;

		if (cend >= cbegin) {
			int c0r,c1r,c1n,c0n = 0;
			float desc;
			float c0x,c0y,c1x,c1y;
			static NVGtextRow rows[BND_MAX_ROWS];
			int nrows = nvgTextBreakLines(
				ctx, label, label+cend+1, w, rows, BND_MAX_ROWS);
			nvgTextMetrics(ctx, NULL, &desc, &lh);

			mybndCaretPosition(ctx, x, y, desc, lh, label+cbegin,
				rows, nrows, &c0r, &c0x, &c0y, &c0n);
			mybndCaretPosition(ctx, x, y, desc, lh, label+cend,
				rows, nrows, &c1r, &c1x, &c1y, &c1n);

			nvgBeginPath(ctx);
			if (cbegin == cend) {
				nvgFillColor(ctx, caretcolor);
				nvgRect(ctx, caretOffset + x + c0n * dx * dxScale, c0y, dx * dxScale, lh);
			} else {
				nvgFillColor(ctx, caretcolor);
				if (c0r == c1r) {
					nvgRect(ctx, caretOffset + c0x-1, c0y, c1x-c0x+1, lh);
				} else {
					int blk=c1r-c0r-1;
					nvgRect(ctx, caretOffset + c0x-1, c0y, x+w-c0x+1, lh);
					nvgRect(ctx, caretOffset + x, c1y, c1x-x+1, lh);

					if (blk)
						nvgRect(ctx, caretOffset + x, c0y+lh, w, blk*lh);
				}
			}
			nvgFill(ctx);
		}

		nvgBeginPath(ctx);
		nvgFillColor(ctx, color);
		for (int i = 0; i < (int)strlen(label); ++i)
		{
			nvgTextBox(ctx,x + dx * dxScale * i,y,w,label + i, label + i + 1);
		}
	}

	ComposerTextField() {
		multiline = false;
		textOffset = math::Vec(0, 0);
		fontPath = asset::plugin(pluginInstance, "res/CozetteVector.ttf");
	}

	void step() override {
		LedDisplayTextField::step();
		// if (module && module->dirty) {
		if (module) {
			// setText(module->text);
			// module->dirty = false;
		}
	}

	// action is triggered when pressing Enter 
	void onAction(const ActionEvent& a) override {
		dirty = true;
	}

	void onChange(const ChangeEvent& e) override {
		if (module) {
			if ((int)getText().size() > steps) {
				text = getText().substr(0, steps);
			}
			if ((int)getText().size() < steps) {
				std::string pad = std::string(steps - getText().size(), ' ');
				text = getText() + pad;
			}
		}
	}

	void draw(const DrawArgs& args) override {
		if (!initSizes) {
			calculateCharacterWidth(args);
			initSizes = true;
		}

		LedDisplayTextField::draw(args);
	}

	void calculateCharacterWidth(const DrawArgs& args) {
		std::shared_ptr<window::Font> font = APP->window->loadFont(fontPath);
		if (font && font->handle >= 0) {
			bndSetFont(font->handle);
			nvgFontFaceId(args.vg, font->handle);
			nvgFontSize(args.vg, fontSize);
			nvgTextAlign(args.vg, NVG_ALIGN_LEFT|NVG_ALIGN_BASELINE);
			static NVGglyphPosition diffglyphs[2];
			nvgTextGlyphPositions(args.vg, textOffset.x, textOffset.y, "ab", NULL, diffglyphs, 2);
			dx = diffglyphs[1].x - diffglyphs[0].x;
			nvgTextMetrics(args.vg, NULL, NULL, &lh);
		}
	}

	void drawLayer(const DrawArgs& args, int layer) override {
		nvgScissor(args.vg, RECT_ARGS(args.clipBox));

		if (layer == 1) {
			calculateCharacterWidth(args);
			// Background grid
			for (int i = 0; i < steps; i += bgSteps)
			{	
				NVGcolor gridColor = color;
				gridColor.a = ((i % gridModulo == 0) ? 0.2 : 0.08);
				if (!noHighlight && i == stepHighlight * bgSteps) gridColor.r *= 3.0;
				nvgBeginPath(args.vg);
				nvgRect(args.vg, caretOffset + textRadius + i * dx * dxScale, 0.1, bgSteps * dx * dxScale * 0.9, lh); // 
				nvgFillColor(args.vg, gridColor);
				nvgFill(args.vg);
			}
			// Text
			std::shared_ptr<window::Font> font = APP->window->loadFont(fontPath);
			if (font && font->handle >= 0) {
				bndSetFont(font->handle);

				NVGcolor highlightColor = color;
				highlightColor.a = 0.5;
				int begin = std::min(cursor, selection);
				int end = (this == APP->event->selectedWidget) ? std::max(cursor, selection) : -1;

				mybndIconLabelCaret(args.vg,
					textOffset.x, textOffset.y,
					box.size.x, box.size.y,
					-1, color, fontSize, text.c_str(), highlightColor, begin, end, font->handle, dx, dxScale, caretOffset);

				bndSetFont(APP->window->uiFont->handle);
			}
		}

		Widget::drawLayer(args, layer);
		nvgResetScissor(args.vg);
	}

	bool filterCharacter(std::string ch) {
		if (allowedCharacters == "" ) // empty (default) allows all characters
			return true;
		if (allowedCharacters.find(ch) != std::string::npos) {
			return true;
		} else {
			return false;
		}
	}

	void onSelectText(const SelectTextEvent& e) override {
		if (filterCharacter(std::string(1, (unsigned char)e.codepoint))) {
			if (e.codepoint < 128) {
				// DEBUG("curlen before: %zu", getText().size());
				if (selection == cursor) selection = cursor + 1;
			}
			LedDisplayTextField::onSelectText(e);
			// DEBUG("curlen after: %zu %d", getText().size(), cursor);
			if (((int)getText().size() == steps) && (cursor == steps)) cursor = selection = steps - 1;
		} else {
			e.consume(this);
		}
	}

	void onSelectKey(const SelectKeyEvent& e) override {
		if (e.key == GLFW_KEY_UP && e.action == 0) {
			if (prevField) {
				APP->event->setSelectedWidget(prevField);
				ComposerTextField* prev = dynamic_cast<ComposerTextField*>(prevField);			
				prev->cursor = prev->selection = std::floor((float)(prev->steps / (float)steps) * cursor);
			}
			e.consume(this);
		} else if (e.key == GLFW_KEY_DOWN && e.action == 0) {
			if (nextField) {
				APP->event->setSelectedWidget(nextField);
				ComposerTextField* next = dynamic_cast<ComposerTextField*>(nextField);			
				next->cursor = next->selection = std::floor((float)(next->steps / (float)steps) * cursor);
			}
			e.consume(this);
		} else {
			LedDisplayTextField::onSelectKey(e);
			if (cursor >= steps - 1) {
				cursor = selection = steps - 1;
			}
		}
	}

	int	getTextPosition(math::Vec mousePos) override {
		return std::min(steps - 1, (int)((mousePos.x - textRadius) / (dx * dxScale)));
	}
};

struct SequenceDisplay : LedDisplay {
	AcidComposer* module;
	ComposerSequence* targetSeq;

	ComposerTextField* headerField;
	ComposerTextField* notesField;
	ComposerTextField* octaveField;
	ComposerTextField* slideAccentField;
	ComposerTextField* timeField;

	void step() override {
		if (module) {
			if (headerField->dirty ||
					notesField->dirty ||
					octaveField->dirty ||
					slideAccentField->dirty ||
					timeField->dirty) {
				targetSeq->headerStr = headerField->text;
				targetSeq->notesStr = notesField->text;
				targetSeq->octaveStr = octaveField->text;
				targetSeq->slideAccentStr = slideAccentField->text;
				targetSeq->timeStr = timeField->text;
				targetSeq->dirty = true;
				headerField->dirty = false;
				notesField->dirty = false;
				octaveField->dirty = false;
				slideAccentField->dirty = false;
				timeField->dirty = false;
			}
			headerField->stepHighlight = module->stepIndexRun;
			notesField->stepHighlight = module->stepIndexRun;
			octaveField->stepHighlight = module->stepIndexRun;
			slideAccentField->stepHighlight = module->stepIndexRun;
			timeField->stepHighlight = module->stepIndexRun;
		}
	}

	void draw(const DrawArgs& args) override {
		math::Rect r = box.zeroPos();

		// Black background
		nvgBeginPath(args.vg);
		nvgRect(args.vg, RECT_ARGS(r));
		NVGcolor topColor = nvgRGB(0x22, 0x22, 0x22);
		NVGcolor bottomColor = nvgRGB(0x12, 0x12, 0x12);
		nvgFillPaint(args.vg, nvgLinearGradient(args.vg, 0.0, 0.0, 0.0, 25.0, topColor, bottomColor));
		// nvgFillColor(args.vg, bottomColor);
		nvgFill(args.vg);

		// Outer strokes
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, 0.0, -0.5);
		nvgLineTo(args.vg, box.size.x, -0.5);
		nvgStrokeColor(args.vg, nvgRGBAf(0, 0, 0, 0.24));
		nvgStrokeWidth(args.vg, 1.0);
		nvgStroke(args.vg);

		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, 0.0, box.size.y + 0.5);
		nvgLineTo(args.vg, box.size.x, box.size.y + 0.5);
		nvgStrokeColor(args.vg, nvgRGBAf(1, 1, 1, 0.30));
		nvgStrokeWidth(args.vg, 1.0);
		nvgStroke(args.vg);

		// Inner strokes
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, 0.0, 1.0);
		nvgLineTo(args.vg, box.size.x, 1.0);
		nvgStrokeColor(args.vg, nvgRGBAf(1, 1, 1, 0.20));
		nvgStrokeWidth(args.vg, 1.0);
		nvgStroke(args.vg);

		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, 0.0, box.size.y - 1.0);
		nvgLineTo(args.vg, box.size.x, box.size.y - 1.0);
		nvgStrokeColor(args.vg, nvgRGBAf(1, 1, 1, 0.20));
		nvgStrokeWidth(args.vg, 1.0);
		nvgStroke(args.vg);

		// Black borders on left and right
		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, 0.8, box.size.y - .4);
		nvgLineTo(args.vg, 0.8, .4);
		nvgStrokeColor(args.vg, bottomColor);
		nvgStrokeWidth(args.vg, 1.5);
		nvgStroke(args.vg);

		nvgBeginPath(args.vg);
		nvgMoveTo(args.vg, box.size.x - 0.8, box.size.y - .4);
		nvgLineTo(args.vg, box.size.x - 0.8, .4);
		nvgStrokeColor(args.vg, bottomColor);
		nvgStrokeWidth(args.vg, 1.5);
		nvgStroke(args.vg);

		// Draw children inside box
		nvgScissor(args.vg, RECT_ARGS(args.clipBox));
		Widget::draw(args);
		nvgResetScissor(args.vg);

	}

	void setModule(AcidComposer* module) {
		this->module = module;

		if (module) {
			headerField = createWidget<ComposerTextField>(Vec(0, 2));
			headerField->box.size = mm2px(Vec(box.size.x, 4.0));
			headerField->color = nvgRGB(161, 161, 161);
			headerField->bgColor = nvgRGBA(0, 255, 0, 30);
			headerField->fontSize = 11;
			headerField->gridModulo = 1;
			headerField->steps = 8;
			headerField->text = module->sequence.headerStr;
			headerField->dxScale = 1.3;
			headerField->caretOffset = 0.2;
			headerField->allowedCharacters = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+- ";
			headerField->noHighlight = true;
			headerField->module = module;
			addChild(headerField);

			notesField = createWidget<ComposerTextField>(Vec(0, 2 + 12 * 1));
			notesField->box.size = mm2px(Vec(box.size.x, 4.0));
			notesField->color = nvgRGB(161, 161, 161);
			notesField->bgColor = nvgRGBA(0, 255, 0, 30);
			notesField->fontSize = 11;
			notesField->gridModulo = 8;
			notesField->steps = 32;
			notesField->bgSteps = 2;
			notesField->text = module->sequence.notesStr;
			notesField->dxScale = 1.3;
			notesField->caretOffset = 0.2;
			notesField->allowedCharacters = "ABCDEFGabcdefg# ";
			notesField->module = module;
			addChild(notesField);

			octaveField = createWidget<ComposerTextField>(Vec(0, 2 + 12 * 2));
			octaveField->box.size = mm2px(Vec(box.size.x, 4.0));
			octaveField->color = nvgRGB(161, 161, 161);
			octaveField->bgColor = nvgRGBA(255, 0, 0, 30);
			octaveField->fontSize = 11;
			octaveField->steps = 16;
			octaveField->text = module->sequence.octaveStr;
			octaveField->dxScale = 2.6;
			octaveField->caretOffset = 0.2;
			octaveField->allowedCharacters = "DUdu ";
			octaveField->module = module;
			addChild(octaveField);

			slideAccentField = createWidget<ComposerTextField>(Vec(0, 2 + 12 * 3));
			slideAccentField->box.size = mm2px(Vec(box.size.x, 4.0));
			slideAccentField->color = nvgRGB(161, 161, 161);
			slideAccentField->bgColor = nvgRGBA(255, 0, 0, 30);
			slideAccentField->fontSize = 11;
			slideAccentField->gridModulo = 8;
			slideAccentField->steps = 32;
			slideAccentField->bgSteps = 2;
			slideAccentField->text = module->sequence.slideAccentStr;
			slideAccentField->dxScale = 1.3;
			slideAccentField->caretOffset = 0.2;
			slideAccentField->allowedCharacters = "SAsa ";
			slideAccentField->module = module;
			addChild(slideAccentField);

			timeField = createWidget<ComposerTextField>(Vec(0, 2 + 12 * 4));
			timeField->box.size = mm2px(Vec(box.size.x, 4.0));
			timeField->color = nvgRGB(161, 161, 161);
			timeField->bgColor = nvgRGBA(255, 0, 0, 30);
			timeField->fontSize = 11;
			timeField->steps = 16;
			timeField->text = module->sequence.timeStr;
			timeField->dxScale = 2.6;
			timeField->caretOffset = 0.2;
			timeField->allowedCharacters = "o_- ";
			timeField->module = module;
			addChild(timeField);

			headerField->nextField = notesField;

			notesField->prevField = headerField;
			notesField->nextField = octaveField;

			octaveField->prevField = notesField;
			octaveField->nextField = slideAccentField;

			slideAccentField->nextField = timeField;
			slideAccentField->prevField = octaveField;

			timeField->prevField = slideAccentField;

			DEBUG("setModule");
		}

	}
};

struct Small303Knob : RoundSmallBlackKnob {
    Small303Knob() {
        setSvg(Svg::load(asset::plugin(pluginInstance, "res/303Knob_0_4.svg")));
        bg->setSvg(Svg::load(asset::plugin(pluginInstance, "")));
    }
};

struct _303PJ301MPort : PJ301MPort {
	_303PJ301MPort() {
        setSvg(Svg::load(asset::plugin(pluginInstance, "res/PJ301M_acid.svg")));
	}
};

struct AcidComposerWidget : ModuleWidget {

	struct GreenLight : GrayModuleLightWidget {
		GreenLight() {
			addBaseColor(SCHEME_GREEN);
		}
	
	};

	AcidComposerWidget(AcidComposer* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/AcidComposer_vector.svg")));

		SequenceDisplay* seqDisp = createWidget<SequenceDisplay>(mm2px(Vec(11.5, 10.0)));
		seqDisp->box.size = mm2px(Vec(72.2, 21.5));
		seqDisp->setModule(module);
		seqDisp->targetSeq = &module->sequence;
		addChild(seqDisp);

		// Inputs
		float xGuides[] = {12.51, 25.25, 57.14, 67.72, 78.30};
		float yGuides[] = {51.59, 101.62, 114.75};
		addInput(createInputCentered<_303PJ301MPort>(mm2px(Vec(xGuides[0], yGuides[1])), module, AcidComposer::RESET_INPUT));
		addInput(createInputCentered<_303PJ301MPort>(mm2px(Vec(xGuides[0], yGuides[2])), module, AcidComposer::CLOCK_INPUT));
		// Outputs
		addOutput(createOutputCentered<_303PJ301MPort>(mm2px(Vec(xGuides[2], yGuides[2])), module, AcidComposer::ACCENT_OUTPUT));
		addOutput(createOutputCentered<_303PJ301MPort>(mm2px(Vec(xGuides[3], yGuides[2])), module, AcidComposer::GATE_OUTPUT));
		addOutput(createOutputCentered<_303PJ301MPort>(mm2px(Vec(xGuides[4], yGuides[2])), module, AcidComposer::CV_OUTPUT));

		addParam(createParamCentered<Small303Knob>(mm2px(Vec(xGuides[0], yGuides[0])), module, AcidComposer::RES_PARAM));
		addParam(createParamCentered<Small303Knob>(mm2px(Vec(xGuides[1], yGuides[0])), module, AcidComposer::CAP_PARAM));

		// Buttons bezel and light
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(xGuides[1], yGuides[2])), module, AcidComposer::RUN_PARAM));
		addChild(createLightCentered<LEDBezelLight<RedLight>>(mm2px(Vec(xGuides[1], yGuides[2])), module, AcidComposer::RUN_LIGHT));
		addParam(createParamCentered<LEDBezel>(mm2px(Vec(xGuides[1], yGuides[1])), module, AcidComposer::RESET_PARAM));
		addChild(createLightCentered<LEDBezelLight<RedLight>>(mm2px(Vec(xGuides[1], yGuides[1])), module, AcidComposer::RESET_LIGHT));
	}

	void appendContextMenu(Menu *menu) override {
		AcidComposer *module = dynamic_cast<AcidComposer*>(this->module);
		assert(module);

		menu->addChild(createBoolPtrMenuItem("Reset on run", "", &module->resetOnRun));
	}

};

Model* modelAcidComposer = createModel<AcidComposer, AcidComposerWidget>("AcidComposer");
