/*
 * ffmpeg_init.cpp
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
//C++ Implementation: FFMpeg initialization and custom I/O
//

#include "vsfilesystem.h"
#include "vs_logging.h"
#include "ffmpeg_init.h"

#include <string.h>
#include <utility>

//define a 128k buffer for video streamers
#define BUFFER_SIZE (128 * (1 << 10))

#ifndef ENOENT
#define ENOENT 2
#endif

/*
 * FOLLOWING CODE IS ONLY INCLUDED IF YOU HAVE FFMPEG
 * ********************************************
 */
#ifdef HAVE_FFMPEG

using namespace VSFileSystem;

// Custom AVIO callbacks backed by VSFile. This replaces the legacy
// URLProtocol-based "vsfile:" scheme: modern FFmpeg no longer allows
// applications to register custom protocols, and instead expects them
// to supply an AVIOContext with read/seek callbacks.

extern "C" {

static int vsfile_read(void *opaque, uint8_t *buf, int buf_size) {
    VSFile *f = static_cast<VSFile *>(opaque);
    int bytesRead = static_cast<int>(f->Read(buf, buf_size));
    if (bytesRead <= 0) {
        // Returning 0 from a read callback is deprecated and treated as
        // an error by modern FFmpeg; signal a proper end-of-file.
        return AVERROR_EOF;
    }
    return bytesRead;
}

static int64_t vsfile_seek(void *opaque, int64_t pos, int whence) {
    VSFile *f = static_cast<VSFile *>(opaque);
    switch (whence & ~AVSEEK_FORCE) {
        case AVSEEK_SIZE:
            return f->Size();
        case SEEK_SET:
            f->GoTo(static_cast<long>(pos));
            break;
        case SEEK_CUR:
            f->GoTo(static_cast<long>(f->GetPosition() + pos));
            break;
        case SEEK_END:
            f->GoTo(static_cast<long>(f->Size() + pos));
            break;
        default:
            return AVERROR(EINVAL);
    }
    return f->GetPosition();
}

} // extern "C"

namespace FFMpeg {

void initLibraries() {
    // Modern FFmpeg (4.0+) needs no explicit registration:
    // av_register_all() and custom URLProtocol registration are gone.
    // Kept as an entry point for any future one-time setup.
}

AVFormatContext *openVSFileFormat(const std::string &path, VSFileSystem::VSFileType fileType) {
    initLibraries();

    VSFile *f = new VSFile();
    if (f->OpenReadOnly(path, fileType) > Ok) {
        delete f;
        VS_LOG(warning, (boost::format("FFMpeg: cannot open file %1%") % path));
        return nullptr;
    }

    unsigned char *ioBuffer = static_cast<unsigned char *>(av_malloc(BUFFER_SIZE));
    if (ioBuffer == nullptr) {
        delete f;
        return nullptr;
    }

    AVIOContext *ioCtx = avio_alloc_context(
            ioBuffer, BUFFER_SIZE,
            0 /* read-only */,
            f /* opaque */,
            &vsfile_read,
            nullptr /* no write */,
            &vsfile_seek);
    if (ioCtx == nullptr) {
        av_free(ioBuffer);
        delete f;
        return nullptr;
    }

    AVFormatContext *formatCtx = avformat_alloc_context();
    if (formatCtx == nullptr) {
        av_free(ioCtx->buffer);
        avio_context_free(&ioCtx);
        delete f;
        return nullptr;
    }
    formatCtx->pb = ioCtx;
    formatCtx->flags |= AVFMT_FLAG_CUSTOM_IO;

    // Pass the real filename so libavformat can use the extension as a
    // format hint (useful for extension-probed formats).
    if (avformat_open_input(&formatCtx, path.c_str(), nullptr, nullptr) != 0) {
        // On failure avformat_open_input frees formatCtx but not our custom pb.
        av_free(ioCtx->buffer);
        avio_context_free(&ioCtx);
        delete f;
        VS_LOG(warning, (boost::format("FFMpeg: cannot open %1% (unrecognized format)") % path));
        return nullptr;
    }

    if (avformat_find_stream_info(formatCtx, nullptr) < 0) {
        closeVSFileFormat(formatCtx);
        VS_LOG(warning, (boost::format("FFMpeg: cannot find stream info in %1%") % path));
        return nullptr;
    }

    return formatCtx;
}

void closeVSFileFormat(AVFormatContext *&formatCtx) {
    if (formatCtx == nullptr) {
        return;
    }
    AVIOContext *ioCtx = formatCtx->pb;
    avformat_close_input(&formatCtx);
    if (ioCtx != nullptr) {
        delete static_cast<VSFile *>(ioCtx->opaque);
        av_free(ioCtx->buffer);
        avio_context_free(&ioCtx);
    }
}

} // namespace FFMpeg

#else //No FFMPEG

namespace FFMpeg {
void initLibraries() {
    //No-op stub
}
}

#endif
