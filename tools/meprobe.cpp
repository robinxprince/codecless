// Command-line harness for the decoder core: inspect a file, benchmark it, and hash frames
// so they can be checked against FFmpeg's software decode (`ffmpeg -f framemd5`).
//
//   meprobe info  <file>
//   meprobe bench <file>
//   meprobe md5   <file> <frame>...     (hash of the frame as planar YUV, like framemd5)
//   meprobe audio <file> <out.f32le>    (sequential read in uneven chunks, interleaved)
//   meprobe seekaudio <file>            (random audio reads vs a sequential read)
#include <sys/resource.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

#include "../src/audio_decoder.h"
#include "../src/video_decoder.h"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/md5.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace {

double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

double cpuSeconds() {
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
    return u.ru_utime.tv_sec + u.ru_utime.tv_usec / 1e6 + u.ru_stime.tv_sec + u.ru_stime.tv_usec / 1e6;
}

[[noreturn]] void fail(const std::string& message) {
    std::fprintf(stderr, "error: %s\n", message.c_str());
    std::exit(1);
}

std::unique_ptr<me::VideoDecoder> openVideo(const char* path) {
    std::string error;
    auto video = me::VideoDecoder::open(path, error);
    if (!video) fail(error);
    return video;
}

const char* backendName(const me::VideoDecoder& v) {
    return v.backend() == me::Backend::Hardware ? "media engine (VideoToolbox)" : "software";
}

// Same bytes `ffmpeg -f framemd5` hashes for planar yuv420p / yuv420p10le output.
std::string planarMd5(const AVFrame* frame) {
    AVFrame* sw = av_frame_alloc();
    AVFrame* planar = av_frame_alloc();
    const AVFrame* src = frame;
    if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
        if (av_hwframe_transfer_data(sw, frame, 0) < 0) fail("cannot read hardware frame");
        src = sw;
    }
    const auto* desc = av_pix_fmt_desc_get(AVPixelFormat(src->format));
    const AVPixelFormat target = desc->comp[0].depth > 8 ? AV_PIX_FMT_YUV420P10LE : AV_PIX_FMT_YUV420P;
    planar->format = target;
    planar->width = src->width;
    planar->height = src->height;
    av_frame_get_buffer(planar, 0);
    SwsContext* sws = sws_getContext(src->width, src->height, AVPixelFormat(src->format), src->width, src->height,
                                     target, SWS_POINT | SWS_ACCURATE_RND | SWS_BITEXACT, nullptr, nullptr, nullptr);
    sws_scale(sws, src->data, src->linesize, 0, src->height, planar->data, planar->linesize);
    sws_freeContext(sws);

    const int size = av_image_get_buffer_size(target, planar->width, planar->height, 1);
    std::vector<uint8_t> packed(static_cast<size_t>(size));
    av_image_copy_to_buffer(packed.data(), size, planar->data, planar->linesize, target, planar->width,
                            planar->height, 1);
    uint8_t digest[16];
    av_md5_sum(digest, packed.data(), size);
    char hex[33];
    for (int i = 0; i < 16; ++i) std::snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    av_frame_free(&sw);
    av_frame_free(&planar);
    return hex;
}

void info(const char* path) {
    const double t0 = now();
    auto video = openVideo(path);
    const double opened = now() - t0;
    const me::VideoInfo& v = video->info();
    std::printf("video     %s %dx%d %d-bit, %d/%d fps, %lld frames, SAR %d:%d\n", v.codec.c_str(), v.width,
                v.height, v.bitDepth, v.frameRate.num, v.frameRate.den, (long long)v.frameCount,
                v.sampleAspect.num, v.sampleAspect.den);
    std::printf("color     primaries=%s transfer=%s matrix=%s range=%s\n", av_color_primaries_name(v.primaries),
                av_color_transfer_name(v.transfer), av_color_space_name(v.matrix), v.fullRange ? "full" : "limited");
    std::printf("decoder   %s\n", backendName(*video));
    std::printf("open      %.0f ms (index + first frame)\n", opened * 1000);
    std::string error;
    auto audio = me::AudioDecoder::open(path, v.startSeconds, error);
    if (audio)
        std::printf("audio     %d Hz, %d ch, %lld samples\n", audio->sampleRate(), audio->channels(),
                    (long long)audio->sampleCount());
    else
        std::printf("audio     %s\n", error.empty() ? "none" : error.c_str());
}

void bench(const char* path) {
    auto video = openVideo(path);
    const int64_t frames = video->info().frameCount;
    std::string error;

    double c0 = cpuSeconds(), t0 = now();
    for (int64_t i = 0; i < frames; ++i)
        if (!video->frameAt(i, error)) fail(error);
    double wall = now() - t0, cpu = cpuSeconds() - c0;
    std::printf("decoder     %s\n", backendName(*video));
    std::printf("sequential  %lld frames in %.2f s = %.0f fps, CPU %.2f s (%.0f%% of one core)\n",
                (long long)frames, wall, frames / wall, cpu, 100 * cpu / wall);

    std::mt19937 rng(42);
    std::uniform_int_distribution<int64_t> pick(0, frames - 1);
    const int seeks = 40;
    double worst = 0;
    c0 = cpuSeconds(), t0 = now();
    for (int i = 0; i < seeks; ++i) {
        const double s = now();
        if (!video->frameAt(pick(rng), error)) fail(error);
        worst = std::max(worst, now() - s);
    }
    wall = now() - t0, cpu = cpuSeconds() - c0;
    std::printf("random      %d seeks, avg %.1f ms, worst %.1f ms, CPU %.2f s\n", seeks, wall / seeks * 1000,
                worst * 1000, cpu);

    t0 = now();
    for (int64_t i = frames - 1; i >= std::max<int64_t>(frames - 31, 0); --i)
        if (!video->frameAt(i, error)) fail(error);
    std::printf("reverse     30 frames backwards from the end, avg %.1f ms\n", (now() - t0) / 30 * 1000);
}

void md5(const char* path, int argc, char** argv) {
    // Requests alternate between two decoders of the file, as the importer's playhead and
    // background lanes do.
    auto video = openVideo(path);
    std::string error;
    auto sibling = video->sibling(error);
    if (!sibling) fail(error);
    for (int i = 0; i < argc; ++i) {
        const int64_t frame = std::atoll(argv[i]);
        const AVFrame* f = (i % 2 ? sibling : video)->frameAt(frame, error);
        if (!f) fail(error);
        std::printf("%lld %s\n", (long long)frame, planarMd5(f).c_str());
    }
}

std::unique_ptr<me::AudioDecoder> openAudio(const char* path) {
    auto video = openVideo(path);
    std::string error;
    auto audio = me::AudioDecoder::open(path, video->info().startSeconds, error);
    if (!audio) fail(error.empty() ? "no audio stream" : error);
    return audio;
}

std::vector<std::vector<float>> readAll(me::AudioDecoder& audio, int64_t total) {
    std::vector<std::vector<float>> pcm(size_t(audio.channels()), std::vector<float>(size_t(total)));
    std::mt19937 rng(7);
    std::uniform_int_distribution<int64_t> chunk(100, 9000);
    std::string error;
    for (int64_t pos = 0; pos < total;) {
        const int64_t n = std::min(chunk(rng), total - pos);
        std::vector<float*> out;
        for (auto& ch : pcm) out.push_back(ch.data() + pos);
        if (!audio.read(pos, n, out.data(), error)) fail(error);
        pos += n;
    }
    return pcm;
}

void audioDump(const char* path, const char* outPath) {
    auto audio = openAudio(path);
    const double t0 = now();
    const auto pcm = readAll(*audio, audio->sampleCount());
    std::printf("decoded %lld samples x %d ch in %.0f ms\n", (long long)audio->sampleCount(), audio->channels(),
                (now() - t0) * 1000);
    FILE* out = std::fopen(outPath, "wb");
    if (!out) fail("cannot write output");
    for (int64_t i = 0; i < audio->sampleCount(); ++i)
        for (const auto& ch : pcm) std::fwrite(&ch[size_t(i)], sizeof(float), 1, out);
    std::fclose(out);
}

void seekAudio(const char* path) {
    auto audio = openAudio(path);
    const int64_t total = audio->sampleCount();
    const auto reference = readAll(*audio, total);
    std::mt19937 rng(3);
    std::uniform_int_distribution<int64_t> pick(0, std::max<int64_t>(total - 4800, 0));
    std::string error;
    double worst = 0;
    for (int i = 0; i < 30; ++i) {
        const int64_t start = pick(rng), n = 4800;
        std::vector<std::vector<float>> got(size_t(audio->channels()), std::vector<float>(size_t(n)));
        std::vector<float*> out;
        for (auto& ch : got) out.push_back(ch.data());
        if (!audio->read(start, n, out.data(), error)) fail(error);
        for (size_t c = 0; c < got.size(); ++c)
            for (int64_t s = 0; s < n && start + s < total; ++s)
                worst = std::max(worst, double(std::fabs(got[c][size_t(s)] - reference[c][size_t(start + s)])));
    }
    std::printf("random audio reads vs sequential: max abs difference %.6f\n", worst);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: meprobe info|bench|md5|audio|seekaudio <file> [args]\n");
        return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "info") info(argv[2]);
    else if (cmd == "bench") bench(argv[2]);
    else if (cmd == "md5") md5(argv[2], argc - 3, argv + 3);
    else if (cmd == "audio" && argc == 4) audioDump(argv[2], argv[3]);
    else if (cmd == "seekaudio") seekAudio(argv[2]);
    else fail("unknown command");
    return 0;
}
