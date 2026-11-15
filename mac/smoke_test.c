// Smoke test for the macOS FFmpeg dylibs, the way GucciBot uses them.
//
// It never links against FFmpeg: like GucciBot's renderer
// (src/render/ffmpeg.hpp), it dlopens the seven libraries from one folder by
// full path, in dependency order, and looks every function up by name. Then it
// encodes, muxes, and decodes back:
//   A  .mp4  libx264 yuv420p crf 23        + aac   (required)
//   B  .mkv  libx264 yuv444p qp 0          + flac  (required; GucciBot's Showcase preset)
//   C  .mp4  h264_videotoolbox nv12        + aac   (reported; runner VMs may lack the encoder)
//   D  .mp4  libx264 yuv420p               + aac_at (reported; AudioToolbox's AAC encoder)
// Each file is decoded with sws_scale to RGBA (as Video Mode does) and must
// give back exactly 60 video frames.
//
// usage: smoke_test <folder with the dylibs> <scratch folder>

#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavfilter/avfilter.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

static const char* kLibs[7] = {"libavutil.60.dylib",   "libswresample.6.dylib", "libswscale.9.dylib",
                               "libavcodec.62.dylib",  "libavformat.62.dylib",  "libavfilter.11.dylib",
                               "libavdevice.62.dylib"};
static void* g_libs[7];

// The 46 functions GucciBot's renderer and Video Mode resolve (ffmpeg.hpp).
static const char* kGucciBotNames[] = {
    "avformat_alloc_output_context2", "avcodec_find_encoder_by_name", "avcodec_alloc_context3",
    "avcodec_get_supported_config", "av_dict_set", "avcodec_open2", "av_dict_free", "avformat_new_stream",
    "avcodec_parameters_from_context", "avio_open", "avformat_write_header", "av_frame_alloc",
    "av_frame_get_buffer", "av_packet_alloc", "av_frame_unref", "av_frame_make_writable",
    "av_image_fill_linesizes", "avcodec_send_frame", "avcodec_receive_packet", "av_packet_rescale_ts",
    "av_interleaved_write_frame", "av_packet_unref", "av_channel_layout_default", "swr_alloc_set_opts2",
    "swr_init", "swr_convert", "av_write_trailer", "avio_close", "swr_free", "avcodec_free_context",
    "av_packet_free", "avformat_open_input", "avformat_find_stream_info", "avformat_close_input",
    "avcodec_find_decoder", "avcodec_parameters_to_context", "av_read_frame", "avcodec_send_packet",
    "avcodec_receive_frame", "av_frame_free", "av_seek_frame", "avcodec_flush_buffers", "sws_getContext",
    "sws_scale", "sws_freeContext", "av_dict_get"};

#define FUNCS(X)                                                                                                       \
    X(av_version_info) X(avutil_license) X(avcodec_configuration) X(avcodec_license) X(avutil_version)                 \
    X(avcodec_version) X(avformat_version) X(avfilter_version) X(avdevice_version) X(swscale_version)                  \
    X(swresample_version) X(av_strerror) X(av_frame_alloc) X(av_frame_get_buffer) X(av_frame_make_writable)            \
    X(av_frame_free) X(av_dict_set) X(av_dict_free) X(av_channel_layout_default) X(av_get_pix_fmt_name)                \
    X(av_get_sample_fmt_name) X(avcodec_find_encoder_by_name) X(avcodec_find_decoder) X(avcodec_find_decoder_by_name) \
    X(avcodec_alloc_context3) X(avcodec_open2) X(avcodec_send_frame) X(avcodec_receive_packet) X(avcodec_send_packet)  \
    X(avcodec_receive_frame) X(avcodec_free_context) X(avcodec_parameters_from_context)                                \
    X(avcodec_parameters_to_context) X(avcodec_get_supported_config) X(av_packet_alloc) X(av_packet_free)              \
    X(av_packet_unref) X(av_packet_rescale_ts) X(avformat_alloc_output_context2) X(avformat_new_stream) X(avio_open)   \
    X(avio_closep) X(avformat_write_header) X(av_interleaved_write_frame) X(av_write_trailer)                          \
    X(avformat_free_context) X(avformat_open_input) X(avformat_find_stream_info) X(av_read_frame)                      \
    X(avformat_close_input) X(av_find_input_format) X(av_guess_format) X(swr_alloc_set_opts2) X(swr_init)              \
    X(swr_convert) X(swr_free) X(sws_getContext) X(sws_scale) X(sws_freeContext) X(avfilter_get_by_name)               \
    X(avdevice_register_all)

#define DECL(n) static __typeof__(n)* p_##n;
FUNCS(DECL)

static void* sym(const char* name) {
    for (int i = 0; i < 7; i++) {
        void* p = dlsym(g_libs[i], name);
        if (p)
            return p;
    }
    return NULL;
}

#define LOAD(n)                                                                                                        \
    p_##n = (__typeof__(n)*)sym(#n);                                                                                   \
    if (!p_##n) {                                                                                                      \
        printf("FAIL: symbol %s not found\n", #n);                                                                     \
        exit(1);                                                                                                       \
    }

static const char* err(int e) {
    static char buf[256];
    p_av_strerror(e, buf, sizeof buf);
    return buf;
}

#define CHECK(expr, what)                                                                                              \
    do {                                                                                                               \
        int r_ = (expr);                                                                                               \
        if (r_ < 0) {                                                                                                  \
            printf("  %s failed: %s\n", what, err(r_));                                                                \
            goto fail;                                                                                                 \
        }                                                                                                              \
    } while (0)

enum { W = 320, H = 240, FPS = 60, FRAMES = 60, RATE = 48000 };

static int drain(AVCodecContext* c, AVStream* st, AVFormatContext* oc, AVPacket* pkt) {
    for (;;) {
        int r = p_avcodec_receive_packet(c, pkt);
        if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
            return 0;
        if (r < 0)
            return r;
        p_av_packet_rescale_ts(pkt, c->time_base, st->time_base);
        pkt->stream_index = st->index;
        r = p_av_interleaved_write_frame(oc, pkt);
        if (r < 0)
            return r;
    }
}

static void fill_video(AVFrame* f, int i) {
    int const fmt = f->format;
    // Y (or packed NV12 luma)
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            f->data[0][y * f->linesize[0] + x] = (uint8_t)(16 + ((x + y + i * 3) % 200));
    if (fmt == AV_PIX_FMT_NV12) {
        for (int y = 0; y < H / 2; y++)
            for (int x = 0; x < W; x++)
                f->data[1][y * f->linesize[1] + x] = (uint8_t)(64 + ((x * 2 + i) % 128));
        return;
    }
    int const cw = fmt == AV_PIX_FMT_YUV444P ? W : W / 2, ch = fmt == AV_PIX_FMT_YUV444P ? H : H / 2;
    for (int p = 1; p < 3; p++)
        for (int y = 0; y < ch; y++)
            for (int x = 0; x < cw; x++)
                f->data[p][y * f->linesize[p] + x] = (uint8_t)(64 + ((x * p + y + i) % 128));
}

// Returns 0 on success, 1 on failure.
static int encode_case(const char* label, const char* path, const char* vname, enum AVPixelFormat pix,
                       const char* const* vopts, const char* aname) {
    AVFormatContext* oc = NULL;
    AVCodecContext *vc = NULL, *ac = NULL;
    AVFrame *vf = NULL, *af = NULL;
    AVPacket* pkt = NULL;
    SwrContext* swr = NULL;
    AVDictionary* d = NULL;
    float* pcm = NULL;
    int ok = 0;

    printf("[%s] %s: %s %s + %s\n", label, path, vname, p_av_get_pix_fmt_name(pix), aname);
    const AVCodec* venc = p_avcodec_find_encoder_by_name(vname);
    const AVCodec* aenc = p_avcodec_find_encoder_by_name(aname);
    if (!venc || !aenc) {
        printf("  encoder %s not found\n", !venc ? vname : aname);
        goto fail;
    }
    CHECK(p_avformat_alloc_output_context2(&oc, NULL, NULL, path), "avformat_alloc_output_context2");

    vc = p_avcodec_alloc_context3(venc);
    vc->width = W;
    vc->height = H;
    vc->time_base = (AVRational){1, FPS};
    vc->framerate = (AVRational){FPS, 1};
    vc->pix_fmt = pix;
    vc->gop_size = 30;
    if (!vopts)
        vc->bit_rate = 4000000; // VideoToolbox; x264 runs on crf / qp
    if (oc->oformat->flags & AVFMT_GLOBALHEADER)
        vc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    for (int i = 0; vopts && vopts[i]; i += 2)
        p_av_dict_set(&d, vopts[i], vopts[i + 1], 0);
    CHECK(p_avcodec_open2(vc, venc, &d), "open video encoder");
    p_av_dict_free(&d);
    AVStream* vs = p_avformat_new_stream(oc, NULL);
    vs->time_base = vc->time_base;
    CHECK(p_avcodec_parameters_from_context(vs->codecpar, vc), "video parameters");

    ac = p_avcodec_alloc_context3(aenc);
    ac->sample_rate = RATE;
    p_av_channel_layout_default(&ac->ch_layout, 2);
    ac->time_base = (AVRational){1, RATE};
    ac->bit_rate = 192000;
    {
        const void* cfg = NULL;
        int n = 0;
        p_avcodec_get_supported_config(ac, aenc, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &cfg, &n);
        ac->sample_fmt = (cfg && n > 0) ? ((const enum AVSampleFormat*)cfg)[0] : AV_SAMPLE_FMT_FLTP;
    }
    if (oc->oformat->flags & AVFMT_GLOBALHEADER)
        ac->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    CHECK(p_avcodec_open2(ac, aenc, NULL), "open audio encoder");
    AVStream* as = p_avformat_new_stream(oc, NULL);
    as->time_base = ac->time_base;
    CHECK(p_avcodec_parameters_from_context(as->codecpar, ac), "audio parameters");
    printf("  audio sample format %s, frame size %d\n", p_av_get_sample_fmt_name(ac->sample_fmt), ac->frame_size);

    // GucciBot hands swresample interleaved float stereo (renderer.cpp).
    {
        AVChannelLayout stereo;
        p_av_channel_layout_default(&stereo, 2);
        CHECK(p_swr_alloc_set_opts2(&swr, &ac->ch_layout, ac->sample_fmt, RATE, &stereo, AV_SAMPLE_FMT_FLT, RATE, 0,
                                    NULL),
              "swr_alloc_set_opts2");
        CHECK(p_swr_init(swr), "swr_init");
    }

    CHECK(p_avio_open(&oc->pb, path, AVIO_FLAG_WRITE), "avio_open");
    CHECK(p_avformat_write_header(oc, NULL), "write header");

    pkt = p_av_packet_alloc();
    vf = p_av_frame_alloc();
    vf->format = pix;
    vf->width = W;
    vf->height = H;
    CHECK(p_av_frame_get_buffer(vf, 0), "video frame buffer");
    int const asz = ac->frame_size > 0 ? ac->frame_size : 1024;
    af = p_av_frame_alloc();
    af->format = ac->sample_fmt;
    af->nb_samples = asz;
    af->sample_rate = RATE;
    p_av_channel_layout_default(&af->ch_layout, 2);
    CHECK(p_av_frame_get_buffer(af, 0), "audio frame buffer");
    pcm = malloc(sizeof(float) * 2 * asz);

    int vi = 0;
    int64_t apts = 0;
    while (vi < FRAMES || apts < RATE) {
        // Whichever stream is behind goes next.
        if (vi < FRAMES && (apts >= RATE || (int64_t)vi * RATE < apts * FPS)) {
            CHECK(p_av_frame_make_writable(vf), "video make writable");
            fill_video(vf, vi);
            vf->pts = vi++;
            CHECK(p_avcodec_send_frame(vc, vf), "send video frame");
            CHECK(drain(vc, vs, oc, pkt), "write video");
        } else {
            CHECK(p_av_frame_make_writable(af), "audio make writable");
            for (int s = 0; s < asz; s++) {
                float v = 0.25f * sinf(2.0f * 3.14159265f * 440.0f * (float)(apts + s) / RATE);
                pcm[2 * s] = pcm[2 * s + 1] = v;
            }
            const uint8_t* in[1] = {(const uint8_t*)pcm};
            int got = p_swr_convert(swr, af->data, asz, in, asz);
            if (got != asz) {
                printf("  swr_convert gave %d of %d\n", got, asz);
                goto fail;
            }
            af->pts = apts;
            apts += asz;
            CHECK(p_avcodec_send_frame(ac, af), "send audio frame");
            CHECK(drain(ac, as, oc, pkt), "write audio");
        }
    }
    CHECK(p_avcodec_send_frame(vc, NULL), "flush video");
    CHECK(drain(vc, vs, oc, pkt), "write video (flush)");
    CHECK(p_avcodec_send_frame(ac, NULL), "flush audio");
    CHECK(drain(ac, as, oc, pkt), "write audio (flush)");
    CHECK(p_av_write_trailer(oc), "write trailer");
    ok = 1;

fail:
    free(pcm);
    p_av_frame_free(&vf);
    p_av_frame_free(&af);
    p_av_packet_free(&pkt);
    p_swr_free(&swr);
    p_avcodec_free_context(&vc);
    p_avcodec_free_context(&ac);
    p_av_dict_free(&d);
    if (oc) {
        if (oc->pb)
            p_avio_closep(&oc->pb);
        p_avformat_free_context(oc);
    }
    return ok ? 0 : 1;
}

static int decode_case(const char* path, enum AVPixelFormat want_pix) {
    AVFormatContext* ic = NULL;
    AVCodecContext *vd = NULL, *ad = NULL;
    AVPacket* pkt = NULL;
    AVFrame *f = NULL, *rgba = NULL;
    struct SwsContext* sws = NULL;
    int ok = 0, vframes = 0, vi = -1, ai = -1;
    int64_t asamples = 0;

    CHECK(p_avformat_open_input(&ic, path, NULL, NULL), "avformat_open_input");
    CHECK(p_avformat_find_stream_info(ic, NULL), "avformat_find_stream_info");
    for (unsigned i = 0; i < ic->nb_streams; i++) {
        if (ic->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && vi < 0)
            vi = (int)i;
        if (ic->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO && ai < 0)
            ai = (int)i;
    }
    if (vi < 0 || ai < 0) {
        printf("  missing stream (video %d, audio %d)\n", vi, ai);
        goto fail;
    }
    const AVCodec* vdec = p_avcodec_find_decoder(ic->streams[vi]->codecpar->codec_id);
    const AVCodec* adec = p_avcodec_find_decoder(ic->streams[ai]->codecpar->codec_id);
    if (!vdec || !adec) {
        printf("  no decoder\n");
        goto fail;
    }
    vd = p_avcodec_alloc_context3(vdec);
    CHECK(p_avcodec_parameters_to_context(vd, ic->streams[vi]->codecpar), "video parameters_to_context");
    CHECK(p_avcodec_open2(vd, vdec, NULL), "open video decoder");
    ad = p_avcodec_alloc_context3(adec);
    CHECK(p_avcodec_parameters_to_context(ad, ic->streams[ai]->codecpar), "audio parameters_to_context");
    CHECK(p_avcodec_open2(ad, adec, NULL), "open audio decoder");
    printf("  demuxer %s, video decoder %s, audio decoder %s\n", ic->iformat->name, vdec->name, adec->name);

    pkt = p_av_packet_alloc();
    f = p_av_frame_alloc();
    rgba = p_av_frame_alloc();
    rgba->format = AV_PIX_FMT_RGBA;
    rgba->width = W / 2;
    rgba->height = H / 2;
    CHECK(p_av_frame_get_buffer(rgba, 0), "rgba buffer");
    int pix_ok = 1;

    for (int eof = 0; !eof;) {
        int r = p_av_read_frame(ic, pkt);
        if (r == AVERROR_EOF)
            eof = 1;
        else
            CHECK(r, "av_read_frame");
        for (int s = 0; s < 2; s++) {
            AVCodecContext* c = s == 0 ? vd : ad;
            int idx = s == 0 ? vi : ai;
            if (!eof && pkt->stream_index != idx)
                continue;
            CHECK(p_avcodec_send_packet(c, eof ? NULL : pkt), "send packet");
            for (;;) {
                int g = p_avcodec_receive_frame(c, f);
                if (g == AVERROR(EAGAIN) || g == AVERROR_EOF)
                    break;
                CHECK(g, "receive frame");
                if (s == 0) {
                    if (f->format != want_pix)
                        pix_ok = 0;
                    if (!sws)
                        sws = p_sws_getContext(f->width, f->height, f->format, W / 2, H / 2, AV_PIX_FMT_RGBA,
                                               SWS_BILINEAR, NULL, NULL, NULL);
                    if (!sws || p_sws_scale(sws, (const uint8_t* const*)f->data, f->linesize, 0, f->height,
                                            rgba->data, rgba->linesize) != H / 2) {
                        printf("  sws_scale failed\n");
                        goto fail;
                    }
                    vframes++;
                } else {
                    asamples += f->nb_samples;
                }
            }
        }
        if (!eof)
            p_av_packet_unref(pkt);
    }
    printf("  decoded %d video frames (want %d), %lld audio samples, decoded pixel format %s\n", vframes, FRAMES,
           (long long)asamples, pix_ok ? "as encoded" : "DIFFERENT");
    ok = vframes == FRAMES && pix_ok && asamples >= RATE - 2048;

fail:
    p_sws_freeContext(sws);
    p_av_frame_free(&f);
    p_av_frame_free(&rgba);
    p_av_packet_free(&pkt);
    p_avcodec_free_context(&vd);
    p_avcodec_free_context(&ad);
    p_avformat_close_input(&ic);
    return ok ? 0 : 1;
}

static int run_case(const char* label, const char* dir, const char* file, const char* vname, enum AVPixelFormat pix,
                    const char* const* vopts, const char* aname) {
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", dir, file);
    if (encode_case(label, path, vname, pix, vopts, aname)) {
        printf("[%s] ENCODE FAILED\n", label);
        return 1;
    }
    FILE* fp = fopen(path, "rb");
    long size = 0;
    if (fp) {
        fseek(fp, 0, SEEK_END);
        size = ftell(fp);
        fclose(fp);
    }
    printf("  wrote %ld bytes\n", size);
    // VideoToolbox's NV12 comes back from the h264 decoder as yuv420p.
    if (decode_case(path, pix == AV_PIX_FMT_NV12 ? AV_PIX_FMT_YUV420P : pix)) {
        printf("[%s] DECODE FAILED\n", label);
        return 1;
    }
    printf("[%s] PASS\n", label);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 3) {
        printf("usage: %s <dylib folder> <scratch folder>\n", argv[0]);
        return 2;
    }
    for (int i = 0; i < 7; i++) {
        char path[2048];
        snprintf(path, sizeof path, "%s/%s", argv[1], kLibs[i]);
        g_libs[i] = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!g_libs[i]) {
            printf("FAIL: dlopen %s: %s\n", path, dlerror());
            return 1;
        }
        printf("loaded %s\n", path);
    }
    FUNCS(LOAD)
    int missing = 0;
    for (size_t i = 0; i < sizeof kGucciBotNames / sizeof kGucciBotNames[0]; i++)
        if (!sym(kGucciBotNames[i])) {
            printf("FAIL: GucciBot's %s not found\n", kGucciBotNames[i]);
            missing++;
        }
    printf("GucciBot's %zu functions: %s\n", sizeof kGucciBotNames / sizeof kGucciBotNames[0],
           missing ? "MISSING SOME" : "all found");
    if (missing)
        return 1;

    printf("version %s\nlicense %s / %s\n", p_av_version_info(), p_avutil_license(), p_avcodec_license());
    printf("avutil %u avcodec %u avformat %u avfilter %u avdevice %u swscale %u swresample %u\n",
           p_avutil_version() >> 16, p_avcodec_version() >> 16, p_avformat_version() >> 16,
           p_avfilter_version() >> 16, p_avdevice_version() >> 16, p_swscale_version() >> 16,
           p_swresample_version() >> 16);
    printf("configuration: %s\n", p_avcodec_configuration());
    p_avdevice_register_all();

    int fails = 0;
    // Components Video Mode and the renderer lean on; all must be present.
    static const char* const kDecoders[] = {"h264", "hevc", "vp8", "vp9", "libdav1d", "mpeg4", "prores", "mjpeg",
                                            "aac", "mp3float", "opus", "flac", NULL};
    for (int i = 0; kDecoders[i]; i++)
        if (!p_avcodec_find_decoder_by_name(kDecoders[i])) {
            printf("FAIL: decoder %s missing\n", kDecoders[i]);
            fails++;
        }
    const AVCodec* av1 = p_avcodec_find_decoder(AV_CODEC_ID_AV1);
    printf("AV1 decoder: %s\n", av1 ? av1->name : "NONE");
    if (!av1)
        fails++;
    static const char* const kDemuxers[] = {"mov", "matroska", "avi", NULL};
    for (int i = 0; kDemuxers[i]; i++)
        if (!p_av_find_input_format(kDemuxers[i])) {
            printf("FAIL: demuxer %s missing\n", kDemuxers[i]);
            fails++;
        }
    static const char* const kMuxers[] = {"mp4", "mov", "matroska", "webm", "avi", NULL};
    for (int i = 0; kMuxers[i]; i++)
        if (!p_av_guess_format(kMuxers[i], NULL, NULL)) {
            printf("FAIL: muxer %s missing\n", kMuxers[i]);
            fails++;
        }
    static const char* const kFilters[] = {"buffer", "buffersink", "scale", "format", NULL};
    for (int i = 0; kFilters[i]; i++)
        if (!p_avfilter_get_by_name(kFilters[i])) {
            printf("FAIL: filter %s missing\n", kFilters[i]);
            fails++;
        }

    static const char* const kCrf[] = {"preset", "veryfast", "crf", "23", NULL};
    static const char* const kQp0[] = {"preset", "ultrafast", "qp", "0", NULL};
    fails += run_case("A", argv[2], "a_x264_420.mp4", "libx264", AV_PIX_FMT_YUV420P, kCrf, "aac");
    fails += run_case("B", argv[2], "b_x264_444_qp0.mkv", "libx264", AV_PIX_FMT_YUV444P, kQp0, "flac");
    int vt = run_case("C", argv[2], "c_videotoolbox.mp4", "h264_videotoolbox", AV_PIX_FMT_NV12, NULL, "aac");
    printf("h264_videotoolbox: %s\n", vt ? "NOT WORKING on this machine (reported, not a failure)" : "works");
    int at = run_case("D", argv[2], "d_aac_at.mp4", "libx264", AV_PIX_FMT_YUV420P, kCrf, "aac_at");
    printf("aac_at: %s\n", at ? "NOT WORKING on this machine (reported, not a failure)" : "works");

    printf(fails ? "SMOKE TEST FAILED (%d)\n" : "SMOKE TEST PASSED\n", fails);
    return fails ? 1 : 0;
}
