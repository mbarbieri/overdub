// Throwaway runtime spike: the chunked BS-RoFormer Separation on ONNX Runtime 1.23 (x86_64 macOS).
// Mirrors spikes/stem-separation/onnx/reference.py exactly; see that file for the algorithm.
//
// Usage: separate_spike <model.onnx> <input.f32> <out_dir> [threads]
//   input.f32: raw interleaved stereo float32 at 44.1 kHz (ffmpeg -f f32le -ac 2 -ar 44100).
// Writes <out_dir>/<part>.wav (32-bit float) and prints one line per chunk, then a summary.

#include "pocketfft_hdronly.hpp"

#include <onnxruntime_cxx_api.h>

#include <sys/resource.h>

#include <chrono>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
constexpr int channels = 2;
constexpr int sampleRate = 44100;

struct StftConfig
{
    int nFft = 0;
    int hop = 0;
    int win = 0;
    bool normalized = false;
    int bins() const { return nFft / 2 + 1; }
    int framesFor (int length) const { return 1 + length / hop; }
};

std::vector<float> periodicHann (int length)
{
    std::vector<float> window (length);
    for (int n = 0; n < length; ++n)
        window[n] = static_cast<float> (0.5 - 0.5 * std::cos (2.0 * M_PI * n / length));
    return window;
}

std::vector<float> triangularWeight (int length)
{
    const int half = length / 2;
    std::vector<double> weight;
    weight.reserve (length);
    for (int i = 1; i <= half; ++i)
        weight.push_back (i);
    for (int i = length - half; i > 0; --i)
        weight.push_back (i);
    double maximum = 0;
    for (const auto w : weight)
        maximum = std::max (maximum, w);
    std::vector<float> result (length);
    for (int i = 0; i < length; ++i)
        result[i] = static_cast<float> (std::max (weight[i] / maximum, 1e-20));
    return result;
}

// torch.stft (center=True, pad_mode="reflect", onesided) of one channel of `length` samples.
// Writes real/imag planes laid out [bin][frame].
void stft (const float* signal, int length, const StftConfig& config, const std::vector<float>& window, float* real, float* imag)
{
    const int pad = config.nFft / 2;
    std::vector<float> padded (length + 2 * pad);
    for (int i = 0; i < length + 2 * pad; ++i)
    {
        int source = i - pad;
        if (source < 0)
            source = -source;
        if (source >= length)
            source = 2 * (length - 1) - source;
        padded[i] = signal[source];
    }

    const int frames = config.framesFor (length);
    const int bins = config.bins();
    const float scale = config.normalized ? 1.0f / std::sqrt (static_cast<float> (config.nFft)) : 1.0f;
    std::vector<float> frame (config.nFft);
    std::vector<std::complex<float>> spectrum (bins);
    for (int t = 0; t < frames; ++t)
    {
        for (int n = 0; n < config.nFft; ++n)
            frame[n] = padded[t * config.hop + n] * window[n];
        pocketfft::r2c<float> ({ static_cast<size_t> (config.nFft) }, { sizeof (float) }, { sizeof (std::complex<float>) }, 0, pocketfft::FORWARD, frame.data(), spectrum.data(), scale);
        for (int k = 0; k < bins; ++k)
        {
            real[k * frames + t] = spectrum[k].real();
            imag[k * frames + t] = spectrum[k].imag();
        }
    }
}

// torch.istft (center=True, length=`length`) of [bin][frame] planes into `out`.
void istft (const float* real, const float* imag, int length, const StftConfig& config, const std::vector<float>& window, float* out)
{
    const int frames = config.framesFor (length);
    const int bins = config.bins();
    const int total = config.nFft + config.hop * (frames - 1);
    std::vector<float> accumulated (total, 0.0f);
    std::vector<float> envelope (total, 0.0f);
    std::vector<std::complex<float>> spectrum (bins);
    std::vector<float> frame (config.nFft);
    const float scale = config.normalized ? 1.0f / std::sqrt (static_cast<float> (config.nFft)) : 1.0f / config.nFft;
    for (int t = 0; t < frames; ++t)
    {
        for (int k = 0; k < bins; ++k)
            spectrum[k] = { real[k * frames + t], imag[k * frames + t] };
        pocketfft::c2r<float> ({ static_cast<size_t> (config.nFft) }, { sizeof (std::complex<float>) }, { sizeof (float) }, 0, pocketfft::BACKWARD, spectrum.data(), frame.data(), scale);
        for (int n = 0; n < config.nFft; ++n)
        {
            accumulated[t * config.hop + n] += frame[n] * window[n];
            envelope[t * config.hop + n] += window[n] * window[n];
        }
    }
    const int pad = config.nFft / 2;
    for (int i = 0; i < length; ++i)
        out[i] = accumulated[pad + i] / envelope[pad + i];
}

std::vector<float> readInterleaved (const std::string& path)
{
    std::ifstream file (path, std::ios::binary | std::ios::ate);
    if (! file)
        throw std::runtime_error ("cannot open " + path);
    const auto bytes = static_cast<size_t> (file.tellg());
    std::vector<float> samples (bytes / sizeof (float));
    file.seekg (0);
    file.read (reinterpret_cast<char*> (samples.data()), static_cast<std::streamsize> (samples.size() * sizeof (float)));
    return samples;
}

void writeFloatWav (const std::string& path, const std::vector<float>& left, const std::vector<float>& right)
{
    std::ofstream file (path, std::ios::binary);
    const auto put32 = [&] (uint32_t v) { file.write (reinterpret_cast<const char*> (&v), 4); };
    const auto put16 = [&] (uint16_t v) { file.write (reinterpret_cast<const char*> (&v), 2); };
    const auto dataBytes = static_cast<uint32_t> (left.size() * channels * sizeof (float));
    file.write ("RIFF", 4);
    put32 (36 + dataBytes);
    file.write ("WAVEfmt ", 8);
    put32 (16);
    put16 (3); // IEEE float
    put16 (channels);
    put32 (sampleRate);
    put32 (sampleRate * channels * sizeof (float));
    put16 (channels * sizeof (float));
    put16 (32);
    file.write ("data", 4);
    put32 (dataBytes);
    for (size_t i = 0; i < left.size(); ++i)
    {
        file.write (reinterpret_cast<const char*> (&left[i]), 4);
        file.write (reinterpret_cast<const char*> (&right[i]), 4);
    }
}

std::vector<std::string> parseSources (const std::string& json)
{
    std::vector<std::string> names;
    size_t position = 0;
    while ((position = json.find ('"', position)) != std::string::npos)
    {
        const auto end = json.find ('"', position + 1);
        names.push_back (json.substr (position + 1, end - position - 1));
        position = end + 1;
    }
    return names;
}

long peakRssMb()
{
    rusage usage {};
    getrusage (RUSAGE_SELF, &usage);
    return usage.ru_maxrss / (1024 * 1024); // bytes on macOS
}
} // namespace

int main (int argc, char** argv)
{
    if (argc < 4)
    {
        std::fprintf (stderr, "usage: separate_spike <model.onnx> <input.f32> <out_dir> [threads]\n");
        return 2;
    }
    const std::string modelPath = argv[1];
    const std::string outDir = argv[3];
    const int threads = argc > 4 ? std::stoi (argv[4]) : static_cast<int> (std::thread::hardware_concurrency());
    const auto started = std::chrono::steady_clock::now();

    Ort::Env env (ORT_LOGGING_LEVEL_WARNING, "separate_spike");
    Ort::SessionOptions options;
    options.SetIntraOpNumThreads (threads);
    options.SetGraphOptimizationLevel (GraphOptimizationLevel::ORT_ENABLE_ALL);
    Ort::Session session (env, modelPath.c_str(), options);

    Ort::AllocatorWithDefaultOptions allocator;
    const auto metadata = session.GetModelMetadata();
    const auto meta = [&] (const char* key) { return std::string (metadata.LookupCustomMetadataMapAllocated (key, allocator).get()); };
    const auto sources = parseSources (meta ("sources"));
    const int segment = std::stoi (meta ("segment_samples"));
    StftConfig config { std::stoi (meta ("stft_n_fft")), std::stoi (meta ("stft_hop_length")), std::stoi (meta ("stft_win_length")), meta ("stft_normalized") == "true" };
    if (config.win != config.nFft)
        throw std::runtime_error ("spike assumes win_length == n_fft");

    const auto interleaved = readInterleaved (argv[2]);
    const int length = static_cast<int> (interleaved.size() / channels);
    std::vector<std::vector<float>> input (channels, std::vector<float> (length));
    for (int i = 0; i < length; ++i)
        for (int c = 0; c < channels; ++c)
            input[c][i] = interleaved[static_cast<size_t> (i) * channels + c];

    const auto window = periodicHann (config.win);
    const auto weight = triangularWeight (segment);
    const int stride = static_cast<int> (0.75 * segment);
    const int bins = config.bins();
    const int frames = config.framesFor (segment);
    const size_t planeSize = static_cast<size_t> (bins) * frames;
    const int parts = static_cast<int> (sources.size());

    std::vector<std::vector<std::vector<float>>> out (parts, std::vector<std::vector<float>> (channels, std::vector<float> (length, 0.0f)));
    std::vector<float> weightSum (length, 0.0f);
    std::vector<float> specReal (channels * planeSize), specImag (channels * planeSize);
    std::vector<float> chunk (segment), partAudio (segment);
    const std::array<int64_t, 4> inputShape { 1, channels, bins, frames };
    const auto memory = Ort::MemoryInfo::CreateCpu (OrtArenaAllocator, OrtMemTypeDefault);
    const char* inputNames[] = { "spec_real", "spec_imag" };
    const char* outputNames[] = { "out_spec_real", "out_spec_imag" };

    const int chunks = (length + stride - 1) / stride;
    for (int index = 0, offset = 0; offset < length; ++index, offset += stride)
    {
        const auto chunkStarted = std::chrono::steady_clock::now();
        const int count = std::min (segment, length - offset);
        for (int c = 0; c < channels; ++c)
        {
            std::fill (chunk.begin(), chunk.end(), 0.0f);
            std::copy_n (input[c].begin() + offset, count, chunk.begin());
            stft (chunk.data(), segment, config, window, specReal.data() + c * planeSize, specImag.data() + c * planeSize);
        }

        std::array<Ort::Value, 2> inputs { Ort::Value::CreateTensor<float> (memory, specReal.data(), specReal.size(), inputShape.data(), inputShape.size()),
                                           Ort::Value::CreateTensor<float> (memory, specImag.data(), specImag.size(), inputShape.data(), inputShape.size()) };
        auto outputs = session.Run (Ort::RunOptions { nullptr }, inputNames, inputs.data(), 2, outputNames, 2);
        const float* outReal = outputs[0].GetTensorData<float>();
        const float* outImag = outputs[1].GetTensorData<float>();

        for (int p = 0; p < parts; ++p)
            for (int c = 0; c < channels; ++c)
            {
                const size_t plane = (static_cast<size_t> (p) * channels + c) * planeSize;
                istft (outReal + plane, outImag + plane, segment, config, window, partAudio.data());
                for (int i = 0; i < count; ++i)
                    out[p][c][offset + i] += partAudio[i] * weight[i];
            }
        for (int i = 0; i < count; ++i)
            weightSum[offset + i] += weight[i];

        const auto seconds = std::chrono::duration<double> (std::chrono::steady_clock::now() - chunkStarted).count();
        std::printf ("chunk %d/%d %.1f s\n", index + 1, chunks, seconds);
        std::fflush (stdout);
    }

    for (int p = 0; p < parts; ++p)
    {
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < length; ++i)
                out[p][c][i] /= weightSum[i];
        writeFloatWav (outDir + "/" + sources[p] + ".wav", out[p][0], out[p][1]);
    }

    const auto total = std::chrono::duration<double> (std::chrono::steady_clock::now() - started).count();
    std::printf ("done: %.1f s for %.1f s of audio (%.2fx realtime), %d threads, peak memory %ld MB\n", total, static_cast<double> (length) / sampleRate, total * sampleRate / length, threads, peakRssMb());
    return 0;
}
