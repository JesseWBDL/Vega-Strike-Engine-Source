/*
 * vid_file.cpp
 *
 * Vega Strike - Space Simulation, Combat and Trading
 * Copyright (C) 2001-2025 The Vega Strike Contributors:
 * Project creator: Daniel Horn
 * Original development team: As listed in the AUTHORS file
 * Current development team: Roy Falk, Benjamen R. Meyer, Stephen G. Tuggy
 *
 *
 * https://github.com/vegastrike/Vega-Strike-Engine-Source
 *
 * This file is part of Vega Strike.
 *
 * Vega Strike is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Vega Strike is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Vega Strike.  If not, see <https://www.gnu.org/licenses/>.
 */


//
//C++ Implementation: vid_file
//
// Decoding uses the modern (FFmpeg 3.1+) avcodec_send_packet() /
// avcodec_receive_frame() API; file access goes through VSFileSystem
// via a custom AVIOContext (see ffmpeg_init.cpp).
//

#include "vid_file.h"
#include "vsfilesystem.h"
#include "vs_logging.h"
#include "ffmpeg_init.h"

#include <string.h>
#include <math.h>
#include <utility>

#ifndef ENOENT
#define ENOENT (2)
#endif
#include <sys/types.h>

/*
 * FOLLOWING CODE IS ONLY INCLUDED IF YOU HAVE FFMPEG
 * ********************************************
 */
#ifdef HAVE_FFMPEG

using namespace VSFileSystem;

class VidFileImpl {
private:
    AVFormatContext *pFormatCtx;
    AVCodecContext *pCodecCtx;
    const AVCodec *pCodec;
    AVFrame *pFrameRGB;
    AVFrame *pFrameYUV;
    AVFrame *pNextFrameYUV;
    AVStream *pStream;
    AVPacket *packet;
    SwsContext *pSWSCtx;
    int swsSrcFormat;   // source pixel format the scaler was built for (-1 = none yet)
    int swsSrcW;
    int swsSrcH;
    int videoStreamIndex;
    bool frameReady;
    bool draining;

/** Framebuffer dimensions limit, useful for bandwidth-limited GPUs */
    size_t fbDimensionLimit;
    bool fbForcePOT;

    int64_t fbPTS;
    int64_t sizePTS;
    int64_t prevPTS;
    int64_t nextPTS;
    int64_t lastDTS;

    void convertFrame() {
        if (frameReady) {
            // (Re)build the scaler against the ACTUAL decoded frame's pixel
            // format and dimensions. pCodecCtx->pix_fmt can be AV_PIX_FMT_NONE
            // until the first frame is decoded, so a scaler built up-front (in
            // open(), before priming) is set up for the wrong format and emits
            // an all-black image. Building it here, from pNextFrameYUV, fixes
            // that and also handles mid-stream format changes.
            int srcFmt = (pNextFrameYUV->format != AV_PIX_FMT_NONE)
                    ? pNextFrameYUV->format
                    : (int) pCodecCtx->pix_fmt;
            if (pSWSCtx == nullptr || swsSrcFormat != srcFmt
                    || swsSrcW != pNextFrameYUV->width
                    || swsSrcH != pNextFrameYUV->height) {
                if (pSWSCtx != nullptr) {
                    sws_freeContext(pSWSCtx);
                    pSWSCtx = nullptr;
                }
                pSWSCtx = sws_getContext(pNextFrameYUV->width, pNextFrameYUV->height,
                        (AVPixelFormat) srcFmt, width, height, AV_PIX_FMT_RGB24,
                        SWS_LANCZOS, nullptr, nullptr, nullptr);
                swsSrcFormat = srcFmt;
                swsSrcW = pNextFrameYUV->width;
                swsSrcH = pNextFrameYUV->height;
            }
            if (pSWSCtx == nullptr) {
                return;
            }
            sws_scale(pSWSCtx, pNextFrameYUV->data, pNextFrameYUV->linesize, 0,
                    pNextFrameYUV->height, pFrameRGB->data, pFrameRGB->linesize);
            prevPTS = fbPTS;
            fbPTS = nextPTS;

            if (prevPTS > fbPTS) {
                prevPTS = fbPTS;
            }

            std::swap(pNextFrameYUV, pFrameYUV);
        }
    }

    void nextFrame() {
        //Decode until the codec hands us a complete frame
        for (;;) {
            int err = avcodec_receive_frame(pCodecCtx, pNextFrameYUV);
            if (err == 0) {
                int64_t pts = pNextFrameYUV->best_effort_timestamp;
                if (pts == AV_NOPTS_VALUE) {
                    pts = (pNextFrameYUV->pts != AV_NOPTS_VALUE) ? pNextFrameYUV->pts : lastDTS;
                }
                nextPTS = pts;
                frameReady = true;
                VS_LOG(trace, (boost::format("Decoded frame with pts %1%") % nextPTS));
                return;
            } else if (err == AVERROR_EOF) {
                throw VidFile::EndOfStreamException();
            } else if (err != AVERROR(EAGAIN)) {
                throw VidFile::FrameDecodeException("Error decoding frame");
            }

            //Codec needs more input
            if (draining) {
                throw VidFile::EndOfStreamException();
            }
            for (;;) {
                av_packet_unref(packet);
                if (av_read_frame(pFormatCtx, packet) < 0) {
                    //Signal end of input; the codec will flush remaining frames
                    avcodec_send_packet(pCodecCtx, nullptr);
                    draining = true;
                    break;
                }
                if (packet->stream_index != videoStreamIndex) {
                    continue;
                }
                if (packet->dts != AV_NOPTS_VALUE) {
                    lastDTS = packet->dts;
                }
                err = avcodec_send_packet(pCodecCtx, packet);
                if ((err < 0) && (err != AVERROR(EAGAIN))) {
                    throw VidFile::FrameDecodeException("Error sending packet to decoder");
                }
                break;
            }
        }
    }

public:
    float frameRate;
    float duration;
    uint8_t *_frameBuffer;
    uint8_t *frameBuffer;
    int64_t frameBufferStride;
    size_t frameBufferSize;
    size_t width;
    size_t height;

    VidFileImpl(size_t maxDimensions, bool forcePOT) :
            pFormatCtx(nullptr)
            , pCodecCtx(nullptr)
            , pCodec(nullptr)
            , pFrameRGB(nullptr)
            , pFrameYUV(nullptr)
            , pNextFrameYUV(nullptr)
            , pStream(nullptr)
            , packet(nullptr)
            , pSWSCtx(nullptr)
            , swsSrcFormat(-1)
            , swsSrcW(0)
            , swsSrcH(0)
            , videoStreamIndex(-1)
            , frameReady(false)
            , draining(false)
            , fbDimensionLimit(maxDimensions)
            , fbForcePOT(forcePOT)
            , fbPTS(0)
            , sizePTS(0)
            , prevPTS(0)
            , nextPTS(0)
            , lastDTS(0)
            , _frameBuffer(nullptr)
            , frameBuffer(nullptr) {
    }

    ~VidFileImpl() {
        if (_frameBuffer != nullptr) {
            delete[] _frameBuffer;
            _frameBuffer = nullptr;
            frameBuffer = nullptr;
        }
        if (pSWSCtx != nullptr) {
            sws_freeContext(pSWSCtx);
            pSWSCtx = nullptr;
        }
        if (packet != nullptr) {
            av_packet_free(&packet);
        }
        if (pFrameRGB != nullptr) {
            av_frame_free(&pFrameRGB);
        }
        if (pFrameYUV != nullptr) {
            av_frame_free(&pFrameYUV);
        }
        if (pNextFrameYUV != nullptr) {
            av_frame_free(&pNextFrameYUV);
        }
        if (pCodecCtx != nullptr) {
            avcodec_free_context(&pCodecCtx);
        }
        if (pFormatCtx != nullptr) {
            FFMpeg::closeVSFileFormat(pFormatCtx);
        }
    }

    void open(const std::string &path) {
        if (pCodecCtx != nullptr) {
            throw VidFile::Exception("Already open");
        }

        std::string errbase = std::string("Cannot open URL \"") + path + "\"";

        pFormatCtx = FFMpeg::openVSFileFormat(path, VideoFile);
        if (pFormatCtx == nullptr) {
            throw VidFile::FileOpenException(errbase + " (wrong format or)");
        }

        //Dump format info in case we want to know...
#ifdef VS_DEBUG
        av_dump_format(pFormatCtx, 0, path.c_str(), false);
#endif

        //Find first video stream
        videoStreamIndex = -1;
        VS_LOG(debug, (boost::format("Loaded %1%") % path));
        for (unsigned int i = 0; i < pFormatCtx->nb_streams; ++i) {
            const AVCodecParameters *codecpar = pFormatCtx->streams[i]->codecpar;
            VS_LOG(trace, (boost::format("  Stream %1%: type %2% (%3%) first dts %4%") % i %
                    ((codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
                            ? "Video"
                            : ((codecpar->codec_type == AVMEDIA_TYPE_AUDIO) ? "Audio" : "unk")) %
                    codecpar->codec_type % int64_t(pFormatCtx->streams[i]->start_time)));
            if ((videoStreamIndex < 0) && (codecpar->codec_type == AVMEDIA_TYPE_VIDEO)) {
                videoStreamIndex = i;
                pStream = pFormatCtx->streams[i];
            }
        }
        if (videoStreamIndex < 0) {
            throw VidFile::FileOpenException(errbase + " (no video stream)");
        }

        //Find codec for video stream and open it
        pCodec = avcodec_find_decoder(pStream->codecpar->codec_id);
        if (pCodec == nullptr) {
            throw VidFile::UnsupportedCodecException(errbase + " (unsupported codec)");
        }
        pCodecCtx = avcodec_alloc_context3(pCodec);
        if (pCodecCtx == nullptr) {
            throw VidFile::Exception("Problem during codec context initialization");
        }
        if (avcodec_parameters_to_context(pCodecCtx, pStream->codecpar) < 0) {
            throw VidFile::UnsupportedCodecException(errbase + " (unsupported codec)");
        }
        if (avcodec_open2(pCodecCtx, pCodec, nullptr) < 0) {
            throw VidFile::UnsupportedCodecException(errbase + " (unsupported codec)");
        }

        packet = av_packet_alloc();
        pFrameYUV = av_frame_alloc();
        pNextFrameYUV = av_frame_alloc();
        if ((packet == nullptr) || (pFrameYUV == nullptr) || (pNextFrameYUV == nullptr)) {
            throw VidFile::Exception(
                    "Problem during YUV framebuffer initialization");
        }

        //Get some info
        AVRational rate = av_guess_frame_rate(pFormatCtx, pStream, nullptr);
        if ((rate.num == 0) || (rate.den == 0)) {
            rate = pStream->r_frame_rate;
        }
        frameRate = (rate.den != 0) ? float(rate.num) / float(rate.den) : 25.0f;
        duration = (pStream->duration != AV_NOPTS_VALUE)
                ? float(pStream->duration * pStream->time_base.num) / float(pStream->time_base.den)
                : 0.0f;
        VS_LOG(trace, (boost::format("  Framerate: %1%/%2%") % rate.num % rate.den));
        VS_LOG(trace, (boost::format("  Stream timebase: %1%/%2%") % pStream->time_base.num % pStream->time_base.den));

        //Get POT dimensions
        if (fbForcePOT) {
            width = height = 1;
            while (width < (size_t) pCodecCtx->width && width <= (fbDimensionLimit / 2)) {
                width *= 2;
            }
            while (height < (size_t) pCodecCtx->height && height <= (fbDimensionLimit / 2)) {
                height *= 2;
            }
        } else {
            width = pCodecCtx->width;
            height = pCodecCtx->height;
            while ((width > fbDimensionLimit) || (height > fbDimensionLimit)) {
                width /= 2;
                height /= 2;
            }
        }
        VS_LOG(debug, (boost::format("  playing at %1%x%2%") % width % height));

        //Allocate RGB frame buffer
        pFrameRGB = av_frame_alloc();
        if (pFrameRGB == nullptr) {
            throw VidFile::Exception("Problem during RGB framebuffer initialization");
        }
        int bufferSize = av_image_get_buffer_size(AV_PIX_FMT_RGB24, width, height, 1);
        if (bufferSize < 0) {
            throw VidFile::Exception("Problem during RGB framebuffer initialization");
        }
        _frameBuffer = new uint8_t[bufferSize];
        if (av_image_fill_arrays(pFrameRGB->data, pFrameRGB->linesize,
                _frameBuffer, AV_PIX_FMT_RGB24, width, height, 1) < 0) {
            throw VidFile::Exception("Problem during RGB framebuffer initialization");
        }
        frameBuffer = pFrameRGB->data[0];
        frameBufferSize = pFrameRGB->linesize[0] * height;
        frameBufferStride = pFrameRGB->linesize[0];

        //Initialize timebase counters
        prevPTS = fbPTS = nextPTS = lastDTS = 0;

        // NOTE: the scaler (pSWSCtx) is built lazily in convertFrame() from the
        // first decoded frame's actual pixel format. Building it here from
        // pCodecCtx->pix_fmt is unreliable because that can be AV_PIX_FMT_NONE
        // until a frame has been decoded, which produced an all-black image.

        // Prime the first frame so the initial seek()/convertFrame() has real
        // decoded data instead of an empty YUV buffer (which rendered blank).
        try {
            nextFrame();
        } catch (const VidFile::EndOfStreamException &) {
            // empty/zero-frame video; leave frameReady false
        }
    }

    bool seek(float time) {
        if (time < 0) {
            time = 0;
        }

        //Translate float time to frametime
        int64_t targetPTS = int64_t(floor(double(time) * pStream->time_base.den / pStream->time_base.num));
        VS_LOG(trace, (boost::format("Seeking to %1$.3fs pts %2%") % time % targetPTS));
        if ((targetPTS >= prevPTS) && (targetPTS < nextPTS)) {
            //same frame
            if (targetPTS >= fbPTS) {
                try {
                    prevPTS = fbPTS;
                    convertFrame();
                    nextFrame();
                    return true;
                }
                catch (const VidFile::EndOfStreamException &e) {
                    sizePTS = fbPTS + 1;
                    throw e;
                }
            }
            return false;
        } else {
            if (targetPTS < fbPTS) {
                //frame backwards
                int64_t backPTS = targetPTS - 1 - pStream->time_base.den / pStream->time_base.num / 2;
                if (backPTS < 0) {
                    backPTS = 0;
                }

                VS_LOG(trace, (boost::format("backseeking to %1% (at %2%)") % backPTS % nextPTS));
                av_seek_frame(pFormatCtx, videoStreamIndex, backPTS, AVSEEK_FLAG_BACKWARD);
                avcodec_flush_buffers(pCodecCtx);
                draining = false;
                frameReady = false;

                prevPTS = backPTS;
                nextFrame();
            }
            //frame forward
            try {
                while (nextPTS < targetPTS) {
                    prevPTS = nextPTS;
                    nextFrame();
                    VS_LOG(trace, (boost::format("decoding to %1% (at %2%-%3%)")
                            % targetPTS
                            % prevPTS
                            % nextPTS));
                }
                convertFrame();
                nextFrame();
            }
            catch (const VidFile::EndOfStreamException &e) {
                sizePTS = fbPTS + 1;
                throw e;
            }

            return true;
        }
    }
};

#else /* !HAVE_FFMPEG */
class VidFileImpl {
private:
    VidFileImpl(size_t, bool) {
    }

public:
    //Avoid having to put ifdef's everywhere.
    float frameRate, duration;
    int width, height;
    void *frameBuffer;
    int frameBufferStride;

    bool seek(float time) {
        return false;
    }
};

#endif /* !HAVE_FFMPEG */

/* ************************************ */

VidFile::VidFile() :
        impl(nullptr) {
}

VidFile::~VidFile() {
    if (impl != nullptr) {
        delete impl;
        impl = nullptr;
    }
}

bool VidFile::isOpen() const {
    return impl != nullptr;
}

void VidFile::open(const std::string &path, size_t maxDimension, bool forcePOT) {
#ifdef HAVE_FFMPEG
    if (!impl) {
        impl = new VidFileImpl(maxDimension, forcePOT);
    }
    if (impl) {
        impl->open(path);
    }
#else
    throw VidFile::UnsupportedCodecException("Engine was built without video support (FFmpeg)");
#endif
}

void VidFile::close() {
    if (impl != nullptr) {
        delete impl;
        impl = nullptr;
    }
}

float VidFile::getFrameRate() const {
    return impl ? impl->frameRate : 0;
}

float VidFile::getDuration() const {
    return impl ? impl->duration : 0;
}

int VidFile::getWidth() const {
    return impl ? impl->width : 0;
}

int VidFile::getHeight() const {
    return impl ? impl->height : 0;
}

void *VidFile::getFrameBuffer() const {
    return impl ? impl->frameBuffer : 0;
}

int VidFile::getFrameBufferStride() const {
    return impl ? impl->frameBufferStride : 0;
}

bool VidFile::seek(float time) {
    return (impl != 0) && impl->seek(time);
}
