// JPEGsnoop - JPEG Image Decoder & Analysis Utility
// Copyright (C) 2018 - Calvin Hass
// http://www.impulseadventure.com/photo/jpeg-snoop.html
//
//    This program is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 2 of the License, or
//    (at your option) any later version.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

// ==========================================================================
// CLASS DESCRIPTION:
// - Provides a cache for file access
// - Allows random access to a file but only issues new file I/O if
//   the requested address is outside of the current cache window
// - Provides an overlay for temporary (local) buffer overwrites
// - Buffer search methods
//
// ==========================================================================

#pragma once

#ifndef JPEGSNOOP_WINDOWBUF_H
#define JPEGSNOOP_WINDOWBUF_H

#include <QFile>
#include <QString>

#include "log/ILog.h"

// For now, we only ever use MAX_BUF_WINDOW bytes, even though we
// have allocated MAX_BUF bytes up front. I might change this
// later. We don't want the window size to be too large as it
// could have an impact on performance.
#define MAX_BUF            262144
#define MAX_BUF_WINDOW     131072
#define MAX_BUF_WINDOW_REV 16384  //1024L

#define NUM_OVERLAYS       500
#define MAX_OVERLAY        500     // 500 bytes

#define NUM_HOLES          10

#define    MAX_BUF_READ_STR 255     // Max number of bytes to fetch in BufReadStr()

struct Overlay {
    bool enabled;               // Enabled? -- not used currently
    uint32_t start;             // File position
    uint32_t len;               // MCU Length
    uint8_t data[MAX_OVERLAY];  // Byte data

    // For reporting purposes:
    uint32_t mcuX;               // Starting MCU X
    uint32_t mcuY;               // Starting MCU Y
    uint32_t mcuLen;             // Number of MCUs deleted
    uint32_t mcuLenIns;          // Number of MCUs inserted
    int dcAdjustY;
    int dcAdjustCb;
    int dcAdjustCr;
};

class WindowBuf {
    Q_DISABLE_COPY(WindowBuf)
public:
    explicit WindowBuf(ILog &log);
    ~WindowBuf();

    bool isBufferOk() const;
    qint64 fileSize() const;
    qint64 position() const;

    void setFile(QFile *file);
    void unsetFile();
    bool loadWindow(qint64 position);

    uint8_t getByte(uint32_t offset, bool clean = false);
    uint32_t getDataX(uint32_t offset, uint32_t size, bool byteSwap = false);

    unsigned char getData1(uint32_t &offset, bool byteSwap);
    uint16_t getData2(uint32_t &offset, bool byteSwap);
    uint32_t getData4(uint32_t &offset, bool byteSwap);

    QString readStr(uint32_t nPosition);
    QString readUniStr(uint32_t nPosition);
    QString readUniStr2(uint32_t nPos, uint32_t nBufLen);
    QString readStrN(uint32_t nPosition, uint32_t nLen);

    bool search(uint32_t startPosition, uint32_t searchValue, uint32_t searchLength, bool forward, uint32_t &foundPosition);
    bool searchX(uint32_t nStartPos, uint8_t *anSearchVal, uint32_t nSearchLen, bool bDirFwd, uint32_t &nFoundPos);

    bool overlayAlloc(uint32_t nInd);
    bool overlayInstall(uint32_t nOvrInd, uint8_t *pOverlay, uint32_t nLen, uint32_t nBegin,
                        uint32_t nMcuX, uint32_t nMcuY, uint32_t nMcuLen, uint32_t nMcuLenIns, int nAdjY, int nAdjCb,
                        int nAdjCr);
    void overlayRemove();
    void overlayRemoveAll();
    bool overlayGet(uint32_t nOvrInd, uint8_t *&pOverlay, uint32_t &nLen, uint32_t &nBegin);
    uint32_t overlayGetNum();
    void reportOverlays(ILog &log);

private:
    void reset();

    ILog &_log;
    unsigned char *_buf;

    QFile *_file = nullptr;

    bool _bufOk = false;
    qint64 _position = 0;
    qint64 _fileSize = 0;
    uint32_t _bufWinSize = 0;
    uint32_t _bufWinStart = 0;

    uint32_t _overlayMax;       // Number of overlays allocated (limited by mem)
    uint32_t _overlayNum;
    Overlay *_overlays[NUM_OVERLAYS]{};
};

#endif
