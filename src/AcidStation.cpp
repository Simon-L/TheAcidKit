#include "plugin.hpp"

#include <slime/dsp/LadderFilter.hpp>
#include <slime/Math.hpp>
#include <slime/cv/Digital.hpp>

struct Envelope3Generator {
	enum class Stage {
		IDLE,
		ATTACK,
		DECAY,
		RELEASE,
	};

	static constexpr float OVERSHOOT = 1.15f;
	static constexpr float COEFF = 2.03688192726f;  // -ln(1 - 1 / 1.15)) causes convergence at t=1
	static constexpr float IDLE_EPS = 0.0f;

	float attack_time = 0.5f;
	float decay_time = 1.0f;
	float value = 0.0f;

	Stage stage = Stage::IDLE;
	float target = 0.0f;
	bool attack_triggered = false;
	bool decay_triggered = false;

	void reset() {
		stage = Stage::IDLE;
		target = 0.0f;
		attack_triggered = false;
		decay_triggered = false;
		value = 0.0f;
	}

	void trigger() {
		if (stage == Stage::ATTACK)
			return;

		target = OVERSHOOT;
		stage = Stage::ATTACK;
		attack_triggered = true;
	}

	void release() {
		// if (stage == Stage::ATTACK || stage == Stage::IDLE)
		// 	return;

		stage = Stage::RELEASE;
	}

	float process(float delta_time) {
		if (stage == Stage::IDLE)
			return 0.0f;

		if (stage == Stage::ATTACK) {
			value += COEFF * delta_time * (target - value) / attack_time;

			if (value > 1.0f) {
				value = 1.0f;
				target = 1.0f - OVERSHOOT;
				stage = Stage::DECAY;
				decay_triggered = true;
			}
		} else if (stage == Stage::DECAY || stage == Stage::RELEASE) {
			value += COEFF * delta_time * (target - value) / ( stage == Stage::DECAY ? decay_time : 6e-3);

			if (value < IDLE_EPS) {
				value = 0.0f;
				stage = Stage::IDLE;
			}
		}

		return value;
	}

	bool isIdle(void) {
		return stage == Stage::IDLE;
	}

	bool attackWasTriggered(void) {
		bool result = attack_triggered;
		attack_triggered = false;
		return result;
	}

	bool decayWasTriggered(void) {
		bool result = decay_triggered;
		decay_triggered = false;
		return result;
	}
};

struct AcidStation : Module {

	// Mutable config
	float eg1_decay = 1e6f;
	float eg2_decay = 1e6f;
	float eg2_memory = 0.0f; // "wow" filter on vcf envelope
	float eg2_memory_last = 0.0f;
	float eg2_memory_intensity = 0.999;

	// Internal state
	Envelope3Generator eg1, eg2;
	slime::cv::SchmittTrigger trigger1_filter, trigger2_filter, hold_filter;

	// Internal state
	std::array<slime::dsp::FourPoleLadderLowpass<slime::math::float_simd>, slime::math::SIMD_PAR> filters;
	std::array<slime::math::float_simd, slime::math::SIMD_PAR> frequency;
	rack::dsp::PeakFilter level_filter;
	rack::dsp::ClockDivider level_divider, param_divider, light_divider, expander_divider;
	float drive;
	bool accent = false;

	enum ParamIds { FREQ_PARAM,
		RES_PARAM,
		FM_AMOUNT_PARAM,
		VCA_DECAY_PARAM,
		VCF_DECAY_PARAM,
		ENVMOD_PARAM,
		ACCENT_PARAM,
		HOLD_PARAM,
		DRIVE_PARAM,
		PARAMS_LEN
	};
	enum InputIds { FREQ_INPUT,
		FM_INPUT,
		SIGNAL_INPUT,
		ACCENT_INPUT,
		GATE_INPUT,
		INPUTS_LEN
	};
	enum OutputId {
		SIGNAL_OUTPUT,
		OUTPUTS_LEN
	};
	enum LightIds {
		DRIVE_LIGHT,
		VCA_DECAY_LIGHT,
		VCF_DECAY_LIGHT,
		LIGHTS_LEN
	};

	AcidStation() {
		config(PARAMS_LEN, INPUTS_LEN, OUTPUTS_LEN, LIGHTS_LEN);

		configInput(ACCENT_INPUT, "Accent");
		configInput(GATE_INPUT, "Gate");
		configInput(FREQ_INPUT, "Cutoff");
		configInput(FM_INPUT, "FM");
		configInput(SIGNAL_INPUT, "Signal");
		configOutput(SIGNAL_OUTPUT, "Signal");
		configBypass(SIGNAL_INPUT, SIGNAL_OUTPUT);

		configParam(FREQ_PARAM, 0.0f, slime::math::LOG_2_10 * 3.0f, slime::math::LOG_2_10 * 1.5f, "Frequency", " Hz",
					2.0f, 20.0f);
		configParam(RES_PARAM, 0.0f, 1.2f, 0.0f, "Resonance", "%", 0.0f, 100.0f);
		configParam(FM_AMOUNT_PARAM, -1.0f, 1.0f, 0.0f, "FM Amount", "%", 0.0f, 100.0f);
		configParam(VCA_DECAY_PARAM, -3.0f, 1.0f, 0.919078f, "VCA Decay", " ms", 10.0f, 1000.0f);
		configParam(VCF_DECAY_PARAM, -3.0f, 1.0f, -0.187086f, "VCF Decay", " ms", 10.0f, 1000.0f);
		configParam(ENVMOD_PARAM, 0.0f, 1.0f, 0.0f, "Envelope modulation", "%", 0.0f, 100.0f);
		configParam(ACCENT_PARAM, 0.0f, 1.0f, 0.5f, "Accent amount", "%", 0.0f, 100.0f);
		configParam(DRIVE_PARAM, 0.0f, 1.0f, 0.0f, "Drive", "", 0.0f, 1.0f);

		configSwitch(HOLD_PARAM, 0.0f, 1.0f, 0.0f, "Hold", {"OFF", "ON"});
		getParamQuantity(HOLD_PARAM)->randomizeEnabled = false;

		param_divider.setDivision(8);  // Min 5512 Hz
		light_divider.setDivision(512);
		expander_divider.setDivision(8192);
		level_divider.setDivision(64);
		level_filter.setLambda(5.0f);

		onReset();
	}

	void onReset(void) override {
		for (auto& filter : filters) {
			filter.reset();
		}

		level_filter.reset();
		level_divider.reset();
		param_divider.reset();
		light_divider.reset();
		frequency.fill(20.0f * rack::dsp::approxExp2_taylor5<slime::math::float_simd>(slime::math::LOG_2_10 * 1.5f));

		eg1.attack_time = std::pow(10.0f, -2.522878f);
		eg2.attack_time = std::pow(10.0f, -2.522878f);
		eg1.reset();
		eg2.reset();

		drive = 9.5f;
	}

	void process(const ProcessArgs& args) override {
		size_t channels = std::max(std::max(inputs[SIGNAL_INPUT].getChannels(), inputs[FREQ_INPUT].getChannels()),
								   inputs[FM_INPUT].getChannels());
		if (channels < 1) {
			channels = 1;
		}

		outputs[SIGNAL_OUTPUT].setChannels(channels);

		// Update params
		if (param_divider.process()) {
			slime::math::float_simd base_res = params[RES_PARAM].getValue();

			hold_filter.process(params[HOLD_PARAM].getValue() * 2.0f);
			if (hold_filter.isRising()) {
				eg1_decay = 1.0; // 10000ms
				eg1.decay_time = std::pow(10.0f, eg1_decay);
			}
			if (hold_filter.isFalling()) {
				eg1_decay = params[VCA_DECAY_PARAM].getValue();
				eg1.decay_time = std::pow(10.0f, eg1_decay);
			}

			if (!hold_filter.isHigh()) {
				if (eg1_decay != params[VCA_DECAY_PARAM].getValue()) {
					eg1_decay = params[VCA_DECAY_PARAM].getValue();
					eg1.decay_time = std::pow(10.0f, eg1_decay);
				}
			}

			if (eg2_decay != params[VCF_DECAY_PARAM].getValue() && !accent) {
				eg2_decay = params[VCF_DECAY_PARAM].getValue();
				eg2.decay_time = std::pow(10.0f, eg2_decay);
			}

			for (size_t ch = 0; ch < channels; ch += slime::math::float_simd::size) {
				size_t simd_index = ch / slime::math::float_simd::size;

				auto* filter = &filters[simd_index];

				// Resonance from expander
				slime::math::float_simd res = base_res;

				res = rack::simd::clamp(res, 0.0f, 1.2f);

				filter->setResonance(res);
			}

			drive = 9.5f - 9.0f * params[DRIVE_PARAM].getValue();
		}

		trigger2_filter.process(2.0f * inputs[ACCENT_INPUT].getVoltage());
		trigger1_filter.process(2.0f * inputs[GATE_INPUT].getVoltage());

		if (trigger1_filter.isRising()) {
			// Kinda S&H accent input to gate
			// Accent should come on the same edge as gate
			if (trigger2_filter.isRising() && !accent) {
				accent = true;
				eg2_decay = -0.7; // 200ms
				eg2.decay_time = std::pow(10.0f, eg2_decay);
			}
			if (!trigger2_filter.isHigh() && accent) {
				accent = false;
				eg2.release();
				eg2_decay = params[VCF_DECAY_PARAM].getValue();
				eg2.decay_time = std::pow(10.0f, eg2_decay);
			}
			eg1.trigger();
			eg2.trigger();
		}
		if (trigger1_filter.isFalling()) {
			if (!hold_filter.isHigh()) eg1.release();
			if (!accent && !hold_filter.isHigh()) eg2.release();
		}

		eg1.process(args.sampleTime);
		eg2.process(args.sampleTime);

		eg2_memory = (eg2.value * accent) * (1 - eg2_memory_intensity) + eg2_memory_last * eg2_memory_intensity;
		eg2_memory_last = eg2_memory;

		// Cutoff param updates continuously while the envelope is active or when the divider just triggered
		if (param_divider.clock == 0 || !eg2.isIdle()) {
			for (size_t ch = 0; ch < channels; ch += slime::math::float_simd::size) {
				size_t simd_index = ch / slime::math::float_simd::size;

				auto* filter = &filters[simd_index];
				float eg2_mix = (eg2.value - 0.3137) + (accent ? eg2.value * params[ACCENT_PARAM].getValue() * (1.0f - params[RES_PARAM].getValue())
					+ eg2_memory * 1.5 * params[ACCENT_PARAM].getValue() * params[RES_PARAM].getValue() : 0.0f);
				slime::math::float_simd pitch = rack::simd::clamp(
				params[FREQ_PARAM].getValue() + (eg2_mix * 2.0f * params[ENVMOD_PARAM].getValue()) + inputs[FREQ_INPUT].getPolyVoltageSimd<slime::math::float_simd>(ch) +
					params[FM_AMOUNT_PARAM].getValue() * inputs[FM_INPUT].getPolyVoltageSimd<slime::math::float_simd>(ch),
					0.0f, slime::math::LOG_2_10 * 3.0f);
				slime::math::float_simd freq = 20.0f * rack::dsp::approxExp2_taylor5<slime::math::float_simd>(pitch);
				filter->setCutoffFrequency(freq);
			}
		}

		slime::math::float_simd signal, clipped;
		// Run filter
		for (size_t ch = 0; ch < channels; ch += slime::math::float_simd::size) {
			size_t simd_index = ch / slime::math::float_simd::size;
			auto* filter = &filters[simd_index];

			slime::math::float_simd in = inputs[SIGNAL_INPUT].getPolyVoltageSimd<slime::math::float_simd>(ch);
			in += 1e-6f * (2.0f * rack::random::uniform() - 1.0f);
			filter->process(args.sampleTime, in);

			float vca_env = (eg1.value * eg1.value) + (accent ? eg2.value * eg2.value * params[ACCENT_PARAM].getValue() :  0.0f);

			signal = filter->lowpass4() * vca_env;
			clipped = 9.0f * slime::math::tanh_rational5(signal / drive);
			outputs[SIGNAL_OUTPUT].setVoltageSimd(clipped, ch);
			float eg2_mix = (eg2.value - 0.3137) + (accent ? eg2.value * params[ACCENT_PARAM].getValue() * (1.0f - params[RES_PARAM].getValue())
				+ eg2_memory * 1.5 * params[ACCENT_PARAM].getValue() * params[RES_PARAM].getValue() : 0.0f);
			slime::math::float_simd pitch = rack::simd::clamp(
			params[FREQ_PARAM].getValue() + (eg2_mix * 2.0f * params[ENVMOD_PARAM].getValue()) + inputs[FREQ_INPUT].getPolyVoltageSimd<slime::math::float_simd>(ch) +
				params[FM_AMOUNT_PARAM].getValue() * inputs[FM_INPUT].getPolyVoltageSimd<slime::math::float_simd>(ch),
				0.0f, slime::math::LOG_2_10 * 3.0f);
		}

		if (level_divider.process()) {
			level_filter.process(args.sampleTime * static_cast<float>(level_divider.division),
								 std::abs(clipped[0] - signal[0]));
		}

		// Lights
		if (light_divider.process()) {
			lights[VCA_DECAY_LIGHT].setSmoothBrightness(
				(eg1.decayWasTriggered() || (eg1.stage == Envelope3Generator::Stage::DECAY)) ? 1.0f : 0.0f,
				args.sampleTime * light_divider.division * 0.1f);
			lights[VCF_DECAY_LIGHT].setSmoothBrightness(
				(eg2.decayWasTriggered() || (eg2.stage == Envelope3Generator::Stage::DECAY)) ? 1.0f : 0.0f,
				args.sampleTime * light_divider.division * 0.1f);
			lights[DRIVE_LIGHT].setBrightness(level_filter.out - 1.0f);
		}
	}
};

struct Small303Knob : RoundSmallBlackKnob {
    Small303Knob() {
        setSvg(Svg::load(asset::plugin(pluginInstance, "res/303Knob_0_4.svg")));
        bg->setSvg(Svg::load(asset::plugin(pluginInstance, "")));
    }
};

struct Huge303Knob : RoundSmallBlackKnob {
    Huge303Knob() {
        setSvg(Svg::load(asset::plugin(pluginInstance, "res/303Knob_0_8.svg")));
        bg->setSvg(Svg::load(asset::plugin(pluginInstance, "")));
    }
};

struct _303Trimpot : RoundSmallBlackKnob {
    _303Trimpot() {
        setSvg(Svg::load(asset::plugin(pluginInstance, "res/303Knob_0_24.svg")));
        bg->setSvg(Svg::load(asset::plugin(pluginInstance, "")));
    }
};

struct _303PJ301MPort : PJ301MPort {
	_303PJ301MPort() {
        setSvg(Svg::load(asset::plugin(pluginInstance, "res/PJ301M_acid.svg")));
	}
};

struct AcidStationWidget : ModuleWidget {
	AcidStationWidget(AcidStation* module) {
		setModule(module);
		setPanel(createPanel(asset::plugin(pluginInstance, "res/AcidStation_vector.svg")));

		// Big knob
		addParam(createParamCentered<Huge303Knob>(mm2px(Vec(32.83f, 27.94f)), module, AcidStation::FREQ_PARAM));

		// Small knobs
		rack::math::Vec knobs[] = {{25.3f, 54.62f}, {40.5f, 54.62f}, {10.16f, 20.62f}, {10.16f, 37.62f}, {10.16f, 54.62f}, {10.16f, 71.62f}, };
		for (size_t i = 0; i < 6; i++) {
			addParam(createParamCentered<Small303Knob>(mm2px(Vec(knobs[i].x, knobs[i].y)), module, AcidStation::RES_PARAM + i));
		}
		// Trimpot
		addParam(createParamCentered<_303Trimpot>(mm2px(Vec(36.34f, 70.77f)), module, AcidStation::DRIVE_PARAM));

		// Button
		addParam(createParamCentered<LEDButton>(mm2px(Vec(24.94f, 70.77f)), module, AcidStation::HOLD_PARAM));

		// Inputs
		rack::math::Vec inputs[] = {{25.46f, 90.17f}, {40.70f, 90.17f}, {25.46f, 106.69f}, {10.16f, 90.17f}, {10.16f, 106.69f}};
		for (size_t i = 0; i < AcidStation::INPUTS_LEN; i++) {
			addInput(createInputCentered<_303PJ301MPort>(mm2px(Vec(inputs[i].x, inputs[i].y)), module, AcidStation::FREQ_INPUT + i));
		}

		// Output
		addOutput(createOutputCentered<_303PJ301MPort>(mm2px(Vec(40.7f, 106.69f)), module, AcidStation::SIGNAL_OUTPUT));

		// Lights
		rack::math::Vec lights[] = {{32.00f, 76.00f}, {2.8f, 28.38f}, {2.8f, 45.38f}};
		for (size_t i = 0; i < AcidStation::LIGHTS_LEN; i++) {
			addChild(createLightCentered<SmallLight<WhiteLight>>(mm2px(Vec(lights[i].x, lights[i].y)),
					 module, AcidStation::DRIVE_LIGHT + i));
		}
	}
};

Model* modelAcidStation = createModel<AcidStation, AcidStationWidget>("AcidStation");