// Sample-accurate audio decoding (Opus, Vorbis, AAC, ...) to planar float, aligned to the
// video's frame 0 so picture and sound stay in sync.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
}

namespace me {

// Not thread-safe: callers serialize access per instance.
class AudioDecoder {
public:
    // Returns nullptr with an empty `error` when the file has no audio stream.
    // Sample 0 is the instant `originSeconds` on the container clock.
    static std::unique_ptr<AudioDecoder> open(const std::string& path, double originSeconds, std::string& error);
    ~AudioDecoder();

    int sampleRate() const { return codec_->sample_rate; }
    int channels() const { return codec_->ch_layout.nb_channels; }
    int64_t sampleCount() const { return sampleCount_; }

    // Writes `count` samples per channel starting at sample `start` into out[0..channels).
    // Positions without audio (before the stream starts, past its end) are silence.
    bool read(int64_t start, int64_t count, float* const* out, std::string& error);

private:
    AudioDecoder() = default;
    void seek(int64_t sample, int attempt);
    int decodeNext();  // appends to buffer_; 0, AVERROR_EOF or an error
    int64_t samplePosition(int64_t ts);
    void buildSampleIndex();

    struct PacketStart {
        int64_t ts;      // packet timestamp in stream time base
        int64_t sample;  // exact position of its first decoded sample
    };

    AVFormatContext* format_ = nullptr;
    AVCodecContext* codec_ = nullptr;
    SwrContext* resampler_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;
    std::string path_;
    int stream_ = -1;
    double origin_ = 0;
    int64_t sampleCount_ = 0;

    std::vector<std::vector<float>> buffer_;  // decoded samples per channel
    int64_t bufferStart_ = 0;                 // sample position of buffer_[c][0]
    bool positioned_ = false;                 // bufferStart_ is known
    bool atStreamStart_ = false;              // buffer_ begins with the stream's first sample
    bool eof_ = false;

    // Timestamps coarser than one sample (Matroska/WebM count milliseconds) can't place a
    // seek exactly; this table, built from the true packet durations on the first seek, can.
    std::vector<PacketStart> sampleIndex_;
    bool sampleIndexBuilt_ = false;
};

}  // namespace me
