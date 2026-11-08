/*
 * ffmpeg_init.h
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
#ifndef VEGA_STRIKE_ENGINE_FFMPEG_INIT_H
#define VEGA_STRIKE_ENGINE_FFMPEG_INIT_H

//
//C++ Interface: FFMpeg initialization and custom I/O
//

namespace FFMpeg {
void initLibraries();
}

#ifdef HAVE_FFMPEG

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

// This code requires the send/receive decoding API (FFmpeg 3.1+,
// libavcodec 57.48.101). Everything older is long past end-of-life.
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(57, 48, 101)
#error "Vega Strike requires FFmpeg 3.1 or newer"
#endif

#include "vsfilesystem.h"

namespace FFMpeg {

/**
 * Open a file through VSFileSystem for use with libavformat.
 *
 * Returns an AVFormatContext whose I/O is backed by a VSFile (replacing
 * the legacy "vsfile:" URLProtocol, which modern FFmpeg no longer
 * supports). The header (stream info) is already read.
 *
 * Returns nullptr on failure (file not found / not a recognized format).
 * The result must be released with closeVSFileFormat().
 */
AVFormatContext *openVSFileFormat(const std::string &path, VSFileSystem::VSFileType fileType);

/**
 * Close an AVFormatContext obtained from openVSFileFormat(), releasing
 * the underlying VSFile and custom I/O buffers.
 */
void closeVSFileFormat(AVFormatContext *&formatCtx);

}

#endif //HAVE_FFMPEG

#endif //VEGA_STRIKE_ENGINE_FFMPEG_INIT_H
