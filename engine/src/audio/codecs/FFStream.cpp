/*
 * FFStream.cpp
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
// C++ Implementation: Audio::FFStream
//
// Decoding uses the modern (FFmpeg 3.1+) avcodec_send_packet() /
// avcodec_receive_frame() API. Since modern decoders commonly output
// planar formats (e.g. fltp for mp3/aac), all output is converted to
// interleaved signed 16-bit via libswresample.
//


#ifdef HAVE_FFMPEG

#include "FFStream.h"

#include <utility>
#include <limits>
#include <math.h>
#include <assert.h>
#include <string.h>

#include "vsfilesystem.h"

#ifndef BUFFER_SIZE
// in samples
#define BUFFER_SIZE 4096
#endif

#ifndef BUFFER_ALIGNMENT
#define BUFFER_ALIGNMENT 0x20
#endif

#include "ffmpeg_init.h"

using namespace std;

namespace Audio {

/**
 * Packet decode exception
 * @remarks thrown when an attempt to decode a frame fails,
 *      forcing a discard of all remaining data.
 */
class PacketDecodeException : public Exception {
public:
    PacketDecodeException() {
    }

    PacketDecodeException(const CodecNotFoundException &other) : Exception(other) {
    }
};

namespace __impl {
struct FFData {
    AVFormatContext *pFormatCtx;
    AVCodecContext *pCodecCtx;
    const AVCodec *pCodec;
    AVStream *pStream;
    AVPacket *packet;
    AVFrame *frame;
    SwrContext *swrCtx;
    int streamIndex;
    bool draining;

    size_t sampleSize;
    size_t streamSize; // in samples

    void *sampleBufferBase;
    void *sampleBufferAligned;
    void *sampleBuffer;
    size_t sampleBufferSize; // in samples
    size_t sampleBufferAlloc; // in bytes
    size_t sampleBufferCapacity; // in samples
    uint64_t sampleBufferStart; // in samples

    std::string filepath;
    VSFileSystem::VSFileType filetype;
    int audioStreamIndex;

    FFData(const std::string &path, VSFileSystem::VSFileType type, Format &fmt, int streamIdx) :
            pFormatCtx(0),
            pCodecCtx(0),
            pCodec(0),
            pStream(0),
            packet(0),
            frame(0),
            swrCtx(0),
            streamIndex(-1),
            draining(false),
            sampleBufferBase(0),
            filepath(path),
            filetype(type),
            audioStreamIndex(streamIdx) {
        // Initialize libavcodec/libavformat if necessary
        FFMpeg::initLibraries();

        // Open file
        std::string errbase = std::string("Cannot open URL \"") + path + "\"";

        pFormatCtx = FFMpeg::openVSFileFormat(path, type);
        if (pFormatCtx == 0) {
            throw FileOpenException(errbase + " (wrong format or file not found)");
        }

        // Dump format info in case we want to know...
#ifdef VS_DEBUG
        av_dump_format(pFormatCtx, 0, path.c_str(), false);
#endif

        // Find the streamIdx-th audio stream
        int audioStreamsLeft = streamIdx;
        for (unsigned int i = 0; (streamIndex < 0) && (i < pFormatCtx->nb_streams); ++i) {
            if ((pFormatCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) && (audioStreamsLeft-- == 0)) {
                streamIndex = i;
                pStream = pFormatCtx->streams[i];
            }
        }
        if (streamIndex < 0) {
            throw FileOpenException(errbase + " (wrong or no audio stream)");
        }

        // Find codec for the audio stream and open it
        pCodec = avcodec_find_decoder(pStream->codecpar->codec_id);
        if (pCodec == 0) {
            throw CodecNotFoundException(errbase + " (unsupported codec)");
        }

        pCodecCtx = avcodec_alloc_context3(pCodec);
        if (pCodecCtx == 0) {
            throw CodecNotFoundException(errbase + " (cannot allocate codec context)");
        }
        if (avcodec_parameters_to_context(pCodecCtx, pStream->codecpar) < 0) {
            throw CodecNotFoundException(errbase + " (unsupported codec)");
        }
        if (avcodec_open2(pCodecCtx, pCodec, 0) < 0) {
            throw CodecNotFoundException(errbase + " (unsupported codec)");
        }

        packet = av_packet_alloc();
        frame = av_frame_alloc();
        if ((packet == 0) || (frame == 0)) {
            throw CodecNotFoundException(errbase + " (cannot allocate decode buffers)");
        }

        // All output is converted to interleaved signed 16-bit, which is
        // what the audio renderer expects, regardless of the codec's
        // native sample format (commonly planar float on modern FFmpeg).
        fmt.sampleFrequency = pCodecCtx->sample_rate;
        fmt.channels = pCodecCtx->ch_layout.nb_channels;
        fmt.nativeOrder = 1; // always so for ffmpeg
        fmt.bitsPerSample = 16;
        fmt.signedSamples = 1;

        if ((fmt.sampleFrequency <= 0) || (fmt.channels <= 0)) {
            throw CodecNotFoundException(errbase + " (unsupported audio format)");
        }

        AVChannelLayout outLayout;
        av_channel_layout_default(&outLayout, pCodecCtx->ch_layout.nb_channels);
        int swrErr = swr_alloc_set_opts2(&swrCtx,
                &outLayout, AV_SAMPLE_FMT_S16, pCodecCtx->sample_rate,
                &pCodecCtx->ch_layout, pCodecCtx->sample_fmt, pCodecCtx->sample_rate,
                0, 0);
        av_channel_layout_uninit(&outLayout);
        if ((swrErr < 0) || (swr_init(swrCtx) < 0)) {
            throw CodecNotFoundException(errbase + " (unsupported audio format)");
        }

        sampleSize = (fmt.bitsPerSample + 7) / 8 * fmt.channels;
        assert(sampleSize > 0);

        // Initialize timebase counter
        sampleBufferStart = 0;
        streamSize = 0;

        // Initialize sample buffer
        sampleBufferCapacity = BUFFER_SIZE;
        sampleBufferBase = malloc(sampleSize * sampleBufferCapacity + BUFFER_ALIGNMENT);
        ptrdiff_t offs = ((reinterpret_cast<ptrdiff_t>(sampleBufferBase)) & (BUFFER_ALIGNMENT - 1));
        sampleBufferAligned = ((char *) sampleBufferBase) + BUFFER_ALIGNMENT - offs;
        sampleBufferAlloc = sampleSize * sampleBufferCapacity;
        sampleBuffer = 0;
        sampleBufferSize = 0;
    }

    ~FFData() {
        // Free sample buffer
        if (sampleBufferBase != nullptr) {
            free(sampleBufferBase);
            sampleBufferBase = nullptr;
        }

        if (swrCtx != nullptr) {
            swr_free(&swrCtx);
        }

        if (frame != nullptr) {
            av_frame_free(&frame);
        }

        if (packet != nullptr) {
            av_packet_free(&packet);
        }

        // Close the codec
        if (pCodecCtx != nullptr) {
            avcodec_free_context(&pCodecCtx);
        }

        // Close the file
        if (pFormatCtx != nullptr) {
            FFMpeg::closeVSFileFormat(pFormatCtx);
        }
    }

    bool saneTimeStamps() const {
        return pStream->time_base.num != 0;
    }

    int64_t timeToPts(double time) const {
        return int64_t(floor(time * pStream->time_base.den / pStream->time_base.num));
    }

    double ptsToTime(int64_t pts) const {
        return double(pts) * pStream->time_base.num / pStream->time_base.den;
    }

    bool hasFrame() const {
        return sampleBuffer && sampleBufferSize;
    }

    void ensureBufferCapacity(size_t samples) {
        if (samples <= sampleBufferCapacity) {
            return;
        }
        free(sampleBufferBase);
        sampleBufferCapacity = samples;
        sampleBufferBase = malloc(sampleSize * sampleBufferCapacity + BUFFER_ALIGNMENT);
        ptrdiff_t offs = ((reinterpret_cast<ptrdiff_t>(sampleBufferBase)) & (BUFFER_ALIGNMENT - 1));
        sampleBufferAligned = ((char *) sampleBufferBase) + BUFFER_ALIGNMENT - offs;
        sampleBufferAlloc = sampleSize * sampleBufferCapacity;
    }

    void resetAfterSeek() {
        avcodec_flush_buffers(pCodecCtx);
        draining = false;
        sampleBuffer = 0;
        sampleBufferSize = 0;
    }

    /**
     * Decode the next audio frame into the sample buffer.
     * @throws EndOfStreamException at end of stream.
     */
    void decodeFrame() {
        for (;;) {
            int err = avcodec_receive_frame(pCodecCtx, frame);
            if (err == 0) {
                convertFrame();
                return;
            } else if (err == AVERROR_EOF) {
                throw EndOfStreamException();
            } else if (err != AVERROR(EAGAIN)) {
                throw PacketDecodeException();
            }

            // Codec needs more input
            if (draining) {
                throw EndOfStreamException();
            }
            for (;;) {
                av_packet_unref(packet);
                if (av_read_frame(pFormatCtx, packet) < 0) {
                    // Signal end of input; the codec will flush remaining frames
                    avcodec_send_packet(pCodecCtx, 0);
                    draining = true;
                    break;
                }
                if (packet->stream_index != streamIndex) {
                    continue;
                }
                err = avcodec_send_packet(pCodecCtx, packet);
                if ((err < 0) && (err != AVERROR(EAGAIN))) {
                    throw PacketDecodeException();
                }
                break;
            }
        }
    }

private:
    void convertFrame() {
        ensureBufferCapacity(frame->nb_samples);

        uint8_t *out[1] = {static_cast<uint8_t *>(sampleBufferAligned)};
        int converted = swr_convert(swrCtx,
                out, (int) sampleBufferCapacity,
                const_cast<const uint8_t **>(frame->extended_data), frame->nb_samples);
        if (converted < 0) {
            throw PacketDecodeException();
        }

        // Track stream position: prefer frame timestamps when available
        int64_t pts = frame->best_effort_timestamp;
        if (pts == AV_NOPTS_VALUE) {
            pts = frame->pts;
        }
        if (pts != AV_NOPTS_VALUE) {
            sampleBufferStart = uint64_t(floor(ptsToTime(pts) * pCodecCtx->sample_rate));
        } else {
            sampleBufferStart += sampleBufferSize;
        }

        sampleBuffer = sampleBufferAligned;
        sampleBufferSize = converted;

        if (sampleBufferStart + sampleBufferSize > streamSize) {
            streamSize = sampleBufferStart + sampleBufferSize;
        }
    }
};
}

FFStream::FFStream(const std::string &path, int streamIndex, VSFileSystem::VSFileType type)
        : Stream(path) {
    ffData = new __impl::FFData(path, type, getFormatInternal(), streamIndex);
}

FFStream::~FFStream() {
    // destructor closes the file already
    delete ffData;
}

double FFStream::getLengthImpl() const {
    return double(ffData->streamSize) / getFormat().sampleFrequency;
}

double FFStream::getPositionImpl() const {
    return double(ffData->sampleBufferStart) / getFormat().sampleFrequency;
}

void FFStream::seekImpl(double position) {
    if (position < 0) {
        position = 0;
    }

    // Translate float time to frametime
    uint64_t targetSample = uint64_t(position * getFormat().sampleFrequency);

    if ((targetSample >= ffData->sampleBufferStart)
            && (targetSample < ffData->sampleBufferStart + ffData->sampleBufferSize)) {
        // just skip data
        int advance = int(targetSample - ffData->sampleBufferStart);
        (char *&) ffData->sampleBuffer += ffData->sampleSize * advance;
        ffData->sampleBufferStart += advance;
        ffData->sampleBufferSize -= advance;
    } else {
        if (ffData->saneTimeStamps()) {
            // rough seek
            av_seek_frame(ffData->pFormatCtx, ffData->streamIndex, ffData->timeToPts(position),
                    (targetSample < ffData->sampleBufferStart + ffData->sampleBufferSize) ? AVSEEK_FLAG_BACKWARD : 0);
            ffData->resetAfterSeek();
        } else if (targetSample < ffData->sampleBufferStart) {
            // cannot seek but have to seek backwards, so...
            // ...close the file and reopen (yack)
            std::string path = ffData->filepath;
            VSFileSystem::VSFileType type = ffData->filetype;
            int streamIndex = ffData->audioStreamIndex;

            delete ffData;
            ffData = new __impl::FFData(path, type, getFormatInternal(), streamIndex);
        }

        // just skip data (big steps)
        do {
            nextBufferImpl();
        } while (targetSample >= ffData->sampleBufferStart + ffData->sampleBufferSize);

        // just skip data (small steps)
        int advance = int(targetSample - ffData->sampleBufferStart);
        (char *&) ffData->sampleBuffer += ffData->sampleSize * advance;
        ffData->sampleBufferStart += advance;
        ffData->sampleBufferSize -= advance;
    }
}

void FFStream::getBufferImpl(void *&buffer, unsigned int &bufferSize) {
    if (!ffData->hasFrame()) {
        throw NoBufferException();
    }

    buffer = ffData->sampleBuffer;
    bufferSize = ffData->sampleSize * ffData->sampleBufferSize;
}

void FFStream::nextBufferImpl() {
    ffData->decodeFrame();
}

}

#endif // HAVE_FFMPEG
