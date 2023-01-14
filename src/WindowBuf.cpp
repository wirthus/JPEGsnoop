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

#include <stdexcept>

#include "WindowBuf.h"

WindowBuf::WindowBuf(ILog &log) :
    _log(log) {

    _buf = new unsigned char[MAX_BUF];

    reset();

    // Initialize all overlays as not defined.
    // Only create space for them as required
    _overlayNum = 0;
    _overlayMax = 0;

    for (uint32_t nInd = 0; nInd < NUM_OVERLAYS; nInd++) {
        _overlays[nInd] = nullptr;
    }
}

WindowBuf::~WindowBuf() {
    delete[] _buf;
    _buf = nullptr;

    _bufOk = false;

    // Clear up overlays
    for (uint32_t nInd = 0; nInd < NUM_OVERLAYS; nInd++) {
        if (_overlays[nInd]) {
            delete _overlays[nInd];

            _overlays[nInd] = nullptr;
        }
    }
}

void WindowBuf::reset() {
    _bufOk = false;
    unsetFile();
}

bool WindowBuf::isBufferOk() const {
    return _bufOk;
}

qint64 WindowBuf::fileSize() const {
    return _fileSize;
}

qint64 WindowBuf::position() const {
    return _position;
}

void WindowBuf::setFile(QFile *file) {
    if (_file == file) return;
    _file = file;

    _fileSize = _file->size();
    _bufOk = false;
}

void WindowBuf::unsetFile() {
    _file = nullptr;
    _fileSize = 0;
    _bufOk = false;
}

bool WindowBuf::loadWindow(qint64 position) {
    const qint64 positionAdj = position >= MAX_BUF_WINDOW_REV ? position - MAX_BUF_WINDOW_REV : 0;
    if (_position == positionAdj && _bufOk) return true;

    if (!_file) return false;

    _position = positionAdj;
    _bufOk = false;
    _bufWinSize = 0;
    _bufWinStart = 0;

    if (positionAdj >= _fileSize) return false;
    if (!_file->seek(positionAdj)) return false;
    const auto readBytes = _file->read(reinterpret_cast<char *>(_buf), MAX_BUF_WINDOW);

    if (readBytes == 0) return false;

    _bufOk = true;
    _bufWinStart = positionAdj;
    _bufWinSize = readBytes;

    return true;
}

// Search for a value in the buffer from a given starting position
// and direction, limited to a maximum search depth
// - Search value can be 8-bit, 16-bit or 32-bit
// - Update progress in lengthy searches
//
// INPUT:
// - nStartPos                  Starting byte offset for search
// - nSearchVal                 Value to search for (up to 32-bit unsigned)
// - nSearchLen                 Maximum number of bytes to search
// - bDirFwd                    TRUE for forward, FALSE for backwards
//
// PRE:
// - m_nPosEof
//
// OUTPUT:
// - nFoundPos                  Byte offset in buffer for start of search match
//
// RETURN:
// - Success in finding the value
//
bool WindowBuf::search(uint32_t startPosition, uint32_t searchValue, uint32_t searchLength, bool forward, uint32_t &foundPosition) {
    if (searchLength < 1 || searchLength > 4) throw std::logic_error("SearchLength out of range.");

    auto currentPos = startPosition;

    uint32_t currentValue;
    while (currentPos + searchLength < _fileSize) {
        // Update progress in status bar
        // - Note that we only check timer when step counter has
        //   reached a certain threshold. This limits the overhead
        //   associated with the timer comparison
        // if (((curPos % (16 * 1024)) == 0)) {
        //     time_t tmNow = clock();
        //
        //     if ((tmNow - tmLast) > (CLOCKS_PER_SEC / 8)) {
        //         tmLast = tmNow;
        //         // const auto fProgress = (curPos * 100.0f) / _posEof;
        //         // strStatus = QString("Searching %3.f%% (%lu of %lu)...").arg(fProgress).arg(nCurPos).arg(_posEof);
        //         // m_pStatBar->showMessage(strStatus);
        //     }
        // }

        if (searchLength == 4) {
            currentValue = (getByte(currentPos + 0) << 24) + (getByte(currentPos + 1) << 16) + (getByte(currentPos + 2) << 8) + getByte(currentPos + 3);
        } else if (searchLength == 3) {
            currentValue = (getByte(currentPos + 0) << 16) + (getByte(currentPos + 1) << 8) + getByte(currentPos + 2);
        } else if (searchLength == 2) {
            currentValue = (getByte(currentPos + 0) << 8) + getByte(currentPos + 1);
        } else if (searchLength == 1) {
            currentValue = getByte(currentPos + 0);
        } else {
            return false;
        }

        if (currentValue == searchValue) {
            foundPosition = currentPos;
            return true;
        }

        if (forward) {
            if (currentPos < _fileSize) currentPos++;
            else return false;
        } else {
            if (currentPos > 0) currentPos--;
            else return false;
        }
    }

    return false;
}

// Search for a variable-length byte string in the buffer from a given starting position
// and direction, limited to a maximum search depth
// - Search string is array of unsigned bytes
// - Update progress in lengthy searches
//
// INPUT:
// - nStartPos                  Starting byte offset for search
// - anSearchVal                Byte array to search for
// - nSearchLen                 Maximum number of bytes to search
// - bDirFwd                    TRUE for forward, FALSE for backwards
//
// PRE:
// - m_nPosEof
// - m_pStatBar
//
// OUTPUT:
// - nFoundPos                  Byte offset in buffer for start of search match
//
// RETURN:
// - Success in finding the value
//
bool WindowBuf::searchX(uint32_t nStartPos, unsigned char *anSearchVal, uint32_t nSearchLen, bool bDirFwd, uint32_t &nFoundPos) {
    // Save the current position
    uint32_t nCurPos;

    uint32_t nByteCur;
    uint32_t nByteSearch;

    uint32_t nCurPosOffset;
    uint32_t nMatchStartPos = 0;

    //bool                  bMatchStart = false;
    bool bMatchOn = false;

    // QString strStatus;
    // time_t tmLast = clock();

    nCurPosOffset = 0;
    nCurPos = nStartPos;
    bool bDone = false;

    bool bFound = false;          // Matched entire search string

    while (!bDone) {

        if (bDirFwd) {
            nCurPos++;

            if (nCurPos + (nSearchLen - 1) >= _fileSize) {
                bDone = true;
            }
        } else {
            if (nCurPos > 0) {
                nCurPos--;
            } else {
                bDone = true;
            }
        }

        // Update progress in status bar
        // - Note that we only check timer when step counter has
        //   reached a certain threshold. This limits the overhead
        //   associated with the timer comparison
        // if (((nCurPos % (16 * 1024)) == 0)) {
        //     time_t tmNow = clock();
        //
        //     if ((tmNow - tmLast) > (CLOCKS_PER_SEC / 8)) {
        //         tmLast = tmNow;
        //         const auto fProgress = (nCurPos * 100.0f) / _fileSize;
        //
        //         // strStatus = QString("Searching %3.f%% (%lu of %lu)...").arg(fProgress).arg(nCurPos).arg(_fileSize);
        //         // m_pStatBar->showMessage(strStatus);
        //     }
        // }

        nByteSearch = anSearchVal[nCurPosOffset];
        nByteCur = getByte(nCurPos);

        if (nByteSearch == nByteCur) {
            if (!bMatchOn) {
                // Since we aren't in match mode, we are beginning a new
                // sequence, so save the starting position in case we
                // have to rewind
                nMatchStartPos = nCurPos;
                bMatchOn = true;
            }

            nCurPosOffset++;
        } else {
            if (bMatchOn) {
                // Since we were in a sequence of matches, but ended early,
                // we now need to reset our position to just after the start
                // of the previous 1st match.
                nCurPos = nMatchStartPos;
                bMatchOn = false;
            }

            nCurPosOffset = 0;
        }

        if (nCurPosOffset >= nSearchLen) {
            // We matched the entire length of our search string!
            bFound = true;
            bDone = true;
        }
    }

    // if (m_pStatBar) {
    //     m_pStatBar->showMessage("Done");
    // }

    if (bFound) {
        nFoundPos = nMatchStartPos;
    }

    return bFound;
}

// Allocate a new buffer overlay into the array
// over overlays. Limits the number of overlays
// to NUM_OVERLAYS.
// - If the indexed overlay already exists, no changes are made
// - TODO: Replace with vector
//
// INPUT:
// - nInd               Overlay index to allocate
//
// POST:
// - m_psOverlay[]
//
bool WindowBuf::overlayAlloc(uint32_t nInd) {
    if (nInd >= NUM_OVERLAYS) {
        _log.error("ERROR: Maximum number of overlays reached");
        return false;

    } else if (_overlays[nInd]) {
        // Already allocated, move on
        return true;

    } else {
        _overlays[nInd] = new Overlay();

        if (!_overlays[nInd]) {
            _log.error("NOTE: Out of memory for extra file overlays");
            return false;
        } else {
            memset(_overlays[nInd], 0, sizeof(Overlay));
            // FIXME: may not be necessary
            _overlays[nInd]->enabled = false;
            _overlays[nInd]->start = 0;
            _overlays[nInd]->len = 0;

            _overlays[nInd]->mcuX = 0;
            _overlays[nInd]->mcuY = 0;
            _overlays[nInd]->mcuLen = 0;
            _overlays[nInd]->mcuLenIns = 0;
            _overlays[nInd]->dcAdjustY = 0;
            _overlays[nInd]->dcAdjustCb = 0;
            _overlays[nInd]->dcAdjustCr = 0;

            // FIXME: Need to ensure that this is right
            if (nInd + 1 >= _overlayMax) {
                _overlayMax = nInd + 1;
            }
            //m_nOverlayMax++;

            return true;
        }
    }
}

// Report out the list of overlays thave have been allocated
//
// PRE:
// - m_nOverlayNum
// - m_psOverlay[]
//
void WindowBuf::reportOverlays(ILog &pLog) {
    QString strTmp;

    if (_overlayNum > 0) {
        strTmp = QString("  Buffer Overlays active: %1").arg(_overlayNum);
        pLog.info(strTmp);

        for (uint32_t ind = 0; ind < _overlayNum; ind++) {
            if (_overlays[ind]) {
                strTmp =
                    QString(
                        "    %03u: MCU[%4u,%4u] MCU DelLen=[%2u] InsLen=[%2u] DC Offset YCC=[%5d,%5d,%5d] Overlay Byte Len=[%4u]").
                        arg(ind).arg(_overlays[ind]->mcuX).arg(_overlays[ind]->mcuY).arg(_overlays[ind]->mcuLen).arg(
                        _overlays[ind]->
                            mcuLenIns).
                        arg(_overlays[ind]->dcAdjustY).arg(_overlays[ind]->dcAdjustCb).arg(_overlays[ind]->dcAdjustCr).
                        arg(_overlays[ind]->len);
                pLog.info(strTmp);
            }
        }

        pLog.info("");
    }
}

// Define the content of an overlay
//
// INPUT:
// - nOvrInd                    The overlay index to update/replace
// - pOverlay                   The byte array that defines the overlay content
// - nLen                               Byte length of the overlay
// - nBegin                             Starting byte offset for the overlay
// - nMcuX                              Additional info for this overlay
// - nMcuY                              Additional info for this overlay
// - nMcuLen                    Additional info for this overlay
// - nMcuLenIns                 Additional info for this overlay
// - nAdjY                              Additional info for this overlay
// - nAdjCb                             Additional info for this overlay
// - nAdjCr                             Additional info for this overlay
//
bool WindowBuf::overlayInstall(uint32_t nOvrInd, unsigned char *pOverlay, uint32_t nLen, uint32_t nBegin,
                               uint32_t nMcuX, uint32_t nMcuY, uint32_t nMcuLen, uint32_t nMcuLenIns,
                               int nAdjY, int nAdjCb, int nAdjCr) {
    nOvrInd;                      // Unreferenced param

    // Ensure that the overlay is allocated, and allocate it
    // if required. Fail out if we can't add (or run out of space)
    if (!overlayAlloc(_overlayNum)) {
        return false;
    }

    if (nLen < MAX_OVERLAY) {
        _overlays[_overlayNum]->enabled = true;
        _overlays[_overlayNum]->len = nLen;

        // Copy the overlay content, but clip to maximum size and pad if shorter
        for (uint32_t i = 0; i < MAX_OVERLAY; i++) {
            _overlays[_overlayNum]->data[i] = (i < nLen) ? pOverlay[i] : 0x00;
        }

        _overlays[_overlayNum]->start = nBegin;

        // For reporting, save the extra data
        _overlays[_overlayNum]->mcuX = nMcuX;
        _overlays[_overlayNum]->mcuY = nMcuY;
        _overlays[_overlayNum]->mcuLen = nMcuLen;
        _overlays[_overlayNum]->mcuLenIns = nMcuLenIns;
        _overlays[_overlayNum]->dcAdjustY = nAdjY;
        _overlays[_overlayNum]->dcAdjustCb = nAdjCb;
        _overlays[_overlayNum]->dcAdjustCr = nAdjCr;

        _overlayNum++;
    } else {
        _log.error("ERROR: CwindowBuf:overlayInstall() overlay too large");
        return false;
    }

    return true;
}

// Remove latest overlay entry
//
// POST:
// - m_nOverlayNum
// - m_psOverlay[]
//
void WindowBuf::overlayRemove() {
    if (_overlayNum <= 0) {
        return;
    }

    _overlayNum--;

    // Note that we've already decremented the m_nOverlayNum
    if (_overlays[_overlayNum]) {
        // Don't need to delete the overlay struct as we might as well reuse it
        _overlays[_overlayNum]->enabled = false;
        //delete m_psOverlay[m_nOverlayNum];
        //m_psOverlay[m_nOverlayNum] = nullptr;
    }
}

// Disable all buffer overlays
//
// POST:
// - m_nOverlayNum
// - m_psOverlay[]
//
void WindowBuf::overlayRemoveAll() {
    _overlayNum = 0;

    for (uint32_t nInd = 0; nInd < _overlayMax; nInd++) {
        if (_overlays[nInd]) {
            _overlays[nInd]->enabled = false;
        }
    }
}

// Fetch the indexed buffer overlay
//
// INPUT:
// - nOvrInd            The overlay index
//
// OUTPUT:
// - pOverlay           A pointer to the indexed buffer
// - nLen                       Length of the overlay string
// - nBegin                     Starting file offset for the overlay
//
// RETURN:
// - Success if overlay index was allocated and enabled
//
bool WindowBuf::overlayGet(uint32_t nOvrInd, unsigned char *&pOverlay, uint32_t &nLen, uint32_t &nBegin) {
    if ((_overlays[nOvrInd]) && (_overlays[nOvrInd]->enabled)) {
        pOverlay = _overlays[nOvrInd]->data;
        nLen = _overlays[nOvrInd]->len;
        nBegin = _overlays[nOvrInd]->start;
        return _overlays[nOvrInd]->enabled;
    } else {
        return false;
    }
}

// Get the number of buffer overlays allocated
//
uint32_t WindowBuf::overlayGetNum() {
    return _overlayNum;
}

// Replaces the direct buffer access with a managed refillable window/cache.
// - Support for 1-byte access only
// - Support for overlays (optional)
//
// INPUT:
// - nOffset                    File offset to fetch from (via cache)
// - bClean                             Flag that indicates if overlays can be used
//                      If set to FALSE, then content from overlays that span
//                      the offset address will be returned instead of the file content
//
// RETURN:
// - Byte from the desired address
uint8_t WindowBuf::getByte(uint32_t offset, bool clean) {
    // We are requesting address "nOffset"
    // Our current window runs from "m_nBufWinStart...buf_win_end" (m_nBufWinSize)
    // Therefore, our relative addr is nOffset-m_nBufWinStart

    long nWinRel;

    unsigned char currentValue = 0;

    Q_ASSERT(_file);

    // Allow for overlay buffer capability (if not in "clean" mode)
    if (!clean) {
        auto inOverlayWindow = false;

        // Now handle any overlays
        for (uint32_t nInd = 0; nInd < _overlayNum; nInd++) {
            if (_overlays[nInd]) {
                if (_overlays[nInd]->enabled) {
                    if ((offset >= _overlays[nInd]->start) &&
                        (offset < _overlays[nInd]->start + _overlays[nInd]->len)) {
                        currentValue = _overlays[nInd]->data[offset - _overlays[nInd]->start];
                        inOverlayWindow = true;
                    }
                }
            }
        }

        if (inOverlayWindow) {
            // Before we return, make sure that the real buffer handles this region!
            nWinRel = offset - _bufWinStart;

            if ((nWinRel >= 0) && (nWinRel < (long) _bufWinSize)) {
            } else {
                // Address is outside of current window
                if (!loadWindow(offset)) {
                    _bufOk = false;
                    return 0;
                }
            }

            return currentValue;
        }
    }

    // Now that we've finished any overlays, proceed to actual cache content

    // Determine if the offset is within the current cache
    // If not, reload a new cache around the desired address
    nWinRel = offset - _bufWinStart;

    if ((nWinRel >= 0) && (nWinRel < (long) _bufWinSize)) {
        // Address is within current window
        return _buf[nWinRel];
    } else {
        // Address is outside of current window
        if (!loadWindow(offset)) {
            _bufOk = false;
            return 0;
        }

        // Now we assume that the address is in range
        // m_nBufWinStart has now been updated
        // TODO: check this
        nWinRel = offset - _bufWinStart;

        // Now recheck the window
        // TODO: Rewrite the following in a cleaner manner
        if ((nWinRel >= 0) && (nWinRel < (long) _bufWinSize)) {
            return _buf[nWinRel];
        } else {
            // Still bad after refreshing window, so it must be bad addr
            _bufOk = false;
            // FIXME: Need to report error somehow
            //log->AddLine("ERROR: Overhead buffer - file may be truncated"),9);
            return 0;
        }
    }
}

// Replaces the direct buffer access with a managed refillable window/cache.
// - Supports 1/2/4 byte fetch
// - No support for overlays
//
// INPUT:
// - nOffset                    File offset to fetch from (via cache)
// - nSz                                Size of word to fetch (1,2,4)
// - nByteSwap                  Flag to indicate if UINT16 or UINT32 should be byte-swapped
//
// RETURN:
// - 1/2/4 unsigned bytes from the desired address
//
uint32_t WindowBuf::getDataX(uint32_t offset, uint32_t size, bool byteSwap) {
    long nWinRel;

    Q_ASSERT(_file);

    nWinRel = offset - _bufWinStart;
    if ((nWinRel >= 0) && (nWinRel + size < _bufWinSize)) {
        // Address is within current window
        if (!byteSwap) {
            if (size == 4) {
                return ((_buf[nWinRel + 0] << 24) + (_buf[nWinRel + 1] << 16) +
                        (_buf[nWinRel + 2] << 8) +
                        (_buf[nWinRel + 3]));
            } else if (size == 2) {
                return ((_buf[nWinRel + 0] << 8) + (_buf[nWinRel + 1]));
            } else if (size == 1) {
                return (_buf[nWinRel + 0]);
            } else {
                _log.error("ERROR: getDataX() with bad size");
                return 0;
            }
        } else {
            if (size == 4) {
                return ((_buf[nWinRel + 3] << 24) + (_buf[nWinRel + 2] << 16) +
                        (_buf[nWinRel + 1] << 8) +
                        (_buf[nWinRel + 0]));
            } else if (size == 2) {
                return ((_buf[nWinRel + 1] << 8) + (_buf[nWinRel + 0]));
            } else if (size == 1) {
                return (_buf[nWinRel + 0]);
            } else {
                _log.error("ERROR: getDataX() with bad size");
                return 0;
            }
        }
    } else {
        // Address is outside of current window
        if (!loadWindow(offset)) {
            _bufOk = false;
            return 0;
        }

        // Now we assume that the address is in range
        // m_nBufWinStart has now been updated
        // TODO: Check this
        nWinRel = offset - _bufWinStart;

        // Now recheck the window
        // TODO: Rewrite the following in a cleaner manner
        if ((nWinRel >= 0) && (nWinRel + size < _bufWinSize)) {
            if (!byteSwap) {
                if (size == 4) {
                    return ((_buf[nWinRel + 0] << 24) + (_buf[nWinRel + 1] << 16) +
                            (_buf[nWinRel + 2] << 8) +
                            (_buf[nWinRel + 3]));
                } else if (size == 2) {
                    return ((_buf[nWinRel + 0] << 8) + (_buf[nWinRel + 1]));
                } else if (size == 1) {
                    return (_buf[nWinRel + 0]);
                } else {
                    _log.error("ERROR: getDataX() with bad size");
                    return 0;
                }
            } else {
                if (size == 4) {
                    return ((_buf[nWinRel + 3] << 24) + (_buf[nWinRel + 2] << 16) +
                            (_buf[nWinRel + 1] << 8) +
                            (_buf[nWinRel + 0]));
                } else if (size == 2) {
                    return ((_buf[nWinRel + 1] << 8) + (_buf[nWinRel + 0]));
                } else if (size == 1) {
                    return (_buf[nWinRel + 0]);
                } else {
                    _log.error("ERROR: getDataX() with bad size");
                    return 0;
                }
            }
        } else {
            // Still bad after refreshing window, so it must be bad addr
            _bufOk = false;
            // FIXME: Need to report error somehow
            //log->AddLine("ERROR: Overread buffer - file may be truncated"),9);
            return 0;
        }
    }
}

unsigned char WindowBuf::getData1(uint32_t &offset, bool byteSwap) {
    const auto result = static_cast<unsigned char>(getDataX(offset, 1, byteSwap));
    offset += 1;

    return result;
}

uint16_t WindowBuf::getData2(uint32_t &offset, bool byteSwap) {
    const auto result = static_cast<uint16_t>(getDataX(offset, 2, byteSwap));
    offset += 2;

    return result;
}

uint32_t WindowBuf::getData4(uint32_t &offset, bool byteSwap) {
    const auto result = getDataX(offset, 4, byteSwap);
    offset += 4;

    return result;
}

// Read a null-terminated string from the buffer/cache at the
// indicated file offset.
// - Does not affect the current file pointer nPosition
// - String length is limited by encountering either the NULL character
//   of exceeding the maximum length of MAX_BUF_READ_STR
//
// INPUT:
// - nPosition                  File offset to start string fetch
//
// RETURN:
// - String fetched from file
//
QString WindowBuf::readStr(uint32_t nPosition) {
    // Try to read a NULL-terminated string from file offset "nPosition"
    // up to a maximum of MAX_BUF_READ_STR bytes. Result is max length MAX_BUF_READ_STR
    QString strRd = "";

    unsigned char cRd;

    bool bDone = false;

    uint32_t nIndex = 0;

    while (!bDone) {
        cRd = getByte(nPosition + nIndex);
        // Only add if printable
        if (isprint(cRd)) {
            strRd += cRd;
        }
        nIndex++;
        if (cRd == 0) {
            bDone = true;
        } else if (nIndex >= MAX_BUF_READ_STR) {
            bDone = true;
            // No need to null-terminate the string since we are using QString
        }
    }
    return strRd;
}

// Read a null-terminated 16-bit unicode string from the buffer/cache at the
// indicated file offset.
// - FIXME: Replace faked out unicode-to-ASCII conversion with real implementation
// - Does not affect the current file pointer nPosition
// - String length is limited by encountering either the NULL character
//   of exceeding the maximum length of MAX_BUF_READ_STR
// - Reference: BUG: #1112
//
// INPUT:
// - nPosition                  File offset to start string fetch
//
// RETURN:
// - String fetched from file
//
QString WindowBuf::readUniStr(uint32_t nPosition) {
    // Try to read a NULL-terminated string from file offset "nPosition"
    // up to a maximum of MAX_BUF_READ_STR bytes. Result is max length MAX_BUF_READ_STR
    QString strRd;

    unsigned char readByte;

    bool bDone = false;

    uint32_t nIndex = 0;

    while (!bDone) {
        readByte = getByte(nPosition + nIndex);

        // Make sure it is a printable char!
        // FIXME: No, we can't check for this as it will cause
        // _tcslen() call in the calling function to get the wrong
        // length as it isn't null-terminated. Skip for now.
        //              if (isprint(cRd)) {
        //                      strRd += cRd;
        //              } else {
        //                      strRd += ".");
        //              }
        strRd.append(readByte);

        nIndex += 2;
        if (readByte == 0) {
            bDone = true;
        } else if (nIndex >= (MAX_BUF_READ_STR * 2)) {
            bDone = true;
        }
    }

    return strRd;
}

// Wrapper for ByteStr2Unicode that uses local Window Buffer
#define MAX_UNICODE_STRLEN    255

QString WindowBuf::readUniStr2(uint32_t nPos, uint32_t nBufLen) {
    // Convert byte array into unicode string
    // TODO: Replace with call to ByteStr2Unicode()

    bool bByteSwap = false;

    QString strVal;

    uint32_t nStrLenTrunc;

    unsigned char nChVal;

    unsigned char anStrBuf[(MAX_UNICODE_STRLEN + 1) * 2];

    // wchar_t acStrBuf[(MAX_UNICODE_STRLEN + 1)];

    // Start with length before any truncation
    nStrLenTrunc = nBufLen;

    // Read unicode bytes into byte array
    // Truncate the string, leaving room for terminator
    if (nStrLenTrunc > MAX_UNICODE_STRLEN) {
        nStrLenTrunc = MAX_UNICODE_STRLEN;
    }

    for (uint32_t nInd = 0; nInd < nStrLenTrunc; nInd++) {
        if (bByteSwap) {
            // Reverse the order of the bytes
            nChVal = getByte(nPos + (nInd * 2) + 0);
            anStrBuf[(nInd * 2) + 1] = nChVal;
            nChVal = getByte(nPos + (nInd * 2) + 1);
            anStrBuf[(nInd * 2) + 0] = nChVal;
        } else {
            // No byte reversal
            nChVal = getByte(nPos + (nInd * 2) + 0);
            anStrBuf[(nInd * 2) + 0] = nChVal;
            nChVal = getByte(nPos + (nInd * 2) + 1);
            anStrBuf[(nInd * 2) + 1] = nChVal;
        }
    }

    // Ensure it is terminated
    anStrBuf[nStrLenTrunc * 2 + 0] = 0;
    anStrBuf[nStrLenTrunc * 2 + 1] = 0;
    // Copy into unicode string
    // Ensure that it is terminated first!
    //  lstrcpyW(acStrBuf, (LPCWSTR) anStrBuf);
    // Copy into QString
    strVal = QString(reinterpret_cast<char *>(anStrBuf));

    return strVal;
}

// Read a string from the buffer/cache at the indicated file offset.
// - Does not affect the current file pointer nPosition
// - String length is limited by encountering either the NULL character
//   of exceeding the maximum length parameter
//
// INPUT:
// - nPosition                  File offset to start string fetch
// - nLen                               Maximum number of bytes to fetch
//
// RETURN:
// - String fetched from file
//
QString WindowBuf::readStrN(uint32_t nPosition, uint32_t nLen) {
    // Try to read a fixed-length string from file offset "nPosition"
    // up to a maximum of "nLen" bytes. Result is length "nLen"
    QString strRd = "";

    unsigned char cRd;

    bool bDone = false;

    if (nLen > 0) {
        for (uint32_t nInd = 0; ((!bDone) && (nInd < nLen)); nInd++) {
            cRd = getByte(nPosition + nInd);
            if (isprint(cRd)) {
                strRd += cRd;
            }
            if (cRd == char(0)) {
                bDone = true;
            }
        }
        return strRd;
    } else {
        return "";
    }
}
