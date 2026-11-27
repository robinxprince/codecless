// Premiere Pro importer: serves AV1/VP9 pictures decoded on the media engine and their audio.
// Pictures reach Premiere in VideoToolbox's own layout (NV12 / P010 biplanar) with a plain
// plane copy; any other requested format goes through libswscale.
#include <CoreFoundation/CoreFoundation.h>
#include <CoreVideo/CoreVideo.h>

#include <chrono>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <mutex>
#include <string>

#include "PrSDKImport.h"
#include "PrSDKPPix2Suite.h"
#include "PrSDKPPixCacheSuite.h"
#include "PrSDKPPixCreatorSuite.h"
#include "PrSDKPPixSuite.h"
#include "PrSDKTimeSuite.h"
#include "SPBasic.h"

#include "audio_decoder.h"
#include "stats.h"
#include "video_decoder.h"

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

namespace {

constexpr csSDK_int32 kFileType = 0x4D456176;  // 'MEav'
constexpr char kExtensions[] = "webm\0mkv\0mp4";  // the literal's terminator makes the list double-null

void log(const char* format, ...) {
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    const char* home = std::getenv("HOME");
    FILE* file = std::fopen((std::string(home ? home : "/tmp") + "/Library/Logs/MediaEngine.log").c_str(), "a");
    if (!file) return;
    const std::time_t now = std::time(nullptr);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", std::localtime(&now));
    std::fprintf(file, "%s ", stamp);
    va_list args;
    va_start(args, format);
    std::vfprintf(file, format, args);
    va_end(args);
    std::fputc('\n', file);
    std::fclose(file);
}

std::string utf8Path(const prUTF16Char* path) {
    CFStringRef string = CFStringCreateWithCharacters(nullptr, reinterpret_cast<const UniChar*>(path),
                                                      prUTF16CharLength(path));
    if (!string) return {};
    char buffer[PATH_MAX * 4];
    const bool ok = CFStringGetFileSystemRepresentation(string, buffer, sizeof buffer);
    CFRelease(string);
    return ok ? buffer : std::string();
}

struct Suites {
    SPBasicSuite* basic = nullptr;
    PrSDKPPixCreatorSuite* creator = nullptr;
    PrSDKPPixSuite* ppix = nullptr;
    PrSDKPPix2Suite* ppix2 = nullptr;
    PrSDKPPixCacheSuite* cache = nullptr;
    PrSDKTimeSuite* time = nullptr;

    bool acquire(imStdParms* stdParms) {
        if (basic) return true;
        basic = stdParms->piSuites->utilFuncs->getSPBasicSuite();
        if (!basic) return false;
        basic->AcquireSuite(kPrSDKPPixCreatorSuite, kPrSDKPPixCreatorSuiteVersion, (const void**)&creator);
        basic->AcquireSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion, (const void**)&ppix);
        basic->AcquireSuite(kPrSDKPPix2Suite, kPrSDKPPix2SuiteVersion, (const void**)&ppix2);
        basic->AcquireSuite(kPrSDKPPixCacheSuite, kPrSDKPPixCacheSuiteVersion, (const void**)&cache);
        basic->AcquireSuite(kPrSDKTimeSuite, kPrSDKTimeSuiteVersion, (const void**)&time);
        return creator && ppix && ppix2 && cache && time;
    }

    void release() {
        if (!basic) return;
        if (creator) basic->ReleaseSuite(kPrSDKPPixCreatorSuite, kPrSDKPPixCreatorSuiteVersion);
        if (ppix) basic->ReleaseSuite(kPrSDKPPixSuite, kPrSDKPPixSuiteVersion);
        if (ppix2) basic->ReleaseSuite(kPrSDKPPix2Suite, kPrSDKPPix2SuiteVersion);
        if (cache) basic->ReleaseSuite(kPrSDKPPixCacheSuite, kPrSDKPPixCacheSuiteVersion);
        if (time) basic->ReleaseSuite(kPrSDKTimeSuite, kPrSDKTimeSuiteVersion);
        *this = Suites();
    }
};

// A video decoder with its own read position, used by one thread at a time.
struct Lane {
    std::mutex mutex;
    std::unique_ptr<me::VideoDecoder> decoder;
    SwsContext* scaler = nullptr;

    ~Lane() { sws_freeContext(scaler); }
};

// One imported file. Decoders are closed when Premiere quiets the file and reopened on use.
struct Clip {
    std::string path;
    Suites suites;
    csSDK_uint32 importerID = 0;
    PrTime ticksPerFrame = 1;
    me::VideoInfo info;
    PrPixelFormat nativeFormat = PrPixelFormat_Invalid;
    bool hasAudio = false;

    // Premiere's bookkeeping (media analysis, thumbnails) samples a clip all over, long after
    // import; on its own lane it never waits behind, or moves, the decoder following the
    // playhead. Lock order: background before playhead.
    Lane playhead;
    Lane background;

    std::mutex audioMutex;
    std::unique_ptr<me::AudioDecoder> audio;
    int64_t nextAudioSample = 0;

    ~Clip() { suites.release(); }

    // The caller holds lane.mutex.
    bool openVideo(Lane& lane, std::string& error) {
        if (lane.decoder) return true;
        if (&lane == &background) {  // a sibling of the playhead's decoder skips the index pass
            std::lock_guard<std::mutex> lock(playhead.mutex);
            if (!openVideo(playhead, error)) return false;
            lane.decoder = playhead.decoder->sibling(error);
            return lane.decoder != nullptr;
        }
        lane.decoder = me::VideoDecoder::open(path, error);
        if (lane.decoder)
            log("decoding %s on %s", path.c_str(),
                lane.decoder->backend() == me::Backend::Hardware ? "the media engine" : "the CPU (software fallback)");
        return lane.decoder != nullptr;
    }

    bool openAudio(std::string& error) {
        if (!audio && hasAudio) audio = me::AudioDecoder::open(path, info.startSeconds, error);
        return audio != nullptr;
    }
};

// Premiere hands privatedata back on every call; it holds a memory-suite handle to this.
struct ClipRecord {
    Clip* clip;
};
using ClipRecordH = ClipRecord**;

Clip* clipFrom(void* privatedata) {
    auto handle = reinterpret_cast<ClipRecordH>(privatedata);
    return handle && *handle ? (*handle)->clip : nullptr;
}

Clip* clipFor(imStdParms* stdParms, void** privatedata, const prUTF16Char* path) {
    if (Clip* clip = clipFrom(*privatedata)) return clip;
    auto handle = reinterpret_cast<ClipRecordH>(stdParms->piSuites->memFuncs->newHandle(sizeof(ClipRecord)));
    if (!handle) return nullptr;
    (*handle)->clip = new Clip;
    (*handle)->clip->path = utf8Path(path);
    *privatedata = handle;
    return (*handle)->clip;
}

// The Premiere format matching the decoder's NV12 / P010 output and the stream's color.
PrPixelFormat nativeFormat(const me::VideoInfo& v) {
    const bool full = v.fullRange;
    if (v.bitDepth > 8) {
        if (v.transfer == AVCOL_TRC_SMPTE2084)
            return full ? PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_BIPLANAR_10u_as16u_2020_HDR_FullRange
                        : PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_BIPLANAR_10u_as16u_2020_HDR;
        if (v.transfer == AVCOL_TRC_ARIB_STD_B67)
            return full ? PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_BIPLANAR_10u_as16u_2020_HDR_HLG_FullRange
                        : PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_BIPLANAR_10u_as16u_2020_HDR_HLG;
        if (v.primaries == AVCOL_PRI_BT2020 || v.matrix == AVCOL_SPC_BT2020_NCL)
            return full ? PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_BIPLANAR_10u_as16u_2020_FullRange
                        : PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_BIPLANAR_10u_as16u_2020;
        return full ? PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_BIPLANAR_10u_as16u_709_FullRange
                    : PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_BIPLANAR_10u_as16u_709;
    }
    if (v.matrix == AVCOL_SPC_SMPTE170M || v.matrix == AVCOL_SPC_BT470BG)
        return full ? PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_BIPLANAR_8u_601_FullRange
                    : PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_BIPLANAR_8u_601;
    return full ? PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_BIPLANAR_8u_709_FullRange
                : PrPixelFormat_YUV_420_MPEG4_FRAME_PICTURE_BIPLANAR_8u_709;
}

int64_t frameNumber(const Clip& clip, PrTime time) { return time / clip.ticksPerFrame; }

// Copies the media engine's picture straight into Premiere's planes when layouts agree.
bool copyHardwarePlanes(const AVFrame* frame, bool tenBit, char* y, int yStride, char* uv, int uvStride,
                        int width, int height) {
    if (frame->format != AV_PIX_FMT_VIDEOTOOLBOX) return false;
    auto buffer = reinterpret_cast<CVPixelBufferRef>(frame->data[3]);
    const OSType type = CVPixelBufferGetPixelFormatType(buffer);
    const bool matches = tenBit ? (type == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange ||
                                   type == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange)
                                : (type == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange ||
                                   type == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange);
    if (!matches || int(CVPixelBufferGetWidth(buffer)) != width || int(CVPixelBufferGetHeight(buffer)) != height)
        return false;
    if (CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return false;
    const size_t bytesPerSample = tenBit ? 2 : 1;
    struct Plane {
        char* dst;
        int dstStride;
        int rows;
        size_t rowBytes;
    } planes[] = {{y, yStride, height, size_t(width) * bytesPerSample},
                  {uv, uvStride, (height + 1) / 2, size_t((width + 1) / 2) * 2 * bytesPerSample}};
    for (size_t p = 0; p < 2; ++p) {
        const auto* src = static_cast<const char*>(CVPixelBufferGetBaseAddressOfPlane(buffer, p));
        const size_t srcStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, p);
        for (int row = 0; row < planes[p].rows; ++row)
            std::memcpy(planes[p].dst + ptrdiff_t(row) * planes[p].dstStride, src + row * srcStride,
                        planes[p].rowBytes);
    }
    CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
    return true;
}

// Any picture into any of our formats at any size (software-decoded frames, BGRA requests).
bool convert(const Clip& clip, SwsContext*& scaler, const AVFrame* frame, AVPixelFormat target,
             uint8_t* const dst[], const int dstStride[], int width, int height) {
    AVFrame* software = nullptr;
    if (frame->format == AV_PIX_FMT_VIDEOTOOLBOX) {
        software = av_frame_alloc();
        if (!software || av_hwframe_transfer_data(software, frame, 0) < 0) {
            av_frame_free(&software);
            return false;
        }
        frame = software;
    }
    scaler = sws_getCachedContext(scaler, frame->width, frame->height, AVPixelFormat(frame->format), width, height,
                                  target, SWS_BICUBIC, nullptr, nullptr, nullptr);
    bool ok = scaler != nullptr;
    if (ok) {
        const int matrix = clip.info.matrix == AVCOL_SPC_SMPTE170M || clip.info.matrix == AVCOL_SPC_BT470BG
                               ? SWS_CS_ITU601
                               : clip.info.matrix == AVCOL_SPC_BT2020_NCL ? SWS_CS_BT2020 : SWS_CS_ITU709;
        const int* coefficients = sws_getCoefficients(matrix);
        sws_setColorspaceDetails(scaler, coefficients, clip.info.fullRange, coefficients, 1, 0, 1 << 16, 1 << 16);
        ok = sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, dst, dstStride) > 0;
    }
    av_frame_free(&software);
    return ok;
}

bool writePicture(const Clip& clip, SwsContext*& scaler, const AVFrame* frame, PPixHand ppix, PrPixelFormat format,
                  int width, int height) {
    const Suites& s = clip.suites;
    if (format == PrPixelFormat_BGRA_4444_8u) {
        char* base = nullptr;
        csSDK_int32 rowBytes = 0;
        s.ppix->GetPixels(ppix, PrPPixBufferAccess_WriteOnly, &base);
        s.ppix->GetRowBytes(ppix, &rowBytes);
        if (!base) return false;
        // 4444 formats have their origin at the bottom left (PrSDKImageProcessingSuite.h),
        // so start at the last row and walk backwards.
        uint8_t* dst[4] = {reinterpret_cast<uint8_t*>(base) + ptrdiff_t(height - 1) * rowBytes};
        const int stride[4] = {-rowBytes};
        return convert(clip, scaler, frame, AV_PIX_FMT_BGRA, dst, stride, width, height);
    }

    char *y = nullptr, *u = nullptr, *v = nullptr;
    csSDK_uint32 yStride = 0, uStride = 0, vStride = 0;
    s.ppix2->GetYUV420PlanarBuffers(ppix, PrPPixBufferAccess_WriteOnly, &y, &yStride, &u, &uStride, &v, &vStride);
    static std::once_flag logged;
    std::call_once(logged, [&] {
        log("biplanar ppix %dx%d: Y %p stride %u, UV %p stride %u, V %p", width, height, (void*)y, yStride,
            (void*)u, uStride, (void*)v);
    });
    if (!y || !u) {
        // The SDK documents this accessor for planar formats only; biplanar pixels are
        // otherwise laid out as the luma plane followed by the interleaved chroma plane.
        char* base = nullptr;
        csSDK_int32 rowBytes = 0;
        s.ppix->GetPixels(ppix, PrPPixBufferAccess_WriteOnly, &base);
        s.ppix->GetRowBytes(ppix, &rowBytes);
        if (!base || rowBytes <= 0) return false;
        y = base;
        u = base + ptrdiff_t(rowBytes) * height;
        yStride = uStride = csSDK_uint32(rowBytes);
    }
    const bool tenBit = clip.info.bitDepth > 8;
    if (copyHardwarePlanes(frame, tenBit, y, int(yStride), u, int(uStride), width, height)) return true;
    uint8_t* dst[4] = {reinterpret_cast<uint8_t*>(y), reinterpret_cast<uint8_t*>(u)};
    const int stride[4] = {int(yStride), int(uStride)};
    return convert(clip, scaler, frame, tenBit ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12, dst, stride, width, height);
}

prMALError init(imImportInfoRec* info) {
    info->canSave = kPrFalse;
    info->canDelete = kPrFalse;
    info->hasSetup = kPrFalse;
    info->dontCache = kPrFalse;
    info->keepLoaded = kPrFalse;
    // Ahead of Premiere's own MP4 importer; files that aren't AV1/VP9 are declined and fall through.
    info->priority = 100;
    info->canTrim = kPrFalse;
    info->canCalcSizes = kPrFalse;
    info->avoidAudioConform = kPrTrue;  // audio reads are sample-accurate and cheap
    return imIsCacheable;
}

prMALError indFormat(csSDK_size_t index, imIndFormatRec* format) {
    if (index != 0) return imBadFormatIndex;
    format->filetype = kFileType;
    format->flags = xfCanImport | xfIsMovie;
    std::snprintf(format->FormatName, sizeof format->FormatName, "AV1 / VP9 (Apple media engine)");
    std::snprintf(format->FormatShortName, sizeof format->FormatShortName, "AV1 / VP9");
    std::memcpy(format->PlatformExtension, kExtensions, sizeof kExtensions);
    return malNoError;
}

prMALError openFile(imStdParms* stdParms, imFileRef* fileRef, imFileOpenRec8* open) {
    Clip* clip = clipFor(stdParms, &open->privatedata, open->fileinfo.filepath);
    if (!clip) return imMemErr;
    open->fileinfo.filetype = kFileType;
    *fileRef = reinterpret_cast<imFileRef>(clip);  // decoders open their own file descriptors
    return malNoError;
}

prMALError getInfo(imStdParms* stdParms, imFileAccessRec8* access, imFileInfoRec8* fileInfo) {
    Clip* clip = clipFor(stdParms, &fileInfo->privatedata, access->filepath);
    if (!clip) return imMemErr;
    std::string error;
    bool hardware = false;
    {
        std::lock_guard<std::mutex> lock(clip->playhead.mutex);
        if (!clip->openVideo(clip->playhead, error)) {
            log("declined %s: %s", clip->path.c_str(), error.c_str());
            return imBadFile;  // lets a lower-priority importer try the file
        }
        clip->info = clip->playhead.decoder->info();
        hardware = clip->playhead.decoder->backend() == me::Backend::Hardware;
    }
    if (!clip->suites.acquire(stdParms)) return imOtherErr;

    const me::VideoInfo& v = clip->info;
    PrTime ticksPerSecond = 0;
    clip->suites.time->GetTicksPerSecond(&ticksPerSecond);
    clip->ticksPerFrame = ticksPerSecond * v.frameRate.den / v.frameRate.num;
    clip->importerID = fileInfo->vidInfo.importerID;
    clip->nativeFormat = nativeFormat(v);

    char summary[160];
    std::snprintf(summary, sizeof summary, "%s %dx%d, %.3g fps, %d-bit, %s", v.codec == "av1" ? "AV1" : "VP9", v.width,
                  v.height, av_q2d(v.frameRate), v.bitDepth, hardware ? "media engine" : "software decode");
    const size_t slash = clip->path.find_last_of('/');
    me::stats::addClip(clip, slash == std::string::npos ? clip->path : clip->path.substr(slash + 1), summary);

    fileInfo->hasVideo = kPrTrue;
    imImageInfoRec& image = fileInfo->vidInfo;
    image.imageWidth = v.width;
    image.imageHeight = v.height;
    image.pixelAspectNum = csSDK_uint32(v.sampleAspect.num);
    image.pixelAspectDen = csSDK_uint32(v.sampleAspect.den);
    image.depth = 24;
    image.subType = v.codec == "av1" ? 0x61763031 : 0x76703039;  // 'av01' / 'vp09'
    image.fieldType = prFieldsNone;
    image.alphaType = alphaNone;
    image.isStill = kPrFalse;
    image.noDuration = imNoDurationFalse;
    image.supportsAsyncIO = kPrFalse;
    image.supportsGetSourceVideo = kPrTrue;
    image.hasPulldown = kPrFalse;
    image.frameRate = clip->ticksPerFrame;
    image.bitDepth = v.bitDepth;
    image.colorSpaceSupport = imColorSpaceSupport_Fixed;  // see indColorSpace
    fileInfo->vidScale = v.frameRate.num;
    fileInfo->vidSampleSize = v.frameRate.den;
    fileInfo->vidDuration = csSDK_int32(std::min<int64_t>(v.frameCount * v.frameRate.den, INT32_MAX));
    fileInfo->vidDurationInFrames = v.frameCount;
    fileInfo->accessModes = kRandomAccessImport;
    fileInfo->hasDataRate = kPrFalse;

    std::lock_guard<std::mutex> lock(clip->audioMutex);
    clip->hasAudio = true;
    if (clip->openAudio(error)) {
        fileInfo->hasAudio = kPrTrue;
        fileInfo->audInfo.numChannels = clip->audio->channels();
        fileInfo->audInfo.sampleRate = float(clip->audio->sampleRate());
        fileInfo->audInfo.sampleType = kPrAudioSampleType_Compressed;
        fileInfo->audDuration = clip->audio->sampleCount();
    } else {
        clip->hasAudio = false;
        fileInfo->hasAudio = kPrFalse;
        if (!error.empty()) log("no audio from %s: %s", clip->path.c_str(), error.c_str());
    }
    return malNoError;
}

prMALError indPixelFormat(csSDK_size_t index, imIndPixelFormatRec* rec) {
    const Clip* clip = clipFrom(rec->privatedata);
    if (!clip) return imOtherErr;
    switch (index) {
        case 0: rec->outPixelFormat = clip->nativeFormat; return malNoError;
        case 1: rec->outPixelFormat = PrPixelFormat_BGRA_4444_8u; return malNoError;
        default: return imBadFormatIndex;
    }
}

// Without this Premiere's color management assumes Rec. 709 and HDR clips look washed out.
// FFmpeg's color enums and Premiere's SEI codes are both ITU-T H.273 values.
prMALError indColorSpace(csSDK_size_t index, imIndColorSpaceRec* rec) {
    const Clip* clip = clipFrom(rec->inPrivateData);
    if (!clip) return imOtherErr;
    if (index != 0) return imBadFormatIndex;
    const me::VideoInfo& v = clip->info;
    auto specified = [](int code, int unspecified) { return code > 0 && code != unspecified ? code : 1; };
    rec->outColorSpaceType = kPrSDKColorSpaceType_SEITags;
    prSEIColorCodesRec& sei = rec->outSEICodesRec;
    sei.colorPrimariesCode = specified(v.primaries, AVCOL_PRI_UNSPECIFIED);
    sei.transferCharacteristicCode = specified(v.transfer, AVCOL_TRC_UNSPECIFIED);
    sei.matrixEquationsCode = specified(v.matrix, AVCOL_SPC_UNSPECIFIED);
    sei.bitDepth = v.bitDepth;
    sei.isFullRange = v.fullRange ? kPrTrue : kPrFalse;
    sei.isRGB = kPrFalse;
    sei.isSceneReferred = kPrFalse;
    return malNoError;
}

// Requests that sample a clip out of order for Premiere's own use rather than follow the
// playhead. Media analysis arrives without an intent.
bool isBookkeeping(imRenderIntent intent) {
    return intent == imRenderIntent_Unknown || intent == imRenderIntent_Thumbnail ||
           intent == imRenderIntent_Analysis || intent == imRenderIntent_DistantPrefetch;
}

prMALError sourceVideo(imSourceVideoRec* rec) {
    Clip* clip = clipFrom(rec->inPrivateData);
    if (!clip) return imOtherErr;
    const Suites& s = clip->suites;
    Lane& lane = isBookkeeping(rec->inRenderContext.inIntent) ? clip->background : clip->playhead;

    imFrameFormat want{clip->info.width, clip->info.height, clip->nativeFormat};
    if (rec->inFrameFormats && rec->inNumFrameFormats > 0) {
        want.inPixelFormat = PrPixelFormat_BGRA_4444_8u;  // when nothing else requested is ours
        for (csSDK_int32 i = 0; i < rec->inNumFrameFormats; ++i) {
            const imFrameFormat& f = rec->inFrameFormats[i];
            if (f.inPixelFormat == clip->nativeFormat || f.inPixelFormat == PrPixelFormat_BGRA_4444_8u) {
                want.inPixelFormat = f.inPixelFormat;
                if (f.inFrameWidth > 0 && f.inFrameHeight > 0) {
                    want.inFrameWidth = f.inFrameWidth;
                    want.inFrameHeight = f.inFrameHeight;
                }
                break;
            }
        }
    }
    // A non-native size can only be produced by scaling, which the BGRA path does.
    if (want.inFrameWidth != clip->info.width || want.inFrameHeight != clip->info.height)
        want.inPixelFormat = PrPixelFormat_BGRA_4444_8u;

    const auto frame = csSDK_int32(frameNumber(*clip, rec->inFrameTime));
    using Clock = std::chrono::steady_clock;
    const auto requested = Clock::now();
    me::stats::Request stat{clip, frame, int(rec->inRenderContext.inIntent), &lane == &clip->background};
    stat.width = want.inFrameWidth;
    stat.height = want.inFrameHeight;
    auto finish = [&](me::stats::Outcome outcome, prMALError result) {
        stat.outcome = outcome;
        stat.workMs = std::chrono::duration<double, std::milli>(Clock::now() - requested).count() - stat.waitMs;
        me::stats::record(stat);
        return result;
    };
    if (s.cache->GetFrameFromCache(clip->importerID, 0, frame, 1, &want, rec->outFrame, nullptr, 0) == 0)
        return finish(me::stats::Outcome::HostCache, malNoError);

    std::lock_guard<std::mutex> lock(lane.mutex);
    stat.waitMs = std::chrono::duration<double, std::milli>(Clock::now() - requested).count();
    const int64_t decodedBefore = lane.decoder ? lane.decoder->picturesDecoded() : 0;
    std::string error;
    const AVFrame* picture = clip->openVideo(lane, error) ? lane.decoder->frameAt(frame, error) : nullptr;
    if (lane.decoder) stat.pictures = lane.decoder->picturesDecoded() - decodedBefore;
    if (!picture) {
        log("frame %d of %s: %s", frame, clip->path.c_str(), error.c_str());
        return finish(me::stats::Outcome::Failed, imFrameNotFound);
    }
    prRect bounds;
    prSetRect(&bounds, 0, 0, want.inFrameWidth, want.inFrameHeight);
    if (s.creator->CreatePPix(rec->outFrame, PrPPixBufferAccess_ReadWrite, want.inPixelFormat, &bounds) != 0)
        return finish(me::stats::Outcome::Failed, imMemErr);
    if (!writePicture(*clip, lane.scaler, picture, *rec->outFrame, want.inPixelFormat, want.inFrameWidth,
                      want.inFrameHeight)) {
        log("frame %d of %s: cannot convert to pixel format 0x%08x", frame, clip->path.c_str(), want.inPixelFormat);
        s.ppix->Dispose(*rec->outFrame);
        *rec->outFrame = nullptr;
        return finish(me::stats::Outcome::Failed, imDecompressionError);
    }
    s.cache->AddFrameToCache(clip->importerID, 0, *rec->outFrame, frame, nullptr, 0);
    return finish(me::stats::Outcome::Decoded, malNoError);
}

prMALError importAudio(imImportAudioRec7* rec) {
    Clip* clip = clipFrom(rec->privateData);
    if (!clip) return imOtherErr;
    std::lock_guard<std::mutex> lock(clip->audioMutex);
    std::string error;
    const int64_t start = rec->position < 0 ? clip->nextAudioSample : rec->position;
    if (!clip->openAudio(error) || !clip->audio->read(start, rec->size, rec->buffer, error)) {
        log("audio at %lld of %s: %s", (long long)start, clip->path.c_str(), error.c_str());
        return imDecompressionError;
    }
    clip->nextAudioSample = start + rec->size;
    return malNoError;
}

// Shown in Premiere's Properties panel, including which decoder is in use.
prMALError analysis(imAnalysisRec* rec) {
    Clip* clip = clipFrom(rec->privatedata);
    if (!clip || !rec->buffer || rec->buffersize <= 0) return imOtherErr;
    std::string error, decoder = "not open";
    {
        std::lock_guard<std::mutex> lock(clip->playhead.mutex);
        if (clip->openVideo(clip->playhead, error))
            decoder = clip->playhead.decoder->backend() == me::Backend::Hardware
                          ? "Apple media engine (VideoToolbox)"
                          : "software (media engine declined this stream)";
    }
    const me::VideoInfo& v = clip->info;
    std::snprintf(rec->buffer, size_t(rec->buffersize),
                  "MediaEngine importer\n%s %dx%d, %d-bit, %.3f fps\nColor: %s / %s / %s, %s range\nDecoder: %s\n",
                  v.codec == "av1" ? "AV1" : "VP9", v.width, v.height, v.bitDepth, av_q2d(v.frameRate),
                  av_color_primaries_name(v.primaries), av_color_transfer_name(v.transfer),
                  av_color_space_name(v.matrix), v.fullRange ? "full" : "limited", decoder.c_str());
    return malNoError;
}

prMALError quietFile(imFileRef* fileRef, void* privatedata) {
    if (Clip* clip = clipFrom(privatedata)) {
        std::lock_guard<std::mutex> backgroundLock(clip->background.mutex);
        std::lock_guard<std::mutex> playheadLock(clip->playhead.mutex);
        std::lock_guard<std::mutex> audioLock(clip->audioMutex);
        clip->background.decoder.reset();
        clip->playhead.decoder.reset();
        clip->audio.reset();
    }
    if (fileRef) *fileRef = imInvalidHandleValue;
    return malNoError;
}

prMALError closeFile(imStdParms* stdParms, imFileRef* fileRef, void* privatedata) {
    quietFile(fileRef, privatedata);
    auto handle = reinterpret_cast<ClipRecordH>(privatedata);
    if (handle) {
        me::stats::removeClip((*handle)->clip);
        delete (*handle)->clip;
        stdParms->piSuites->memFuncs->disposeHandle(reinterpret_cast<char**>(handle));
    }
    return malNoError;
}

}  // namespace

extern "C" PREMPLUGENTRY DllExport xImportEntry(csSDK_int32 selector, imStdParms* stdParms, void* param1,
                                                 void* param2) {
    switch (selector) {
        case imInit: return init(static_cast<imImportInfoRec*>(param1));
        case imGetSupports8: return malSupports8;
        case imGetIndFormat: return indFormat(reinterpret_cast<csSDK_size_t>(param1), static_cast<imIndFormatRec*>(param2));
        case imOpenFile8:
            return openFile(stdParms, static_cast<imFileRef*>(param1), static_cast<imFileOpenRec8*>(param2));
        case imGetInfo8:
            return getInfo(stdParms, static_cast<imFileAccessRec8*>(param1), static_cast<imFileInfoRec8*>(param2));
        case imGetIndPixelFormat:
            return indPixelFormat(reinterpret_cast<csSDK_size_t>(param1), static_cast<imIndPixelFormatRec*>(param2));
        case imGetIndColorSpace:
            return indColorSpace(reinterpret_cast<csSDK_size_t>(param1), static_cast<imIndColorSpaceRec*>(param2));
        case imGetSourceVideo: return sourceVideo(static_cast<imSourceVideoRec*>(param2));
        case imImportAudio7: return importAudio(static_cast<imImportAudioRec7*>(param2));
        case imAnalysis: return analysis(static_cast<imAnalysisRec*>(param2));
        case imQuietFile: return quietFile(static_cast<imFileRef*>(param1), param2);
        case imCloseFile: return closeFile(stdParms, static_cast<imFileRef*>(param1), param2);
        default: return imUnsupported;
    }
}
