#include "audio_decoder.h"

#include <algorithm>
#include <cmath>

extern "C" {
#include <libavutil/intreadwrite.h>
}

namespace me {
namespace {

std::string averror(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof buf);
    return buf;
}

// Reads ahead of the decoded window by less than this decode forward instead of seeking.
constexpr double kMaxForwardDecodeSeconds = 5.0;
// How much earlier each retry seeks when the container lands after the wanted sample;
// the last attempt restarts from the beginning of the stream.
constexpr double kSeekBackoffSeconds[] = {0.0, 1.0, 5.0};
constexpr int kSeekAttempts = 3;

}  // namespace

std::unique_ptr<AudioDecoder> AudioDecoder::open(const std::string& path, double originSeconds, std::string& error) {
    std::unique_ptr<AudioDecoder> d(new AudioDecoder);
    d->path_ = path;
    d->origin_ = originSeconds;
    d->packet_ = av_packet_alloc();
    d->frame_ = av_frame_alloc();
    if (!d->packet_ || !d->frame_) {
        error = "out of memory";
        return nullptr;
    }
    if (int r = avformat_open_input(&d->format_, path.c_str(), nullptr, nullptr); r < 0) {
        error = "cannot open file: " + averror(r);
        return nullptr;
    }
    if (int r = avformat_find_stream_info(d->format_, nullptr); r < 0) {
        error = "cannot read stream info: " + averror(r);
        return nullptr;
    }
    const AVCodec* decoder = nullptr;
    d->stream_ = av_find_best_stream(d->format_, AVMEDIA_TYPE_AUDIO, -1, -1, &decoder, 0);
    if (d->stream_ == AVERROR_STREAM_NOT_FOUND) return nullptr;
    if (d->stream_ < 0 || !decoder) {
        error = "no decoder for the audio stream";
        return nullptr;
    }
    for (unsigned i = 0; i < d->format_->nb_streams; ++i)
        if (int(i) != d->stream_) d->format_->streams[i]->discard = AVDISCARD_ALL;

    const AVStream* st = d->format_->streams[d->stream_];
    d->codec_ = avcodec_alloc_context3(decoder);
    if (!d->codec_) {
        error = "out of memory";
        return nullptr;
    }
    avcodec_parameters_to_context(d->codec_, st->codecpar);
    d->codec_->pkt_timebase = st->time_base;  // lets libavcodec trim encoder delay exactly
    if (int r = avcodec_open2(d->codec_, decoder, nullptr); r < 0) {
        error = std::string("cannot open audio decoder: ") + averror(r);
        return nullptr;
    }
    if (d->codec_->sample_rate <= 0 || d->codec_->ch_layout.nb_channels <= 0) {
        error = "audio stream has no sample rate or channels";
        return nullptr;
    }
    d->buffer_.resize(size_t(d->channels()));

    double endSeconds = 0;
    if (st->duration != AV_NOPTS_VALUE && st->start_time != AV_NOPTS_VALUE)
        endSeconds = (st->start_time + st->duration) * av_q2d(st->time_base);
    else if (d->format_->duration != AV_NOPTS_VALUE)
        endSeconds = double(std::max<int64_t>(d->format_->start_time, 0) + d->format_->duration) / AV_TIME_BASE;
    d->sampleCount_ = std::max<int64_t>(std::llround((endSeconds - originSeconds) * d->sampleRate()), 0);

    // Decoders trim the encoder delay from the first packet they decode, so that packet must
    // be the stream's first; this also leaves the decoder ready for a sequential read.
    if (d->decodeNext() == AVERROR_EOF) d->eof_ = true;
    d->atStreamStart_ = d->positioned_;
    return d;
}

AudioDecoder::~AudioDecoder() {
    swr_free(&resampler_);
    avcodec_free_context(&codec_);
    avformat_close_input(&format_);
    av_packet_free(&packet_);
    av_frame_free(&frame_);
}

bool AudioDecoder::read(int64_t start, int64_t count, float* const* out, std::string& error) {
    for (int c = 0; c < channels(); ++c) std::fill(out[c], out[c] + count, 0.0f);
    if (count <= 0) return true;
    const int64_t end = start + count;
    auto bufferEnd = [&] { return bufferStart_ + int64_t(buffer_[0].size()); };

    // Before the window is fine when it starts at the stream's first sample: that's silence.
    if (positioned_ && (start >= bufferStart_ || atStreamStart_) &&
        start <= bufferEnd() + int64_t(kMaxForwardDecodeSeconds * sampleRate())) {
        // Sequential read: drop what has been consumed and keep decoding forward.
        const auto consumed = size_t(std::max<int64_t>(std::min(start, bufferEnd()) - bufferStart_, 0));
        for (auto& ch : buffer_) ch.erase(ch.begin(), ch.begin() + ptrdiff_t(consumed));
        bufferStart_ += int64_t(consumed);
        atStreamStart_ = atStreamStart_ && consumed == 0;
    } else {
        buildSampleIndex();
        seek(start, 0);
    }

    for (int attempt = 0; !(positioned_ && bufferEnd() >= end) && !eof_;) {
        int r = decodeNext();
        if (r == AVERROR_EOF) {
            eof_ = true;
        } else if (r < 0 && r != AVERROR_INVALIDDATA) {
            error = "audio decode failed: " + averror(r);
            return false;
        } else if (positioned_ && bufferStart_ > start && attempt + 1 < kSeekAttempts) {
            seek(start, ++attempt);  // the container landed after the wanted sample
        }
    }

    const int64_t from = std::max(start, bufferStart_);
    const int64_t to = std::min(end, bufferEnd());
    for (int c = 0; c < channels() && to > from; ++c)
        std::copy_n(buffer_[size_t(c)].begin() + (from - bufferStart_), to - from, out[c] + (from - start));
    return true;
}

void AudioDecoder::seek(int64_t sample, int attempt) {
    const AVStream* st = format_->streams[stream_];
    if (attempt + 1 >= kSeekAttempts) {
        int64_t first = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
        avformat_seek_file(format_, stream_, INT64_MIN, first, first, 0);
    } else {
        // Opus needs ~0.3 s of pre-roll to converge (80 ms is audibly off on noisy material).
        const double preroll = std::max(double(codec_->seek_preroll) / sampleRate(), 0.3);
        const double seconds = origin_ + double(sample) / sampleRate() - preroll - kSeekBackoffSeconds[attempt];
        av_seek_frame(format_, stream_, int64_t(std::floor(seconds / av_q2d(st->time_base))), AVSEEK_FLAG_BACKWARD);
    }
    avcodec_flush_buffers(codec_);
    for (auto& ch : buffer_) ch.clear();
    positioned_ = false;
    atStreamStart_ = false;
    eof_ = false;
}

int64_t AudioDecoder::samplePosition(int64_t ts) {
    if (ts == AV_NOPTS_VALUE) return 0;
    if (!sampleIndex_.empty()) {
        auto it = std::lower_bound(sampleIndex_.begin(), sampleIndex_.end(), ts,
                                   [](const PacketStart& p, int64_t t) { return p.ts < t; });
        if (it != sampleIndex_.end() && it->ts == ts) return it->sample;
    }
    return std::llround((ts * av_q2d(format_->streams[stream_]->time_base) - origin_) * sampleRate());
}

void AudioDecoder::buildSampleIndex() {
    if (sampleIndexBuilt_) return;
    sampleIndexBuilt_ = true;
    const AVStream* st = format_->streams[stream_];
    if (int64_t(st->time_base.num) * sampleRate() <= st->time_base.den) return;  // already sample-exact

    // A fresh demuxer: seeking back to the start would let Matroska skip the blocks before
    // the first cue, including the one carrying the encoder delay.
    AVFormatContext* scan = nullptr;
    if (avformat_open_input(&scan, path_.c_str(), nullptr, nullptr) < 0) return;
    for (unsigned i = 0; i < scan->nb_streams; ++i)
        if (int(i) != stream_) scan->streams[i]->discard = AVDISCARD_ALL;
    // Parsers know each packet's true duration (e.g. from the Opus TOC byte).
    AVCodecParserContext* parser = av_parser_init(st->codecpar->codec_id);
    const int64_t first = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
    // The first output sample sits at the stream's start time; the first packet also
    // carries the encoder delay that the decoder trims.
    int64_t sample = std::llround((first * av_q2d(st->time_base) - origin_) * sampleRate());
    bool firstPacket = true;
    while (av_read_frame(scan, packet_) >= 0) {
        if (packet_->stream_index == stream_ && packet_->pts != AV_NOPTS_VALUE) {
            int64_t duration = 0;
            if (parser) {
                uint8_t* data = nullptr;
                int size = 0;
                av_parser_parse2(parser, codec_, &data, &size, packet_->data, packet_->size, packet_->pts,
                                 packet_->dts, packet_->pos);
                duration = parser->duration;
            }
            if (duration <= 0)
                duration = av_rescale_q(packet_->duration, st->time_base, AVRational{1, sampleRate()});
            if (firstPacket) {
                size_t skipSize = 0;
                const uint8_t* skip = av_packet_get_side_data(packet_, AV_PKT_DATA_SKIP_SAMPLES, &skipSize);
                sample -= skip && skipSize >= 4 ? int64_t(AV_RL32(skip)) : 0;
                firstPacket = false;
            }
            sampleIndex_.push_back({packet_->pts, sample});
            sample += duration;
        }
        av_packet_unref(packet_);
    }
    av_parser_close(parser);
    avformat_close_input(&scan);
    std::sort(sampleIndex_.begin(), sampleIndex_.end(),
              [](const PacketStart& a, const PacketStart& b) { return a.ts < b.ts; });
}

int AudioDecoder::decodeNext() {
    for (bool draining = false;;) {
        int r = avcodec_receive_frame(codec_, frame_);
        if (r == AVERROR(EAGAIN) && !draining) {
            if (av_read_frame(format_, packet_) < 0) {
                draining = true;
                avcodec_send_packet(codec_, nullptr);
            } else {
                if (packet_->stream_index == stream_) r = avcodec_send_packet(codec_, packet_);
                av_packet_unref(packet_);
                if (r < 0 && r != AVERROR(EAGAIN)) return r;
            }
            continue;
        }
        if (r == AVERROR(EAGAIN)) return AVERROR_EOF;
        if (r < 0) return r;

        const int samples = frame_->nb_samples;
        const bool direct = frame_->format == AV_SAMPLE_FMT_FLTP && frame_->sample_rate == sampleRate() &&
                            av_channel_layout_compare(&frame_->ch_layout, &codec_->ch_layout) == 0;
        if (!positioned_) {
            bufferStart_ = samplePosition(frame_->best_effort_timestamp);
            positioned_ = true;
        }
        if (direct) {
            for (int c = 0; c < channels(); ++c) {
                const auto* src = reinterpret_cast<const float*>(frame_->extended_data[c]);
                buffer_[size_t(c)].insert(buffer_[size_t(c)].end(), src, src + samples);
            }
        } else {
            // Any other sample format, layout or rate is converted to this stream's own,
            // configured from the first such frame.
            if (!resampler_ &&
                swr_alloc_set_opts2(&resampler_, &codec_->ch_layout, AV_SAMPLE_FMT_FLTP, sampleRate(),
                                    &frame_->ch_layout, AVSampleFormat(frame_->format), frame_->sample_rate, 0,
                                    nullptr) < 0) {
                av_frame_unref(frame_);
                return AVERROR(ENOMEM);
            }
            if (!swr_is_initialized(resampler_) && swr_init(resampler_) < 0) {
                av_frame_unref(frame_);
                return AVERROR(EINVAL);
            }
            const int capacity = swr_get_out_samples(resampler_, samples);
            std::vector<uint8_t*> dst(static_cast<size_t>(channels()));
            for (int c = 0; c < channels(); ++c) {
                auto& ch = buffer_[size_t(c)];
                ch.resize(ch.size() + size_t(capacity));
                dst[size_t(c)] = reinterpret_cast<uint8_t*>(ch.data() + ch.size() - capacity);
            }
            const int got = swr_convert(resampler_, dst.data(), capacity,
                                        const_cast<const uint8_t**>(frame_->extended_data), samples);
            for (auto& ch : buffer_) ch.resize(ch.size() - size_t(capacity - std::max(got, 0)));
        }
        av_frame_unref(frame_);
        return 0;
    }
}

}  // namespace me
