#include "video_decoder.h"

#include <algorithm>
#include <cmath>
#include <list>
#include <map>
#include <mutex>
#include <utility>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
}

namespace me {
namespace {

std::string averror(int code) {
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buf, sizeof buf);
    return buf;
}

// Refusing every software format makes a failed VideoToolbox setup surface as a decode
// error, which triggers the software fallback instead of silently decoding on the CPU.
AVPixelFormat requireVideoToolbox(AVCodecContext*, const AVPixelFormat* formats) {
    for (const AVPixelFormat* f = formats; *f != AV_PIX_FMT_NONE; ++f)
        if (*f == AV_PIX_FMT_VIDEOTOOLBOX) return *f;
    return AV_PIX_FMT_NONE;
}

// Containers often store rounded rates (2997/100); editors expect the exact NTSC ones.
AVRational snapFrameRate(AVRational rate) {
    static const AVRational common[] = {{24000, 1001}, {24, 1}, {25, 1}, {30000, 1001}, {30, 1},
                                        {48, 1}, {50, 1}, {60000, 1001}, {60, 1}, {120, 1}};
    double value = av_q2d(rate);
    for (AVRational c : common)
        if (std::fabs(value - av_q2d(c)) < 0.01) return c;
    av_reduce(&rate.num, &rate.den, rate.num, rate.den, 1 << 30);
    return rate;
}

bool validRate(AVRational r) { return r.num > 0 && r.den > 0 && av_q2d(r) < 1000; }

// The pixel layout of a picture's data, seen through a hardware frame's wrapper.
AVPixelFormat pixelFormatOf(const AVFrame* frame) {
    if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX && frame->hw_frames_ctx)
        return reinterpret_cast<const AVHWFramesContext*>(frame->hw_frames_ctx->data)->sw_format;
    return AVPixelFormat(frame->format);
}

size_t pictureBytes(const AVFrame* frame) {
    return size_t(av_image_get_buffer_size(pixelFormatOf(frame), frame->width, frame->height, 1));
}

// Sequential decoding beats seeking for short hops even across a keyframe.
constexpr int64_t kShortHop = 8;
constexpr int kSeekRetries = 3;

// Decoded pictures shared by every open clip, least recently used evicted first. Reaching a
// picture in a long GOP means decoding the GOP from its keyframe; keeping those pictures
// makes stepping backwards, reverse playback and scrubbing within the GOP free.
class FrameCache {
public:
    static FrameCache& shared() {
        static FrameCache cache;
        return cache;
    }

    // Puts a new reference to the cached picture in `out`.
    bool get(const void* owner, int64_t index, AVFrame* out) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find({owner, index});
        if (it == entries_.end()) {
            ++misses_;
            return false;
        }
        ++hits_;
        recent_.splice(recent_.begin(), recent_, it->second);
        av_frame_unref(out);
        return av_frame_ref(out, it->second->frame) == 0;
    }

    void put(const void* owner, int64_t index, const AVFrame* frame, size_t bytes) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (entries_.count({owner, index})) return;
        AVFrame* copy = av_frame_clone(frame);  // a reference, not a pixel copy
        if (!copy) return;
        recent_.push_front({owner, index, copy, bytes});
        entries_[{owner, index}] = recent_.begin();
        bytes_ += bytes;
        while (bytes_ > kBudgetBytes && !recent_.empty()) erase(std::prev(recent_.end()));
    }

    FrameCacheUsage usage() {
        std::lock_guard<std::mutex> lock(mutex_);
        return {bytes_, kBudgetBytes, recent_.size(), hits_, misses_};
    }

    void purge(const void* owner) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = recent_.begin(); it != recent_.end();) {
            auto next = std::next(it);
            if (it->owner == owner) erase(it);
            it = next;
        }
    }

private:
    struct Entry {
        const void* owner;
        int64_t index;
        AVFrame* frame;
        size_t bytes;
    };
    using Iterator = std::list<Entry>::iterator;

    void erase(Iterator it) {
        bytes_ -= it->bytes;
        entries_.erase({it->owner, it->index});
        av_frame_free(&it->frame);
        recent_.erase(it);
    }

    static constexpr size_t kBudgetBytes = size_t(1) << 30;  // ~80 4K or ~330 1080p pictures
    std::mutex mutex_;
    std::list<Entry> recent_;  // most recently used first
    std::map<std::pair<const void*, int64_t>, Iterator> entries_;
    size_t bytes_ = 0;
    int64_t hits_ = 0;
    int64_t misses_ = 0;
};

}  // namespace

FrameCacheUsage frameCacheUsage() { return FrameCache::shared().usage(); }

bool VideoDecoder::openFile(const std::string& path, std::string& error) {
    packet_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    next_ = av_frame_alloc();
    cached_ = av_frame_alloc();
    if (!packet_ || !frame_ || !next_ || !cached_) {
        error = "out of memory";
        return false;
    }
    if (int r = avformat_open_input(&format_, path.c_str(), nullptr, nullptr); r < 0) {
        error = "cannot open file: " + averror(r);
        return false;
    }
    if (int r = avformat_find_stream_info(format_, nullptr); r < 0) {
        error = "cannot read stream info: " + averror(r);
        return false;
    }
    stream_ = av_find_best_stream(format_, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (stream_ < 0) {
        error = "no video stream";
        return false;
    }
    const AVCodecParameters* par = format_->streams[stream_]->codecpar;
    if (par->codec_id != AV_CODEC_ID_AV1 && par->codec_id != AV_CODEC_ID_VP9) {
        error = std::string("video is ") + avcodec_get_name(par->codec_id) + ", not AV1 or VP9";
        return false;
    }
    for (unsigned i = 0; i < format_->nb_streams; ++i)
        if (int(i) != stream_) format_->streams[i]->discard = AVDISCARD_ALL;
    return true;
}

std::unique_ptr<VideoDecoder> VideoDecoder::open(const std::string& path, std::string& error) {
    std::unique_ptr<VideoDecoder> d(new VideoDecoder);
    if (!d->openFile(path, error) || !d->buildIndex(error)) return nullptr;

    AVStream* st = d->format_->streams[d->stream_];
    const AVCodecParameters* par = st->codecpar;
    VideoInfo& info = d->info_;
    info.codec = par->codec_id == AV_CODEC_ID_AV1 ? "av1" : "vp9";
    info.width = par->width;
    info.height = par->height;
    AVRational sar = av_guess_sample_aspect_ratio(d->format_, st, nullptr);
    if (sar.num > 0 && sar.den > 0) info.sampleAspect = sar;
    info.primaries = par->color_primaries;
    info.transfer = par->color_trc;
    info.matrix = par->color_space;
    info.fullRange = par->color_range == AVCOL_RANGE_JPEG;

    const double tb = av_q2d(st->time_base);
    const double span = (d->timestamps_.back() - d->timestamps_.front()) * tb;
    AVRational rate = validRate(st->avg_frame_rate) ? st->avg_frame_rate : st->r_frame_rate;
    if (!validRate(rate)) {
        rate = d->timestamps_.size() > 1 && span > 0
                   ? av_d2q((d->timestamps_.size() - 1) / span, 1 << 20)
                   : AVRational{30, 1};
    }
    info.frameRate = snapFrameRate(rate);
    info.frameCount = std::llround(span * av_q2d(info.frameRate)) + 1;
    info.startSeconds = d->timestamps_.front() * tb;

    // Decoding frame 0 settles the backend: a stream the media engine rejects falls back
    // to software inside decodeIndex.
    std::string hwError;
    if (!d->openCodec(Backend::Hardware, hwError) && !d->openCodec(Backend::Software, error)) return nullptr;
    const AVFrame* first = d->decodeIndex(0, error);
    if (!first) return nullptr;
    // Container probing can miss the pixel format (YouTube section downloads do); the
    // decoded picture is authoritative.
    if (const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(pixelFormatOf(first))) info.bitDepth = desc->comp[0].depth;
    return d;
}

std::unique_ptr<VideoDecoder> VideoDecoder::sibling(std::string& error) const {
    std::unique_ptr<VideoDecoder> d(new VideoDecoder);
    if (!d->openFile(format_->url, error)) return nullptr;
    d->info_ = info_;
    d->timestamps_ = timestamps_;
    d->keyframes_ = keyframes_;
    d->hidden_ = hidden_;
    if (!d->openCodec(backend_, error)) return nullptr;
    return d;
}

VideoDecoder::~VideoDecoder() {
    FrameCache::shared().purge(this);
    av_frame_free(&cached_);
    avcodec_free_context(&codec_);
    av_buffer_unref(&hwDevice_);
    avformat_close_input(&format_);
    av_packet_free(&packet_);
    av_frame_free(&frame_);
    av_frame_free(&next_);
}

// One demux pass over the file gives exact frame count, timestamps and keyframe positions,
// which makes every later seek frame-accurate regardless of how good the container index is.
bool VideoDecoder::buildIndex(std::string& error) {
    struct Packet {
        int64_t ts;
        bool key;
        bool hidden;  // decoded for reference but never shown (an MP4 edit list trims it)
    };
    std::vector<Packet> packets;
    while (av_read_frame(format_, packet_) >= 0) {
        if (packet_->stream_index == stream_) {
            int64_t ts = packet_->pts != AV_NOPTS_VALUE ? packet_->pts : packet_->dts;
            if (ts != AV_NOPTS_VALUE)
                packets.push_back({ts, (packet_->flags & AV_PKT_FLAG_KEY) != 0,
                                   (packet_->flags & AV_PKT_FLAG_DISCARD) != 0});
        }
        av_packet_unref(packet_);
    }
    std::sort(packets.begin(), packets.end(), [](const Packet& a, const Packet& b) { return a.ts < b.ts; });
    // A hidden keyframe still starts the GOP its shown pictures belong to; seeking to the first
    // shown one lands on it, because the demuxer seeks back to the keyframe.
    bool keyPending = false;
    for (const Packet& p : packets) {
        keyPending = keyPending || p.key;
        if (p.hidden) hidden_.push_back(p.ts);
        if (p.hidden || (!timestamps_.empty() && timestamps_.back() == p.ts)) continue;
        if (keyPending) keyframes_.push_back(int64_t(timestamps_.size()));
        keyPending = false;
        timestamps_.push_back(p.ts);
    }
    if (timestamps_.empty()) {
        error = "video stream has no frames";
        return false;
    }
    return true;
}

bool VideoDecoder::openCodec(Backend backend, std::string& error) {
    avcodec_free_context(&codec_);
    const bool av1 = format_->streams[stream_]->codecpar->codec_id == AV_CODEC_ID_AV1;
    // libavcodec's native "av1" decoder exists only as a hardware front end; dav1d is the
    // fastest software AV1 decoder.
    const char* name = backend == Backend::Hardware ? (av1 ? "av1" : "vp9") : (av1 ? "libdav1d" : "vp9");
    const AVCodec* decoder = avcodec_find_decoder_by_name(name);
    if (!decoder) {
        error = std::string("decoder not available: ") + name;
        return false;
    }
    codec_ = avcodec_alloc_context3(decoder);
    if (!codec_) {
        error = "out of memory";
        return false;
    }
    const AVStream* st = format_->streams[stream_];
    avcodec_parameters_to_context(codec_, st->codecpar);
    codec_->pkt_timebase = st->time_base;
    if (backend == Backend::Hardware) {
        if (!hwDevice_) {
            if (int r = av_hwdevice_ctx_create(&hwDevice_, AV_HWDEVICE_TYPE_VIDEOTOOLBOX, nullptr, nullptr, 0); r < 0) {
                error = "VideoToolbox unavailable: " + averror(r);
                return false;
            }
        }
        codec_->hw_device_ctx = av_buffer_ref(hwDevice_);
        codec_->get_format = requireVideoToolbox;
    } else {
        codec_->thread_count = 0;  // one per core
    }
    if (int r = avcodec_open2(codec_, decoder, nullptr); r < 0) {
        error = std::string("cannot open ") + name + ": " + averror(r);
        avcodec_free_context(&codec_);
        return false;
    }
    backend_ = backend;
    current_ = -1;
    packetPending_ = false;
    draining_ = false;
    return true;
}

const AVFrame* VideoDecoder::frameAt(int64_t frame, std::string& error) {
    // The picture on screen at the middle of the frame's display interval: exact for
    // constant-rate clips whose timestamps were rounded by the container, and the
    // natural choice for variable-rate ones.
    const AVRational tb = format_->streams[stream_]->time_base;
    const double seconds = info_.startSeconds + (double(std::max<int64_t>(frame, 0)) + 0.5) / av_q2d(info_.frameRate);
    const int64_t ts = int64_t(std::floor(seconds / av_q2d(tb)));
    auto it = std::upper_bound(timestamps_.begin(), timestamps_.end(), ts);
    const int64_t index = std::max<int64_t>(int64_t(it - timestamps_.begin()) - 1, 0);
    return decodeIndex(index, error);
}

const AVFrame* VideoDecoder::decodeIndex(int64_t index, std::string& error) {
    if (index == current_) return frame_;
    if (FrameCache::shared().get(this, index, cached_)) return cached_;

    auto keyframeBefore = [&](int64_t i) {
        auto it = std::upper_bound(keyframes_.begin(), keyframes_.end(), i);
        return it == keyframes_.begin() ? int64_t(0) : *(it - 1);
    };
    const bool continueForward =
        current_ >= 0 && index > current_ && (keyframeBefore(index) <= current_ || index - current_ <= kShortHop);
    int retries = 0;
    if (!continueForward) seekToKeyframeBefore(index, 0);

    for (;;) {
        int r = decodeNext();
        if (r == 0) {
            if (current_ == index) return frame_;
            if (current_ < index) continue;
            // Overshot: the container seek landed past the keyframe, or the target picture
            // is undecodable. Start further back, then settle for the nearest picture.
            if (retries < kSeekRetries) {
                seekToKeyframeBefore(index, ++retries);
                continue;
            }
            return frame_;
        }
        if (r == AVERROR_EOF) {
            if (current_ >= 0) return frame_;  // index past the last decodable picture
            error = "no decodable frame at index " + std::to_string(index);
            return nullptr;
        }
        if (backend_ == Backend::Hardware) {
            // The media engine rejected this stream mid-way (e.g. an unsupported profile
            // switch). Software decoding is the authority on what the file contains.
            if (!openCodec(Backend::Software, error)) return nullptr;
            seekToKeyframeBefore(index, 0);
            continue;
        }
        error = "decode failed: " + averror(r);
        return nullptr;
    }
}

void VideoDecoder::seekToKeyframeBefore(int64_t index, int stepsBack) {
    auto it = std::upper_bound(keyframes_.begin(), keyframes_.end(), index);
    int64_t pos = int64_t(it - keyframes_.begin()) - 1 - stepsBack;
    const int64_t picture = stepsBack >= kSeekRetries || pos < 0 ? 0 : keyframes_[size_t(pos)];
    const int64_t ts = timestamps_[size_t(picture)];
    if (av_seek_frame(format_, stream_, ts, AVSEEK_FLAG_BACKWARD) < 0)
        avformat_seek_file(format_, stream_, INT64_MIN, ts, ts, 0);
    avcodec_flush_buffers(codec_);
    av_frame_unref(frame_);
    current_ = -1;
    packetPending_ = false;
    draining_ = false;
}

int VideoDecoder::decodeNext() {
    for (;;) {
        int r = avcodec_receive_frame(codec_, next_);
        if (r == 0) {
            ++picturesDecoded_;
            int64_t ts = next_->best_effort_timestamp != AV_NOPTS_VALUE ? next_->best_effort_timestamp : next_->pts;
            if (ts != AV_NOPTS_VALUE && std::binary_search(hidden_.begin(), hidden_.end(), ts)) {
                av_frame_unref(next_);
                continue;
            }
            av_frame_unref(frame_);
            av_frame_move_ref(frame_, next_);
            current_ = ts != AV_NOPTS_VALUE ? indexOfTimestamp(ts) : current_ + 1;
            FrameCache::shared().put(this, current_, frame_, pictureBytes(frame_));
            return 0;
        }
        if (r != AVERROR(EAGAIN)) return r;
        if (draining_) return AVERROR_EOF;

        if (!packetPending_) {
            if (av_read_frame(format_, packet_) < 0) {
                draining_ = true;
                avcodec_send_packet(codec_, nullptr);
                continue;
            }
            if (packet_->stream_index != stream_) {
                av_packet_unref(packet_);
                continue;
            }
            // libavcodec drops every picture decoded from a discard-flagged packet, including
            // AV1 alt-refs decoded in an edit list's hidden pre-roll and shown after it.
            // Hidden pictures are filtered by timestamp above instead.
            packet_->flags &= ~AV_PKT_FLAG_DISCARD;
        }
        r = avcodec_send_packet(codec_, packet_);
        packetPending_ = r == AVERROR(EAGAIN);
        if (packetPending_) continue;
        av_packet_unref(packet_);
        if (r == AVERROR_INVALIDDATA && backend_ == Backend::Software) continue;  // skip a corrupt packet
        if (r < 0) return r;
    }
}

int64_t VideoDecoder::indexOfTimestamp(int64_t ts) const {
    auto it = std::upper_bound(timestamps_.begin(), timestamps_.end(), ts);
    return std::max<int64_t>(int64_t(it - timestamps_.begin()) - 1, 0);
}

}  // namespace me
