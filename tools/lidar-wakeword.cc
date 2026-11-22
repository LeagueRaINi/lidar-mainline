// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * lidar-wakeword: listens for a wake word ("Hey Jarvis") in 16 kHz 16-bit
 * mono audio on stdin, which is what lidar-voice writes.
 *
 * The detector is microWakeWord (Kevin Ahrendt, ESPHome), run the way
 * ESPHome's micro_wake_word component runs it:
 *
 *   features  TensorFlow's audio "microfrontend", with the settings every
 *             microWakeWord model is trained with: a 30 ms window every
 *             10 ms, 40 mel channels from 125 to 7500 Hz, noise reduction,
 *             PCAN gain control and a log scale. Each value v (0 to about
 *             670) goes to the model as (v * 256 + 333) / 666 - 128, an int8
 *   model     a streaming TensorFlow Lite model on TensorFlow Lite Micro.
 *             It takes as many feature slices at a time as its input tensor
 *             holds (three: one inference every 30 ms), keeps its own
 *             history in resource variables, and returns the probability
 *             that the wake word just ended, a uint8 (255 for certain)
 *   decision  the wake word is detected once the mean of the last
 *             sliding_window_size probabilities is above probability_cutoff
 *             (both from the model's JSON manifest; the cutoff is rounded
 *             down to n/255). The detector then forgets its probabilities
 *             and stays quiet for the next 100 feature slices (1 s) with a
 *             probability below the cutoff, and so it does for the first
 *             second after start
 *
 * Prints one line per detection to stdout:
 *   wake <model> <mean probability> <seconds since boot>
 *
 * This file is GPL-2.0-or-later, unlike the other tools here: the binary
 * links TensorFlow Lite Micro, the microfrontend, FlatBuffers (Apache-2.0)
 * and KISS FFT (BSD-3-Clause), and Apache-2.0 code combines with GPL
 * version 3, not with version 2 alone.
 *
 * Usage: lidar-wakeword [-m model.tflite] [-c model.json] [-t threshold] [-v]
 *   -m  the model, default /usr/share/lidar/wakeword/hey_jarvis.tflite
 *   -c  its manifest, default the model's path with .json for .tflite
 *   -t  probability cutoff (0-1) instead of the manifest's
 *   -v  the model's parameters, then for every inference the time in the
 *       stream (s) and the probability, on stderr
 */
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <unistd.h>
#include <vector>

#include "tensorflow/lite/experimental/microfrontend/lib/frontend.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend_util.h"
#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"

namespace {

constexpr int kRate = 16000;
constexpr int kFeatures = 40;
constexpr int kWindowMs = 30;
/* feature slices without detection after start and after each detection */
constexpr int kQuietSlices = 100;
/*
 * The manifest's tensor_arena_size is what a 32-bit ESP32 needs. TensorFlow
 * Lite Micro's own records hold pointers and size_t, twice as large here.
 */
constexpr int kArenaScale = 4;
/* the model's resource variables: hey_jarvis has six */
constexpr int kVariables = 20;

bool read_file(const std::string &path, std::vector<uint8_t> &data)
{
	FILE *f = fopen(path.c_str(), "rb");
	uint8_t buf[4096];
	size_t n;

	if (!f)
		return false;
	while ((n = fread(buf, 1, sizeof buf, f)) > 0)
		data.insert(data.end(), buf, buf + n);
	bool ok = !ferror(f);
	fclose(f);
	return ok;
}

/* the text after "key": in a JSON object without nested keys of that name */
const char *json_value(const std::string &json, const char *key)
{
	size_t at = json.find(std::string("\"") + key + "\"");

	if (at == std::string::npos)
		return nullptr;
	at = json.find_first_not_of(" \t\r\n", at + strlen(key) + 2);
	if (at == std::string::npos || json[at] != ':')
		return nullptr;
	at = json.find_first_not_of(" \t\r\n", at + 1);
	return at == std::string::npos ? nullptr : json.c_str() + at;
}

bool json_number(const std::string &json, const char *key, double &value)
{
	const char *v = json_value(json, key);
	char *end;

	if (!v)
		return false;
	value = strtod(v, &end);
	return end != v;
}

double uptime()
{
	struct timespec ts;

	clock_gettime(CLOCK_BOOTTIME, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

}  // namespace

int main(int argc, char **argv)
{
	std::string model_path = "/usr/share/lidar/wakeword/hey_jarvis.tflite", manifest_path;
	double threshold = -1;
	bool verbose = false;
	int opt;

	while ((opt = getopt(argc, argv, "m:c:t:v")) != -1) {
		switch (opt) {
		case 'm': model_path = optarg; break;
		case 'c': manifest_path = optarg; break;
		case 't': threshold = atof(optarg); break;
		case 'v': verbose = true; break;
		default:
			fprintf(stderr, "usage: lidar-wakeword [-m model.tflite] [-c model.json] [-t threshold] [-v]\n");
			return 2;
		}
	}
	if (threshold != -1 && (threshold <= 0 || threshold > 1)) {
		fprintf(stderr, "lidar-wakeword: bad option\n");
		return 2;
	}
	const size_t dot = model_path.rfind('.'), slash = model_path.rfind('/');
	const bool has_ext = dot != std::string::npos && (slash == std::string::npos || dot > slash);
	const std::string stem = has_ext ? model_path.substr(0, dot) : model_path;
	const std::string name = slash == std::string::npos ? stem : stem.substr(slash + 1);

	if (manifest_path.empty())
		manifest_path = stem + ".json";

	std::vector<uint8_t> manifest_data;
	double cutoff_f, window_f, step_f, arena_f;

	if (!read_file(manifest_path, manifest_data)) {
		fprintf(stderr, "lidar-wakeword: %s: %s\n", manifest_path.c_str(), strerror(errno));
		return 1;
	}
	const std::string manifest(manifest_data.begin(), manifest_data.end());

	if (!json_number(manifest, "probability_cutoff", cutoff_f) ||
	    !json_number(manifest, "sliding_window_size", window_f) ||
	    !json_number(manifest, "feature_step_size", step_f) ||
	    !json_number(manifest, "tensor_arena_size", arena_f) ||
	    cutoff_f <= 0 || cutoff_f > 1 || window_f < 1 || window_f > 1000 ||
	    step_f < 1 || step_f > kWindowMs || arena_f < 1 || arena_f > (64 << 20)) {
		fprintf(stderr, "lidar-wakeword: %s: not a microWakeWord manifest\n", manifest_path.c_str());
		return 1;
	}
	if (threshold != -1)
		cutoff_f = threshold;
	const int cutoff = static_cast<int>(cutoff_f * 255);
	const int window = static_cast<int>(window_f), step_ms = static_cast<int>(step_f);

	std::vector<uint8_t> model_data;

	if (!read_file(model_path, model_data)) {
		fprintf(stderr, "lidar-wakeword: %s: %s\n", model_path.c_str(), strerror(errno));
		return 1;
	}
	flatbuffers::Verifier verifier(model_data.data(), model_data.size());

	if (!tflite::VerifyModelBuffer(verifier) ||
	    tflite::GetModel(model_data.data())->version() != TFLITE_SCHEMA_VERSION) {
		fprintf(stderr, "lidar-wakeword: %s: not a TensorFlow Lite model\n", model_path.c_str());
		return 1;
	}

	/* what hey_jarvis is made of, and nothing else */
	tflite::MicroMutableOpResolver<12> ops;

	if (ops.AddCallOnce() != kTfLiteOk || ops.AddVarHandle() != kTfLiteOk ||
	    ops.AddReadVariable() != kTfLiteOk || ops.AddAssignVariable() != kTfLiteOk ||
	    ops.AddReshape() != kTfLiteOk || ops.AddStridedSlice() != kTfLiteOk ||
	    ops.AddConcatenation() != kTfLiteOk || ops.AddConv2D() != kTfLiteOk ||
	    ops.AddDepthwiseConv2D() != kTfLiteOk || ops.AddFullyConnected() != kTfLiteOk ||
	    ops.AddLogistic() != kTfLiteOk || ops.AddQuantize() != kTfLiteOk) {
		fprintf(stderr, "lidar-wakeword: cannot register the model's operators\n");
		return 1;
	}
	/*
	 * The table of resource variables lives in an arena of its own; their
	 * contents come from the interpreter's.
	 */
	std::vector<uint8_t> variable_arena(8192), arena(static_cast<size_t>(arena_f) * kArenaScale);
	tflite::MicroAllocator *variable_allocator =
		tflite::MicroAllocator::Create(variable_arena.data(), variable_arena.size());
	tflite::MicroResourceVariables *variables = variable_allocator ?
		tflite::MicroResourceVariables::Create(variable_allocator, kVariables) : nullptr;

	if (!variables) {
		fprintf(stderr, "lidar-wakeword: no room for the model's variables\n");
		return 1;
	}
	tflite::MicroInterpreter interpreter(tflite::GetModel(model_data.data()), ops,
					     arena.data(), arena.size(), variables);

	if (interpreter.AllocateTensors() != kTfLiteOk) {
		fprintf(stderr, "lidar-wakeword: %s: cannot set up the model\n", model_path.c_str());
		return 1;
	}
	TfLiteTensor *input = interpreter.input(0), *output = interpreter.output(0);

	if (interpreter.inputs_size() != 1 || interpreter.outputs_size() != 1 ||
	    input->type != kTfLiteInt8 || input->dims->size != 3 || input->dims->data[0] != 1 ||
	    input->dims->data[1] < 1 || input->dims->data[2] != kFeatures ||
	    output->type != kTfLiteUInt8 || output->dims->size != 2 ||
	    output->dims->data[0] != 1 || output->dims->data[1] != 1) {
		fprintf(stderr, "lidar-wakeword: %s: not a streaming microWakeWord model\n", model_path.c_str());
		return 1;
	}
	const int stride = input->dims->data[1];
	int8_t *slices = tflite::GetTensorData<int8_t>(input);

	struct FrontendConfig config;
	struct FrontendState frontend;

	config.window.size_ms = kWindowMs;
	config.window.step_size_ms = step_ms;
	config.filterbank.num_channels = kFeatures;
	config.filterbank.lower_band_limit = 125;
	config.filterbank.upper_band_limit = 7500;
	config.noise_reduction.smoothing_bits = 10;
	config.noise_reduction.even_smoothing = 0.025f;
	config.noise_reduction.odd_smoothing = 0.06f;
	config.noise_reduction.min_signal_remaining = 0.05f;
	config.pcan_gain_control.enable_pcan = 1;
	config.pcan_gain_control.strength = 0.95f;
	config.pcan_gain_control.offset = 80;
	config.pcan_gain_control.gain_bits = 21;
	config.log_scale.enable_log = 1;
	config.log_scale.scale_shift = 6;
	if (!FrontendPopulateState(&config, &frontend, kRate)) {
		fprintf(stderr, "lidar-wakeword: cannot set up the feature frontend\n");
		return 1;
	}
	if (verbose)
		fprintf(stderr, "lidar-wakeword: %s: cutoff %d/255, window %d, step %d ms, stride %d, arena %zu of %zu bytes\n",
			name.c_str(), cutoff, window, step_ms, stride, interpreter.arena_used_bytes(), arena.size());
	setvbuf(stdout, nullptr, _IOLBF, 0);

	std::vector<uint8_t> recent(window, 0);
	int16_t pcm[kRate / 10];
	size_t have = 0;	/* bytes in pcm */
	uint64_t consumed = 0;	/* samples */
	int slice = 0, recent_at = 0, latest = 0, quiet = kQuietSlices;

	for (;;) {
		ssize_t n = read(STDIN_FILENO, reinterpret_cast<char *>(pcm) + have, sizeof pcm - have);

		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			fprintf(stderr, "lidar-wakeword: stdin: %s\n", strerror(errno));
			return 1;
		}
		if (n == 0)
			break;
		have += n;

		const int16_t *samples = pcm;
		size_t left = have / sizeof *pcm;

		while (left) {
			size_t used;
			struct FrontendOutput features = FrontendProcessSamples(&frontend, samples, left, &used);

			samples += used;
			left -= used;
			consumed += used;
			if (!features.size)
				continue;
			for (int i = 0; i < kFeatures; i++)
				slices[slice * kFeatures + i] = static_cast<int8_t>(
					std::min((features.values[i] * 256 + 333) / 666 - 128, 127));
			bool inferred = false;

			if (++slice == stride) {
				slice = 0;
				if (interpreter.Invoke() != kTfLiteOk) {
					fprintf(stderr, "lidar-wakeword: inference failed\n");
					return 1;
				}
				latest = output->data.uint8[0];
				recent[recent_at] = latest;
				recent_at = (recent_at + 1) % window;
				inferred = true;
				if (verbose)
					fprintf(stderr, "%.2f %.3f\n", static_cast<double>(consumed) / kRate,
						latest / 255.0);
			}
			if (latest < cutoff && quiet)
				quiet--;
			if (!inferred || quiet)
				continue;

			int sum = 0;

			for (uint8_t p : recent)
				sum += p;
			if (sum <= cutoff * window)
				continue;
			printf("wake %s %.3f %.2f\n", name.c_str(), sum / (255.0 * window), uptime());
			std::fill(recent.begin(), recent.end(), 0);
			latest = 0;
			quiet = kQuietSlices;
		}
		/* half a sample: keep it for the next read */
		if (have % sizeof *pcm)
			memcpy(pcm, reinterpret_cast<char *>(pcm) + have - 1, 1);
		have %= sizeof *pcm;
	}
	return 0;
}
