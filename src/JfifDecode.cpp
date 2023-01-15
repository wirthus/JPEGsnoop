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

#include "JfifDecode.h"

#include <cstring>

#include <QCoreApplication>
#include <QtDebug>
#include <QtGlobal>
#include <QByteArray>

// #include "DbSigs.h"
#include "General.h"
#include "Md5.h"
#include "SnoopConfig.h"
#include "WindowBuf.h"
#include "log/ILog.h"
#include "Snoop.h"

// Maximum number of component values to extract into array for display
static constexpr uint32_t MAX_anValues = 64;
static constexpr uint32_t MAX_SEGMENT_SIZE = 20 * 1014 * 1024;

// Macro to avoid multi-character constant definitions
#define FOURC_INT(a, b, c, d)    (((a)<<24) | ((b)<<16) | ((c)<<8) | (d))

//-----------------------------------------------------------------------------
// Initialize the JFIF decoder. Several class pointers are provided
// as parameters, so that we can directly access the output log, the
// file buffer and the image scan decoder.
// Loads up the signature database.
//
// INPUT:
// - pLog                       Ptr to log file class
// - _writeBuf                      Ptr to Window Buf class
// - pImgDec            Ptr to Image Decoder class
//
// PRE:
// - Requires that CDocLog, CwindowBuf and CimgDecode classes
//   are already initialized
//
JfifDecode::JfifDecode(ILog &log, WindowBuf &buf, ImgDecode &imgDec, SnoopConfig &appConfig, QObject *parent) :
    QObject(parent),
    _log(log),
    _wbuf(buf),
    _imgDec(imgDec),
    _appConfig(appConfig) {

    log.debug(QStringLiteral("JfifDecode::JfifDecode() Begin"));

    // Need to zero out the private members
    _outputDb = false;          // mySQL output for web

    // Enable verbose reporting
    _verbose = false;

    _imgSrcDirty = true;

    // Generate lookup tables for Huffman codes
    genLookupHuffMask();

    // Reset decoding state
    reset();

    // Load the local database (if it exists)
    //@@  m_pDbSigs->DatabaseExtraLoad();

    // Allocate the Photoshop decoder
    _psDec = std::make_unique<DecodePs>(&_wbuf, &_log);
}

JfifDecode::~JfifDecode() = default;

// Clear out internal members
void JfifDecode::reset() {
    // File handling
    _pos = 0;
    _posSos = 0;
    _posEoi = 0;
    _posEmbedStart = 0;
    _posEmbedEnd = 0;
    _posFileEnd = 0;

    // SOS / SOF handling
    m_nSofNumLines_Y = 0;
    m_nSofSampsPerLine_X = 0;
    m_nSofNumComps_Nf = 0;

    // Quantization tables
    clearDqt();

    // Photoshop
    m_nImgQualPhotoshopSfw = 0;
    m_nImgQualPhotoshopSa = 0;

    m_nApp14ColTransform = -1;

    // Restart marker
    m_nImgRstEn = false;
    m_nImgRstInterval = 0;

    // Basic metadata
    m_strImgExifMake = "???";
    m_nImgExifMakeSubtype = 0;
    m_strImgExifModel = "???";
    m_bImgExifMakernotes = false;
    m_strImgExtras = "";
    m_strComment = "";
    m_strSoftware = "";
    m_bImgProgressive = false;
    m_bImgSofUnsupported = false;
    strcpy(_app0Identifier, "");

    // Derived metadata
    m_strHash = "NONE";
    m_strHashRot = "NONE";
    m_eImgLandscape = ENUM_LANDSCAPE_UNSET;
    m_strImgQualExif = "";
    _avi = false;
    _aviMjpeg = false;
    _psd = false;

    // Misc
    _imgOk = false;             // Set during SOF to indicate further proc OK
    _bufFakeDht = false;        // Start in normal Buf mode
    m_eImgEdited = EDITED_UNSET;
    m_eDbReqSuggest = DB_ADD_SUGGEST_UNSET;
    m_bSigExactInDB = false;

    // Embedded thumbnail
    m_nImgExifThumbComp = 0;
    m_nImgExifThumbOffset = 0;
    m_nImgExifThumbLen = 0;
    m_strHashThumb = "NONE";      // Will go into DB to say NONE!
    m_strHashThumbRot = "NONE";   // Will go into DB to say NONE!
    m_nImgThumbNumLines = 0;
    m_nImgThumbSampsPerLine = 0;

    // Now clear out any previously generated bitmaps
    // or image decoding parameters
    if (_imgSrcDirty) {
        _imgDec.reset();
    }

    // Reset the decoding state checks
    // These are to help ensure we don't start decoding SOS
    // if we haven't seen other valid markers yet! Otherwise
    // we could run into very bad loops (e.g. .PSD files)
    // just because we saw FFD8FF first then JFIF_SOS
    _stateAbort = false;
    _stateSoi = false;
    _stateDht = false;
    _stateDhtOk = false;
    _stateDhtFake = false;
    _stateDqt = false;
    _stateDqtOk = false;
    _stateSof = false;
    _stateSofOk = false;
    _stateSos = false;
    _stateSosOk = false;
    _stateEoi = false;
}

//-----------------------------------------------------------------------------
// Set the AVI mode flag for this file
//
// POST:
// - m_bAvi
// - m_bAviMjpeg
//
void JfifDecode::setAviMode(bool isAvi, bool isMjpeg) {
    _avi = isAvi;
    _aviMjpeg = isMjpeg;
}

//-----------------------------------------------------------------------------
// Fetch the AVI mode flag for this file
//
// PRE:
// - m_bAvi
// - m_bAviMjpeg
//
// OUTPUT:
// - bIsAvi
// - bIsMjpeg
//
void JfifDecode::getAviMode(bool &isAvi, bool &isMjpeg) {
    isAvi = _avi;
    isMjpeg = _aviMjpeg;
}

//-----------------------------------------------------------------------------
// Fetch the starting file position of the embedded thumbnail
//
// PRE:
// - m_nPosEmbedStart
//
// RETURN:
// - File position
//
uint32_t JfifDecode::getPosEmbedStart() const {
    return _posEmbedStart;
}

//-----------------------------------------------------------------------------
// Fetch the ending file position of the embedded thumbnail
//
// PRE:
// - m_nPosEmbedEnd
//
// RETURN:
// - File position
//
uint32_t JfifDecode::getPosEmbedEnd() const {
    return _posEmbedEnd;
}

//-----------------------------------------------------------------------------
// Determine if the last analysis revealed a JFIF with known markers
//
// RETURN:
// - true if file (at position during analysis) appeared to decode OK
//
bool JfifDecode::getDecodeStatus() const {
    return _imgOk;
}

//-----------------------------------------------------------------------------
// Fetch a summary of the JFIF decoder results
// These details are used in preparation of signature submission to the DB
//
// PRE:
// - m_strHash
// - m_strHashRot
// - m_strImgExifMake
// - m_strImgExifModel
// - m_strImgQualExif
// - m_strSoftware
// - m_eDbReqSuggest
//
// OUTPUT:
// - strHash
// - strHashRot
// - strImgExifMake
// - strImgExifModel
// - strImgQualExif
// - strSoftware
// - nDbReqSuggest
//
void
JfifDecode::getDecodeSummary(QString &strHash, QString &strHashRot, QString &strImgExifMake, QString &strImgExifModel,
                             QString &strImgQualExif, QString &strSoftware, teDbAdd &eDbReqSuggest) {
    strHash = m_strHash;
    strHashRot = m_strHashRot;
    strImgExifMake = m_strImgExifMake;
    strImgExifModel = m_strImgExifModel;
    strImgQualExif = m_strImgQualExif;
    strSoftware = m_strSoftware;
    eDbReqSuggest = m_eDbReqSuggest;
}

//-----------------------------------------------------------------------------
// Fetch an element from the "standard" luminance quantization table
//
// PRE:
// - glb_anStdQuantLum[]
//
// RETURN:
// - DQT matrix element
//
uint32_t JfifDecode::getDqtQuantStd(uint32_t nInd) {
    if (nInd < MAX_DQT_COEFF) {
        return glb_anStdQuantLum[nInd];
    } else {
#ifdef DEBUG_LOG
        QString strTmp;

        QString strDebug;

        strTmp = QString("getDqtQuantStd() with nInd out of range. nInd=[%1]").arg(nInd);
        strDebug = QString("## File=[%1] Block=[%2] Error=[%3]").arg(_appConfig.curFileName, -100).arg("JfifDecode",
                                                                                                       -10).arg(
            strTmp);
        _log.debug(strDebug);
#else
        Q_ASSERT(false);
#endif
        return 0;
    }
}

//-----------------------------------------------------------------------------
// Fetch the DQT ordering index (with optional zigzag sequence)
//
// INPUT:
// - nInd                       Coefficient index
// - bZigZag            Use zig-zag ordering
//
// RETURN:
// - Sequence index
//
uint32_t JfifDecode::getDqtZigZagIndex(uint32_t nInd, bool bZigZag) {
    if (nInd < MAX_DQT_COEFF) {
        if (bZigZag) {
            return nInd;
        } else {
            return glb_anZigZag[nInd];
        }
    } else {
#ifdef DEBUG_LOG
        QString strTmp;
        QString strDebug;

        strTmp = QString("getDqtZigZagIndex() with nInd out of range. nInd=[%1]").arg(nInd);
        strDebug = QString("## File=[%1] Block=[%2] Error=[%3]").arg(_appConfig.curFileName, -100).arg("JfifDecode",
                                                                                                       -10).arg(
            strTmp);
        _log.debug(strDebug);
#else
        Q_ASSERT(false);
#endif
        return 0;
    }
}

//-----------------------------------------------------------------------------
// Reset the DQT tables
//
// POST:
// - m_anImgDqtTbl[][]
// - m_anImgThumbDqt[][]
// - m_adImgDqtQual[]
// - m_abImgDqtSet[]
// - m_abImgDqtThumbSet[]
//
void JfifDecode::clearDqt() {
    for (uint32_t nTblInd = 0; nTblInd < MAX_DQT_DEST_ID; nTblInd++) {
        for (uint32_t nCoeffInd = 0; nCoeffInd < MAX_DQT_COEFF; nCoeffInd++) {
            m_anImgDqtTbl[nTblInd][nCoeffInd] = 0;
            m_anImgThumbDqt[nTblInd][nCoeffInd] = 0;
        }

        m_adImgDqtQual[nTblInd] = 0;
        m_abImgDqtSet[nTblInd] = false;
        m_abImgDqtThumbSet[nTblInd] = false;
    }
}

//-----------------------------------------------------------------------------
// Set the DQT matrix element
//
// INPUT:
// - dqt0[]                             Matrix array for table 0
// - dqt1[]                             Matrix array for table 1
//
// POST:
// - m_anImgDqtTbl[][]
// - m_eImgLandscape
// - m_abImgDqtSet[]
// - m_strImgQuantCss
//
void JfifDecode::setDqtQuick(uint16_t anDqt0[], uint16_t anDqt1[]) {
    m_eImgLandscape = ENUM_LANDSCAPE_YES;

    for (uint32_t ind = 0; ind < MAX_DQT_COEFF; ind++) {
        m_anImgDqtTbl[0][ind] = anDqt0[ind];
        m_anImgDqtTbl[1][ind] = anDqt1[ind];
    }

    m_abImgDqtSet[0] = true;
    m_abImgDqtSet[1] = true;
    m_strImgQuantCss = "NA";
}

//-----------------------------------------------------------------------------
// Construct a lookup table for the Huffman code masks
// The result is a simple bit sequence of zeros followed by
// an increasing number of 1 bits.
//   00000000...00000001
//   00000000...00000011
//   00000000...00000111
//   ...
//   01111111...11111111
//   11111111...11111111
//
// POST:
// - m_anMaskLookup[]
//
void JfifDecode::genLookupHuffMask() {
    uint32_t mask;

    for (uint32_t len = 0; len < 32; len++) {
        mask = (1 << len) - 1;
        mask <<= 32 - len;
        m_anMaskLookup[len] = mask;
    }
}

//-----------------------------------------------------------------------------
// Provide a short-hand alias for the m_pWBuf buffer
// Also support redirection to a local table in case we are
// faking out the DHT (eg. for MotionJPEG files).
//
// PRE:
// - m_bBufFakeDHT                      Flag to include Fake DHT table
// - _motionJpegDhtSeg[]           DHT table used if m_bBufFakeDHT=true
//
// INPUT:
// - nOffset                            File offset to read from
// - bClean                                     Forcibly disables any redirection to Fake DHT table
//
// POST:
// - m_pLog
//
// RETURN:
// - Byte from file (or local table)
//
uint8_t JfifDecode::getByte(uint32_t nOffset, bool bClean = false) {
    // Buffer can be redirected to internal array for AVI DHT
    // tables, so check for it here.
    if (_bufFakeDht) return _motionJpegDhtSeg[nOffset];

    return _wbuf.getByte(nOffset, bClean);
}

//-----------------------------------------------------------------------------
// Write out a line to the log buffer if we are in verbose mode
//
// PRE:
// - m_bVerbose                         Verbose mode
//
// INPUT:
// - strLine                            String to output
//
// OUTPUT:
// - none
//
// POST:
// - m_pLog
//
// RETURN:
// - none
//
void JfifDecode::dbgAddLine(const QString &strLine) {
    if (_verbose) {
        _log.info(strLine);
    }
}

//-----------------------------------------------------------------------------
// Convert a UINT32 and decompose into 4 bytes, but support
// either endian byte-swap mode
//
// PRE:
// - m_nImgExifEndian           Byte swap mode (0=little, 1=big)
//
// INPUT:
// - nVal                                       Input UINT32
//
// OUTPUT:
// - nByte0                                     Byte #1
// - nByte1                                     Byte #2
// - nByte2                                     Byte #3
// - nByte3                                     Byte #4
//
// RETURN:
// - none
//
void JfifDecode::unByteSwap4(uint32_t nVal, uint32_t &nByte0, uint32_t &nByte1, uint32_t &nByte2, uint32_t &nByte3) {
    if (m_nImgExifEndian == 0) {
        // Little Endian
        nByte3 = (nVal & 0xFF000000) >> 24;
        nByte2 = (nVal & 0x00FF0000) >> 16;
        nByte1 = (nVal & 0x0000FF00) >> 8;
        nByte0 = (nVal & 0x000000FF);
    } else {
        // Big Endian
        nByte0 = (nVal & 0xFF000000) >> 24;
        nByte1 = (nVal & 0x00FF0000) >> 16;
        nByte2 = (nVal & 0x0000FF00) >> 8;
        nByte3 = (nVal & 0x000000FF);
    }
}

//-----------------------------------------------------------------------------
// Perform conversion from 4 bytes into UINT32 with
// endian byte-swapping support
//
// PRE:
// - m_nImgExifEndian           Byte swap mode (0=little, 1=big)
//
// INPUT:
// - nByte0                                             Byte #1
// - nByte1                                             Byte #2
// - nByte2                                             Byte #3
// - nByte3                                             Byte #4
//
// RETURN:
// - UINT32
//
uint32_t JfifDecode::byteSwap4(uint32_t nByte0, uint32_t nByte1, uint32_t nByte2, uint32_t nByte3) {
    uint32_t nVal;

    if (m_nImgExifEndian == 0) {
        // Little endian, byte swap required
        nVal = (nByte3 << 24) + (nByte2 << 16) + (nByte1 << 8) + nByte0;
    } else {
        // Big endian, no swap required
        nVal = (nByte0 << 24) + (nByte1 << 16) + (nByte2 << 8) + nByte3;
    }

    return nVal;
}

//-----------------------------------------------------------------------------
// Perform conversion from 2 bytes into half-word with
// endian byte-swapping support
//
// PRE:
// - m_nImgExifEndian           Byte swap mode (0=little, 1=big)
//
// INPUT:
// - nByte0                                             Byte #1
// - nByte1                                             Byte #2
//
// RETURN:
// - UINT16
//
uint32_t JfifDecode::byteSwap2(uint32_t nByte0, uint32_t nByte1) {
    uint32_t nVal;

    if (m_nImgExifEndian == 0) {
        // Little endian, byte swap required
        nVal = (nByte1 << 8) + nByte0;
    } else {
        // Big endian, no swap required
        nVal = (nByte0 << 8) + nByte1;
    }

    return nVal;
}

//-----------------------------------------------------------------------------
// Decode Canon Makernotes
// Only the most common makernotes are supported; there are a large
// number of makernotes that have not been documented anywhere.
CStr2 JfifDecode::lookupMakerCanonTag(uint32_t nMainTag, uint32_t nSubTag, uint32_t nVal) {
    QString strTmp;

    CStr2 sRetVal;

    sRetVal.strTag = "???";
    sRetVal.bUnknown = false;     // Set to true in default clauses
    sRetVal.strVal = QString("%1").arg(nVal);     // Provide default value

    uint32_t nValHi, nValLo;

    nValHi = (nVal & 0xff00) >> 8;
    nValLo = (nVal & 0x00ff);

    switch (nMainTag) {
        case 0x0001:
            switch (nSubTag) {
                case 0x0001:
                    sRetVal.strTag = "Canon.Cs1.Macro";
                    break;                // Short Macro mode

                case 0x0002:
                    sRetVal.strTag = "Canon.Cs1.Selftimer";
                    break;                // Short Self timer

                case 0x0003:
                    sRetVal.strTag = "Canon.Cs1.Quality";

                    if (nVal == 2) {
                        sRetVal.strVal = "norm";
                    } else if (nVal == 3) {
                        sRetVal.strVal = "fine";
                    } else if (nVal == 5) {
                        sRetVal.strVal = "superfine";
                    } else {
                        sRetVal.strVal = "?";
                    }

                    // Save the quality string for later
                    m_strImgQualExif = sRetVal.strVal;
                    break;                // Short Quality

                case 0x0004:
                    sRetVal.strTag = "Canon.Cs1.FlashMode";
                    break;                // Short Flash mode setting

                case 0x0005:
                    sRetVal.strTag = "Canon.Cs1.DriveMode";
                    break;                // Short Drive mode setting

                case 0x0007:
                    sRetVal.strTag = "Canon.Cs1.FocusMode";       // Short Focus mode setting

                    switch (nVal) {
                        case 0:
                            sRetVal.strVal = "One-shot";
                            break;

                        case 1:
                            sRetVal.strVal = "AI Servo";
                            break;

                        case 2:
                            sRetVal.strVal = "AI Focus";
                            break;

                        case 3:
                            sRetVal.strVal = "Manual Focus";
                            break;

                        case 4:
                            sRetVal.strVal = "Single";
                            break;

                        case 5:
                            sRetVal.strVal = "Continuous";
                            break;

                        case 6:
                            sRetVal.strVal = "Manual Focus";
                            break;

                        default:
                            sRetVal.strVal = "?";
                            break;
                    }

                    break;

                case 0x000a:
                    sRetVal.strTag = "Canon.Cs1.ImageSize";       // Short Image size

                    if (nVal == 0) {
                        sRetVal.strVal = "Large";
                    } else if (nVal == 1) {
                        sRetVal.strVal = "Medium";
                    } else if (nVal == 2) {
                        sRetVal.strVal = "Small";
                    } else {
                        sRetVal.strVal = "?";
                    }

                    break;

                case 0x000b:
                    sRetVal.strTag = "Canon.Cs1.EasyMode";
                    break;                // Short Easy shooting mode

                case 0x000c:
                    sRetVal.strTag = "Canon.Cs1.DigitalZoom";
                    break;                // Short Digital zoom

                case 0x000d:
                    sRetVal.strTag = "Canon.Cs1.Contrast";
                    break;                // Short Contrast setting

                case 0x000e:
                    sRetVal.strTag = "Canon.Cs1.Saturation";
                    break;                // Short Saturation setting

                case 0x000f:
                    sRetVal.strTag = "Canon.Cs1.Sharpness";
                    break;                // Short Sharpness setting

                case 0x0010:
                    sRetVal.strTag = "Canon.Cs1.ISOSpeed";
                    break;                // Short ISO speed setting

                case 0x0011:
                    sRetVal.strTag = "Canon.Cs1.MeteringMode";
                    break;                // Short Metering mode setting

                case 0x0012:
                    sRetVal.strTag = "Canon.Cs1.FocusType";
                    break;                // Short Focus type setting

                case 0x0013:
                    sRetVal.strTag = "Canon.Cs1.AFPoint";
                    break;                // Short AF point selected

                case 0x0014:
                    sRetVal.strTag = "Canon.Cs1.ExposureProgram";
                    break;                // Short Exposure mode setting

                case 0x0016:
                    sRetVal.strTag = "Canon.Cs1.LensType";
                    break;                //

                case 0x0017:
                    sRetVal.strTag = "Canon.Cs1.Lens";
                    break;                // Short 'long' and 'short' focal length of lens (in 'focal m_nImgUnits' and 'focal m_nImgUnits' per mm

                case 0x001a:
                    sRetVal.strTag = "Canon.Cs1.MaxAperture";
                    break;                //

                case 0x001b:
                    sRetVal.strTag = "Canon.Cs1.MinAperture";
                    break;                //

                case 0x001c:
                    sRetVal.strTag = "Canon.Cs1.FlashActivity";
                    break;                // Short Flash activity

                case 0x001d:
                    sRetVal.strTag = "Canon.Cs1.FlashDetails";
                    break;                // Short Flash details

                case 0x0020:
                    sRetVal.strTag = "Canon.Cs1.FocusMode";
                    break;                // Short Focus mode setting

                default:
                    sRetVal.strTag = QString("Canon.Cs1.x%1").arg(nSubTag, 4, 16, QChar('0'));
                    sRetVal.bUnknown = true;
                    break;
            }                         // switch nSubTag

            break;

        case 0x0004:
            switch (nSubTag) {
                case 0x0002:
                    sRetVal.strTag = "Canon.Cs2.ISOSpeed";
                    break;                // Short ISO speed used
                case 0x0004:
                    sRetVal.strTag = "Canon.Cs2.TargetAperture";
                    break;                // Short Target Aperture
                case 0x0005:
                    sRetVal.strTag = "Canon.Cs2.TargetShutterSpeed";
                    break;                // Short Target shutter speed
                case 0x0007:
                    sRetVal.strTag = "Canon.Cs2.WhiteBalance";
                    break;                // Short White balance setting
                case 0x0009:
                    sRetVal.strTag = "Canon.Cs2.Sequence";
                    break;                // Short Sequence number (if in a continuous burst
                case 0x000e:
                    sRetVal.strTag = "Canon.Cs2.AFPointUsed";
                    break;                // Short AF point used
                case 0x000f:
                    sRetVal.strTag = "Canon.Cs2.FlashBias";
                    break;                // Short Flash bias
                case 0x0013:
                    sRetVal.strTag = "Canon.Cs2.SubjectDistance";
                    break;                // Short Subject distance (m_nImgUnits are not clear
                case 0x0015:
                    sRetVal.strTag = "Canon.Cs2.ApertureValue";
                    break;                // Short Aperture
                case 0x0016:
                    sRetVal.strTag = "Canon.Cs2.ShutterSpeedValue";
                    break;                // Short Shutter speed
                default:
                    sRetVal.strTag = QString("Canon.Cs2.x%1").arg(nSubTag, 4, 16, QChar('0'));
                    sRetVal.bUnknown = true;
                    break;
            }                         // switch nSubTag

            break;

        case 0x000F:
            // CustomFunctions are different! Tag given by high byte, value by low
            // Index order (usually the nSubTag) is not used.
            sRetVal.strVal = QString("%1").arg(nValLo);       // Provide default value

            switch (nValHi) {
                case 0x0001:
                    sRetVal.strTag = "Canon.Cf.NoiseReduction";
                    break;                // Short Long exposure noise reduction
                case 0x0002:
                    sRetVal.strTag = "Canon.Cf.ShutterAeLock";
                    break;                // Short Shutter/AE lock buttons
                case 0x0003:
                    sRetVal.strTag = "Canon.Cf.MirrorLockup";
                    break;                // Short Mirror lockup
                case 0x0004:
                    sRetVal.strTag = "Canon.Cf.ExposureLevelIncrements";
                    break;                // Short Tv/Av and exposure level
                case 0x0005:
                    sRetVal.strTag = "Canon.Cf.AFAssist";
                    break;                // Short AF assist light
                case 0x0006:
                    sRetVal.strTag = "Canon.Cf.FlashSyncSpeedAv";
                    break;                // Short Shutter speed in Av mode
                case 0x0007:
                    sRetVal.strTag = "Canon.Cf.AEBSequence";
                    break;                // Short AEB sequence/auto cancellation
                case 0x0008:
                    sRetVal.strTag = "Canon.Cf.ShutterCurtainSync";
                    break;                // Short Shutter curtain sync
                case 0x0009:
                    sRetVal.strTag = "Canon.Cf.LensAFStopButton";
                    break;                // Short Lens AF stop button Fn. Switch
                case 0x000a:
                    sRetVal.strTag = "Canon.Cf.FillFlashAutoReduction";
                    break;                // Short Auto reduction of fill flash
                case 0x000b:
                    sRetVal.strTag = "Canon.Cf.MenuButtonReturn";
                    break;                // Short Menu button return position
                case 0x000c:
                    sRetVal.strTag = "Canon.Cf.SetButtonFunction";
                    break;                // Short SET button func. when shooting
                case 0x000d:
                    sRetVal.strTag = "Canon.Cf.SensorCleaning";
                    break;                // Short Sensor cleaning
                case 0x000e:
                    sRetVal.strTag = "Canon.Cf.SuperimposedDisplay";
                    break;                // Short Superimposed display
                case 0x000f:
                    sRetVal.strTag = "Canon.Cf.ShutterReleaseNoCFCard";
                    break;                // Short Shutter Release W/O CF Card
                default:
                    sRetVal.strTag = QString("Canon.Cf.x%1").arg(nValHi, 4, 16, QChar('0'));
                    sRetVal.bUnknown = true;
                    break;
            }
            // switch nSubTag
            break;

            /*
	// Other ones assumed to use high-byte/low-byte method:
	case 0x00C0:
    sRetVal.strVal = QString("%u"),nValLo; // Provide default value
		switch(nValHi)
		{
			//case 0x0001: sRetVal.strTag = "Canon.x00C0.???";break; //
			default:
        sRetVal.strTag = QString("Canon.x00C0.x%04X"),nValHi;
				break;
		}
		break;

	case 0x00C1:
    sRetVal.strVal = QString("%u"),nValLo; // Provide default value
		switch(nValHi)
		{
			//case 0x0001: sRetVal.strTag = "Canon.x00C1.???";break; //
			default:
        sRetVal.strTag = QString("Canon.x00C1.x%04X"),nValHi;
				break;
		}
		break;
*/

        case 0x0012:
            switch (nSubTag) {
                case 0x0002:
                    sRetVal.strTag = "Canon.Pi.ImageWidth";
                    break;                //
                case 0x0003:
                    sRetVal.strTag = "Canon.Pi.ImageHeight";
                    break;                //
                case 0x0004:
                    sRetVal.strTag = "Canon.Pi.ImageWidthAsShot";
                    break;                //
                case 0x0005:
                    sRetVal.strTag = "Canon.Pi.ImageHeightAsShot";
                    break;                //
                case 0x0016:
                    sRetVal.strTag = "Canon.Pi.AFPointsUsed";
                    break;                //
                case 0x001a:
                    sRetVal.strTag = "Canon.Pi.AFPointsUsed20D";
                    break;                //
                default:
                    sRetVal.strTag = QString("Canon.Pi.x%1").arg(nSubTag, 4, 16, QChar('0'));
                    sRetVal.bUnknown = true;
                    break;
            }                         // switch nSubTag

            break;

        default:
            sRetVal.strTag = QString("Canon.x%1.x%1")
                .arg(nMainTag, 4, 16, QChar('0'))
                .arg(nSubTag, 4, 16, QChar('0'));
            sRetVal.bUnknown = true;
            break;

    }                             // switch mainTag

    return sRetVal;
}

//-----------------------------------------------------------------------------
// Perform decode of EXIF IFD tags including MakerNote tags
//
// PRE:
// - m_strImgExifMake   Used for MakerNote decode
//
// INPUT:
// - strSect                    IFD section
// - nTag                               Tag code value
//
// OUTPUT:
// - bUnknown                   Was the tag unknown?
//
// RETURN:
// - Formatted string
//
QString JfifDecode::lookupExifTag(const QString &strSect, uint32_t nTag, bool &bUnknown) {
    QString strTmp;

    bUnknown = false;

    if (strSect == "IFD0") {
        switch (nTag) {
            case 0x010E:
                return QString("ImageDescription");
                break;                  // ascii string Describes image
            case 0x010F:
                return QString("Make");
                break;                  // ascii string Shows manufacturer of digicam
            case 0x0110:
                return QString("Model");
                break;                  // ascii string Shows model number of digicam
            case 0x0112:
                return QString("Orientation");
                break;                  // unsigned short 1  The orientation of the camera relative to the scene, when the image was captured. The start point of stored data is, '1' means upper left, '3' lower right, '6' upper right, '8' lower left, '9' undefined.
            case 0x011A:
                return QString("XResolution");
                break;                  // unsigned rational 1  Display/Print resolution of image. Large number of digicam uses 1/72inch, but it has no mean because personal computer doesn't use this value to display/print out.
            case 0x011B:
                return QString("YResolution");
                break;                  // unsigned rational 1
            case 0x0128:
                return QString("ResolutionUnit");
                break;                  // unsigned short 1  Unit of XResolution(0x011a)/YResolution(0x011b. '1' means no-unit, '2' means inch, '3' means centimeter.
            case 0x0131:
                return QString("Software");
                break;                  //  ascii string Shows firmware(internal software of digicam version number.
            case 0x0132:
                return QString("DateTime");
                break;                  // ascii string 20  Date/Time of image was last modified. Data format is "YYYY:MM:DD HH:MM:SS"+0x00, total 20bytes. In usual, it has the same value of DateTimeOriginal(0x9003
            case 0x013B:
                return QString("Artist");
                break;                  // Seems to be here and not only in SubIFD (maybe instead of SubIFD
            case 0x013E:
                return QString("WhitePoint");
                break;                  // unsigned rational 2  Defines chromaticity of white point of the image. If the image uses CIE Standard Illumination D65(known as international standard of 'daylight', the values are '3127/10000,3290/10000'.
            case 0x013F:
                return QString("PrimChromaticities");
                break;                  // unsigned rational 6  Defines chromaticity of the primaries of the image. If the image uses CCIR Recommendation 709 primearies, values are '640/1000,330/1000,300/1000,600/1000,150/1000,0/1000'.
            case 0x0211:
                return QString("YCbCrCoefficients");
                break;                  // unsigned rational 3  When image format is YCbCr, this value shows a constant to translate it to RGB format. In usual, values are '0.299/0.587/0.114'.
            case 0x0213:
                return QString("YCbCrPositioning");
                break;                  // unsigned short 1  When image format is YCbCr and uses 'Subsampling'(cropping of chroma data, all the digicam do that, defines the chroma sample point of subsampling pixel array. '1' means the center of pixel array, '2' means the datum point.
            case 0x0214:
                return QString("ReferenceBlackWhite");
                break;                  // unsigned rational 6  Shows reference value of black point/white point. In case of YCbCr format, first 2 show black/white of Y, next 2 are Cb, last 2 are Cr. In case of RGB format, first 2 show black/white of R, next 2 are G, last 2 are B.
            case 0x8298:
                return QString("Copyright");
                break;                  // ascii string Shows copyright information
            case 0x8769:
                return QString("ExifOffset");
                break;                  //unsigned long 1  Offset to Exif Sub IFD
            case 0x8825:
                return QString("GPSOffset");
                break;                  //unsigned long 1  Offset to Exif GPS IFD
                //NEW:
            case 0x9C9B:
                return QString("XPTitle");
                break;
            case 0x9C9C:
                return QString("XPComment");
                break;
            case 0x9C9D:
                return QString("XPAuthor");
                break;
            case 0x9C9e:
                return QString("XPKeywords");
                break;
            case 0x9C9f:
                return QString("XPSubject");
                break;
                //NEW: The following were found in IFD0 even though they should just be SubIFD?
            case 0xA401:
                return QString("CustomRendered");
                break;
            case 0xA402:
                return QString("ExposureMode");
                break;
            case 0xA403:
                return QString("WhiteBalance");
                break;
            case 0xA406:
                return QString("SceneCaptureType");
                break;

            default:
                strTmp = QString("IFD0.0x%1").arg(nTag, 4, 16, QChar('0'));
                bUnknown = true;
                return strTmp;
                break;
        }
    } else if (strSect == "SubIFD") {
        switch (nTag) {
            case 0x00fe:
                return QString("NewSubfileType");
                break;                  //  unsigned long 1
            case 0x00ff:
                return QString("SubfileType");
                break;                  //  unsigned short 1
            case 0x012d:
                return QString("TransferFunction");
                break;                  //  unsigned short 3
            case 0x013b:
                return QString("Artist");
                break;                  //  ascii string
            case 0x013d:
                return QString("Predictor");
                break;                  //  unsigned short 1
            case 0x0142:
                return QString("TileWidth");
                break;                  //  unsigned short 1
            case 0x0143:
                return QString("TileLength");
                break;                  //  unsigned short 1
            case 0x0144:
                return QString("TileOffsets");
                break;                  //  unsigned long
            case 0x0145:
                return QString("TileByteCounts");
                break;                  //  unsigned short
            case 0x014a:
                return QString("SubIFDs");
                break;                  //  unsigned long
            case 0x015b:
                return QString("JPEGTables");
                break;                  //  undefined
            case 0x828d:
                return QString("CFARepeatPatternDim");
                break;                  //  unsigned short 2
            case 0x828e:
                return QString("CFAPattern");
                break;                  //  unsigned byte
            case 0x828f:
                return QString("BatteryLevel");
                break;                  //  unsigned rational 1
            case 0x829A:
                return QString("ExposureTime");
                break;
            case 0x829D:
                return QString("FNumber");
                break;
            case 0x83bb:
                return QString("IPTC/NAA");
                break;                  //  unsigned long
            case 0x8773:
                return QString("InterColorProfile");
                break;                  //  undefined
            case 0x8822:
                return QString("ExposureProgram");
                break;
            case 0x8824:
                return QString("SpectralSensitivity");
                break;                  //  ascii string
            case 0x8825:
                return QString("GPSInfo");
                break;                  //  unsigned long 1
            case 0x8827:
                return QString("ISOSpeedRatings");
                break;
            case 0x8828:
                return QString("OECF");
                break;                  //  undefined
            case 0x8829:
                return QString("Interlace");
                break;                  //  unsigned short 1
            case 0x882a:
                return QString("TimeZoneOffset");
                break;                  //  signed short 1
            case 0x882b:
                return QString("SelfTimerMode");
                break;                  //  unsigned short 1
            case 0x9000:
                return QString("ExifVersion");
                break;
            case 0x9003:
                return QString("DateTimeOriginal");
                break;
            case 0x9004:
                return QString("DateTimeDigitized");
                break;
            case 0x9101:
                return QString("ComponentsConfiguration");
                break;
            case 0x9102:
                return QString("CompressedBitsPerPixel");
                break;
            case 0x9201:
                return QString("ShutterSpeedValue");
                break;
            case 0x9202:
                return QString("ApertureValue");
                break;
            case 0x9203:
                return QString("BrightnessValue");
                break;
            case 0x9204:
                return QString("ExposureBiasValue");
                break;
            case 0x9205:
                return QString("MaxApertureValue");
                break;
            case 0x9206:
                return QString("SubjectDistance");
                break;
            case 0x9207:
                return QString("MeteringMode");
                break;
            case 0x9208:
                return QString("LightSource");
                break;
            case 0x9209:
                return QString("Flash");
                break;
            case 0x920A:
                return QString("FocalLength");
                break;
            case 0x920b:
                return QString("FlashEnergy");
                break;                  //  unsigned rational 1
            case 0x920c:
                return QString("SpatialFrequencyResponse");
                break;                  //  undefined
            case 0x920d:
                return QString("Noise");
                break;                  //  undefined
            case 0x9211:
                return QString("ImageNumber");
                break;                  //  unsigned long 1
            case 0x9212:
                return QString("SecurityClassification");
                break;                  //  ascii string 1
            case 0x9213:
                return QString("ImageHistory");
                break;                  //  ascii string
            case 0x9214:
                return QString("SubjectLocation");
                break;                  //  unsigned short 4
            case 0x9215:
                return QString("ExposureIndex");
                break;                  //  unsigned rational 1
            case 0x9216:
                return QString("TIFF/EPStandardID");
                break;                  //  unsigned byte 4
            case 0x927C:
                return QString("MakerNote");
                break;
            case 0x9286:
                return QString("UserComment");
                break;
            case 0x9290:
                return QString("SubSecTime");
                break;                  //  ascii string
            case 0x9291:
                return QString("SubSecTimeOriginal");
                break;                  //  ascii string
            case 0x9292:
                return QString("SubSecTimeDigitized");
                break;                  //  ascii string
            case 0xA000:
                return QString("FlashPixVersion");
                break;
            case 0xA001:
                return QString("ColorSpace");
                break;
            case 0xA002:
                return QString("ExifImageWidth");
                break;
            case 0xA003:
                return QString("ExifImageHeight");
                break;
            case 0xA004:
                return QString("RelatedSoundFile");
                break;
            case 0xA005:
                return QString("ExifInteroperabilityOffset");
                break;
            case 0xa20b:
                return QString("FlashEnergy  unsigned");
                break;                  // rational 1
            case 0xa20c:
                return QString("SpatialFrequencyResponse");
                break;                  //  unsigned short 1
            case 0xA20E:
                return QString("FocalPlaneXResolution");
                break;
            case 0xA20F:
                return QString("FocalPlaneYResolution");
                break;
            case 0xA210:
                return QString("FocalPlaneResolutionUnit");
                break;
            case 0xa214:
                return QString("SubjectLocation");
                break;                  //  unsigned short 1
            case 0xa215:
                return QString("ExposureIndex");
                break;                  //  unsigned rational 1
            case 0xA217:
                return QString("SensingMethod");
                break;
            case 0xA300:
                return QString("FileSource");
                break;
            case 0xA301:
                return QString("SceneType");
                break;
            case 0xa302:
                return QString("CFAPattern");
                break;                  //  undefined 1
            case 0xa401:
                return QString("CustomRendered");
                break;                  // Short Custom image processing
            case 0xa402:
                return QString("ExposureMode");
                break;                  // Short Exposure mode
            case 0xa403:
                return QString("WhiteBalance");
                break;                  // Short White balance
            case 0xa404:
                return QString("DigitalZoomRatio");
                break;                  // Rational Digital zoom ratio
            case 0xa405:
                return QString("FocalLengthIn35mmFilm");
                break;                  // Short Focal length in 35 mm film
            case 0xa406:
                return QString("SceneCaptureType");
                break;                  // Short Scene capture type
            case 0xa407:
                return QString("GainControl");
                break;                  // Rational Gain control
            case 0xa408:
                return QString("Contrast");
                break;                  // Short Contrast
            case 0xa409:
                return QString("Saturation");
                break;                  // Short Saturation
            case 0xa40a:
                return QString("Sharpness");
                break;                  // Short Sharpness
            case 0xa40b:
                return QString("DeviceSettingDescription");
                break;                  // Undefined Device settings description
            case 0xa40c:
                return QString("SubjectDistanceRange");
                break;                  // Short Subject distance range
            case 0xa420:
                return QString("ImageUniqueID");
                break;                  // Ascii Unique image ID

            default:
                strTmp = QString("SubIFD.0x%1").arg(nTag, 4, 16, QChar('0'));
                bUnknown = true;
                return strTmp;
                break;
        }
    } else if (strSect == "IFD1") {
        switch (nTag) {
            case 0x0100:
                return QString("ImageWidth");
                break;                  //  unsigned short/long 1  Shows size of thumbnail image.
            case 0x0101:
                return QString("ImageLength");
                break;                  //  unsigned short/long 1
            case 0x0102:
                return QString("BitsPerSample");
                break;                  //  unsigned short 3  When image format is no compression, this value shows the number of bits per component for each pixel. Usually this value is '8,8,8'
            case 0x0103:
                return QString("Compression");
                break;                  //  unsigned short 1  Shows compression method. '1' means no compression, '6' means JPEG compression.
            case 0x0106:
                return QString("PhotometricInterpretation");
                break;                  //  unsigned short 1  Shows the color space of the image data components. '1' means monochrome, '2' means RGB, '6' means YCbCr.
            case 0x0111:
                return QString("StripOffsets");
                break;                  //  unsigned short/long When image format is no compression, this value shows offset to image data. In some case image data is striped and this value is plural.
            case 0x0115:
                return QString("SamplesPerPixel");
                break;                  //  unsigned short 1  When image format is no compression, this value shows the number of components stored for each pixel. At color image, this value is '3'.
            case 0x0116:
                return QString("RowsPerStrip");
                break;                  //  unsigned short/long 1  When image format is no compression and image has stored as strip, this value shows how many rows stored to each strip. If image has not striped, this value is the same as ImageLength(0x0101.
            case 0x0117:
                return QString("StripByteConunts");
                break;                  //  unsigned short/long  When image format is no compression and stored as strip, this value shows how many bytes used for each strip and this value is plural. If image has not stripped, this value is single and means whole data size of image.
            case 0x011a:
                return QString("XResolution");
                break;                  //  unsigned rational 1  Display/Print resolution of image. Large number of digicam uses 1/72inch, but it has no mean because personal computer doesn't use this value to display/print out.
            case 0x011b:
                return QString("YResolution");
                break;                  //  unsigned rational 1
            case 0x011c:
                return QString("PlanarConfiguration");
                break;                  //  unsigned short 1  When image format is no compression YCbCr, this value shows byte aligns of YCbCr data. If value is '1', Y/Cb/Cr value is chunky format, contiguous for each subsampling pixel. If value is '2', Y/Cb/Cr value is separated and stored to Y plane/Cb plane/Cr plane format.
            case 0x0128:
                return QString("ResolutionUnit");
                break;                  //  unsigned short 1  Unit of XResolution(0x011a)/YResolution(0x011b. '1' means inch, '2' means centimeter.
            case 0x0201:
                return QString("JpegIFOffset");
                break;                  //  unsigned long 1  When image format is JPEG, this value show offset to JPEG data stored.
            case 0x0202:
                return QString("JpegIFByteCount");
                break;                  //  unsigned long 1  When image format is JPEG, this value shows data size of JPEG image.
            case 0x0211:
                return QString("YCbCrCoefficients");
                break;                  //  unsigned rational 3  When image format is YCbCr, this value shows constants to translate it to RGB format. In usual, '0.299/0.587/0.114' are used.
            case 0x0212:
                return QString("YCbCrSubSampling");
                break;                  //  unsigned short 2  When image format is YCbCr and uses subsampling(cropping of chroma data, all the digicam do that, this value shows how many chroma data subsampled. First value shows horizontal, next value shows vertical subsample rate.
            case 0x0213:
                return QString("YCbCrPositioning");
                break;                  //  unsigned short 1  When image format is YCbCr and uses 'Subsampling'(cropping of chroma data, all the digicam do that), this value defines the chroma sample point of subsampled pixel array. '1' means the center of pixel array, '2' means the datum point(0,0.
            case 0x0214:
                return QString("ReferenceBlackWhite");
                break;                  //  unsigned rational 6  Shows reference value of black point/white point. In case of YCbCr format, first 2 show black/white of Y, next 2 are Cb, last 2 are Cr. In case of RGB format, first 2 show black/white of R, next 2 are G, last 2 are B.

            default:
                strTmp = QString("IFD1.0x%1").arg(nTag, 4, 16, QChar('0'));
                bUnknown = true;
                return strTmp;
                break;

        }

    } else if (strSect == "InteropIFD") {
        switch (nTag) {
            case 0x0001:
                return QString("InteroperabilityIndex");
                break;
            case 0x0002:
                return QString("InteroperabilityVersion");
                break;
            case 0x1000:
                return QString("RelatedImageFileFormat");
                break;
            case 0x1001:
                return QString("RelatedImageWidth");
                break;
            case 0x1002:
                return QString("RelatedImageLength");
                break;

            default:
                strTmp = QString("Interop.0x%1").arg(nTag, 4, 16, QChar('0'));
                bUnknown = true;
                return strTmp;
                break;
        }
    } else if (strSect == "GPSIFD") {
        switch (nTag) {
            case 0x0000:
                return QString("GPSVersionID");
                break;
            case 0x0001:
                return QString("GPSLatitudeRef");
                break;
            case 0x0002:
                return QString("GPSLatitude");
                break;
            case 0x0003:
                return QString("GPSLongitudeRef");
                break;
            case 0x0004:
                return QString("GPSLongitude");
                break;
            case 0x0005:
                return QString("GPSAltitudeRef");
                break;
            case 0x0006:
                return QString("GPSAltitude");
                break;
            case 0x0007:
                return QString("GPSTimeStamp");
                break;
            case 0x0008:
                return QString("GPSSatellites");
                break;
            case 0x0009:
                return QString("GPSStatus");
                break;
            case 0x000A:
                return QString("GPSMeasureMode");
                break;
            case 0x000B:
                return QString("GPSDOP");
                break;
            case 0x000C:
                return QString("GPSSpeedRef");
                break;
            case 0x000D:
                return QString("GPSSpeed");
                break;
            case 0x000E:
                return QString("GPSTrackRef");
                break;
            case 0x000F:
                return QString("GPSTrack");
                break;
            case 0x0010:
                return QString("GPSImgDirectionRef");
                break;
            case 0x0011:
                return QString("GPSImgDirection");
                break;
            case 0x0012:
                return QString("GPSMapDatum");
                break;
            case 0x0013:
                return QString("GPSDestLatitudeRef");
                break;
            case 0x0014:
                return QString("GPSDestLatitude");
                break;
            case 0x0015:
                return QString("GPSDestLongitudeRef");
                break;
            case 0x0016:
                return QString("GPSDestLongitude");
                break;
            case 0x0017:
                return QString("GPSDestBearingRef");
                break;
            case 0x0018:
                return QString("GPSDestBearing");
                break;
            case 0x0019:
                return QString("GPSDestDistanceRef");
                break;
            case 0x001A:
                return QString("GPSDestDistance");
                break;
            case 0x001B:
                return QString("GPSProcessingMethod");
                break;
            case 0x001C:
                return QString("GPSAreaInformation");
                break;
            case 0x001D:
                return QString("GPSDateStamp");
                break;
            case 0x001E:
                return QString("GPSDifferential");
                break;

            default:
                strTmp = QString("GPS.0x%").arg(nTag, 4, 16, QChar('0'));
                bUnknown = true;
                return strTmp;
                break;
        }
    } else if (strSect == "MakerIFD") {

        // Makernotes need special handling
        // We only support a few different manufacturers for makernotes.

        // A few Canon tags are supported in this routine, the rest are
        // handled by the LookupMakerCanonTag() call.
        if (m_strImgExifMake == "Canon") {
            switch (nTag) {
                case 0x0001:
                    return QString("Canon.CameraSettings1");
                    break;
                case 0x0004:
                    return QString("Canon.CameraSettings2");
                    break;
                case 0x0006:
                    return QString("Canon.ImageType");
                    break;
                case 0x0007:
                    return QString("Canon.FirmwareVersion");
                    break;
                case 0x0008:
                    return QString("Canon.ImageNumber");
                    break;
                case 0x0009:
                    return QString("Canon.OwnerName");
                    break;
                case 0x000C:
                    return QString("Canon.SerialNumber");
                    break;
                case 0x000F:
                    return QString("Canon.CustomFunctions");
                    break;
                case 0x0012:
                    return QString("Canon.PictureInfo");
                    break;
                case 0x00A9:
                    return QString("Canon.WhiteBalanceTable");
                    break;

                default:
                    strTmp = QString("Canon.0x%1").arg(nTag, 4, 16, QChar('0'));
                    bUnknown = true;
                    return strTmp;
                    break;
            }
        }  // Canon
        else if (m_strImgExifMake == "SIGMA") {
            switch (nTag) {
                case 0x0002:
                    return QString("Sigma.SerialNumber");
                    break;                // Ascii Camera serial number
                case 0x0003:
                    return QString("Sigma.DriveMode");
                    break;                // Ascii Drive Mode
                case 0x0004:
                    return QString("Sigma.ResolutionMode");
                    break;                // Ascii Resolution Mode
                case 0x0005:
                    return QString("Sigma.AutofocusMode");
                    break;                // Ascii Autofocus mode
                case 0x0006:
                    return QString("Sigma.FocusSetting");
                    break;                // Ascii Focus setting
                case 0x0007:
                    return QString("Sigma.WhiteBalance");
                    break;                // Ascii White balance
                case 0x0008:
                    return QString("Sigma.ExposureMode");
                    break;                // Ascii Exposure mode
                case 0x0009:
                    return QString("Sigma.MeteringMode");
                    break;                // Ascii Metering mode
                case 0x000a:
                    return QString("Sigma.LensRange");
                    break;                // Ascii Lens focal length range
                case 0x000b:
                    return QString("Sigma.ColorSpace");
                    break;                // Ascii Color space
                case 0x000c:
                    return QString("Sigma.Exposure");
                    break;                // Ascii Exposure
                case 0x000d:
                    return QString("Sigma.Contrast");
                    break;                // Ascii Contrast
                case 0x000e:
                    return QString("Sigma.Shadow");
                    break;                // Ascii Shadow
                case 0x000f:
                    return QString("Sigma.Highlight");
                    break;                // Ascii Highlight
                case 0x0010:
                    return QString("Sigma.Saturation");
                    break;                // Ascii Saturation
                case 0x0011:
                    return QString("Sigma.Sharpness");
                    break;                // Ascii Sharpness
                case 0x0012:
                    return QString("Sigma.FillLight");
                    break;                // Ascii X3 Fill light
                case 0x0014:
                    return QString("Sigma.ColorAdjustment");
                    break;                // Ascii Color adjustment
                case 0x0015:
                    return QString("Sigma.AdjustmentMode");
                    break;                // Ascii Adjustment mode
                case 0x0016:
                    return QString("Sigma.Quality");
                    break;                // Ascii Quality
                case 0x0017:
                    return QString("Sigma.Firmware");
                    break;                // Ascii Firmware
                case 0x0018:
                    return QString("Sigma.Software");
                    break;                // Ascii Software
                case 0x0019:
                    return QString("Sigma.AutoBracket");
                    break;                // Ascii Auto bracket
                default:
                    strTmp = QString("Sigma.0x%1").arg(nTag, 4, 16, QChar('0'));
                    bUnknown = true;
                    return strTmp;
                    break;
            }
        }                           // SIGMA
        else if (m_strImgExifMake == "SONY") {
            switch (nTag) {
                case 0xb021:
                    return QString("Sony.ColorTemperature");
                    break;
                case 0xb023:
                    return QString("Sony.SceneMode");
                    break;
                case 0xb024:
                    return QString("Sony.ZoneMatching");
                    break;
                case 0xb025:
                    return QString("Sony.DynamicRangeOptimizer");
                    break;
                case 0xb026:
                    return QString("Sony.ImageStabilization");
                    break;
                case 0xb027:
                    return QString("Sony.LensID");
                    break;
                case 0xb029:
                    return QString("Sony.ColorMode");
                    break;
                case 0xb040:
                    return QString("Sony.Macro");
                    break;
                case 0xb041:
                    return QString("Sony.ExposureMode");
                    break;
                case 0xb047:
                    return QString("Sony.Quality");
                    break;
                case 0xb04e:
                    return QString("Sony.LongExposureNoiseReduction");
                    break;
                default:
                    // No real info is known
                    strTmp = QString("Sony.0x%1").arg(nTag, 4, 16, QChar('0'));
                    bUnknown = true;
                    return strTmp;
                    break;
            }
        }                           // SONY
        else if (m_strImgExifMake == "FUJIFILM") {
            switch (nTag) {
                case 0x0000:
                    return QString("Fujifilm.Version");
                    break;                // Undefined Fujifilm Makernote version
                case 0x1000:
                    return QString("Fujifilm.Quality");
                    break;                // Ascii Image quality setting
                case 0x1001:
                    return QString("Fujifilm.Sharpness");
                    break;                // Short Sharpness setting
                case 0x1002:
                    return QString("Fujifilm.WhiteBalance");
                    break;                // Short White balance setting
                case 0x1003:
                    return QString("Fujifilm.Color");
                    break;                // Short Chroma saturation setting
                case 0x1004:
                    return QString("Fujifilm.Tone");
                    break;                // Short Contrast setting
                case 0x1010:
                    return QString("Fujifilm.FlashMode");
                    break;                // Short Flash firing mode setting
                case 0x1011:
                    return QString("Fujifilm.FlashStrength");
                    break;                // SRational Flash firing strength compensation setting
                case 0x1020:
                    return QString("Fujifilm.Macro");
                    break;                // Short Macro mode setting
                case 0x1021:
                    return QString("Fujifilm.FocusMode");
                    break;                // Short Focusing mode setting
                case 0x1030:
                    return QString("Fujifilm.SlowSync");
                    break;                // Short Slow synchro mode setting
                case 0x1031:
                    return QString("Fujifilm.PictureMode");
                    break;                // Short Picture mode setting
                case 0x1100:
                    return QString("Fujifilm.Continuous");
                    break;                // Short Continuous shooting or auto bracketing setting
                case 0x1210:
                    return QString("Fujifilm.FinePixColor");
                    break;                // Short Fuji FinePix Color setting
                case 0x1300:
                    return QString("Fujifilm.BlurWarning");
                    break;                // Short Blur warning status
                case 0x1301:
                    return QString("Fujifilm.FocusWarning");
                    break;                // Short Auto Focus warning status
                case 0x1302:
                    return QString("Fujifilm.AeWarning");
                    break;                // Short Auto Exposure warning status
                default:
                    strTmp = QString("Fujifilm.0x%1").arg(nTag, 4, 16, QChar('0'));
                    bUnknown = true;
                    return strTmp;
                    break;
            }
        }                           // FUJIFILM
        else if (m_strImgExifMake == "NIKON") {
            if (m_nImgExifMakeSubtype == 1) {
                // Type 1
                switch (nTag) {
                    case 0x0001:
                        return QString("Nikon1.Version");
                        break;              // Undefined Nikon Makernote version
                    case 0x0002:
                        return QString("Nikon1.ISOSpeed");
                        break;              // Short ISO speed setting
                    case 0x0003:
                        return QString("Nikon1.ColorMode");
                        break;              // Ascii Color mode
                    case 0x0004:
                        return QString("Nikon1.Quality");
                        break;              // Ascii Image quality setting
                    case 0x0005:
                        return QString("Nikon1.WhiteBalance");
                        break;              // Ascii White balance
                    case 0x0006:
                        return QString("Nikon1.Sharpening");
                        break;              // Ascii Image sharpening setting
                    case 0x0007:
                        return QString("Nikon1.Focus");
                        break;              // Ascii Focus mode
                    case 0x0008:
                        return QString("Nikon1.Flash");
                        break;              // Ascii Flash mode
                    case 0x000f:
                        return QString("Nikon1.ISOSelection");
                        break;              // Ascii ISO selection
                    case 0x0010:
                        return QString("Nikon1.DataDump");
                        break;              // Undefined Data dump
                    case 0x0080:
                        return QString("Nikon1.ImageAdjustment");
                        break;              // Ascii Image adjustment setting
                    case 0x0082:
                        return QString("Nikon1.Adapter");
                        break;              // Ascii Adapter used
                    case 0x0085:
                        return QString("Nikon1.FocusDistance");
                        break;              // Rational Manual focus distance
                    case 0x0086:
                        return QString("Nikon1.DigitalZoom");
                        break;              // Rational Digital zoom setting
                    case 0x0088:
                        return QString("Nikon1.AFFocusPos");
                        break;              // Undefined AF focus position
                    default:
                        strTmp = QString("Nikon1.0x%1").arg(nTag, 4, 16, QChar('0'));
                        bUnknown = true;
                        return strTmp;
                        break;
                }
            } else if (m_nImgExifMakeSubtype == 2) {
                // Type 2
                switch (nTag) {
                    case 0x0003:
                        return QString("Nikon2.Quality");
                        break;              // Short Image quality setting
                    case 0x0004:
                        return QString("Nikon2.ColorMode");
                        break;              // Short Color mode
                    case 0x0005:
                        return QString("Nikon2.ImageAdjustment");
                        break;              // Short Image adjustment setting
                    case 0x0006:
                        return QString("Nikon2.ISOSpeed");
                        break;              // Short ISO speed setting
                    case 0x0007:
                        return QString("Nikon2.WhiteBalance");
                        break;              // Short White balance
                    case 0x0008:
                        return QString("Nikon2.Focus");
                        break;              // Rational Focus mode
                    case 0x000a:
                        return QString("Nikon2.DigitalZoom");
                        break;              // Rational Digital zoom setting
                    case 0x000b:
                        return QString("Nikon2.Adapter");
                        break;              // Short Adapter used
                    default:
                        strTmp = QString("Nikon2.0x%1").arg(nTag, 4, 16, QChar('0'));
                        bUnknown = true;
                        return strTmp;
                        break;
                }
            } else if (m_nImgExifMakeSubtype == 3) {
                // Type 3
                switch (nTag) {
                    case 0x0001:
                        return QString("Nikon3.Version");
                        break;              // Undefined Nikon Makernote version
                    case 0x0002:
                        return QString("Nikon3.ISOSpeed");
                        break;              // Short ISO speed used
                    case 0x0003:
                        return QString("Nikon3.ColorMode");
                        break;              // Ascii Color mode
                    case 0x0004:
                        return QString("Nikon3.Quality");
                        break;              // Ascii Image quality setting
                    case 0x0005:
                        return QString("Nikon3.WhiteBalance");
                        break;              // Ascii White balance
                    case 0x0006:
                        return QString("Nikon3.Sharpening");
                        break;              // Ascii Image sharpening setting
                    case 0x0007:
                        return QString("Nikon3.Focus");
                        break;              // Ascii Focus mode
                    case 0x0008:
                        return QString("Nikon3.FlashSetting");
                        break;              // Ascii Flash setting
                    case 0x0009:
                        return QString("Nikon3.FlashMode");
                        break;              // Ascii Flash mode
                    case 0x000b:
                        return QString("Nikon3.WhiteBalanceBias");
                        break;              // SShort White balance bias
                    case 0x000e:
                        return QString("Nikon3.ExposureDiff");
                        break;              // Undefined Exposure difference
                    case 0x000f:
                        return QString("Nikon3.ISOSelection");
                        break;              // Ascii ISO selection
                    case 0x0010:
                        return QString("Nikon3.DataDump");
                        break;              // Undefined Data dump
                    case 0x0011:
                        return QString("Nikon3.ThumbOffset");
                        break;              // Long Thumbnail IFD offset
                    case 0x0012:
                        return QString("Nikon3.FlashComp");
                        break;              // Undefined Flash compensation setting
                    case 0x0013:
                        return QString("Nikon3.ISOSetting");
                        break;              // Short ISO speed setting
                    case 0x0016:
                        return QString("Nikon3.ImageBoundary");
                        break;              // Short Image boundry
                    case 0x0018:
                        return QString("Nikon3.FlashBracketComp");
                        break;              // Undefined Flash bracket compensation applied
                    case 0x0019:
                        return QString("Nikon3.ExposureBracketComp");
                        break;              // SRational AE bracket compensation applied
                    case 0x0080:
                        return QString("Nikon3.ImageAdjustment");
                        break;              // Ascii Image adjustment setting
                    case 0x0081:
                        return QString("Nikon3.ToneComp");
                        break;              // Ascii Tone compensation setting (contrast
                    case 0x0082:
                        return QString("Nikon3.AuxiliaryLens");
                        break;              // Ascii Auxiliary lens (adapter
                    case 0x0083:
                        return QString("Nikon3.LensType");
                        break;              // Byte Lens type
                    case 0x0084:
                        return QString("Nikon3.Lens");
                        break;              // Rational Lens
                    case 0x0085:
                        return QString("Nikon3.FocusDistance");
                        break;              // Rational Manual focus distance
                    case 0x0086:
                        return QString("Nikon3.DigitalZoom");
                        break;              // Rational Digital zoom setting
                    case 0x0087:
                        return QString("Nikon3.FlashType");
                        break;              // Byte Type of flash used
                    case 0x0088:
                        return QString("Nikon3.AFFocusPos");
                        break;              // Undefined AF focus position
                    case 0x0089:
                        return QString("Nikon3.Bracketing");
                        break;              // Short Bracketing
                    case 0x008b:
                        return QString("Nikon3.LensFStops");
                        break;              // Undefined Number of lens stops
                    case 0x008c:
                        return QString("Nikon3.ToneCurve");
                        break;              // Undefined Tone curve
                    case 0x008d:
                        return QString("Nikon3.ColorMode");
                        break;              // Ascii Color mode
                    case 0x008f:
                        return QString("Nikon3.SceneMode");
                        break;              // Ascii Scene mode
                    case 0x0090:
                        return QString("Nikon3.LightingType");
                        break;              // Ascii Lighting type
                    case 0x0092:
                        return QString("Nikon3.HueAdjustment");
                        break;              // SShort Hue adjustment
                    case 0x0094:
                        return QString("Nikon3.Saturation");
                        break;              // SShort Saturation adjustment
                    case 0x0095:
                        return QString("Nikon3.NoiseReduction");
                        break;              // Ascii Noise reduction
                    case 0x0096:
                        return QString("Nikon3.CompressionCurve");
                        break;              // Undefined Compression curve
                    case 0x0097:
                        return QString("Nikon3.ColorBalance2");
                        break;              // Undefined Color balance 2
                    case 0x0098:
                        return QString("Nikon3.LensData");
                        break;              // Undefined Lens data
                    case 0x0099:
                        return QString("Nikon3.NEFThumbnailSize");
                        break;              // Short NEF thumbnail size
                    case 0x009a:
                        return QString("Nikon3.SensorPixelSize");
                        break;              // Rational Sensor pixel size
                    case 0x00a0:
                        return QString("Nikon3.SerialNumber");
                        break;              // Ascii Camera serial number
                    case 0x00a7:
                        return QString("Nikon3.ShutterCount");
                        break;              // Long Number of shots taken by camera
                    case 0x00a9:
                        return QString("Nikon3.ImageOptimization");
                        break;              // Ascii Image optimization
                    case 0x00aa:
                        return QString("Nikon3.Saturation");
                        break;              // Ascii Saturation
                    case 0x00ab:
                        return QString("Nikon3.VariProgram");
                        break;              // Ascii Vari program

                    default:
                        strTmp = QString("Nikon3.0x%1").arg(nTag, 4, 16, QChar('0'));
                        bUnknown = true;
                        return strTmp;
                        break;
                }
            }
        }                           // NIKON
    }                             // if strSect

    bUnknown = true;
    return QString("???");
}

//-----------------------------------------------------------------------------
// Interpret the MakerNote header to determine any applicable MakerNote subtype.
//
// PRE:
// - m_strImgExifMake
// - buffer
//
// INPUT:
// - none
//
// RETURN:
// - Decode success
//
// POST:
// - m_nImgExifMakeSubtype
//
bool JfifDecode::decodeMakerSubType() {
    QString strTmp;

    m_nImgExifMakeSubtype = 0;

    if (m_strImgExifMake == "NIKON") {
        strTmp = "";
        for (uint32_t nInd = 0; nInd < 5; nInd++) {
            strTmp += getByte(_pos + nInd);
        }

        if (strTmp == "Nikon") {
            if (getByte(_pos + 6) == 1) {
                // Type 1
                _log.info("    Nikon Makernote Type 1 detected");
                m_nImgExifMakeSubtype = 1;
                _pos += 8;
            } else if (getByte(_pos + 6) == 2) {
                // Type 3
                _log.info("    Nikon Makernote Type 3 detected");
                m_nImgExifMakeSubtype = 3;
                _pos += 18;
            } else {
                strTmp = "Unknown Nikon Makernote Type";
                _log.error(strTmp);

                return false;
            }
        } else {
            // Type 2
            _log.info("    Nikon Makernote Type 2 detected");
            //m_nImgExifMakeSubtype = 2;
            // tests on D1 seem to indicate that it uses Type 1 headers
            m_nImgExifMakeSubtype = 1;
            _pos += 0;
        }

    } else if (m_strImgExifMake == "SIGMA") {
        strTmp = "";
        for (uint32_t ind = 0; ind < 8; ind++) {
            if (getByte(_pos + ind) != 0)
                strTmp += getByte(_pos + ind);
        }
        if ((strTmp == "SIGMA") || (strTmp == "FOVEON")) {
            // Valid marker
            // Now skip over the 8-chars and 2 unknown chars
            _pos += 10;
        } else {
            strTmp = "Unknown SIGMA Makernote identifier";
            _log.error(strTmp);

            return false;
        }

    }                             // SIGMA
    else if (m_strImgExifMake == "FUJIFILM") {
        strTmp = "";

        for (uint32_t ind = 0; ind < 8; ind++) {
            if (getByte(_pos + ind) != 0)
                strTmp += getByte(_pos + ind);
        }

        if (strTmp == "FUJIFILM") {
            // Valid marker
            // Now skip over the 8-chars and 4 Pointer chars
            // FIXME: Do I need to dereference this pointer?
            _pos += 12;
        } else {
            strTmp = "Unknown FUJIFILM Makernote identifier";
            _log.error(strTmp);

            return false;
        }

    }                             // FUJIFILM
    else if (m_strImgExifMake == "SONY") {
        strTmp = "";

        for (uint32_t ind = 0; ind < 12; ind++) {
            if (getByte(_pos + ind) != 0)
                strTmp += getByte(_pos + ind);
        }

        if (strTmp == "SONY DSC ") {
            // Valid marker
            // Now skip over the 9-chars and 3 null chars
            _pos += 12;
        } else {
            strTmp = "Unknown SONY Makernote identifier";
            _log.error(strTmp);

            return false;
        }
    }                             // SONY

    return true;
}

//-----------------------------------------------------------------------------
// Read two UINT32 from the buffer (8B) and interpret
// as a rational entry. Convert to floating point.
// Byte swap as required
//
// INPUT:
// - pos                Buffer position
// - val                Floating point value
//
// RETURN:
// - Was the conversion successful?
//
bool JfifDecode::decodeValRational(uint32_t nPos, double &nVal) {
    int nValNumer;

    int nValDenom;

    nVal = 0;

    nValNumer = byteSwap4(getByte(nPos + 0), getByte(nPos + 1), getByte(nPos + 2), getByte(nPos + 3));
    nValDenom = byteSwap4(getByte(nPos + 4), getByte(nPos + 5), getByte(nPos + 6), getByte(nPos + 7));

    if (nValDenom == 0) {
        // Divide by zero!
        return false;
    } else {
        nVal = static_cast<double>(nValNumer) / static_cast<double>(nValDenom);
        return true;
    }
}

//-----------------------------------------------------------------------------
// Read two UINT32 from the buffer (8B) to create a formatted
// fraction string. Byte swap as required
//
// INPUT:
// - pos                Buffer position
//
// RETURN:
// - Formatted string
//
QString JfifDecode::decodeValFraction(uint32_t nPos) {
    QString strTmp;

    int nValNumer = readSwap4(nPos + 0);

    int nValDenom = readSwap4(nPos + 4);

    strTmp = QString("%1/%2").arg(nValNumer).arg(nValDenom);
    return strTmp;
}

//-----------------------------------------------------------------------------
// Convert multiple coordinates into a formatted GPS string
//
// INPUT:
// - nCount             Number of coordinates (1,2,3)
// - fCoord1    Coordinate #1
// - fCoord2    Coordinate #2
// - fCoord3    Coordinate #3
//
// OUTPUT:
// - strCoord   The formatted GPS string
//
// RETURN:
// - Was the conversion successful?
//
bool JfifDecode::printValGps(uint32_t nCount, double fCoord1, double fCoord2, double fCoord3, QString &coord) {
    double fTemp;

    uint32_t nCoordDeg;

    uint32_t nCoordMin;

    double fCoordSec;

    // TODO: Extend to support 1 & 2 coordinate GPS entries
    if (nCount == 3) {
        nCoordDeg = uint32_t(fCoord1);

        nCoordMin = uint32_t(fCoord2);

        if (fCoord3 == 0) {
            fTemp = fCoord2 - static_cast<double>(nCoordMin);
            fCoordSec = fTemp * static_cast<double>(60.0);
        } else {
            fCoordSec = fCoord3;
        }

        coord = QString("%1 deg %2' %.3f\"").arg(nCoordDeg).arg(nCoordMin).arg(fCoordSec);
        return true;
    } else {
        coord = QString("Can't handle %1-comonent GPS coords").arg(nCount);
        return false;
    }

}

//-----------------------------------------------------------------------------
// Read in 3 rational values from the buffer and output as a formatted GPS string
//
// INPUT:
// - pos                Buffer position
//
// OUTPUT:
// - strCoord   The formatted GPS string
//
// RETURN:
// - Was the conversion successful?
//
bool JfifDecode::decodeValGps(uint32_t nPos, QString &strCoord) {
    double fCoord1 = 0;
    double fCoord2 = 0;
    double fCoord3 = 0;

    bool bRet;

    bRet = true;
    if (bRet) {
        bRet = decodeValRational(nPos, fCoord1);
        nPos += 8;
    }

    if (bRet) {
        bRet = decodeValRational(nPos, fCoord2);
        nPos += 8;
    }

    if (bRet) {
        bRet = decodeValRational(nPos, fCoord3);
        nPos += 8;
    }

    if (!bRet) {
        strCoord = QString("???");
        return false;
    } else {
        return printValGps(3, fCoord1, fCoord2, fCoord3, strCoord);
    }
}

//-----------------------------------------------------------------------------
// Read a UINT16 from the buffer, byte swap as required
//
// INPUT:
// - nPos               Buffer position
//
// RETURN:
// - UINT16 from buffer
//
uint32_t JfifDecode::readSwap2(uint32_t nPos) {
    return byteSwap2(getByte(nPos + 0), getByte(nPos + 1));
}

//-----------------------------------------------------------------------------
// Read a UINT32 from the buffer, byte swap as required
//
// INPUT:
// - nPos               Buffer position
//
// RETURN:
// - UINT32 from buffer
//
uint32_t JfifDecode::readSwap4(uint32_t nPos) {
    return byteSwap4(getByte(nPos), getByte(nPos + 1), getByte(nPos + 2), getByte(nPos + 3));
}

//-----------------------------------------------------------------------------
// Read a UINT32 from the buffer, force as big endian
//
// INPUT:
// - nPos               Buffer position
//
// RETURN:
// - UINT32 from buffer
//
uint32_t JfifDecode::readBe4(uint32_t nPos) {
    // Big endian, no swap required
    return (getByte(nPos) << 24) + (getByte(nPos + 1) << 16) + (getByte(nPos + 2) << 8) + getByte(nPos + 3);
}

//-----------------------------------------------------------------------------
// Print hex from array of unsigned char
//
// INPUT:
// - anBytes    Array of unsigned chars
// - nCount             Indicates the number of array entries originally specified
//                              but the printing routine limits it to the maximum array depth
//                              allocated (MAX_anValues) and add an ellipsis "..."
// RETURN:
// - A formatted string
//
QString JfifDecode::printAsHexUc(uint8_t *anBytes, uint32_t nCount) {
    QString strVal;

    QString strFull;

    strFull = "0x[";
    uint32_t nMaxDisplay = MAX_anValues;

    bool bExceedMaxDisplay;

    bExceedMaxDisplay = (nCount > nMaxDisplay);
    for (uint32_t nInd = 0; nInd < nCount; nInd++) {
        if (nInd < nMaxDisplay) {
            if ((nInd % 4) == 0) {
                if (nInd == 0) {
                    // Don't do anything for first value!
                } else {
                    // Every 16 add big spacer / break
                    strFull += " ";
                }
            } else {
                //strFull += " ";
            }

            strVal = QString("%1").arg(anBytes[nInd], 2, 16, QChar('0'));
            strFull += strVal;
        }

        if ((nInd == nMaxDisplay) && (bExceedMaxDisplay)) {
            strFull += "...";
        }

    }

    strFull += "]";
    return strFull;
}

//-----------------------------------------------------------------------------
// Print hex from array of unsigned bytes
//
// INPUT:
// - anBytes    Array is passed as UINT32* even though it only
//                              represents a byte per entry
// - nCount             Indicates the number of array entries originally specified
//                              but the printing routine limits it to the maximum array depth
//                              allocated (MAX_anValues) and add an ellipsis "..."
//
// RETURN:
// - A formatted string
//
QString JfifDecode::printAsHex8(uint32_t *anBytes, uint32_t nCount) {
    QString strVal;

    QString strFull;

    uint32_t nMaxDisplay = MAX_anValues;

    bool bExceedMaxDisplay;

    strFull = "0x[";
    bExceedMaxDisplay = (nCount > nMaxDisplay);

    for (uint32_t nInd = 0; nInd < nCount; nInd++) {
        if (nInd < nMaxDisplay) {
            if ((nInd % 4) == 0) {
                if (nInd == 0) {
                    // Don't do anything for first value!
                } else {
                    // Every 16 add big spacer / break
                    strFull += " ";
                }
            }

            strVal = QString("%1").arg(anBytes[nInd], 2, 16, QChar('0'));
            strFull += strVal;
        }

        if ((nInd == nMaxDisplay) && (bExceedMaxDisplay)) {
            strFull += "...";
        }

    }

    strFull += "]";
    return strFull;
}

//-----------------------------------------------------------------------------
// Print hex from array of unsigned words
//
// INPUT:
// - anWords            Array of UINT32 is passed
// - nCount                     Indicates the number of array entries originally specified
//                                      but the printing routine limits it to the maximum array depth
//                                      allocated (MAX_anValues) and add an ellipsis "..."
//
// RETURN:
// - A formatted string
//
QString JfifDecode::printAsHex32(uint32_t *anWords, uint32_t nCount) {
    QString strVal;

    QString strFull;

    strFull = "0x[";
    uint32_t nMaxDisplay = MAX_anValues / 4;      // Reduce number of words to display since each is 32b not 8b

    bool bExceedMaxDisplay;

    bExceedMaxDisplay = (nCount > nMaxDisplay);

    for (uint32_t nInd = 0; nInd < nCount; nInd++) {
        if (nInd < nMaxDisplay) {
            if (nInd == 0) {
                // Don't do anything for first value!
            } else {
                // Every word add a spacer
                strFull += " ";
            }

            strVal = QString("%1").arg(anWords[nInd], 8, 16, QChar('0'));      // 32-bit words
            strFull += strVal;
        }

        if ((nInd == nMaxDisplay) && (bExceedMaxDisplay)) {
            strFull += "...";
        }

    }

    strFull += "]";
    return strFull;
}

//-----------------------------------------------------------------------------
// Process all of the entries within an EXIF IFD directory
// This is used for the main EXIF IFDs as well as MakerNotes
//
// INPUT:
// - ifdStr                             The IFD section that we are processing
// - pos_exif_start
// - start_ifd_ptr
//
// PRE:
// - m_strImgExifMake
// - m_bImgExifMakeSupported
// - m_nImgExifMakeSubtype
// - m_nImgExifMakerPtr
//
// RETURN:
// - 0                                  Decoding OK
// - 2                                  Decoding failure
//
// POST:
// - m_nPos
// - m_strImgExifMake
// - m_strImgExifModel
// - m_strImgQualExif
// - m_strImgExtras
// - m_strImgExifMake
// - m_nImgExifSubIfdPtr
// - m_nImgExifGpsIfdPtr
// - m_nImgExifInteropIfdPtr
// - m_bImgExifMakeSupported
// - m_bImgExifMakernotes
// - m_nImgExifMakerPtr
// - m_nImgExifThumbComp
// - m_nImgExifThumbOffset
// - m_nImgExifThumbLen
// - m_strSoftware
//
// NOTE:
// - IFD1 typically contains the thumbnail
//
uint32_t JfifDecode::decodeExifIfd(const QString &strIfd, uint32_t nPosExifStart, uint32_t nStartIfdPtr) {
    // Temp variables
    bool bRet;

    QString strTmp;

    CStr2 strRetVal;

    QString strValTmp;

    double fValReal;

    QString strMaker;

    // Display output variables
    QString strFull;

    QString strValOut;

    bool bExtraDecode;

    // Primary IFD variables
    uint8_t acIfdValOffsetStr[5];

    uint32_t nIfdDirLen;

    uint32_t nIfdTagVal;

    uint32_t nIfdFormat;

    uint32_t nIfdNumComps;

    bool nIfdTagUnknown;

    uint32_t nCompsToDisplay;     // Maximum number of values to capture for display

    uint32_t anValues[MAX_anValues];      // Array of decoded values (Uint32)

    int32_t anValuesS[MAX_anValues];       // Array of decoded values (Int32)

    double afValues[MAX_anValues]; // Array of decoded values (float)

    uint32_t nIfdOffset;          // First DWORD decode, usually offset

    // Clear values array
    for (uint32_t ind = 0; ind < MAX_anValues; ind++) {
        anValues[ind] = 0;
        anValuesS[ind] = 0;
        afValues[ind] = 0;
    }

    // ==========================================================================
    // Process IFD directory header
    // ==========================================================================

    // Move the file pointer to the start of the IFD
    _pos = nPosExifStart + nStartIfdPtr;

    strTmp = QString("  EXIF %1 @ Absolute 0x%2").arg(strIfd).arg(_pos, 8, 16, QChar('0'));
    _log.info(strTmp);

    ////////////

    // NOTE: Nikon type 3 starts out with the ASCII string "Nikon\0"
    //       before the rest of the items.
    // TODO: need to process type1,type2,type3
    // see: http://www.gvsoft.homedns.org/exif/makernote-nikon.html

    strTmp = QString("strIfd=[%1] m_strImgExifMake=[%2]").arg(strIfd).arg(m_strImgExifMake);
    dbgAddLine(strTmp);

    // If this is the MakerNotes section, then we may want to skip
    // altogether. Check to see if we are configured to process this
    // section or if it is a supported manufacturer.

    if (strIfd == "MakerIFD") {
        // Mark the image as containing Makernotes
        m_bImgExifMakernotes = true;

        if (!_appConfig.decodeMaker()) {
            strTmp = QString("    Makernote decode option not enabled.");
            _log.info(strTmp);

            // If user didn't enable makernote decode, don't exit, just
            // hide output. We still want to get at some info (such as Quality setting).
            // At end, we'll need to re-enable it again.
        }

        // If this Make is not supported, we'll need to exit
        if (!m_bImgExifMakeSupported) {
            strTmp = QString("    Makernotes not yet supported for [%1]").arg(m_strImgExifMake);
            _log.info(strTmp);
            return 2;
        }

        // Determine the sub-type of the Maker field (if applicable)
        // and advance the m_nPos pointer past the custom header.
        // This call uses the class members: Buf(),m_nPos
        if (!decodeMakerSubType()) {
            // If the subtype decode failed, skip the processing
            return 2;
        }

    }

    // ==========================================================================
    // Process IFD directory entries
    // ==========================================================================

    QString strIfdTag;

    // =========== EXIF IFD Header (Start) ===========
    // - Defined in Exif 2.2 Standard (JEITA CP-3451) section 4.6.2
    // - Contents (2 bytes total)
    //   - Number of fields (2 bytes)

    nIfdDirLen = readSwap2(_pos);
    _pos += 2;
    strTmp = QString("    Dir Length = 0x%1").arg(nIfdDirLen, 4, 16, QChar('0'));
    _log.info(strTmp);

    // =========== EXIF IFD Header (End) ===========

    // Start of IFD processing
    // Step through each IFD entry and determine the type and
    // decode accordingly.
    for (uint32_t nIfdEntryInd = 0; nIfdEntryInd < nIfdDirLen; nIfdEntryInd++) {
        // By default, show single-line value summary
        // bExtraDecode is used to indicate that additional special
        // parsing output is available for this entry
        bExtraDecode = false;

        strTmp = QString("    Entry #%1:").arg(nIfdEntryInd, 2, 10, QChar('0'));
        dbgAddLine(strTmp);

        // =========== EXIF IFD Interoperability entry (Start) ===========
        // - Defined in Exif 2.2 Standard (JEITA CP-3451) section 4.6.2
        // - Contents (12 bytes total)
        //   - Tag (2 bytes)
        //   - Type (2 bytes)
        //   - Count (4 bytes)
        //   - Value Offset (4 bytes)

        // Read Tag #
        nIfdTagVal = readSwap2(_pos);
        _pos += 2;
        nIfdTagUnknown = false;
        strIfdTag = lookupExifTag(strIfd, nIfdTagVal, nIfdTagUnknown);
        strTmp = QString("      Tag # = 0x%1 = [%2]").arg(nIfdTagVal, 4, 16, QChar('0')).arg(strIfdTag);
        dbgAddLine(strTmp);

        // Read Format (or Type)
        nIfdFormat = readSwap2(_pos);
        _pos += 2;
        strTmp = QString("      Format # = 0x%1").arg(nIfdFormat, 4, 16, QChar('0'));
        dbgAddLine(strTmp);

        // Read number of Components
        nIfdNumComps = readSwap4(_pos);
        _pos += 4;
        strTmp = QString("      # Comps = 0x%1").arg(nIfdNumComps, 4, 16, QChar('0'));
        dbgAddLine(strTmp);

        // Check to see how many components have been listed.
        // This helps trap errors in corrupted IFD segments, otherwise
        // we will hang trying to decode millions of entries!
        // See issue & testcase #1148
        if (nIfdNumComps > 4000) {
            // Warn user that we have clippped the component list.
            // Note that this condition is only relevant when we are
            // processing the general array fields. Fields such as MakerNote
            // will also enter this condition so we shouldn't warn in those cases.
            //
            // TODO: Defer this warning message until after we are sure that we
            // didn't handle the large dataset elsewhere.
            // For now, only report this warning if we are not processing MakerNote
            if (strIfdTag != "MakerNote") {
                strTmp = QString("      Excessive # components (%1). Limiting to first 4000.").arg(nIfdNumComps);
                _log.warn(strTmp);
            }
            nIfdNumComps = 4000;
        }

        // Read Component Value / Offset
        // We first treat it as a string and then re-interpret it as an integer

        // ... first as a string (just in case length <=4)
        for (uint32_t i = 0; i < 4; i++) {
            acIfdValOffsetStr[i] = getByte(_pos + i);
        }

        acIfdValOffsetStr[4] = '\0';

        // ... now as an unsigned value
        // This assignment is general-purpose, typically used when
        // we know that the IFD Value/Offset is just an offset
        nIfdOffset = readSwap4(_pos);
        strTmp = QString("      # Val/Offset = 0x%1").arg(nIfdOffset, 8, 16, QChar('0'));
        dbgAddLine(strTmp);

        // =========== EXIF IFD Interoperability entry (End) ===========

        // ==========================================================================
        // Extract the IFD component entries
        // ==========================================================================

        // The EXIF IFD entries can appear in a wide range of
        // formats / data types. The formats that have been
        // publicly documented include:
        //   EXIF_FORMAT_quint8       =  1,
        //   EXIF_FORMAT_ASCII      =  2,
        //   EXIF_FORMAT_SHORT      =  3,
        //   EXIF_FORMAT_LONG       =  4,
        //   EXIF_FORMAT_RATIONAL   =  5,
        //   EXIF_FORMAT_Squint8      =  6,
        //   EXIF_FORMAT_UNDEFINED  =  7,
        //   EXIF_FORMAT_SSHORT     =  8,
        //   EXIF_FORMAT_SLONG      =  9,
        //   EXIF_FORMAT_SRATIONAL  = 10,
        //   EXIF_FORMAT_FLOAT      = 11,
        //   EXIF_FORMAT_DOUBLE     = 12

        // The IFD variable formatter logic operates in two stages:
        // In the first stage, the format type is decoded, which results
        // in a generic decode for the IFD entry. Then, we start a second
        // stage which re-interprets the values for a number of known
        // special types.

        switch (nIfdFormat) {
            // ----------------------------------------
            // --- IFD Entry Type: Unsigned Byte
            // ----------------------------------------
            case 1:
                strFull = "        Unsigned Byte=[";
                strValOut = "";

                // Limit display output
                nCompsToDisplay = qMin(uint32_t(MAX_anValues), nIfdNumComps);

                // If only a single value, use decimal, else use hex
                if (nIfdNumComps == 1) {
                    anValues[0] = getByte(_pos + 0);
                    strTmp = QString("%1").arg(anValues[0]);
                    strValOut += strTmp;
                } else {
                    for (uint32_t nInd = 0; nInd < nCompsToDisplay; nInd++) {
                        if (nIfdNumComps <= 4) {
                            // Components fit inside 4B inline region
                            anValues[nInd] = getByte(_pos + nInd);
                        } else {
                            // Since the components don't fit inside 4B inline region
                            // we need to dereference
                            anValues[nInd] = getByte(nPosExifStart + nIfdOffset + nInd);
                        }
                    }

                    strValOut = printAsHex8(anValues, nIfdNumComps);
                }

                strFull += strValOut;
                strFull += "]";
                dbgAddLine(strFull);
                break;

                // ----------------------------------------
                // --- IFD Entry Type: ASCII string
                // ----------------------------------------
            case 2:
                strFull = "        String=";
                strValOut = "";
                char cVal;

                quint8 nVal;

                // Limit display output
                // TODO: Decide what an appropriate string limit would be
                nCompsToDisplay = qMin(uint(250), nIfdNumComps);

                for (uint32_t nInd = 0; nInd < nCompsToDisplay; nInd++) {
                    if (nIfdNumComps <= 4) {
                        nVal = acIfdValOffsetStr[nInd];
                    } else {
                        // TODO: See if this can be migrated to the "custom decode"
                        // section later in the code. Decoding makernotes here is
                        // less desirable but unfortunately some Nikon makernotes use
                        // a non-standard offset value.
                        if ((strIfd == "MakerIFD") && (m_strImgExifMake == "NIKON") && (m_nImgExifMakeSubtype == 3)) {
                            // It seems that pointers in the Nikon Makernotes are
                            // done relative to the start of Maker IFD
                            // But why 10? Is this 10 = 18-8?
                            nVal = getByte(nPosExifStart + m_nImgExifMakerPtr + nIfdOffset + 10 + nInd);
                        } else if ((strIfd == "MakerIFD") && (m_strImgExifMake == "NIKON")) {
                            // It seems that pointers in the Nikon Makernotes are
                            // done relative to the start of Maker IFD
                            nVal = getByte(nPosExifStart + nIfdOffset + 0 + nInd);
                        } else {
                            // Canon Makernotes seem to be relative to the start
                            // of the EXIF IFD
                            nVal = getByte(nPosExifStart + nIfdOffset + nInd);
                        }
                    }

                    // Just in case the string has been null-terminated early
                    // or we have garbage, replace with '.'
                    // TODO: Clean this up
                    if (nVal != 0) {
                        cVal = static_cast<char>(nVal);

                        if (!isprint(nVal)) {
                            cVal = '.';
                        }

                        strValOut += cVal;
                    }
                }

                strFull += strValOut;
                dbgAddLine(strFull);

                // TODO: Ideally, we would use a different string for display
                // purposes that is wrapped in quotes. Currently "strValOut" is used
                // in other sections of code (eg. in assignment to EXIF Make/Model/Software, etc.)
                // so we don't want to affect that.

                break;

                // ----------------------------------------
                // --- IFD Entry Type: Unsigned Short (2 bytes)
                // ----------------------------------------
            case 3:
                // Limit display output
                nCompsToDisplay = qMin(MAX_anValues, nIfdNumComps);

                // Unsigned Short (2 bytes)
                if (nIfdNumComps == 1) {
                    strFull = "        Unsigned Short=[";
                    // TODO: Confirm endianness is correct here
                    // Refer to Exif2-2 spec, page 14.
                    // Currently selecting 2 byte conversion from [1:0] out of [3:0]
                    anValues[0] = readSwap2(_pos);
                    strValOut = QString("%1").arg(anValues[0]);
                    strFull += strValOut;
                    strFull += "]";
                    dbgAddLine(strFull);
                } else if (nIfdNumComps == 2) {
                    strFull = "        Unsigned Short=[";
                    // 2 unsigned shorts in 1 word
                    anValues[0] = readSwap2(_pos + 0);
                    anValues[1] = readSwap2(_pos + 2);
                    strValOut = QString("%1, %1").arg(anValues[0]).arg(anValues[1]);
                    strFull += strValOut;
                    strFull += "]";
                    dbgAddLine(strFull);
                } else if (nIfdNumComps > MAX_IFD_COMPS) {
                    strValTmp = QString("    Unsigned Short=[Too many entries (%1) to display]").arg(nIfdNumComps);
                    dbgAddLine(strValTmp);
                    strValOut = QString("[Too many entries (%1) to display]").arg(nIfdNumComps);
                } else {
                    // Try to handle multiple entries... note that this
                    // is used by the Maker notes IFD decode

                    strValOut = "";
                    strFull = "        Unsigned Short=[";

                    for (uint32_t nInd = 0; nInd < nCompsToDisplay; nInd++) {
                        if (nInd != 0) {
                            strValOut += ", ";
                        }

                        anValues[nInd] = readSwap2(nPosExifStart + nIfdOffset + (2 * nInd));
                        strValTmp = QString("%1").arg(anValues[nInd]);
                        strValOut += strValTmp;
                    }

                    strFull += strValOut;
                    strFull += "]";
                    dbgAddLine(strFull);
                }

                break;

                // ----------------------------------------
                // --- IFD Entry Type: Unsigned Long (4 bytes)
                // ----------------------------------------
            case 4:
                strFull = "        Unsigned Long=[";
                strValOut = "";

                // Limit display output
                nCompsToDisplay = qMin(MAX_anValues, nIfdNumComps);

                for (uint32_t nInd = 0; nInd < nCompsToDisplay; nInd++) {
                    if (nIfdNumComps == 1) {
                        // Components fit inside 4B inline region
                        anValues[nInd] = readSwap4(_pos + (nInd * 4));
                    } else {
                        // Since the components don't fit inside 4B inline region
                        // we need to dereference
                        anValues[nInd] = readSwap4(nPosExifStart + nIfdOffset + (nInd * 4));
                    }
                }

                strValOut = printAsHex32(anValues, nIfdNumComps);

                // If we only have a single component, then display both the hex and decimal
                if (nCompsToDisplay == 1) {
                    strTmp = QString("%1 / %2").arg(strValOut).arg(anValues[0]);
                    strValOut = strTmp;
                }

                break;

                // ----------------------------------------
                // --- IFD Entry Type: Unsigned Rational (8 bytes)
                // ----------------------------------------
            case 5:
                // Unsigned Rational
                strFull = "        Unsigned Rational=[";
                strValOut = "";

                // Limit display output
                nCompsToDisplay = qMin(MAX_anValues, nIfdNumComps);

                for (uint32_t nInd = 0; nInd < nCompsToDisplay; nInd++) {
                    if (nInd != 0) {
                        strValOut += ", ";
                    }

                    strValTmp = decodeValFraction(nPosExifStart + nIfdOffset + (nInd * 8));
                    bRet = decodeValRational(nPosExifStart + nIfdOffset + (nInd * 8), fValReal);
                    afValues[nInd] = fValReal;
                    strValOut += strValTmp;
                }

                strFull += strValOut;
                strFull += "]";
                dbgAddLine(strFull);
                break;

                // ----------------------------------------
                // --- IFD Entry Type: Undefined (?)
                // ----------------------------------------
            case 7:
                // Undefined -- assume 1 word long
                // This is supposed to be a series of 8-bit bytes
                // It is usually used for 32-bit pointers (in case of offsets), but could
                // also represent ExifVersion, etc.
                strFull = "        Undefined=[";
                strValOut = "";

                // Limit display output
                nCompsToDisplay = qMin(MAX_anValues, nIfdNumComps);

                if (nIfdNumComps <= 4) {
                    // This format is not defined, so output as hex for now
                    for (uint32_t nInd = 0; nInd < nCompsToDisplay; nInd++) {
                        anValues[nInd] = getByte(_pos + nInd);
                    }

                    strValOut = printAsHex8(anValues, nIfdNumComps);
                    strFull += strValOut;
                } else {

                    // Dereference pointer
                    for (uint32_t nInd = 0; nInd < nCompsToDisplay; nInd++) {
                        anValues[nInd] = getByte(nPosExifStart + nIfdOffset + nInd);
                    }

                    strValOut = printAsHex8(anValues, nIfdNumComps);
                    strFull += strValOut;
                }

                strFull += "]";
                dbgAddLine(strFull);

                break;

                // ----------------------------------------
                // --- IFD Entry Type: Signed Short (2 bytes)
                // ----------------------------------------
            case 8:
                // Limit display output
                nCompsToDisplay = qMin(MAX_anValues, nIfdNumComps);

                // Signed Short (2 bytes)
                if (nIfdNumComps == 1) {
                    strFull = "        Signed Short=[";
                    // TODO: Confirm endianness is correct here
                    // Refer to Exif2-2 spec, page 14.
                    // Currently selecting 2 byte conversion from [1:0] out of [3:0]

                    // TODO: Ensure that ReadSwap2 handles signed notation properly
                    anValuesS[0] = readSwap2(_pos);
                    strValOut = QString("%1").arg(anValuesS[0]);
                    strFull += strValOut;
                    strFull += "]";
                    dbgAddLine(strFull);
                } else if (nIfdNumComps == 2) {
                    strFull = "        Signed Short=[";
                    // 2 signed shorts in 1 word

                    // TODO: Ensure that ReadSwap2 handles signed notation properly
                    anValuesS[0] = readSwap2(_pos + 0);
                    anValuesS[1] = readSwap2(_pos + 2);
                    strValOut = QString("%1, %1").arg(anValuesS[0]).arg(anValuesS[0]);
                    strFull += strValOut;
                    strFull += "]";
                    dbgAddLine(strFull);
                } else if (nIfdNumComps > MAX_IFD_COMPS) {
                    // Only print it out if it has less than MAX_IFD_COMPS entries
                    strValTmp = QString("    Signed Short=[Too many entries (%1) to display]").arg(nIfdNumComps);
                    dbgAddLine(strValTmp);
                    strValOut = QString("[Too many entries (%1) to display]").arg(nIfdNumComps);
                } else {
                    // Try to handle multiple entries... note that this
                    // is used by the Maker notes IFD decode

                    // Note that we don't call LookupMakerCanonTag() here
                    // as that is only needed for the "unsigned short", not
                    // "signed short".
                    strValOut = "";
                    strFull = "        Signed Short=[";

                    for (uint32_t nInd = 0; nInd < nCompsToDisplay; nInd++) {
                        if (nInd != 0) {
                            strValOut += ", ";
                        }

                        anValuesS[nInd] = readSwap2(nPosExifStart + nIfdOffset + (2 * nInd));
                        strValTmp = QString("%1").arg(anValuesS[nInd]);
                        strValOut += strValTmp;
                    }

                    strFull += strValOut;
                    strFull += "]";
                    dbgAddLine(strFull);
                }

                break;

                // ----------------------------------------
                // --- IFD Entry Type: Signed Rational (8 bytes)
                // ----------------------------------------
            case 10:
                // Signed Rational
                strFull = "        Signed Rational=[";
                strValOut = "";

                // Limit display output
                nCompsToDisplay = qMin(MAX_anValues, nIfdNumComps);

                for (uint32_t nInd = 0; nInd < nCompsToDisplay; nInd++) {
                    if (nInd != 0) {
                        strValOut += ", ";
                    }

                    strValTmp = decodeValFraction(nPosExifStart + nIfdOffset + (nInd * 8));
                    bRet = decodeValRational(nPosExifStart + nIfdOffset + (nInd * 8), fValReal);
                    afValues[nInd] = fValReal;
                    strValOut += strValTmp;
                }

                strFull += strValOut;
                strFull += "]";
                dbgAddLine(strFull);
                break;

            default:
                strTmp = QString("Unsupported format [%1]").arg(nIfdFormat);
                anValues[0] = readSwap4(_pos);
                strValOut = QString("0x%1???").arg(anValues[0], 4, 16, QChar('0'));
                return 2;
                break;
        }                           // switch nIfdTagVal

        // ==========================================================================
        // Custom Value String decodes
        // ==========================================================================

        // At this point we might re-format the values, thereby
        // overriding the default strValOut. We have access to the
        //   anValues[]  (array of unsigned int)
        //   anValuesS[] (array of signed int)
        //   afValues[]  (array of float)

        // Re-format special output items
        //   This will override "strValOut" that may have previously been defined

        if ((strIfdTag == "GPSLatitude") || (strIfdTag == "GPSLongitude")) {
            bRet = printValGps(nIfdNumComps, afValues[0], afValues[1], afValues[2], strValOut);
        } else if (strIfdTag == "GPSVersionID") {
            strValOut = QString("%1.%2.%3.%4").arg(anValues[0]).arg(anValues[1]).arg(anValues[2]).arg(anValues[3]);
        } else if (strIfdTag == "GPSAltitudeRef") {
            switch (anValues[0]) {
                case 0:
                    strValOut = "Above Sea Level";
                    break;
                case 1:
                    strValOut = "Below Sea Level";
                    break;
            }
        } else if (strIfdTag == "GPSStatus") {
            switch (acIfdValOffsetStr[0]) {
                case 'A':
                    strValOut = "Measurement in progress";
                    break;
                case 'V':
                    strValOut = "Measurement Interoperability";
                    break;
            }
        } else if (strIfdTag == "GPSMeasureMode") {
            switch (acIfdValOffsetStr[0]) {
                case '2':
                    strValOut = "2-dimensional";
                    break;
                case '3':
                    strValOut = "3-dimensional";
                    break;
            }
        } else if ((strIfdTag == "GPSSpeedRef") || (strIfdTag == "GPSDestDistanceRef")) {
            switch (acIfdValOffsetStr[0]) {
                case 'K':
                    strValOut = "km/h";
                    break;
                case 'M':
                    strValOut = "mph";
                    break;
                case 'N':
                    strValOut = "knots";
                    break;
            }
        } else if ((strIfdTag == "GPSTrackRef") || (strIfdTag == "GPSImgDirectionRef") ||
                   (strIfdTag == "GPSDestBearingRef")) {
            switch (acIfdValOffsetStr[0]) {
                case 'T':
                    strValOut = "True direction";
                    break;
                case 'M':
                    strValOut = "Magnetic direction";
                    break;
            }
        } else if (strIfdTag == "GPSDifferential") {
            switch (anValues[0]) {
                case 0:
                    strValOut = "Measurement without differential correction";
                    break;
                case 1:
                    strValOut = "Differential correction applied";
                    break;
            }
        } else if (strIfdTag == "GPSAltitude") {
            strValOut = QString("%.3f m").arg(afValues[0]);
        } else if (strIfdTag == "GPSSpeed") {
            strValOut = QString("%.3f").arg(afValues[0]);
        } else if (strIfdTag == "GPSTimeStamp") {
            strValOut = QString("%.0f:%.0f:%.2f").arg(afValues[0]).arg(afValues[1]).arg(afValues[2]);
        } else if (strIfdTag == "GPSTrack") {
            strValOut = QString("%1").arg(afValues[0], 0, 'f', 2);
        } else if (strIfdTag == "GPSDOP") {
            strValOut = QString("%1").arg(afValues[0], 0, 'f', 4);
        }

        if (strIfdTag == "Compression") {
            switch (anValues[0]) {
                case 1:
                    strValOut = "None";
                    break;
                case 6:
                    strValOut = "JPEG";
                    break;
            }
        } else if (strIfdTag == "ExposureTime") {
            // Assume only one
            strValTmp = strValOut;
            strValOut = QString("%1 s").arg(strValTmp);
        } else if (strIfdTag == "FNumber") {
            // Assume only one
            strValOut = QString("F%1").arg(afValues[0], 0, 'f', 1);
        } else if (strIfdTag == "FocalLength") {
            // Assume only one
            strValOut = QString("%1 mm").arg(afValues[0], 0, 'f', 0);
        } else if (strIfdTag == "ExposureBiasValue") {
            // Assume only one
            // TODO: Need to test negative numbers
            strValOut = QString("%1 eV").arg(afValues[0], 0, 'f', 2);
        } else if (strIfdTag == "ExifVersion") {
            // Assume only one
            strValOut = QString("%1%2.%3%4")
                .arg(static_cast<char>(anValues[0]))
                .arg(static_cast<char>(anValues[1]))
                .arg(static_cast<char>(anValues[2]))
                .arg(static_cast<char>(anValues[3]));
        } else if (strIfdTag == "FlashPixVersion") {
            // Assume only one
            strValOut = QString("%1%2.%3%4")
                .arg(static_cast<char>(anValues[0]))
                .arg(static_cast<char>(anValues[1]))
                .arg(static_cast<char>(anValues[2]))
                .arg(static_cast<char>(anValues[3]));
        } else if (strIfdTag == "PhotometricInterpretation") {
            switch (anValues[0]) {
                case 1:
                    strValOut = "Monochrome";
                    break;
                case 2:
                    strValOut = "RGB";
                    break;
                case 6:
                    strValOut = "YCbCr";
                    break;
            }
        } else if (strIfdTag == "Orientation") {
            switch (anValues[0]) {
                case 1:
                    strValOut = "1 = Row 0: top, Col 0: left";
                    break;
                case 2:
                    strValOut = "2 = Row 0: top, Col 0: right";
                    break;
                case 3:
                    strValOut = "3 = Row 0: bottom, Col 0: right";
                    break;
                case 4:
                    strValOut = "4 = Row 0: bottom, Col 0: left";
                    break;
                case 5:
                    strValOut = "5 = Row 0: left, Col 0: top";
                    break;
                case 6:
                    strValOut = "6 = Row 0: right, Col 0: top";
                    break;
                case 7:
                    strValOut = "7 = Row 0: right, Col 0: bottom";
                    break;
                case 8:
                    strValOut = "8 = Row 0: left, Col 0: bottom";
                    break;
            }
        } else if (strIfdTag == "PlanarConfiguration") {
            switch (anValues[0]) {
                case 1:
                    strValOut = "Chunky format";
                    break;
                case 2:
                    strValOut = "Planar format";
                    break;
            }
        } else if (strIfdTag == "YCbCrSubSampling") {
            switch (anValues[0] * 65536 + anValues[1]) {
                case 0x00020001:
                    strValOut = "4:2:2";
                    break;
                case 0x00020002:
                    strValOut = "4:2:0";
                    break;
            }
        } else if (strIfdTag == "YCbCrPositioning") {
            switch (anValues[0]) {
                case 1:
                    strValOut = "Centered";
                    break;
                case 2:
                    strValOut = "Co-sited";
                    break;
            }
        } else if (strIfdTag == "ResolutionUnit") {
            switch (anValues[0]) {
                case 1:
                    strValOut = "None";
                    break;
                case 2:
                    strValOut = "Inch";
                    break;
                case 3:
                    strValOut = "Centimeter";
                    break;
            }
        } else if (strIfdTag == "FocalPlaneResolutionUnit") {
            switch (anValues[0]) {
                case 1:
                    strValOut = "None";
                    break;
                case 2:
                    strValOut = "Inch";
                    break;
                case 3:
                    strValOut = "Centimeter";
                    break;
            }
        } else if (strIfdTag == "ColorSpace") {
            switch (anValues[0]) {
                case 1:
                    strValOut = "sRGB";
                    break;
                case 0xFFFF:
                    strValOut = "Uncalibrated";
                    break;
            }
        } else if (strIfdTag == "ComponentsConfiguration") {
            // Undefined type, assume 4 bytes
            strValOut = "[";
            for (uint32_t vind = 0; vind < 4; vind++) {
                if (vind != 0) {
                    strValOut += " ";
                }
                switch (anValues[vind]) {
                    case 0:
                        strValOut += ".";
                        break;
                    case 1:
                        strValOut += "Y";
                        break;
                    case 2:
                        strValOut += "Cb";
                        break;
                    case 3:
                        strValOut += "Cr";
                        break;
                    case 4:
                        strValOut += "R";
                        break;
                    case 5:
                        strValOut += "G";
                        break;
                    case 6:
                        strValOut += "B";
                        break;
                    default:
                        strValOut += "?";
                        break;
                }
            }

            strValOut += "]";

        } else if ((strIfdTag == "XPTitle") ||
                   (strIfdTag == "XPComment") || (strIfdTag == "XPAuthor") || (strIfdTag == "XPKeywords") ||
                   (strIfdTag == "XPSubject")) {
            strValOut = "\"";
            QString strVal;

            strVal = _wbuf.readUniStr2(nPosExifStart + nIfdOffset, nIfdNumComps);
            strValOut += strVal;
            strValOut += "\"";
        } else if (strIfdTag == "UserComment") {
            // Character code
            uint32_t anCharCode[8];

            for (uint32_t vInd = 0; vInd < 8; vInd++) {
                anCharCode[vInd] = getByte(nPosExifStart + nIfdOffset + 0 + vInd);
            }

            // Actual string
            strValOut = "\"";
            bool bDone = false;

            uint8_t cTmp;

            for (uint32_t vInd = 0; (vInd < nIfdNumComps - 8) && (!bDone); vInd++) {
                cTmp = getByte(nPosExifStart + nIfdOffset + 8 + vInd);

                if (cTmp == 0) {
                    bDone = true;
                } else {
                    strValOut += cTmp;
                }
            }

            strValOut += "\"";
        } else if (strIfdTag == "MeteringMode") {
            switch (anValues[0]) {
                case 0:
                    strValOut = "Unknown";
                    break;
                case 1:
                    strValOut = "Average";
                    break;
                case 2:
                    strValOut = "CenterWeightedAverage";
                    break;
                case 3:
                    strValOut = "Spot";
                    break;
                case 4:
                    strValOut = "MultiSpot";
                    break;
                case 5:
                    strValOut = "Pattern";
                    break;
                case 6:
                    strValOut = "Partial";
                    break;
                case 255:
                    strValOut = "Other";
                    break;
            }
        } else if (strIfdTag == "ExposureProgram") {
            switch (anValues[0]) {
                case 0:
                    strValOut = "Not defined";
                    break;
                case 1:
                    strValOut = "Manual";
                    break;
                case 2:
                    strValOut = "Normal program";
                    break;
                case 3:
                    strValOut = "Aperture priority";
                    break;
                case 4:
                    strValOut = "Shutter priority";
                    break;
                case 5:
                    strValOut = "Creative program (depth of field)";
                    break;
                case 6:
                    strValOut = "Action program (fast shutter speed)";
                    break;
                case 7:
                    strValOut = "Portrait mode";
                    break;
                case 8:
                    strValOut = "Landscape mode";
                    break;
            }
        } else if (strIfdTag == "Flash") {
            switch (anValues[0] & 1) {
                case 0:
                    strValOut = "Flash did not fire";
                    break;
                case 1:
                    strValOut = "Flash fired";
                    break;
            }
            // TODO: Add other bitfields?
        } else if (strIfdTag == "SensingMethod") {
            switch (anValues[0]) {
                case 1:
                    strValOut = "Not defined";
                    break;
                case 2:
                    strValOut = "One-chip color area sensor";
                    break;
                case 3:
                    strValOut = "Two-chip color area sensor";
                    break;
                case 4:
                    strValOut = "Three-chip color area sensor";
                    break;
                case 5:
                    strValOut = "Color sequential area sensor";
                    break;
                case 7:
                    strValOut = "Trilinear sensor";
                    break;
                case 8:
                    strValOut = "Color sequential linear sensor";
                    break;
            }
        } else if (strIfdTag == "FileSource") {
            switch (anValues[0]) {
                case 3:
                    strValOut = "DSC";
                    break;
            }
        } else if (strIfdTag == "CustomRendered") {
            switch (anValues[0]) {
                case 0:
                    strValOut = "Normal process";
                    break;
                case 1:
                    strValOut = "Custom process";
                    break;
            }
        } else if (strIfdTag == "ExposureMode") {
            switch (anValues[0]) {
                case 0:
                    strValOut = "Auto exposure";
                    break;
                case 1:
                    strValOut = "Manual exposure";
                    break;
                case 2:
                    strValOut = "Auto bracket";
                    break;
            }
        } else if (strIfdTag == "WhiteBalance") {
            switch (anValues[0]) {
                case 0:
                    strValOut = "Auto white balance";
                    break;
                case 1:
                    strValOut = "Manual white balance";
                    break;
            }
        } else if (strIfdTag == "SceneCaptureType") {
            switch (anValues[0]) {
                case 0:
                    strValOut = "Standard";
                    break;
                case 1:
                    strValOut = "Landscape";
                    break;
                case 2:
                    strValOut = "Portrait";
                    break;
                case 3:
                    strValOut = "Night scene";
                    break;
            }
        } else if (strIfdTag == "SceneType") {
            switch (anValues[0]) {
                case 1:
                    strValOut = "A directly photographed image";
                    break;
            }
        } else if (strIfdTag == "LightSource") {
            switch (anValues[0]) {
                case 0:
                    strValOut = "unknown";
                    break;
                case 1:
                    strValOut = "Daylight";
                    break;
                case 2:
                    strValOut = "Fluorescent";
                    break;
                case 3:
                    strValOut = "Tungsten (incandescent light)";
                    break;
                case 4:
                    strValOut = "Flash";
                    break;
                case 9:
                    strValOut = "Fine weather";
                    break;
                case 10:
                    strValOut = "Cloudy weather";
                    break;
                case 11:
                    strValOut = "Shade";
                    break;
                case 12:
                    strValOut = "Daylight fluorescent (D 5700 � 7100K)";
                    break;
                case 13:
                    strValOut = "Day white fluorescent (N 4600 � 5400K)";
                    break;
                case 14:
                    strValOut = "Cool white fluorescent (W 3900 � 4500K)";
                    break;
                case 15:
                    strValOut = "White fluorescent (WW 3200 � 3700K)";
                    break;
                case 17:
                    strValOut = "Standard light A";
                    break;
                case 18:
                    strValOut = "Standard light B";
                    break;
                case 19:
                    strValOut = "Standard light C";
                    break;
                case 20:
                    strValOut = "D55";
                    break;
                case 21:
                    strValOut = "D65";
                    break;
                case 22:
                    strValOut = "D75";
                    break;
                case 23:
                    strValOut = "D50";
                    break;
                case 24:
                    strValOut = "ISO studio tungsten";
                    break;
                case 255:
                    strValOut = "other light source";
                    break;
            }
        } else if (strIfdTag == "SubjectArea") {
            switch (nIfdNumComps) {
                case 2:
                    // coords
                    strValOut = QString("Coords: Center=[%1,%2]").arg(anValues[0]).arg(anValues[1]);
                    break;
                case 3:
                    // circle
                    strValOut = QString("Coords (Circle): Center=[%1,%2] Diameter=%3").arg(anValues[0]).arg(anValues[1]).arg(
                        anValues[2]);
                    break;
                case 4:
                    // rectangle
                    strValOut =
                        QString("Coords (Rect): Center=[%1,%2] Width=%3 Height=%4").arg(anValues[0]).arg(anValues[1]).arg(
                            anValues[2]).
                            arg(anValues[3]);
                    break;
                default:
                    // Leave default decode, unexpected value
                    break;
            }
        }

        if (strIfdTag == "CFAPattern") {
            uint32_t nHorzRepeat, nVertRepeat;
            uint32_t anCfaVal[16][16];
            uint32_t nInd = 0;
            uint32_t nVal;

            QString strLine, strCol;

            nHorzRepeat = anValues[nInd + 0] * 256 + anValues[nInd + 1];
            nVertRepeat = anValues[nInd + 2] * 256 + anValues[nInd + 3];
            nInd += 4;

            if ((nHorzRepeat < 16) && (nVertRepeat < 16)) {
                bExtraDecode = true;
                strTmp = QString("    [%1] =").arg(strIfdTag, -36);
                _log.info(strTmp);

                for (uint32_t nY = 0; nY < nVertRepeat; nY++) {
                    strLine = QString("     %1  = [ ").arg("", -36);

                    for (uint32_t nX = 0; nX < nHorzRepeat; nX++) {
                        if (nInd < MAX_anValues) {
                            nVal = anValues[nInd++];
                            anCfaVal[nY][nX] = nVal;

                            switch (nVal) {
                                case 0:
                                    strCol = "Red";
                                    break;
                                case 1:
                                    strCol = "Grn";
                                    break;
                                case 2:
                                    strCol = "Blu";
                                    break;
                                case 3:
                                    strCol = "Cya";
                                    break;
                                case 4:
                                    strCol = "Mgn";
                                    break;
                                case 5:
                                    strCol = "Yel";
                                    break;
                                case 6:
                                    strCol = "Wht";
                                    break;
                                default:
                                    strCol = QString("x%1").arg(nVal, 2, 16, QChar('0'));
                                    break;
                            }

                            strLine.append(QString("%1 ").arg(strCol));
                        }
                    }

                    strLine.append("]");
                    _log.info(strLine);
                }
            }
        }

        if ((strIfd == "InteropIFD") && (strIfdTag == "InteroperabilityVersion")) {
            // Assume only one
            strValOut = QString("%1%2.%3%4")
                .arg(static_cast<char>(anValues[0]))
                .arg(static_cast<char>(anValues[1]))
                .arg(static_cast<char>(anValues[2]))
                .arg(static_cast<char>(anValues[3]));
        }

        // ==========================================================================

        // ----------------------------------------
        // Handle certain MakerNotes
        //   For Canon, we have a special parser routine to handle these
        // ----------------------------------------
        if (strIfd == "MakerIFD") {

            if ((m_strImgExifMake == "Canon") && (nIfdFormat == 3) && (nIfdNumComps > 4)) {
                // Print summary line now, before sub details
                // Disable later summary line
                bExtraDecode = true;

                if ((!_appConfig.hideUnknownExif()) || (!nIfdTagUnknown)) {
                    strTmp = QString("    [%1]").arg(strIfdTag, -36);
                    _log.info(strTmp);

                    // Assume it is a maker field with subentries!

                    for (uint32_t ind = 0; ind < nIfdNumComps; ind++) {
                        // Limit the number of entries (in case there was a decode error
                        // or simply too many to report)
                        if (ind < MAX_anValues) {
                            strValOut = QString("#%1=%2 ").arg(ind).arg(anValues[ind]);
                            strRetVal = lookupMakerCanonTag(nIfdTagVal, ind, anValues[ind]);
                            strMaker = strRetVal.strTag;
                            strValTmp = QString("      [%1] = %2").arg(strMaker, -34).arg(strRetVal.strVal);

                            if ((!_appConfig.hideUnknownExif()) || (!strRetVal.bUnknown)) {
                                _log.info(strValTmp);
                            }
                        } else if (ind == MAX_anValues) {
                            _log.info("      [... etc ...]");
                        } else {
                            // Don't print!
                        }
                    }
                }

                strValOut = "...";
            }

            // For Nikon & Sigma, we simply support the quality field
            if ((strIfdTag == "Nikon1.Quality") ||
                (strIfdTag == "Nikon2.Quality") || (strIfdTag == "Nikon3.Quality") || (strIfdTag == "Sigma.Quality")) {
                m_strImgQualExif = strValOut;

                // Collect extra details (for later DB submission)
                strTmp = "";
                strTmp = QString("[%1]:[%1],").arg(strIfdTag).arg(strValOut);
                m_strImgExtras += strTmp;
            }

            // Collect extra details (for later DB submission)
            if (strIfdTag == "Canon.ImageType") {
                strTmp = "";
                strTmp = QString("[%1]:[%1],").arg(strIfdTag).arg(strValOut);
                m_strImgExtras += strTmp;
            }
        }

        // ----------------------------------------

        // Now extract some of the important offsets / pointers
        if ((strIfd == "IFD0") && (strIfdTag == "ExifOffset")) {
            // EXIF SubIFD - Pointer
            m_nImgExifSubIfdPtr = nIfdOffset;
            strValOut = QString("@ 0x%1").arg(nIfdOffset, 4, 16, QChar('0'));
        }

        if ((strIfd == "IFD0") && (strIfdTag == "GPSOffset")) {
            // GPS SubIFD - Pointer
            m_nImgExifGpsIfdPtr = nIfdOffset;
            strValOut = QString("@ 0x%1").arg(nIfdOffset, 4, 16, QChar('0'));
        }

        // TODO: Add Interoperability IFD (0xA005)?
        if ((strIfd == "SubIFD") && (strIfdTag == "ExifInteroperabilityOffset")) {
            m_nImgExifInteropIfdPtr = nIfdOffset;
            strValOut = QString("@ 0x%1").arg(nIfdOffset, 4, 16, QChar('0'));
        }

        // Extract software field
        if ((strIfd == "IFD0") && (strIfdTag == "Software")) {
            m_strSoftware = strValOut;
        }

        // -------------------------
        // IFD0 - ExifMake
        // -------------------------
        if ((strIfd == "IFD0") && (strIfdTag == "Make")) {
            m_strImgExifMake = strValOut.trimmed();

        }

        // -------------------------
        // IFD0 - ExifModel
        // -------------------------
        if ((strIfd == "IFD0") && (strIfdTag == "Model")) {
            m_strImgExifModel = strValOut.trimmed();
        }

        if ((strIfd == "SubIFD") && (strIfdTag == "MakerNote")) {
            // Maker IFD - Pointer
            m_nImgExifMakerPtr = nIfdOffset;
            strValOut = QString("@ 0x%1").arg(nIfdOffset, 4, 16, QChar('0'));
        }

        // -------------------------
        // IFD1 - Embedded Thumbnail
        // -------------------------
        if ((strIfd == "IFD1") && (strIfdTag == "Compression")) {
            // Embedded thumbnail, compression format
            m_nImgExifThumbComp = readSwap4(_pos);
        }

        if ((strIfd == "IFD1") && (strIfdTag == "JpegIFOffset")) {
            // Embedded thumbnail, offset
            m_nImgExifThumbOffset = nIfdOffset + nPosExifStart;
            strValOut = QString("@ +0x%1 = @ 0x%2")
                .arg(nIfdOffset, 4, 16, QChar('0'))
                .arg(m_nImgExifThumbOffset, 4, 16, QChar('0'));
        }

        if ((strIfd == "IFD1") && (strIfdTag == "JpegIFByteCount")) {
            // Embedded thumbnail, length
            m_nImgExifThumbLen = readSwap4(_pos);
        }

        // ==========================================================================
        // Determine MakerNote support
        // ==========================================================================

        if (m_strImgExifMake != "") {
            // 1) Identify the supported MakerNotes
            // 2) Remap variations of the Maker field (e.g. Nikon)
            //    as some manufacturers have been inconsistent in their
            //    use of the Make field

            m_bImgExifMakeSupported = false;

            if (m_strImgExifMake == "Canon") {
                m_bImgExifMakeSupported = true;
            } else if (m_strImgExifMake == "PENTAX Corporation") {
                m_strImgExifMake = "PENTAX";
            } else if (m_strImgExifMake == "NIKON CORPORATION") {
                m_strImgExifMake = "NIKON";
                m_bImgExifMakeSupported = true;
            } else if (m_strImgExifMake == "NIKON") {
                m_strImgExifMake = "NIKON";
                m_bImgExifMakeSupported = true;
            } else if (m_strImgExifMake == "SIGMA") {
                m_bImgExifMakeSupported = true;
            } else if (m_strImgExifMake == "SONY") {
                m_bImgExifMakeSupported = true;
            } else if (m_strImgExifMake == "FUJIFILM") {
                // TODO:
                // FUJIFILM Maker notes apparently use
                // big endian format even though main section uses little.
                // Need to switch if in maker section for FUJI
                // For now, disable support
                m_bImgExifMakeSupported = false;
            }
        }

        // Now advance the m_nPos ptr as we have finished with valoffset
        _pos += 4;

        // ==========================================================================
        // SUMMARY REPORT
        // ==========================================================================

        // If we haven't already output a detailed decode of this field
        // then we can output the generic representation here
        if (!bExtraDecode) {
            // Provide option to skip unknown fields
            if ((!_appConfig.hideUnknownExif()) || (!nIfdTagUnknown)) {
                // If the tag is an ASCII string, we want to wrap with quote marks
                if (nIfdFormat == 2) {
                    strTmp = QString("    [%1] = \"%2\"").arg(strIfdTag, -36).arg(strValOut);
                } else {
                    strTmp = QString("    [%1] = %2").arg(strIfdTag, -36).arg(strValOut);
                }

                _log.info(strTmp);
            }
        }

        dbgAddLine("");
    }                             // for nIfdEntryInd

    // =========== EXIF IFD (End) ===========
    // - Defined in Exif 2.2 Standard (JEITA CP-3451) section 4.6.2
    // - Is completed by 4-byte offset to next IFD, which is
    //   read in next iteration.

    return 0;
}

//-----------------------------------------------------------------------------
// Handle APP13 marker
// This includes:
// - Photoshop "Save As" and "Save for Web" quality settings
// - IPTC entries
// PRE:
// - m_nPos
// POST:
// - m_nImgQualPhotoshopSa
uint32_t JfifDecode::decodeApp13Ps() {
    // Photoshop APP13 marker definition doesn't appear to have a
    // well-defined length, so I will read until I encounter a
    // non-"8BIM" entry, then we reset the position counter
    // and move on to the next marker.
    // FIXME: This does not appear to be very robust

    bool bDone = false;

    //uint32_t              nVal = 0x8000;

    QString strBimSig;

    // Reset PsDec decoder state
    _psDec->Reset();

    while (!bDone) {
        // FIXME: Need to check for actual marker section extent, not
        // just the lack of an 8BIM signature as the terminator!

        // Check the signature but don't advance the file pointer
        strBimSig = _wbuf.readStrN(_pos, 4);

        // Check for signature "8BIM"
        if (strBimSig == "8BIM") {
            _psDec->PhotoshopParseImageResourceBlock(_pos, 3);
        } else {
            // Not 8BIM?
            bDone = true;
        }
    }

    // Now that we've finished with the PsDec decoder we can fetch
    // some of the parser state
    // TODO: Migrate into accessor
    m_nImgQualPhotoshopSa = _psDec->m_nQualitySaveAs;
    m_nImgQualPhotoshopSfw = _psDec->m_nQualitySaveForWeb;
    _psd = _psDec->m_bPsd;

    return 0;
}

//-----------------------------------------------------------------------------
// Start decoding a single ICC header segment @ nPos
uint32_t JfifDecode::decodeIccHeader(uint32_t nPos) {
    QString strTmp, strTmp1;

    // Profile header
    uint32_t nProfSz;
    uint32_t nPrefCmmType;
    uint32_t nProfVer;
    uint32_t nProfDevClass;
    uint32_t nDataColorSpace;
    uint32_t nPcs;
    uint32_t anDateTimeCreated[3];
    uint32_t nProfFileSig;
    uint32_t nPrimPlatSig;
    uint32_t nProfFlags;
    uint32_t nDevManuf;
    uint32_t nDevModel;
    uint32_t anDevAttrib[2];
    uint32_t nRenderIntent;
    uint32_t anIllumPcsXyz[3];
    uint32_t nProfCreatorSig;
    uint32_t anProfId[4];
    uint32_t anRsvd[7];

    // Read in all of the ICC header bytes
    nProfSz = readBe4(nPos);
    nPos += 4;
    nPrefCmmType = readBe4(nPos);
    nPos += 4;
    nProfVer = readBe4(nPos);
    nPos += 4;
    nProfDevClass = readBe4(nPos);
    nPos += 4;
    nDataColorSpace = readBe4(nPos);
    nPos += 4;
    nPcs = readBe4(nPos);
    nPos += 4;
    anDateTimeCreated[2] = readBe4(nPos);
    nPos += 4;
    anDateTimeCreated[1] = readBe4(nPos);
    nPos += 4;
    anDateTimeCreated[0] = readBe4(nPos);
    nPos += 4;
    nProfFileSig = readBe4(nPos);
    nPos += 4;
    nPrimPlatSig = readBe4(nPos);
    nPos += 4;
    nProfFlags = readBe4(nPos);
    nPos += 4;
    nDevManuf = readBe4(nPos);
    nPos += 4;
    nDevModel = readBe4(nPos);
    nPos += 4;
    anDevAttrib[1] = readBe4(nPos);
    nPos += 4;
    anDevAttrib[0] = readBe4(nPos);
    nPos += 4;
    nRenderIntent = readBe4(nPos);
    nPos += 4;
    anIllumPcsXyz[2] = readBe4(nPos);
    nPos += 4;
    anIllumPcsXyz[1] = readBe4(nPos);
    nPos += 4;
    anIllumPcsXyz[0] = readBe4(nPos);
    nPos += 4;
    nProfCreatorSig = readBe4(nPos);
    nPos += 4;
    anProfId[3] = readBe4(nPos);
    nPos += 4;
    anProfId[2] = readBe4(nPos);
    nPos += 4;
    anProfId[1] = readBe4(nPos);
    nPos += 4;
    anProfId[0] = readBe4(nPos);
    nPos += 4;
    anRsvd[6] = readBe4(nPos);
    nPos += 4;
    anRsvd[5] = readBe4(nPos);
    nPos += 4;
    anRsvd[4] = readBe4(nPos);
    nPos += 4;
    anRsvd[3] = readBe4(nPos);
    nPos += 4;
    anRsvd[2] = readBe4(nPos);
    nPos += 4;
    anRsvd[1] = readBe4(nPos);
    nPos += 4;
    anRsvd[0] = readBe4(nPos);
    nPos += 4;

    // Now output the formatted version of the above data structures
    strTmp = QString("        %1 : %2 bytes").arg("Profile Size", -33).arg(nProfSz);
    _log.info(strTmp);

    strTmp = QString("        %1 : %2").arg("Preferred CMM Type", -33).arg(Uint2Chars(nPrefCmmType));
    _log.info(strTmp);

    strTmp =
        QString("        %1 : %2.%3.%4.%5 (0x%6)")
            .arg("Profile Version", -33)
            .arg((nProfVer & 0xF0000000) >> 28)
            .arg((nProfVer & 0x0F000000) >> 24)
            .arg((nProfVer & 0x00F00000) >> 20)
            .arg((nProfVer & 0x000F0000) >> 16)
            .arg(nProfVer, 8, 16, QChar('0'));
    _log.info(strTmp);

    switch (nProfDevClass) {
        //CAL! case 'scnr':
        case FOURC_INT('s', 'c', 'n', 'r'):
            strTmp1 = "Input Device profile";
            break;
        case FOURC_INT('m', 'n', 't', 'r'):
            strTmp1 = "Display Device profile";
            break;
        case FOURC_INT('p', 'r', 't', 'r'):
            strTmp1 = "Output Device profile";
            break;
        case FOURC_INT('l', 'i', 'n', 'k'):
            strTmp1 = "DeviceLink Device profile";
            break;
        case FOURC_INT('s', 'p', 'a', 'c'):
            strTmp1 = "ColorSpace Conversion profile";
            break;
        case FOURC_INT('a', 'b', 's', 't'):
            strTmp1 = "Abstract profile";
            break;
        case FOURC_INT('n', 'm', 'c', 'l'):
            strTmp1 = "Named colour profile";
            break;
        default:
            strTmp1 = QString("? (0x%1)").arg(nProfDevClass, 8, 16, QChar('0'));
            break;
    }
    strTmp = QString("        %1 : %2 (%3)").arg("Profile Device/Class",
                                                 -33).arg(strTmp1).arg(Uint2Chars(nProfDevClass));
    _log.info(strTmp);

    switch (nDataColorSpace) {
        case FOURC_INT('X', 'Y', 'Z', ' '):
            strTmp1 = "XYZData";
            break;
        case FOURC_INT('L', 'a', 'b', ' '):
            strTmp1 = "labData";
            break;
        case FOURC_INT('L', 'u', 'v', ' '):
            strTmp1 = "lubData";
            break;
        case FOURC_INT('Y', 'C', 'b', 'r'):
            strTmp1 = "YCbCrData";
            break;
        case FOURC_INT('Y', 'x', 'y', ' '):
            strTmp1 = "YxyData";
            break;
        case FOURC_INT('R', 'G', 'B', ' '):
            strTmp1 = "rgbData";
            break;
        case FOURC_INT('G', 'R', 'A', 'Y'):
            strTmp1 = "grayData";
            break;
        case FOURC_INT('H', 'S', 'V', ' '):
            strTmp1 = "hsvData";
            break;
        case FOURC_INT('H', 'L', 'S', ' '):
            strTmp1 = "hlsData";
            break;
        case FOURC_INT('C', 'M', 'Y', 'K'):
            strTmp1 = "cmykData";
            break;
        case FOURC_INT('C', 'M', 'Y', ' '):
            strTmp1 = "cmyData";
            break;
        case FOURC_INT('2', 'C', 'L', 'R'):
            strTmp1 = "2colourData";
            break;
        case FOURC_INT('3', 'C', 'L', 'R'):
            strTmp1 = "3colourData";
            break;
        case FOURC_INT('4', 'C', 'L', 'R'):
            strTmp1 = "4colourData";
            break;
        case FOURC_INT('5', 'C', 'L', 'R'):
            strTmp1 = "5colourData";
            break;
        case FOURC_INT('6', 'C', 'L', 'R'):
            strTmp1 = "6colourData";
            break;
        case FOURC_INT('7', 'C', 'L', 'R'):
            strTmp1 = "7colourData";
            break;
        case FOURC_INT('8', 'C', 'L', 'R'):
            strTmp1 = "8colourData";
            break;
        case FOURC_INT('9', 'C', 'L', 'R'):
            strTmp1 = "9colourData";
            break;
        case FOURC_INT('A', 'C', 'L', 'R'):
            strTmp1 = "10colourData";
            break;
        case FOURC_INT('B', 'C', 'L', 'R'):
            strTmp1 = "11colourData";
            break;
        case FOURC_INT('C', 'C', 'L', 'R'):
            strTmp1 = "12colourData";
            break;
        case FOURC_INT('D', 'C', 'L', 'R'):
            strTmp1 = "13colourData";
            break;
        case FOURC_INT('E', 'C', 'L', 'R'):
            strTmp1 = "14colourData";
            break;
        case FOURC_INT('F', 'C', 'L', 'R'):
            strTmp1 = "15colourData";
            break;
        default:
            strTmp1 = QString("? (0x%1)").arg(nDataColorSpace, 8, 16, QChar('0'));
            break;
    }
    strTmp = QString("        %1 : %2 (%3)").arg("Data Colour Space",
                                                 -33).arg(strTmp1).arg(Uint2Chars(nDataColorSpace));
    _log.info(strTmp);

    strTmp = QString("        %1 : %2").arg("Profile connection space (PCS)", -33).arg(Uint2Chars(nPcs));
    _log.info(strTmp);

    strTmp = QString("        %1 : %2").arg("Profile creation date", -33).arg(decodeIccDateTime(anDateTimeCreated));
    _log.info(strTmp);

    strTmp = QString("        %1 : %2").arg("Profile file signature", -33).arg(Uint2Chars(nProfFileSig));
    _log.info(strTmp);

    switch (nPrimPlatSig) {
        case FOURC_INT('A', 'P', 'P', 'L'):
            strTmp1 = "Apple Computer, Inc.";
            break;
        case FOURC_INT('M', 'S', 'F', 'T'):
            strTmp1 = "Microsoft Corporation";
            break;
        case FOURC_INT('S', 'G', 'I', ' '):
            strTmp1 = "Silicon Graphics, Inc.";
            break;
        case FOURC_INT('S', 'U', 'N', 'W'):
            strTmp1 = "Sun Microsystems, Inc.";
            break;
        default:
            strTmp1 = QString("? (0x%1)").arg(nPrimPlatSig, 8, 16, QChar('0'));
            break;
    }

    strTmp = QString("        %1 : %2 (%3)").arg("Primary platform", -33).arg(strTmp1).arg(Uint2Chars(nPrimPlatSig));
    _log.info(strTmp);

    strTmp = QString("        %1 : 0x%2").arg("Profile flags", -33).arg(nProfFlags, 8, 16, QChar('0'));
    _log.info(strTmp);
    strTmp1 = (TestBit(nProfFlags, 0)) ? "Embedded profile" : "Profile not embedded";
    strTmp = QString("        %1 > %2").arg("Profile flags", -35).arg(strTmp1);
    _log.info(strTmp);
    strTmp1 =
        (TestBit(nProfFlags, 1)) ? "Profile can be used independently of embedded" :
        "Profile can't be used independently of embedded";
    strTmp = QString("        %1 > %2").arg("Profile flags", -35).arg(strTmp1);
    _log.info(strTmp);

    strTmp = QString("        %1 : %2").arg("Device Manufacturer", -33).arg(Uint2Chars(nDevManuf));
    _log.info(strTmp);

    strTmp = QString("        %1 : %2").arg("Device Model", -33).arg(Uint2Chars(nDevModel));
    _log.info(strTmp);

    strTmp = QString("        %1 : 0x%2_%3")
        .arg("Device attributes", -33)
        .arg(anDevAttrib[1], 8, 16, QChar('0'))
        .arg(anDevAttrib[0], 8, 16, QChar('0'));
    _log.info(strTmp);
    strTmp1 = (TestBit(anDevAttrib[0], 0)) ? "Transparency" : "Reflective";
    strTmp = QString("        %1 > %2")
        .arg("Device attributes", -35)
        .arg(strTmp1);
    _log.info(strTmp);
    strTmp1 = (TestBit(anDevAttrib[0], 1)) ? "Matte" : "Glossy";
    strTmp = QString("        %1 > %2")
        .arg("Device attributes", -35)
        .arg(strTmp1);
    _log.info(strTmp);
    strTmp1 = (TestBit(anDevAttrib[0], 2)) ? "Media polarity = positive" : "Media polarity = negative";
    strTmp = QString("        %1 > %2")
        .arg("Device attributes", -35)
        .arg(strTmp1);
    _log.info(strTmp);
    strTmp1 = (TestBit(anDevAttrib[0], 3)) ? "Colour media" : "Black & white media";
    strTmp = QString("        %1 > %2")
        .arg("Device attributes", -35)
        .arg(strTmp1);
    _log.info(strTmp);

    switch (nRenderIntent) {
        case 0x00000000:
            strTmp1 = "Perceptual";
            break;
        case 0x00000001:
            strTmp1 = "Media-Relative Colorimetric";
            break;
        case 0x00000002:
            strTmp1 = "Saturation";
            break;
        case 0x00000003:
            strTmp1 = "ICC-Absolute Colorimetric";
            break;
        default:
            strTmp1 = QString("0x%1").arg(nRenderIntent, 8, 16, QChar('0'));
            break;
    }

    strTmp = QString("        %1 : %2").arg("Rendering intent", -33).arg(strTmp1);
    _log.info(strTmp);

    // PCS illuminant

    strTmp = QString("        %1 : %2").arg("Profile creator", -33).arg(Uint2Chars(nProfCreatorSig));
    _log.info(strTmp);

    strTmp =
        QString("        %1 : 0x%2_%3_%4_%5")
            .arg("Profile ID", -33)
            .arg(anProfId[3], 8, 16, QChar('0'))
            .arg(anProfId[2], 8, 16, QChar('0'))
            .arg(anProfId[1], 8, 16, QChar('0'))
            .arg(anProfId[0], 8, 16, QChar('0'));
    _log.info(strTmp);

    return 0;
}

//-----------------------------------------------------------------------------
// Provide special output formatter for ICC Date/Time
// NOTE: It appears that the nParts had to be decoded in the
//       reverse order from what I had expected, so one should
//       confirm that the byte order / endianness is appropriate.
QString JfifDecode::decodeIccDateTime(uint32_t anVal[3]) {
    QString strDate;

    uint16_t anParts[6];

    anParts[0] = (anVal[2] & 0xFFFF0000) >> 16;   // Year
    anParts[1] = (anVal[2] & 0x0000FFFF); // Mon
    anParts[2] = (anVal[1] & 0xFFFF0000) >> 16;   // Day
    anParts[3] = (anVal[1] & 0x0000FFFF); // Hour
    anParts[4] = (anVal[0] & 0xFFFF0000) >> 16;   // Min
    anParts[5] = (anVal[0] & 0x0000FFFF); // Sec
    strDate = QString("%1-%2-%3 %4:%5:%6")
        .arg(anParts[0], 4, 10, QChar('0'))
        .arg(anParts[1], 2, 10, QChar('0'))
        .arg(anParts[2], 2, 10, QChar('0'))
        .arg(anParts[3], 2, 10, QChar('0'))
        .arg(anParts[4], 2, 10, QChar('0'))
        .arg(anParts[5], 2, 10, QChar('0'));
    return strDate;
}

//-----------------------------------------------------------------------------
// Parser for APP2 ICC profile marker
uint32_t JfifDecode::decodeApp2IccProfile(uint32_t nLen) {
    QString strTmp;

    uint32_t nMarkerSeqNum;       // Byte

    uint32_t nNumMarkers;         // Byte

    uint32_t nPayloadLen;         // Len of this ICC marker payload

    uint32_t nMarkerPosStart;

    nMarkerSeqNum = getByte(_pos++);
    nNumMarkers = getByte(_pos++);
    nPayloadLen = nLen - 2 - 12 - 2;      // TODO: check?

    strTmp = QString("      Marker Number = %1 of %2").arg(nMarkerSeqNum).arg(nNumMarkers);
    _log.info(strTmp);

    if (nMarkerSeqNum == 1) {
        nMarkerPosStart = _pos;
        decodeIccHeader(nMarkerPosStart);
    } else {
        _log.warn("      Only support decode of 1st ICC Marker");
    }

    return 0;
}

//-----------------------------------------------------------------------------
// Parser for APP2 FlashPix marker
uint32_t JfifDecode::decodeApp2FlashPix() {
    QString strTmp;

    uint32_t nFpxVer;
    uint32_t nFpxSegType;
    uint32_t nFpxInteropCnt;
    uint32_t nFpxEntitySz;
    uint32_t nFpxDefault;

    bool bFpxStorage;

    QString strFpxStorageClsStr;

    uint32_t nFpxStIndexCont;
    uint32_t nFpxStOffset;
    uint32_t nFpxStWByteOrder;
    uint32_t nFpxStWFormat;

    QString strFpxStClsidStr;

    uint32_t nFpxStDwOsVer;
    uint32_t nFpxStRsvd;

    QString streamStr;

    nFpxVer = getByte(_pos++);
    nFpxSegType = getByte(_pos++);

    // FlashPix segments: Contents List or Stream Data

    if (nFpxSegType == 1) {
        // Contents List
        strTmp = QString("    Segment: CONTENTS LIST");
        _log.info(strTmp);

        nFpxInteropCnt = (getByte(_pos++) << 8) + getByte(_pos++);
        strTmp = QString("      Interoperability Count = %1").arg(nFpxInteropCnt);
        _log.info(strTmp);

        for (uint32_t ind = 0; ind < nFpxInteropCnt; ind++) {
            strTmp = QString("      Entity Index #%1").arg(ind);
            _log.info(strTmp);
            nFpxEntitySz = (getByte(_pos++) << 24) + (getByte(_pos++) << 16) + (getByte(_pos++) << 8) + getByte(_pos++);

            // If the "entity size" field is 0xFFFFFFFF, then it should be treated as
            // a "storage". It looks like we should probably be using this to determine
            // that we have a "storage"
            bFpxStorage = false;

            if (nFpxEntitySz == 0xFFFFFFFF) {
                bFpxStorage = true;
            }

            if (!bFpxStorage) {
                strTmp = QString("        Entity Size = %1").arg(nFpxEntitySz);
                _log.info(strTmp);
            } else {
                strTmp = "        Entity is Storage";
                _log.info(strTmp);
            }

            nFpxDefault = getByte(_pos++);

            // BUG: #1112
            //streamStr = m_pWBuf->BufReadUniStr(m_nPos);
            streamStr = _wbuf.readUniStr2(_pos, MAX_BUF_READ_STR);
            _pos += 2 * (static_cast<uint32_t>(streamStr.length()) + 1);        // 2x because unicode

            strTmp = QString("        Stream Name = [%1]").arg(streamStr);
            _log.info(strTmp);

            // In the case of "storage", we decode the next 16 bytes as the class
            if (bFpxStorage) {

                // FIXME:
                // NOTE: Very strange reordering required here. Doesn't seem consistent
                //       This means that other fields are probably wrong as well (endian)
                strFpxStorageClsStr = QString("%1%2%3%4-%5%6-%7%8-%9%10-%11%12%13%14%15%16")
                    .arg(getByte(_pos + 3), 2, 16, QChar('0'))
                    .arg(getByte(_pos + 2), 2, 16, QChar('0'))
                    .arg(getByte(_pos + 1), 2, 16, QChar('0'))
                    .arg(getByte(_pos + 0), 2, 16, QChar('0'))
                    .arg(getByte(_pos + 5), 2, 16, QChar('0'))
                    .arg(getByte(_pos + 4), 2, 16, QChar('0'))
                    .arg(getByte(_pos + 7), 2, 16, QChar('0'))
                    .arg(getByte(_pos + 6), 2, 16, QChar('0'))
                    .arg(getByte(_pos + 8), 2, 16, QChar('0'))
                    .arg(getByte(_pos + 9), 2, 16, QChar('0'))
                    .arg(getByte(_pos + 10), 2, 16, QChar('0'))
                    .arg(getByte(_pos + 11), 2, 16, QChar('0'))
                    .arg(getByte(_pos + 12), 2, 16, QChar('0'))
                    .arg(getByte(_pos + 13), 2, 16, QChar('0'))
                    .arg(getByte(_pos + 14), 2, 16, QChar('0'))
                    .arg(getByte(_pos + 15), 2, 16, QChar('0'));
                _pos += 16;
                strTmp = QString("        Storage Class = [%1]").arg(strFpxStorageClsStr);
                _log.info(strTmp);
            }
        }

        return 0;
    } else if (nFpxSegType == 2) {
        // Stream Data
        strTmp = "    Segment: STREAM DATA";
        _log.info(strTmp);

        nFpxStIndexCont = (getByte(_pos++) << 8) + getByte(_pos++);
        strTmp = QString("      Index in Contents List = %1").arg(nFpxStIndexCont);
        _log.info(strTmp);

        nFpxStOffset = (getByte(_pos++) << 24) + (getByte(_pos++) << 16) + (getByte(_pos++) << 8) + getByte(_pos++);
        strTmp = QString("      Offset in stream = %1 (0x%2)").arg(nFpxStOffset).arg(nFpxStOffset, 8, 16, QChar('0'));
        _log.info(strTmp);

        // Now decode the Property Set Header

        // NOTE: Should only decode this if we are doing first part of stream
        // TODO: How do we know this? First reference to index #?

        nFpxStWByteOrder = (getByte(_pos++) << 8) + getByte(_pos++);
        nFpxStWFormat = (getByte(_pos++) << 8) + getByte(_pos++);
        nFpxStDwOsVer = (getByte(_pos++) << 24) + (getByte(_pos++) << 16) + (getByte(_pos++) << 8) + getByte(_pos++);

        // FIXME:
        // NOTE: Very strange reordering required here. Doesn't seem consistent!
        //       This means that other fields are probably wrong as well (endian)
        strFpxStClsidStr =
            QString("%1%2%3%4-%5%6-%7%8-%9%10-%11%12%13%14%15%16")
                .arg(getByte(_pos + 3), 2, 16, QChar('0'))
                .arg(getByte(_pos + 2), 2, 16, QChar('0'))
                .arg(getByte(_pos + 1), 2, 16, QChar('0'))
                .arg(getByte(_pos + 0), 2, 16, QChar('0'))
                .arg(getByte(_pos + 5), 2, 16, QChar('0'))
                .arg(getByte(_pos + 4), 2, 16, QChar('0'))
                .arg(getByte(_pos + 7), 2, 16, QChar('0'))
                .arg(getByte(_pos + 6), 2, 16, QChar('0'))
                .arg(getByte(_pos + 8), 2, 16, QChar('0'))
                .arg(getByte(_pos + 9), 2, 16, QChar('0'))
                .arg(getByte(_pos + 10), 2, 16, QChar('0'))
                .arg(getByte(_pos + 11), 2, 16, QChar('0'))
                .arg(getByte(_pos + 12), 2, 16, QChar('0'))
                .arg(getByte(_pos + 13), 2, 16, QChar('0'))
                .arg(getByte(_pos + 14), 2, 16, QChar('0'))
                .arg(getByte(_pos + 15), 2, 16, QChar('0'));
        _pos += 16;
        nFpxStRsvd = (getByte(_pos++) << 8) + getByte(_pos++);

        strTmp = QString("      ByteOrder = 0x%1").arg(nFpxStWByteOrder, 4, 16, QChar('0'));
        _log.info(strTmp);

        strTmp = QString("      Format = 0x%1").arg(nFpxStWFormat, 4, 16, QChar('0'));
        _log.info(strTmp);

        strTmp = QString("      OSVer = 0x%1").arg(nFpxStDwOsVer, 8, 16, QChar('0'));
        _log.info(strTmp);

        strTmp = QString("      clsid = %1").arg(strFpxStClsidStr);
        _log.info(strTmp);

        strTmp = QString("      reserved = 0x%1").arg(nFpxStRsvd, 8, 16, QChar('0'));
        _log.info(strTmp);

        // ....

        return 2;

    } else {
        _log.error("      Reserved Segment. Stopping.");
        return 1;
    }
}

//-----------------------------------------------------------------------------
// Decode the DHT marker segment (Huffman Tables)
// In some cases (such as for MotionJPEG), we fake out
// the DHT tables (when bInject=true) with a standard table
// as each JPEG frame in the MJPG does not include the DHT.
// In all other cases (bInject=false), we simply read the
// DHT table from the file buffer via Buf()
//
// ITU-T standard indicates that we can expect up to a
// maximum of 16-bit huffman code bitstrings.
void JfifDecode::decodeDht(bool bInject) {
    uint32_t nLength;
    uint32_t nTmpVal;

    QString strTmp, strFull;

    uint32_t nPosEnd;
    uint32_t nPosSaved = 0;

    bool bRet;

    if (bInject) {
        // Redirect Buf() to DHT table in MJPGDHTSeg[]
        // ... so change mode that Buf() call uses
        _bufFakeDht = true;

        // Preserve the "m_nPos" pointer, at end we undo it
        // And we also start at 2 which is just after FFC4 in array
        nPosSaved = _pos;
        _pos = 2;
    }

    nLength = getByte(_pos) * 256 + getByte(_pos + 1);
    nPosEnd = _pos + nLength;
    _pos += 2;
    strTmp = QString("  Huffman table length = %1").arg(nLength);
    _log.info(strTmp);

    uint32_t nDhtClass_Tc;        // Range 0..1
    uint32_t nDhtHuffTblId_Th;    // Range 0..3

    // In various places, added m_bStateAbort check to allow us
    // to escape in case we get in excessive number of DHT entries
    // See BUG FIX #1003

    while ((!_stateAbort) && (nPosEnd > _pos)) {
        _log.info("  ----");

        nTmpVal = getByte(_pos++);
        nDhtClass_Tc = (nTmpVal & 0xF0) >> 4;       // Tc, range 0..1
        nDhtHuffTblId_Th = nTmpVal & 0x0F;  // Th, range 0..3
        strTmp = QString("  Destination ID = %1").arg(nDhtHuffTblId_Th);
        _log.info(strTmp);
        strTmp = QString("  Class = %1 (%2)").arg(nDhtClass_Tc).arg(nDhtClass_Tc ? "AC Table" : "DC / Lossless Table");
        _log.info(strTmp);

        // Add in some error checking to prevent
        if (nDhtClass_Tc >= MAX_DHT_CLASS) {
            strTmp = QString("Invalid DHT Class (%1). Aborting DHT Load.").arg(nDhtClass_Tc);
            _log.error(strTmp);
            _pos = nPosEnd;
            //m_bStateAbort = true; // Stop decoding
            break;
        }

        if (nDhtHuffTblId_Th >= MAX_DHT_DEST_ID) {
            strTmp = QString("Invalid DHT Dest ID (%1). Aborting DHT Load.").arg(nDhtHuffTblId_Th);
            _log.error(strTmp);
            _pos = nPosEnd;
            //m_bStateAbort = true; // Stop decoding
            break;
        }

        // Read in the array of DHT code lengths
        for (uint32_t i = 1; i <= MAX_DHT_CODELEN; i++) {
            m_anDhtNumCodesLen_Li[i] = getByte(_pos++); // Li, range 0..255
        }

#define DECODE_DHT_MAX_DHT 256
        uint32_t anDhtCodeVal[DECODE_DHT_MAX_DHT + 1];  // Should only need max 162 codes
        uint32_t nDhtInd;
        uint32_t nDhtCodesTotal;

        // Clear out the code list
        for (nDhtInd = 0; nDhtInd < DECODE_DHT_MAX_DHT; nDhtInd++) {
            anDhtCodeVal[nDhtInd] = 0xFFFF;   // Dummy value
        }

        // Now read in all of the DHT codes according to the lengths
        // read in earlier
        nDhtCodesTotal = 0;
        nDhtInd = 0;

        for (uint32_t nIndLen = 1; ((!_stateAbort) && (nIndLen <= MAX_DHT_CODELEN)); nIndLen++) {
            // Keep a total count of the number of DHT codes read
            nDhtCodesTotal += m_anDhtNumCodesLen_Li[nIndLen];

            strFull = QString("    Codes of length %1 bits (%2 total): ")
                .arg(nIndLen, 2, 10, QChar('0'))
                .arg(m_anDhtNumCodesLen_Li[nIndLen], 3, 10, QChar('0'));

            for (uint32_t nIndCode = 0; ((!_stateAbort) && (nIndCode < m_anDhtNumCodesLen_Li[nIndLen])); nIndCode++) {
                // Start a new line for every 16 codes
                if ((nIndCode != 0) && ((nIndCode % 16) == 0)) {
                    strFull = "                                         ";
                }

                nTmpVal = getByte(_pos++);
                strTmp = QString("%1 ").arg(nTmpVal, 2, 16, QChar('0'));
                strFull += strTmp;

                // Only write 16 codes per line
                if ((nIndCode % 16) == 15) {
                    _log.info(strFull);
                    strFull = "";
                }

                // Save the huffman code
                // Just in case we have more DHT codes than we expect, trap
                // the range check here, otherwise we'll have buffer overrun!
                if (nDhtInd < DECODE_DHT_MAX_DHT) {
                    anDhtCodeVal[nDhtInd++] = nTmpVal;    // Vij, range 0..255
                } else {
                    nDhtInd++;
                    strTmp = QString("Excessive DHT entries (%1)... skipping").arg(nDhtInd);
                    _log.error(strTmp);

                    if (!_stateAbort) {
                        decodeErrCheck(true);
                    }
                }
            }

            _log.info(strFull);
        }

        strTmp = QString("    Total number of codes: %1").arg(nDhtCodesTotal, 3, 10, QChar('0'));
        _log.info(strTmp);

        uint32_t nDhtLookupInd = 0;

        // Now print out the actual binary strings!
        uint32_t nCodeVal = 0;

        nDhtInd = 0;

        if (_appConfig.expandDht()) {
            _log.info("");
            _log.info("  Expanded Form of Codes:");
        }

        for (uint32_t nBitLen = 1; ((!_stateAbort) && (nBitLen <= 16)); nBitLen++) {
            if (m_anDhtNumCodesLen_Li[nBitLen] > 0) {
                if (_appConfig.expandDht()) {
                    strTmp = QString("    Codes of length %1 bits:").arg(nBitLen, 2, 10, QChar('0'));
                    _log.info(strTmp);
                }

                // Codes exist for this bit-length
                // Walk through and generate the bitvalues
                for (uint32_t bit_ind = 1; ((!_stateAbort) &&
                                            (bit_ind <= m_anDhtNumCodesLen_Li[nBitLen])); bit_ind++) {
                    uint32_t nDecVal = nCodeVal;
                    uint32_t nBinBit;

                    char acBinStr[17] = "";

                    uint32_t nBinStrLen = 0;

                    // If the user has enabled output of DHT expanded tables,
                    // report the bit-string sequences.
                    if (_appConfig.expandDht()) {
                        for (uint32_t nBinInd = nBitLen; nBinInd >= 1; nBinInd--) {
                            nBinBit = (nDecVal >> (nBinInd - 1)) & 1;
                            acBinStr[nBinStrLen++] = (nBinBit) ? '1' : '0';
                        }

                        acBinStr[nBinStrLen] = '\0';
                        strFull = QString("      %1 = %2").arg(acBinStr).arg(anDhtCodeVal[nDhtInd], 2, 16, QChar('0'));

                        // The following are only valid for AC components
                        // Bug [3442132]
                        if (nDhtClass_Tc == DHT_CLASS_AC) {
                            if (anDhtCodeVal[nDhtInd] == 0x00) {
                                strFull += " (EOB)";
                            }
                            if (anDhtCodeVal[nDhtInd] == 0xF0) {
                                strFull += " (ZRL)";
                            }
                        }

                        strTmp = QString("%1 (Total Len = %2)").arg(strFull, -40).arg(
                            nBitLen + (anDhtCodeVal[nDhtInd] & 0xF), 2);

                        _log.info(strTmp);
                    }

                    // Store the lookup value
                    // Shift left to MSB of 32-bit
                    uint32_t nTmpMask = m_anMaskLookup[nBitLen];
                    uint32_t nTmpBits = nDecVal << (32 - nBitLen);
                    uint32_t nTmpCode = anDhtCodeVal[nDhtInd];

                    bRet = _imgDec.SetDhtEntry(nDhtHuffTblId_Th,
                                               nDhtClass_Tc,
                                               nDhtLookupInd,
                                               nBitLen,
                                               nTmpBits,
                                               nTmpMask,
                                               nTmpCode);

                    decodeErrCheck(bRet);

                    nDhtLookupInd++;

                    // Move to the next code
                    nCodeVal++;
                    nDhtInd++;
                }
            }

            // For each loop iteration (on bit length), we shift the code value
            nCodeVal <<= 1;
        }

        // Now store the dht_lookup_size
        uint32_t nTmpSize = nDhtLookupInd;

        bRet = _imgDec.SetDhtSize(nDhtHuffTblId_Th, nDhtClass_Tc, nTmpSize);

        if (!_stateAbort) {
            decodeErrCheck(bRet);
        }

        _log.info("");
    }

    if (bInject) {
        // Restore position (as if we didn't move)
        _pos = nPosSaved;
        _bufFakeDht = false;
    }
}

//-----------------------------------------------------------------------------
// Check return value of previous call. If failed, then ask
// user if they wish to continue decoding. If no, then flag to
// the decoder that we're done (avoids continuous failures)
void JfifDecode::decodeErrCheck(bool bRet) {
    if (!bRet) {
        // return true;
        // if (_appConfig.isInteractive()) {
        //     _msgBox.setText("Do you want to continue decoding?");
        //     _msgBox.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        //
        //     if (_msgBox.exec() == QMessageBox::No) {
        //         m_bStateAbort = true;
        //     }
        // }
    }
}

//-----------------------------------------------------------------------------
// This routine is called after the expected fields of the marker segment
// have been processed. The file position should line up with the offset
// dictated by the marker length. If a mismatch is detected, report an
// error.
//
// RETURN:
// - True if decode error is fatal (configurable)
//
bool JfifDecode::expectMarkerEnd(uint32_t nMarkerStart, uint32_t nMarkerLen) {
    QString strTmp;

    uint32_t nMarkerEnd = nMarkerStart + nMarkerLen;
    uint32_t nMarkerExtra = nMarkerEnd - _pos;

    if (_pos < nMarkerEnd) {
        // The length indicates that there is more data than we processed
        strTmp = QString("  WARNING: Marker length longer than expected");
        _log.warn(strTmp);

        if (!_appConfig.relaxedParsing()) {
            // Abort
            _log.error("  Stopping decode");
            _log.error("  Use [Img Search Fwd/Rev] to locate other valid embedded JPEGs");
            return false;
        } else {
            // Warn and skip
            strTmp = QString("  Skipping remainder [%1 bytes]").arg(nMarkerExtra);
            _log.warn(strTmp);
            _pos += nMarkerExtra;
        }
    } else if (_pos > nMarkerEnd) {
        // The length indicates that there is less data than we processed
        strTmp = QString("  WARNING: Marker length shorter than expected");
        _log.warn(strTmp);

        if (!_appConfig.relaxedParsing()) {
            // Abort
            _log.error("  Stopping decode");
            _log.error("  Use [Img Search Fwd/Rev] to locate other valid embedded JPEGs");
            return false;
        } else {
            // Warn but no skip
            // Note that we can't skip as the length would imply a rollback
            // Most resilient solution is probably to assume length was
            // wrong and continue from where the marker should have ended.
            // For resiliency, attempt two methods to find point to resume:
            // 1) Current position
            // 2) Actual length defined in marker
            if (getByte(_pos) == 0xFF) {
                // Using actual data expected seems more promising
                _log.warn("  Resuming decode");
            } else if (getByte(nMarkerEnd) == 0xFF) {
                // Using actual length seems more promising
                _pos = nMarkerEnd;
                _log.warn("  Rolling back pointer to end indicated by length");
                _log.warn("  Resuming decode");
            } else {
                // No luck. Expect marker failure now
                _log.warn("  Resuming decode");
            }
        }
    }

    // If we get here, then we haven't seen a fatal issue
    return true;
}

//-----------------------------------------------------------------------------
// Validate an unsigned value to ensure it is in allowable range
// - If the value is outside the range, an error is shown and
//   the parsing stops if relaxed parsing is not enabled
// - An optional override value is provided for the resume case
//
// INPUT:
// - nVal                       Input value (unsigned 32-bit)
// - nMin                       Minimum allowed value
// - nMax                       Maximum allowed value
// - strName            Name of the field
// - bOverride          Should we override the value upon out-of-range?
// - nOverrideVal       Value to override if bOverride and out-of-range
//
// PRE:
// - m_pAppConfig
//
// OUTPUT:
// - nVal                       Output value (including any override)
//
bool JfifDecode::validateValue(uint32_t &nVal, uint32_t nMin, uint32_t nMax, QString strName, bool bOverride,
                               uint32_t nOverrideVal) {
    QString strErr;

    if ((nVal >= nMin) && (nVal <= nMax)) {
        // Value is within range
        return true;
    } else {
        if (nVal < nMin) {
            strErr = QString("%1 value too small (Actual = %2, Expected >= %3)").arg(strName).arg(nVal).arg(
                nMin);
            _log.error(strErr);
        } else if (nVal > nMax) {
            strErr = QString("%1 value too large (Actual = %2, Expected <= %3)").arg(strName).arg(nVal).arg(
                nMax);
            _log.error(strErr);
        }

        if (!_appConfig.relaxedParsing()) {
            // Defined as fatal error
            // TODO: Replace with glb_strMsgStopDecode?
            _log.error("  Stopping decode");
            _log.error("  Use [Relaxed Parsing] to continue");
            return false;
        } else {
            // Non-fatal
            if (bOverride) {
                // Update value with override
                nVal = nOverrideVal;
                strErr = QString("  WARNING: Forcing value to [%1]").arg(nOverrideVal);
                _log.warn(strErr);
                _log.warn("  Resuming decode");
            } else {
                // No override
                strErr = QString("  Resuming decode");
                _log.warn(strErr);
            }
            return true;
        }
    }
}

//-----------------------------------------------------------------------------
// This is the primary JFIF marker parser. It reads the
// marker value at the current file position and launches the
// specific parser routine. This routine exits when
#define DECMARK_OK 0
#define DECMARK_ERR 1
#define DECMARK_EOI 2

uint32_t JfifDecode::decodeMarker() {
    char acIdentifier[MAX_IDENTIFIER];

    QString strTmp;
    QString strFull;              // Used for concatenation

    uint32_t nLength;             // General purpose

    uint32_t nTmpVal;
    uint8_t nTmpVal1;

    uint16_t nTmpVal2;

    uint32_t nCode;
    uint32_t nPosEnd;
    uint32_t nPosSaved;      // General-purpose saved position in file
    uint32_t nPosExifStart;
    uint32_t nRet;                // General purpose return value

    bool bRet;

    uint32_t nPosMarkerStart;        // Offset for current marker
    uint32_t nColTransform = 0;   // Color Transform from APP14 marker

    // For DQT
    QString strDqtPrecision = "";

    // QString strDqtZigZagOrder = "";

    if (getByte(_pos) != 0xFF) {
        if (_pos == 0) {
            // Don't give error message if we've already alerted them of AVI / PSD
            if ((!_avi) && (!_psd)) {
                _log.warn("File did not start with JPEG marker. Consider using [Tools->Img Search Fwd] to locate embedded JPEG.");
            }
        } else {
            strTmp =
                QString(
                    "Expected marker 0xFF, got 0x%1 @ offset 0x%2. Consider using [Tools->Img Search Fwd/Rev].")
                    .arg(getByte(_pos), 2, 16, QChar('0'))
                    .arg(_pos, 8, 16, QChar('0'));
            _log.error(strTmp);
        }

        _pos++;
        return DECMARK_ERR;
    }

    _pos++;

    // Read the current marker code
    nCode = getByte(_pos++);

    // Handle Marker Padding
    //
    // According to Section B.1.1.2:
    //   "Any marker may optionally be preceded by any number of fill bytes, which are bytes assigned code X�FF�."
    //
    uint32_t nSkipMarkerPad = 0;

    while (nCode == 0xFF) {
        // Count the pad
        nSkipMarkerPad++;
        // Read another byte
        nCode = getByte(_pos++);
    }

    // Report out any padding
    if (nSkipMarkerPad > 0) {
        strTmp = QString("*** Skipped %1 marker pad bytes ***").arg(nSkipMarkerPad);
        _log.info(strTmp);
    }

    // Save the current marker offset
    nPosMarkerStart = _pos;

    addHeader(nCode);

    switch (nCode) {
        case JFIF_SOI:             // SOI
            _stateSoi = true;
            break;

        case JFIF_APP12:
            // Photoshop DUCKY (Save For Web)
            nLength = getByte(_pos) * 256 + getByte(_pos + 1);
            //nLength = m_pWBuf->BufX(m_nPos,2,!m_nImgExifEndian);
            strTmp = QString("  Length          = %1").arg(nLength);
            _log.info(strTmp);

            nPosSaved = _pos;

            _pos += 2;              // Move past length now that we've used it

            strcpy(acIdentifier, _wbuf.readStrN(_pos, MAX_IDENTIFIER - 1).toLatin1().data());
            acIdentifier[MAX_IDENTIFIER - 1] = 0;     // Null terminate just in case
            strTmp = QString("  Identifier      = [%1]").arg(acIdentifier);
            _log.info(strTmp);
            _pos += static_cast<uint32_t>(strlen(acIdentifier)) + 1;

            if (strcmp(acIdentifier, "Ducky") != 0) {
                _log.info("    Not Photoshop DUCKY. Skipping remainder.");
            } else                      // Photoshop
            {
                // Please see reference on http://cpan.uwinnipeg.ca/htdocs/Image-ExifTool/Image/ExifTool/APP12.pm.html
                // A direct indexed approach should be safe
                m_nImgQualPhotoshopSfw = getByte(_pos + 6);
                strTmp = QString("  Photoshop Save For Web Quality = [%1]").arg(m_nImgQualPhotoshopSfw);
                _log.info(strTmp);
            }

            // Restore original position in file to a point
            // after the section
            _pos = nPosSaved + nLength;
            break;

        case JFIF_APP14:
            // JPEG Adobe  tag

            nLength = getByte(_pos) * 256 + getByte(_pos + 1);
            strTmp = QString("  Length            = %1").arg(nLength);
            _log.info(strTmp);

            nPosSaved = _pos;

            // Some files had very short segment (eg. nLength=2)
            if (nLength < 2 + 12) {
                _log.info("    Segment too short for Identifier. Skipping remainder.");
                _pos = nPosSaved + nLength;
                break;
            }

            _pos += 2;              // Move past length now that we've used it

            // TODO: Confirm Adobe flag
            _pos += 5;

            nTmpVal = getByte(_pos + 0) * 256 + getByte(_pos + 1);
            strTmp = QString("  DCTEncodeVersion  = %1").arg(nTmpVal);
            _log.info(strTmp);

            nTmpVal = getByte(_pos + 2) * 256 + getByte(_pos + 3);
            strTmp = QString("  APP14Flags0       = %1").arg(nTmpVal);
            _log.info(strTmp);

            nTmpVal = getByte(_pos + 4) * 256 + getByte(_pos + 5);
            strTmp = QString("  APP14Flags1       = %1").arg(nTmpVal);
            _log.info(strTmp);

            nColTransform = getByte(_pos + 6);

            switch (nColTransform) {
                case APP14_COLXFM_UNK_RGB:
                    strTmp = QString("  ColorTransform    = %1 [Unknown (RGB or CMYK)]").arg(nColTransform);
                    break;
                case APP14_COLXFM_YCC:
                    strTmp = QString("  ColorTransform    = %1 [YCbCr]").arg(nColTransform);
                    break;
                case APP14_COLXFM_YCCK:
                    strTmp = QString("  ColorTransform    = %1 [YCCK]").arg(nColTransform);
                    break;
                default:
                    strTmp = QString("  ColorTransform    = %1 [???]").arg(nColTransform);
                    break;
            }

            _log.info(strTmp);
            m_nApp14ColTransform = (nColTransform & 0xFF);

            // Restore original position in file to a point
            // after the section
            _pos = nPosSaved + nLength;

            break;

        case JFIF_APP13:
            // Photoshop (Save As)
            nLength = getByte(_pos) * 256 + getByte(_pos + 1);
            //nLength = m_pWBuf->BufX(m_nPos,2,!m_nImgExifEndian);
            strTmp = QString("  Length          = %1").arg(nLength);
            _log.info(strTmp);

            nPosSaved = _pos;

            // Some files had very short segment (eg. nLength=2)
            if (nLength < 2 + 20) {
                _log.info("    Segment too short for Identifier. Skipping remainder.");
                _pos = nPosSaved + nLength;
                break;
            }

            _pos += 2;              // Move past length now that we've used it

            strcpy(acIdentifier, _wbuf.readStrN(_pos, MAX_IDENTIFIER - 1).toLatin1().data());
            acIdentifier[MAX_IDENTIFIER - 1] = 0;     // Null terminate just in case
            strTmp = QString("  Identifier      = [%1]").arg(acIdentifier);
            _log.info(strTmp);
            _pos += static_cast<uint32_t>(strlen(acIdentifier)) + 1;

            if (strcmp(acIdentifier, "Photoshop 3.0") != 0) {
                _log.info("    Not Photoshop. Skipping remainder.");
            } else                      // Photoshop
            {
                decodeApp13Ps();
            }

            // Restore original position in file to a point
            // after the section
            _pos = nPosSaved + nLength;
            break;

        case JFIF_APP1:
            nLength = getByte(_pos) * 256 + getByte(_pos + 1);
            //nLength = m_pWBuf->BufX(m_nPos,2,!m_nImgExifEndian);
            strTmp = QString("  Length          = %1").arg(nLength);
            _log.info(strTmp);

            nPosSaved = _pos;

            _pos += 2;              // Move past length now that we've used it

            strcpy(acIdentifier, _wbuf.readStrN(_pos, MAX_IDENTIFIER - 1).toLatin1().data());
            acIdentifier[MAX_IDENTIFIER - 1] = 0;     // Null terminate just in case
            strTmp = QString("  Identifier      = [%1]").arg(acIdentifier);
            _log.info(strTmp);
            _pos += static_cast<uint32_t>(strlen(acIdentifier));

            if (strncmp(acIdentifier, "http://ns.adobe.com/xap/1.0/\x00", 29) == 0) {                         //@@
                // XMP

                _log.info("    XMP = ");

                _pos++;

                // NOTE: This code currently treats the strings from the XMP section
                // as single byte characters. In reality, it should probably be
                // updated to support unicode properly.

                uint32_t nPosMarkerEnd = nPosSaved + nLength - 1;
                uint32_t sXmpLen = nPosMarkerEnd - _pos;

                uint8_t cXmpChar;

                bool bNonSpace;

                QString strLine;

                // Reset state
                strLine = "          |";
                bNonSpace = false;

                for (uint32_t nInd = 0; nInd < sXmpLen; nInd++) {
                    // Get the next char (8-bit byte)
                    cXmpChar = _wbuf.getByte(_pos + nInd);

                    // Detect a non-space in line
                    if ((cXmpChar != 0x20) && (cXmpChar != 0x0A)) {
                        bNonSpace = true;
                    }

                    // Detect Linefeed, print out line
                    if (cXmpChar == 0x0A) {
                        // Only print line if some non-space elements!
                        if (bNonSpace) {
                            _log.info(strLine);
                        }
                        // Reset state
                        strLine = "          |";
                        bNonSpace = false;
                    } else {
                        // Add the char
                        strLine += cXmpChar;
                    }
                }
            } else if (strcmp(acIdentifier, "Exif") == 0)        //@@
            {
                // Only decode it further if it is EXIF format

                _pos += 2;            // Skip two 00 bytes

                nPosExifStart = _pos; // Save m_nPos @ start of EXIF used for all IFD offsets

                // =========== EXIF TIFF Header (Start) ===========
                // - Defined in Exif 2.2 Standard (JEITA CP-3451) section 4.5.2
                // - Contents (8 bytes total)
                //   - Byte order (2 bytes)
                //   - 0x002A (2 bytes)
                //   - Offset of 0th IFD (4 bytes)

                uint8_t acIdentifierTiff[9];

                strFull = "";
                strTmp = "";

                strFull = "  Identifier TIFF = ";

                for (uint32_t i = 0; i < 8; i++) {
                    acIdentifierTiff[i] = static_cast<uint8_t>(getByte(_pos++));
                }

                strTmp = printAsHexUc(acIdentifierTiff, 8);
                strFull += strTmp;
                _log.info(strFull);

                switch (acIdentifierTiff[0] * 256 + acIdentifierTiff[1]) {
                    case 0x4949:         // "II"
                        // Intel alignment
                        m_nImgExifEndian = 0;
                        _log.info("  Endian          = Intel (little)");
                        break;
                    case 0x4D4D:         // "MM"
                        // Motorola alignment
                        m_nImgExifEndian = 1;
                        _log.info("  Endian          = Motorola (big)");
                        break;
                }

                // We expect the TAG mark of 0x002A (depending on endian mode)
                uint32_t test_002a;

                test_002a = byteSwap2(acIdentifierTiff[2], acIdentifierTiff[3]);
                strTmp = QString("  TAG Mark x002A  = 0x%1").arg(test_002a, 4, 16, QChar('0'));
                _log.info(strTmp);

                uint32_t nIfdCount;     // Current IFD #

                uint32_t nOffsetIfd1;

                // Mark pointer to EXIF Sub IFD as 0 so that we can
                // detect if the tag never showed up.
                m_nImgExifSubIfdPtr = 0;
                m_nImgExifMakerPtr = 0;
                m_nImgExifGpsIfdPtr = 0;
                m_nImgExifInteropIfdPtr = 0;

                bool exif_done = false;

                nOffsetIfd1 = byteSwap4(acIdentifierTiff[4],
                                        acIdentifierTiff[5],
                                        acIdentifierTiff[6],
                                        acIdentifierTiff[7]);

                // =========== EXIF TIFF Header (End) ===========

                // =========== EXIF IFD 0 ===========
                // Do we start the 0th IFD for the "Primary Image Data"?
                // Even though the nOffsetIfd1 pointer should indicate to
                // us where the IFD should start (0x0008 if immediately after
                // EXIF TIFF Header), I have observed JPEG files that
                // do not contain the IFD. Therefore, we must check for this
                // condition by comparing against the APP marker length.
                // Example file: http://img9.imageshack.us/img9/194/90114543.jpg

                if ((nPosSaved + nLength) <= (nPosExifStart + nOffsetIfd1)) {
                    // We've run out of space for any IFD, so cancel now
                    exif_done = true;
                    _log.info("  No IFD entries");
                }

                nIfdCount = 0;

                while (!exif_done) {
                    _log.info("");

                    strTmp = QString("IFD%1").arg(nIfdCount);

                    // Process the IFD
                    nRet = decodeExifIfd(strTmp, nPosExifStart, nOffsetIfd1);

                    // Now that we have gone through all entries in the IFD directory,
                    // we read the offset to the next IFD
                    nOffsetIfd1 = byteSwap4(getByte(_pos + 0), getByte(_pos + 1), getByte(_pos + 2), getByte(_pos + 3));
                    _pos += 4;

                    strTmp = QString("    Offset to Next IFD = 0x%1").arg(nOffsetIfd1, 8, 16, QChar('0'));
                    _log.info(strTmp);

                    if (nRet != 0) {
                        // Error condition (DecodeExifIfd returned error)
                        nOffsetIfd1 = 0x00000000;
                    }

                    if (nOffsetIfd1 == 0x00000000) {
                        // Either error condition or truly end of IFDs
                        exif_done = true;
                    } else {
                        nIfdCount++;
                    }

                }                       // while ! exif_done

                // If EXIF SubIFD was defined, then handle it now
                if (m_nImgExifSubIfdPtr != 0) {
                    _log.info("");
                    decodeExifIfd("SubIFD", nPosExifStart, m_nImgExifSubIfdPtr);
                }

                if (m_nImgExifMakerPtr != 0) {
                    _log.info("");
                    decodeExifIfd("MakerIFD", nPosExifStart, m_nImgExifMakerPtr);
                }

                if (m_nImgExifGpsIfdPtr != 0) {
                    _log.info("");
                    decodeExifIfd("GPSIFD", nPosExifStart, m_nImgExifGpsIfdPtr);
                }

                if (m_nImgExifInteropIfdPtr != 0) {
                    _log.info("");
                    decodeExifIfd("InteropIFD", nPosExifStart, m_nImgExifInteropIfdPtr);
                }
            } else {
                strTmp = QString("Identifier [%1] not supported. Skipping remainder.").arg(acIdentifier);
                _log.info(strTmp);
            }

            //////////

            // Dump out Makernote area

            // TODO: Disabled for now
#if 0
                                                                                                                                    uint32_t ptr_base;

      if(m_bVerbose)
      {
        if(m_nImgExifMakerPtr != 0)
        {
          // FIXME: Seems that nPosExifStart is not initialized in VERBOSE mode
          ptr_base = nPosExifStart + m_nImgExifMakerPtr;

          m_pLog->AddLine("Exif Maker IFD DUMP");
          strFull = QString("  MarkerOffset @ 0x%08X"), ptr_base;
          m_pLog->AddLine(strFull);
        }
      }
#endif

            // End of dump out makernote area

            // Restore file position
            _pos = nPosSaved;

            // Restore original position in file to a point
            // after the section
            _pos = nPosSaved + nLength;

            break;

        case JFIF_APP2:
            // Typically used for Flashpix and possibly ICC profiles
            // Photoshop (Save As)
            nLength = getByte(_pos) * 256 + getByte(_pos + 1);
            //nLength = m_pWBuf->BufX(m_nPos,2,!m_nImgExifEndian);
            strTmp = QString("  Length          = %1").arg(nLength);
            _log.info(strTmp);

            nPosSaved = _pos;

            _pos += 2;              // Move past length now that we've used it

            strcpy(acIdentifier, _wbuf.readStrN(_pos, MAX_IDENTIFIER - 1).toLatin1().data());
            acIdentifier[MAX_IDENTIFIER - 1] = 0;     // Null terminate just in case
            strTmp = QString("  Identifier      = [%1]").arg(acIdentifier);
            _log.info(strTmp);
            _pos += static_cast<uint32_t>(strlen(acIdentifier)) + 1;

            if (strcmp(acIdentifier, "FPXR") == 0) {
                // Photoshop
                _log.info("    FlashPix:");
                decodeApp2FlashPix();
            } else if (strcmp(acIdentifier, "ICC_PROFILE") == 0) {
                // ICC Profile
                _log.info("    ICC Profile:");
                decodeApp2IccProfile(nLength);
            } else {
                _log.info("    Not supported. Skipping remainder.");
            }

            // Restore original position in file to a point
            // after the section
            _pos = nPosSaved + nLength;
            break;

        case JFIF_APP3:
        case JFIF_APP4:
        case JFIF_APP5:
        case JFIF_APP6:
        case JFIF_APP7:
        case JFIF_APP8:
        case JFIF_APP9:
        case JFIF_APP10:
        case JFIF_APP11:
            //case JFIF_APP12: // Handled separately
            //case JFIF_APP13: // Handled separately
            //case JFIF_APP14: // Handled separately
        case JFIF_APP15:
            nLength = getByte(_pos) * 256 + getByte(_pos + 1);
            //nLength = m_pWBuf->BufX(m_nPos,2,!m_nImgExifEndian);
            _log.info(QString("  Length     = %1").arg(nLength));

            if (_verbose) {
                strFull = "";

                for (uint32_t i = 0; i < nLength; i++) {
                    // Start a new line for every 16 codes
                    if ((i % 16) == 0) {
                        strFull = QString("  MarkerOffset [%1]: ").arg(i, 4, 16, QChar('0'));
                    } else if ((i % 8) == 0) {
                        strFull += "  ";
                    }

                    nTmpVal = getByte(_pos + i);
                    strFull += QString("%1 ").arg(nTmpVal, 2, 16, QChar('0'));

                    if ((i % 16) == 15) {
                        _log.info(strFull);
                        strFull = "";
                    }
                }

                _log.info(strFull);

                strFull = "";

                for (uint32_t i = 0; i < nLength; i++) {
                    // Start a new line for every 16 codes
                    if ((i % 32) == 0) {
                        strFull = QString("  MarkerOffset [%1]: ").arg(i, 4, 16, QChar('0'));
                    } else if ((i % 8) == 0) {
                        strFull += " ";
                    }

                    nTmpVal1 = getByte(_pos + i);

                    if (isprint(nTmpVal1)) {
                        strFull += QString("%1").arg(nTmpVal1);
                    } else {
                        strFull += ".";
                    }

                    if ((i % 32) == 31) {
                        _log.info(strFull);
                    }
                }

                _log.info(strFull);
            }                         // nVerbose

            _pos += nLength;
            break;

        case JFIF_APP0:            // APP0
            nLength = getByte(_pos) * 256 + getByte(_pos + 1);
            //nLength = m_pWBuf->BufX(m_nPos,2,!m_nImgExifEndian);
            _pos += 2;
            strTmp = QString("  Length     = %1").arg(nLength);
            _log.info(strTmp);

            strcpy(_app0Identifier, _wbuf.readStrN(_pos, MAX_IDENTIFIER - 1).toLatin1().data());
            _app0Identifier[MAX_IDENTIFIER - 1] = 0;       // Null terminate just in case
            strTmp = QString("  Identifier = [%1]").arg(_app0Identifier);
            _log.info(strTmp);

            if (strcmp(_app0Identifier, "JFIF")) {
                // Only process remainder if it is JFIF. This marker
                // is also used for application-specific functions.

                _pos += static_cast<uint32_t>((strlen(_app0Identifier)) + 1);

                m_nImgVersionMajor = getByte(_pos++);
                m_nImgVersionMinor = getByte(_pos++);
                strTmp = QString("  version    = [%1.%2]").arg(m_nImgVersionMajor).arg(m_nImgVersionMinor);
                _log.info(strTmp);

                m_nImgUnits = getByte(_pos++);

                m_nImgDensityX = getByte(_pos) * 256 + getByte(_pos + 1);
                //m_nImgDensityX = m_pWBuf->BufX(m_nPos,2,!m_nImgExifEndian);
                _pos += 2;
                m_nImgDensityY = getByte(_pos) * 256 + getByte(_pos + 1);
                //m_nImgDensityY = m_pWBuf->BufX(m_nPos,2,!m_nImgExifEndian);
                _pos += 2;
                strTmp = QString("  density    = %1 x %2 ").arg(m_nImgDensityX).arg(m_nImgDensityY);
                strFull = strTmp;

                switch (m_nImgUnits) {
                    case 0:
                        strFull += "(aspect ratio)";
                        _log.info(strFull);
                        break;

                    case 1:
                        strFull += "DPI (dots per inch)";
                        _log.info(strFull);
                        break;

                    case 2:
                        strFull += "DPcm (dots per cm)";
                        _log.info(strFull);
                        break;

                    default:
                        strTmp = QString("Unknown ImgUnits parameter [%1]").arg(m_nImgUnits);
                        strFull += strTmp;
                        _log.warn(strFull);
                        //return DECMARK_ERR;
                        break;
                }

                m_nImgThumbSizeX = getByte(_pos++);
                m_nImgThumbSizeY = getByte(_pos++);
                strTmp = QString("  thumbnail  = %1 x %2").arg(m_nImgThumbSizeX).arg(m_nImgThumbSizeY);
                _log.info(strTmp);

                // Unpack the thumbnail:
                uint32_t thumbnail_r, thumbnail_g, thumbnail_b;

                if (m_nImgThumbSizeX && m_nImgThumbSizeY) {
                    for (uint32_t y = 0; y < m_nImgThumbSizeY; y++) {
                        strFull = QString("   Thumb[%1] = ").arg(y, 3, 10, QChar('0'));

                        for (uint32_t x = 0; x < m_nImgThumbSizeX; x++) {
                            thumbnail_r = getByte(_pos++);
                            thumbnail_g = getByte(_pos++);
                            thumbnail_b = getByte(_pos++);
                            strTmp = QString("(0x%1,0x%2,0x%3) ")
                                .arg(thumbnail_r, 2, 16, QChar('0'))
                                .arg(thumbnail_g, 2, 16, QChar('0'))
                                .arg(thumbnail_b, 2, 16, QChar('0'));
                            strFull += strTmp;
                            _log.info(strFull);
                        }
                    }
                }

                // TODO:
                // - In JPEG-B mode (GeoRaster), we will need to fake out
                //   the DHT & DQT tables here. Unfortunately, we'll have to
                //   rely on the user to put us into this mode as there is nothing
                //   in the file that specifies this mode.

                /*
           // TODO: Need to ensure that Faked DHT is correct table

           AddHeader(JFIF_DHT_FAKE);
           DecodeDHT(true);
           // Need to mark DHT tables as OK
           m_bStateDht = true;
           m_bStateDhtFake = true;
           m_bStateDhtOk = true;

           // ... same for DQT
         */

            } else if (strncmp(_app0Identifier, "AVI1", 4))   //@@
            {
                // AVI MJPEG type

                // Need to fill in predefined DHT table from spec:
                //   OpenDML file format for AVI, section "Proposed Data Chunk Format"
                //   Described in MMREG.H
                _log.info("  Detected MotionJPEG");
                _log.info("  Importing standard Huffman table...");
                _log.info("");

                addHeader(JFIF_DHT_FAKE);

                decodeDht(true);
                // Need to mark DHT tables as OK
                _stateDht = true;
                _stateDhtFake = true;
                _stateDhtOk = true;

                _pos += nLength - 2;  // Skip over, and undo length short read

            } else {
                // Not JFIF or AVI1
                _log.info("    Not known APP0 type. Skipping remainder.");
                _pos += nLength - 2;
            }

            if (!expectMarkerEnd(nPosMarkerStart, nLength))
                return DECMARK_ERR;

            break;

        case JFIF_DQT:             // Define quantization tables
            _stateDqt = true;

            uint32_t nDqtPrecision_Pq;
            uint32_t nDqtQuantDestId_Tq;

            nLength = getByte(_pos) * 256 + getByte(_pos + 1);    // Lq
            nPosEnd = _pos + nLength;
            _pos += 2;
            strTmp = QString("  Table length = %1").arg(nLength);
            _log.info(strTmp);

            while (nPosEnd > _pos) {
                _log.info("  ----");

                nTmpVal = getByte(_pos++);        // Pq | Tq
                nDqtPrecision_Pq = (nTmpVal & 0xF0) >> 4;       // Pq, range 0-1
                nDqtQuantDestId_Tq = nTmpVal & 0x0F;    // Tq, range 0-3

                // Decode per ITU-T.81 standard
#if 1
                if (nDqtPrecision_Pq == 0) {
                    strDqtPrecision = "8 bits";
                } else if (nDqtPrecision_Pq == 1) {
                    strDqtPrecision = "16 bits";
                } else {
                    _log.warn(QString("    Unsupported precision value [%1]").arg(nDqtPrecision_Pq));
                    strDqtPrecision = "???";
                    // FIXME: Consider terminating marker parsing early
                }

                if (!validateValue(nDqtPrecision_Pq, 0, 1, "DQT Precision <Pq>", true, 0))
                    return DECMARK_ERR;

                if (!validateValue(nDqtQuantDestId_Tq, 0, 3, "DQT Destination ID <Tq>", true, 0))
                    return DECMARK_ERR;

                strTmp = QString("  Precision=%1").arg(strDqtPrecision);
                _log.info(strTmp);
#else
                                                                                                                                        // Decode with additional DQT extension (ITU-T-JPEG-Plus-Proposal_R3.doc)

        if((nDqtPrecision_Pq & 0xE) == 0)
        {
          // Per ITU-T.81 Standard
          if(nDqtPrecision_Pq == 0)
          {
            strDqtPrecision = "8 bits";
          }
          else if(nDqtPrecision_Pq == 1)
          {
            strDqtPrecision = "16 bits";
          }

          strTmp = QString("  Precision=%1"), strDqtPrecision;
          m_pLog->AddLine(strTmp);
        }
        else
        {
          // Non-standard
          // JPEG-Plus-Proposal-R3:
          // - Alternative sub-block-wise sequence
          strTmp = QString("  Non-Standard DQT Extension detected");
          m_pLog->AddLineWarn(strTmp);

          // FIXME: Should prevent attempt to decode until this is implemented

          if(nDqtPrecision_Pq == 0)
          {
            strDqtPrecision = "8 bits";
          }
          else if(nDqtPrecision_Pq == 1)
          {
            strDqtPrecision = "16 bits";
          }
          strTmp = QString("  Precision=%1"), strDqtPrecision;
          m_pLog->AddLine(strTmp);

          if((nDqtPrecision_Pq & 0x2) == 0)
          {
            strDqtZigZagOrder = "Diagonal zig-zag coeff scan seqeunce";
          }
          else if((nDqtPrecision_Pq & 0x2) == 1)
          {
            strDqtZigZagOrder = "Alternate coeff scan seqeunce";
          }

          strTmp = QString("  Coeff Scan Sequence=%s"), strDqtZigZagOrder;
          m_pLog->AddLine(strTmp);

          if((nDqtPrecision_Pq & 0x4) == 1)
          {
            strTmp = QString("  Custom coeff scan sequence");
            m_pLog->AddLine(strTmp);
            // Now expect sequence of 64 coefficient entries
            QString strSequence = "";

            for(uint32_t nInd = 0; nInd < 64; nInd++)
            {
              nTmpVal = Buf(m_nPos++);
              strTmp = QString("%u"), nTmpVal;
              strSequence += strTmp;

              if(nInd != 63)
              {
                strSequence += ", ";
              }
            }

            strTmp = QString("  Custom sequence = [ %s ]"), strSequence;
            m_pLog->AddLine(strTmp);
          }
        }
#endif
                strTmp = QString("  Destination ID=%1").arg(nDqtQuantDestId_Tq);

                if (nDqtQuantDestId_Tq == 0) {
                    strTmp += " (Luminance)";
                } else if (nDqtQuantDestId_Tq == 1) {
                    strTmp += " (Chrominance)";
                } else if (nDqtQuantDestId_Tq == 2) {
                    strTmp += " (Chrominance)";
                } else {
                    strTmp += " (???)";
                }

                _log.info(strTmp);

                // FIXME: The following is somewhat superseded by ValidateValue() above with the exception of skipping remainder
                if (nDqtQuantDestId_Tq >= MAX_DQT_DEST_ID) {
                    _log.error(QString("Destination ID <Tq> = %1, >= %2").arg(nDqtQuantDestId_Tq).arg(
                        MAX_DQT_DEST_ID));

                    if (!_appConfig.relaxedParsing()) {
                        _log.error("  Stopping decode");
                        return DECMARK_ERR;
                    } else {
                        // Now skip remainder of DQT
                        // FIXME
                        _log.warn(QString("  Skipping remainder of marker [%1 bytes]").arg(
                            nPosMarkerStart + nLength - _pos));
                        _log.info("");
                        _pos = nPosMarkerStart + nLength;
                        return DECMARK_OK;
                    }
                }

                bool bQuantAllOnes = true;

                double dComparePercent = 0;
                double dSumPercent = 0;
                double dSumPercentSqr = 0;

                for (uint32_t nCoeffInd = 0; nCoeffInd < MAX_DQT_COEFF; nCoeffInd++) {
                    nTmpVal2 = getByte(_pos++);

                    if (nDqtPrecision_Pq == 1) {
                        // 16-bit DQT entries!
                        nTmpVal2 <<= 8;
                        nTmpVal2 += getByte(_pos++);
                    }

                    m_anImgDqtTbl[nDqtQuantDestId_Tq][glb_anZigZag[nCoeffInd]] = nTmpVal2;

                    /* scaling factor in percent */

                    // Now calculate the comparison with the Annex sample

                    // FIXME: Should probably use check for landscape orientation and
                    //        rotate comparison matrix accordingly

                    if (nDqtQuantDestId_Tq == 0) {
                        if (m_anImgDqtTbl[nDqtQuantDestId_Tq][glb_anZigZag[nCoeffInd]] != 0) {
                            m_afStdQuantLumCompare[glb_anZigZag[nCoeffInd]] =
                                static_cast<double>((glb_anStdQuantLum[glb_anZigZag[nCoeffInd]])) /
                                static_cast<double>((m_anImgDqtTbl[nDqtQuantDestId_Tq][glb_anZigZag[nCoeffInd]]));
                            dComparePercent = 100.0 *
                                              static_cast<double>((m_anImgDqtTbl[nDqtQuantDestId_Tq][glb_anZigZag[nCoeffInd]])) /
                                              static_cast<double>((glb_anStdQuantLum[glb_anZigZag[nCoeffInd]]));
                        } else {
                            m_afStdQuantLumCompare[glb_anZigZag[nCoeffInd]] = 999.99;
                            dComparePercent = 999.99;
                        }
                    } else {
                        if (m_anImgDqtTbl[nDqtQuantDestId_Tq][glb_anZigZag[nCoeffInd]] != 0) {
                            m_afStdQuantChrCompare[glb_anZigZag[nCoeffInd]] =
                                static_cast<double>((glb_anStdQuantChr[glb_anZigZag[nCoeffInd]])) /
                                static_cast<double>((m_anImgDqtTbl[nDqtQuantDestId_Tq][glb_anZigZag[nCoeffInd]]));
                            dComparePercent = 100.0 *
                                              static_cast<double>((m_anImgDqtTbl[nDqtQuantDestId_Tq][glb_anZigZag[nCoeffInd]])) /
                                              static_cast<double>((glb_anStdQuantChr[glb_anZigZag[nCoeffInd]]));
                        } else {
                            m_afStdQuantChrCompare[glb_anZigZag[nCoeffInd]] = 999.99;
                        }
                    }

                    dSumPercent += dComparePercent;
                    dSumPercentSqr += dComparePercent * dComparePercent;

                    // Check just in case entire table are ones (Quality 100)
                    if (m_anImgDqtTbl[nDqtQuantDestId_Tq][glb_anZigZag[nCoeffInd]] != 1)
                        bQuantAllOnes = 0;
                }                       // 0..63

                // Note that the DQT table that we are saving is already
                // after doing zigzag reordering:
                // From high freq -> low freq
                // To X,Y, left-to-right, top-to-bottom

                // Flag this DQT table as being set!
                m_abImgDqtSet[nDqtQuantDestId_Tq] = true;

                uint32_t nCoeffInd;

                // Now display the table
                for (uint32_t nDqtY = 0; nDqtY < 8; nDqtY++) {
                    strFull = QString("    DQT, Row #%1: ").arg(nDqtY);

                    for (uint32_t nDqtX = 0; nDqtX < 8; nDqtX++) {
                        nCoeffInd = nDqtY * 8 + nDqtX;
                        strFull += QString("%1 ").arg(m_anImgDqtTbl[nDqtQuantDestId_Tq][nCoeffInd], 3);

                        // Store the DQT entry into the Image Decoder
                        bRet = _imgDec.setDqtEntry(nDqtQuantDestId_Tq, nCoeffInd, glb_anUnZigZag[nCoeffInd],
                                                   m_anImgDqtTbl[nDqtQuantDestId_Tq][nCoeffInd]);
                        decodeErrCheck(bRet);
                    }

                    // Now add the compare with Annex K
                    // Decided to disable this as it was confusing users
                    /*
             strFull += "   AnnexRatio: <";
             for (uint32_t nDqtX=0;nDqtX<8;nDqtX++) {
             nCoeffInd = nDqtY*8+nDqtX;
             if (nDqtQuantDestId_Tq == 0) {
             strTmp = QString("%5.1f "),m_afStdQuantLumCompare[nCoeffInd];
             } else {
             strTmp = QString("%5.1f "),m_afStdQuantChrCompare[nCoeffInd];
             }
             strFull += strTmp;
             }
             strFull += ">";
           */

                    _log.info(strFull);
                }

                // Perform some statistical analysis of the quality factor
                // to determine the likelihood of the current quantization
                // table being a scaled version of the "standard" tables.
                // If the variance is high, it is unlikely to be the case.
                double dQuality;
                double dVariance;

                dSumPercent /= 64.0;    /* mean scale factor */
                dSumPercentSqr /= 64.0;
                dVariance = dSumPercentSqr - (dSumPercent * dSumPercent);       /* variance */

                // Generate the equivalent IJQ "quality" factor
                if (bQuantAllOnes)       /* special case for all-ones table */
                    dQuality = 100.0;
                else if (dSumPercent <= 100.0)
                    dQuality = (200.0 - dSumPercent) / 2.0;
                else
                    dQuality = 5000.0 / dSumPercent;

                // Save the quality rating for later
                m_adImgDqtQual[nDqtQuantDestId_Tq] = dQuality;
                _log.info(QString("    Approx quality factor = %1 (scaling=%2 variance=%3)")
                              .arg(dQuality, 0, 'f', 2)
                              .arg(dSumPercent, 0, 'f', 2)
                              .arg(dVariance, 0, 'f', 2));
            }

            _stateDqtOk = true;

            if (!expectMarkerEnd(nPosMarkerStart, nLength))
                return DECMARK_ERR;

            break;

        case JFIF_DAC:             // DAC (Arithmetic Coding)
            nLength = getByte(_pos) * 256 + getByte(_pos + 1);    // La
            _pos += 2;
            strTmp = QString("  Arithmetic coding header length = %1").arg(nLength);
            _log.info(strTmp);

            uint32_t nDAC_n;
            uint32_t nDAC_Tc, nDAC_Tb;
            uint32_t nDAC_Cs;

            nDAC_n = (nLength > 2) ? (nLength - 2) / 2 : 0;

            for (uint32_t nInd = 0; nInd < nDAC_n; nInd++) {
                nTmpVal = getByte(_pos++);        // Tc,Tb
                nDAC_Tc = (nTmpVal & 0xF0) >> 4;
                nDAC_Tb = (nTmpVal & 0x0F);
                strTmp = QString("  #%1: Table class                  = %2")
                    .arg(nInd + 1, 2, 10, QChar('0'))
                    .arg(nDAC_Tc);
                _log.info(strTmp);
                strTmp = QString("  #%1: Table destination identifier = %2")
                    .arg(nInd, 2, 10, QChar('0'))
                    .arg(nDAC_Tb);
                _log.info(strTmp);

                nDAC_Cs = getByte(_pos++);        // Cs
                strTmp = QString("  #%1: Conditioning table value     = %2")
                    .arg(nInd + 1, 2, 10, QChar('0'))
                    .arg(nDAC_Cs);
                _log.info(strTmp);

                if (!validateValue(nDAC_Tc, 0, 1, "Table class <Tc>", true, 0))
                    return DECMARK_ERR;
                if (!validateValue(nDAC_Tb, 0, 3, "Table destination ID <Tb>", true, 0))
                    return DECMARK_ERR;

                // Parameter range constraints per Table B.6:
                // ------------|-------------------------|-------------------|------------
                //             |     Sequential DCT      |  Progressive DCT  | Lossless
                //   Parameter |  Baseline    Extended   |                   |
                // ------------|-----------|-------------|-------------------|------------
                //     Cs      |   Undef   | Tc=0: 0-255 | Tc=0: 0-255       | 0-255
                //             |           | Tc=1: 1-63  | Tc=1: 1-63        |
                // ------------|-----------|-------------|-------------------|------------

                // However, to keep it simple (and not depend on lossless mode),
                // we will only check the maximal range
                if (!validateValue(nDAC_Cs, 0, 255, "Conditioning table value <Cs>", true, 0))
                    return DECMARK_ERR;
            }

            if (!expectMarkerEnd(nPosMarkerStart, nLength))
                return DECMARK_ERR;

            break;

        case JFIF_DNL:             // DNL (Define number of lines)
            nLength = getByte(_pos) * 256 + getByte(_pos + 1);    // Ld
            _pos += 2;
            strTmp = QString("  Header length = %1").arg(nLength);
            _log.info(strTmp);

            nTmpVal = getByte(_pos) * 256 + getByte(_pos + 1);    // NL
            _pos += 2;
            strTmp = QString("  Number of lines = %1").arg(nTmpVal);
            _log.info(strTmp);

            if (!validateValue(nTmpVal, 1, 65535, "Number of lines <NL>", true, 1))
                return DECMARK_ERR;

            if (!expectMarkerEnd(nPosMarkerStart, nLength))
                return DECMARK_ERR;

            break;

        case JFIF_EXP:
            nLength = getByte(_pos) * 256 + getByte(_pos + 1);    // Le
            _pos += 2;
            strTmp = QString("  Header length = %1").arg(nLength);
            _log.info(strTmp);

            uint32_t nEXP_Eh, nEXP_Ev;

            nTmpVal = getByte(_pos) * 256 + getByte(_pos + 1);    // Eh,Ev
            nEXP_Eh = (nTmpVal & 0xF0) >> 4;
            nEXP_Ev = (nTmpVal & 0x0F);
            _pos += 2;
            strTmp = QString("  Expand horizontally = %1").arg(nEXP_Eh);
            _log.info(strTmp);
            strTmp = QString("  Expand vertically   = %1").arg(nEXP_Ev);
            _log.info(strTmp);

            if (!validateValue(nEXP_Eh, 0, 1, "Expand horizontally <Eh>", true, 0))
                return DECMARK_ERR;

            if (!validateValue(nEXP_Ev, 0, 1, "Expand vertically <Ev>", true, 0))
                return DECMARK_ERR;

            if (!expectMarkerEnd(nPosMarkerStart, nLength))
                return DECMARK_ERR;
            break;

        case JFIF_SOF0:            // SOF0 (Baseline DCT)
        case JFIF_SOF1:            // SOF1 (Extended sequential)
        case JFIF_SOF2:            // SOF2 (Progressive)
        case JFIF_SOF3:
        case JFIF_SOF5:
        case JFIF_SOF6:
        case JFIF_SOF7:
        case JFIF_SOF9:
        case JFIF_SOF10:
        case JFIF_SOF11:
        case JFIF_SOF13:
        case JFIF_SOF14:
        case JFIF_SOF15:

            // TODO:
            // - JFIF_DHP should be able to reuse the JFIF_SOF marker parsing
            //   however as we don't support hierarchical image decode, we
            //   would want to skip the update of class members.

            _stateSof = true;

            // Determine if this is a SOF mode that we support
            // At this time, we only support Baseline DCT & Extended Sequential Baseline DCT
            // (non-differential) with Huffman coding. Progressive, Lossless,
            // Differential and Arithmetic coded modes are not supported.
            m_bImgSofUnsupported = true;

            if (nCode == JFIF_SOF0) {
                m_bImgSofUnsupported = false;
            }

            if (nCode == JFIF_SOF1) {
                m_bImgSofUnsupported = false;
            }

            // For reference, note progressive scan files even though
            // we don't currently support their decode
            if (nCode == JFIF_SOF2) {
                m_bImgProgressive = true;
            }

            nLength = getByte(_pos) * 256 + getByte(_pos + 1);    // Lf
            _pos += 2;
            strTmp = QString("  Frame header length = %1").arg(nLength);
            _log.info(strTmp);

            m_nSofPrecision_P = getByte(_pos++);        // P
            strTmp = QString("  Precision = %1").arg(m_nSofPrecision_P);
            _log.info(strTmp);

            if (!validateValue(m_nSofPrecision_P, 2, 16, "Precision <P>", true, 8))
                return DECMARK_ERR;

            m_nSofNumLines_Y = getByte(_pos) * 256 + getByte(_pos + 1);   // Y
            _pos += 2;
            strTmp = QString("  Number of Lines = %1").arg(m_nSofNumLines_Y);
            _log.info(strTmp);

            if (!validateValue(m_nSofNumLines_Y, 0, 65535, "Number of Lines <Y>", true, 0))
                return DECMARK_ERR;

            m_nSofSampsPerLine_X = getByte(_pos) * 256 + getByte(_pos + 1);       // X
            _pos += 2;
            strTmp = QString("  Samples per Line = %1").arg(m_nSofSampsPerLine_X);
            _log.info(strTmp);

            if (!validateValue(m_nSofSampsPerLine_X, 1, 65535, "Samples per Line <X>", true, 1))
                return DECMARK_ERR;

            strTmp = QString("  Image Size = %1 x %2").arg(m_nSofSampsPerLine_X).arg(m_nSofNumLines_Y);
            _log.info(strTmp);

            // Determine orientation
            //   m_nSofSampsPerLine_X = X
            //   m_nSofNumLines_Y = Y
            m_eImgLandscape = ENUM_LANDSCAPE_YES;

            if (m_nSofNumLines_Y > m_nSofSampsPerLine_X)
                m_eImgLandscape = ENUM_LANDSCAPE_NO;

            strTmp = QString("  Raw Image Orientation = %1").arg(
                m_eImgLandscape == ENUM_LANDSCAPE_YES ? "Landscape" : "Portrait");
            _log.info(strTmp);

            m_nSofNumComps_Nf = getByte(_pos++);        // Nf, range 1..255
            strTmp = QString("  Number of Img components = %1").arg(m_nSofNumComps_Nf);
            _log.info(strTmp);

            if (!validateValue(m_nSofNumComps_Nf, 1, 255, "Number of Img components <Nf>", true, 1))
                return DECMARK_ERR;

            uint32_t nCompIdent;
            uint32_t anSofSampFact[MAX_SOF_COMP_NF];

            m_nSofHorzSampFactMax_Hmax = 0;
            m_nSofVertSampFactMax_Vmax = 0;

            // Now clear the output image content (all components)
            // TODO: Migrate some of the bitmap allocation / clearing from
            // DecodeScanImg() into ResetImageContent() and call here
            //m_pImgDec->ResetImageContent();

            // Per JFIF v1.02:
            // - Nf = 1 or 3
            // - C1 = Y
            // - C2 = Cb
            // - C3 = Cr

            for (uint32_t nCompInd = 1; ((!_stateAbort) && (nCompInd <= m_nSofNumComps_Nf)); nCompInd++) {
                nCompIdent = getByte(_pos++);     // Ci, range 0..255
                m_anSofQuantCompId[nCompInd] = nCompIdent;

                //if (!ValidateValue(m_anSofQuantCompId[nCompInd],0,255,"Component ID <Ci>"),true,0) return DECMARK_ERR;

                anSofSampFact[nCompIdent] = getByte(_pos++);
                m_anSofQuantTblSel_Tqi[nCompIdent] = getByte(_pos++);     // Tqi, range 0..3

                //if (!ValidateValue(m_anSofQuantTblSel_Tqi[nCompIdent],0,3,"Table Destination ID <Tqi>"),true,0) return DECMARK_ERR;

                // NOTE: We protect against bad input here as replication ratios are
                // determined later that depend on dividing by sampling factor (hence
                // possibility of div by 0).
                m_anSofHorzSampFact_Hi[nCompIdent] = (anSofSampFact[nCompIdent] & 0xF0) >> 4;   // Hi, range 1..4
                m_anSofVertSampFact_Vi[nCompIdent] = (anSofSampFact[nCompIdent] & 0x0F);        // Vi, range 1..4

                if (!validateValue(m_anSofHorzSampFact_Hi[nCompIdent],
                                   1,
                                   4,
                                   "Horizontal Sampling Factor <Hi>",
                                   true,
                                   1))
                    return DECMARK_ERR;

                if (!validateValue(m_anSofVertSampFact_Vi[nCompIdent], 1, 4, "Vertical Sampling Factor <Vi>", true, 1))
                    return DECMARK_ERR;
            }

            // Calculate max sampling factors
            for (uint32_t nCompInd = 1; ((!_stateAbort) && (nCompInd <= m_nSofNumComps_Nf)); nCompInd++) {
                nCompIdent = m_anSofQuantCompId[nCompInd];
                // Calculate maximum sampling factor for the SOF. This is only
                // used for later generation of m_strImgQuantCss an the SOF
                // reporting below. The CimgDecode block is responsible for
                // calculating the maximum sampling factor on a per-scan basis.
                m_nSofHorzSampFactMax_Hmax = qMax(m_nSofHorzSampFactMax_Hmax, m_anSofHorzSampFact_Hi[nCompIdent]);
                m_nSofVertSampFactMax_Vmax = qMax(m_nSofVertSampFactMax_Vmax, m_anSofVertSampFact_Vi[nCompIdent]);
            }

            // Report per-component sampling factors and quantization table selectors
            for (uint32_t nCompInd = 1; ((!_stateAbort) && (nCompInd <= m_nSofNumComps_Nf)); nCompInd++) {
                nCompIdent = m_anSofQuantCompId[nCompInd];

                // Create subsampling ratio
                // - Protect against division-by-zero
                QString strSubsampH = "?";
                QString strSubsampV = "?";

                if (m_anSofHorzSampFact_Hi[nCompIdent] > 0) {
                    strSubsampH = QString("%1").arg(m_nSofHorzSampFactMax_Hmax / m_anSofHorzSampFact_Hi[nCompIdent]);
                }

                if (m_anSofVertSampFact_Vi[nCompIdent] > 0) {
                    strSubsampV = QString("%1").arg(m_nSofVertSampFactMax_Vmax / m_anSofVertSampFact_Vi[nCompIdent]);
                }

                strFull = QString("    Component[%1]: ").arg(nCompInd); // Note i in Ci is 1-based
                strTmp = QString("ID=0x%1, Samp Fac=0x%2 (Subsamp %3 x %4), Quant Tbl Sel=0x%5")
                    .arg(nCompIdent, 2, 16, QChar('0'))
                    .arg(anSofSampFact[nCompIdent], 2, 16, QChar('0'))
                    .arg(strSubsampH)
                    .arg(strSubsampV)
                    .arg(m_anSofQuantTblSel_Tqi[nCompIdent], 2, 16, QChar('0'));
                strFull += strTmp;

                // Mapping from component index (not ID) to colour channel per JFIF
                if (m_nSofNumComps_Nf == 1) {
                    // Assume grayscale
                    strFull += " (Lum: Y)";
                } else if (m_nSofNumComps_Nf == 3) {
                    // Assume YCC
                    if (nCompInd == SCAN_COMP_Y) {
                        strFull += " (Lum: Y)";
                    } else if (nCompInd == SCAN_COMP_CB) {
                        strFull += " (Chrom: Cb)";
                    } else if (nCompInd == SCAN_COMP_CR) {
                        strFull += " (Chrom: Cr)";
                    }
                } else if (m_nSofNumComps_Nf == 4) {
                    // Assume YCCK
                    if (nCompInd == 1) {
                        strFull += " (Y)";
                    } else if (nCompInd == 2) {
                        strFull += " (Cb)";
                    } else if (nCompInd == 3) {
                        strFull += " (Cr)";
                    } else if (nCompInd == 4) {
                        strFull += " (K)";
                    }
                } else {
                    strFull += " (???)";  // Unknown
                }

                _log.info(strFull);
            }

            // Test for bad input, clean up if bad
            for (uint32_t nCompInd = 1; ((!_stateAbort) && (nCompInd <= m_nSofNumComps_Nf)); nCompInd++) {
                nCompIdent = m_anSofQuantCompId[nCompInd];

                if (!validateValue(m_anSofQuantCompId[nCompInd], 0, 255, "Component ID <Ci>", true, 0))
                    return DECMARK_ERR;

                if (!validateValue(m_anSofQuantTblSel_Tqi[nCompIdent], 0, 3, "Table Destination ID <Tqi>", true, 0))
                    return DECMARK_ERR;

                if (!validateValue(m_anSofHorzSampFact_Hi[nCompIdent],
                                   1,
                                   4,
                                   "Horizontal Sampling Factor <Hi>",
                                   true,
                                   1))
                    return DECMARK_ERR;

                if (!validateValue(m_anSofVertSampFact_Vi[nCompIdent], 1, 4, "Vertical Sampling Factor <Vi>", true, 1))
                    return DECMARK_ERR;
            }

            // Finally, assign the cleaned values to the decoder
            for (uint32_t nCompInd = 1; ((!_stateAbort) && (nCompInd <= m_nSofNumComps_Nf)); nCompInd++) {
                nCompIdent = m_anSofQuantCompId[nCompInd];
                // Store the DQT Table selection for the Image Decoder
                //   Param values: Nf,Tqi
                //   Param ranges: 1..255,0..3
                // Note that the Image Decoder doesn't need to see the Component Identifiers
                bRet = _imgDec.SetDqtTables(nCompInd, m_anSofQuantTblSel_Tqi[nCompIdent]);
                decodeErrCheck(bRet);

                // Store the Precision (to handle 12-bit decode)
                _imgDec.SetPrecision(m_nSofPrecision_P);
            }

            if (!_stateAbort) {
                // Set the component sampling factors (chroma subsampling)
                // FIXME: check ranging
                for (uint32_t nCompInd = 1; nCompInd <= m_nSofNumComps_Nf; nCompInd++) {
                    // nCompInd is component index (1...Nf)
                    // nCompIdent is Component Identifier (Ci)
                    // Note that the Image Decoder doesn't need to see the Component Identifiers
                    nCompIdent = m_anSofQuantCompId[nCompInd];
                    _imgDec.SetSofSampFactors(nCompInd,
                                              m_anSofHorzSampFact_Hi[nCompIdent],
                                              m_anSofVertSampFact_Vi[nCompIdent]);
                }

                // Now mark the image as been somewhat OK (ie. should
                // also be suitable for EmbeddedThumb() and PrepareSignature()
                _imgOk = true;

                _stateSofOk = true;
            }

            if (!expectMarkerEnd(nPosMarkerStart, nLength))
                return DECMARK_ERR;

            break;

        case JFIF_COM:             // COM
            nLength = getByte(_pos) * 256 + getByte(_pos + 1);
            _pos += 2;
            strTmp = QString("  Comment length = %1").arg(nLength);
            _log.info(strTmp);

            // Check for JPEG COM vulnerability
            //   http://marc.info/?l=bugtraq&m=109524346729948
            // Note that the recovery is not very graceful. It will assume that the
            // field is actually zero-length, which will make the next byte trigger the
            // "Expected marker 0xFF" error message and probably abort. There is no
            // obvious way to

            if ((nLength == 0) || (nLength == 1)) {
                strTmp = QString("    JPEG Comment Field Vulnerability detected!");
                _log.error(strTmp);
                strTmp = QString("    Skipping data until next marker...");
                _log.error(strTmp);
                nLength = 2;

                bool bDoneSearch = false;

                uint32_t nSkipStart = _pos;

                while (!bDoneSearch) {
                    if (getByte(_pos) != 0xFF) {
                        _pos++;
                    } else {
                        bDoneSearch = true;
                    }

                    if (_pos >= _wbuf.fileSize()) {
                        bDoneSearch = true;
                    }
                }

                strTmp = QString("    Skipped %1 bytes").arg(_pos - nSkipStart);
                _log.error(strTmp);

                // Break out of case statement
                break;
            }

            // Assume COM field valid length (ie. >= 2)
            strFull = "    Comment=";
            m_strComment = "";

            for (uint32_t ind = 0; ind < nLength - 2; ind++) {
                nTmpVal1 = getByte(_pos++);

                if (isprint(nTmpVal1)) {
                    strTmp = QString("%1").arg(nTmpVal1);
                    m_strComment += strTmp;
                } else {
                    m_strComment += ".";
                }
            }

            strFull += m_strComment;
            _log.info(strFull);

            break;

        case JFIF_DHT:             // DHT
            _stateDht = true;
            decodeDht(false);
            _stateDhtOk = true;
            break;

        case JFIF_SOS:             // SOS
            uint32_t nPosScanStart;      // Byte count at start of scan data segment

            _stateSos = true;

            // NOTE: Only want to capture position of first SOS
            //       This should make other function such as AVI frame extract
            //       more robust in case we get multiple SOS segments.
            // We assume that this value is reset when we start a new decode
            if (_posSos == 0) {
                _posSos = _pos - 2; // Used for Extract. Want to include actual marker
            }

            nLength = getByte(_pos) * 256 + getByte(_pos + 1);
            _pos += 2;

            // Ensure that we have seen proper markers before we try this one!
            if (!_stateSofOk) {
                strTmp = QString("  SOS before valid SOF defined");
                _log.error(strTmp);
                return DECMARK_ERR;
            }

            strTmp = QString("  Scan header length = %1").arg(nLength);
            _log.info(strTmp);

            m_nSosNumCompScan_Ns = getByte(_pos++);     // Ns, range 1..4
            strTmp = QString("  Number of img components = %1").arg(m_nSosNumCompScan_Ns);
            _log.info(strTmp);

            // Just in case something got corrupted, don't want to get out
            // of range here. Note that this will be a hard abort, and
            // will not resume decoding.
            if (m_nSosNumCompScan_Ns > MAX_SOS_COMP_NS) {
                strTmp = QString("  Scan decode does not support > %1 components").arg(MAX_SOS_COMP_NS);
                _log.error(strTmp);
                return DECMARK_ERR;
            }

            uint32_t nSosCompSel_Cs;
            uint32_t nSosHuffTblSel;
            uint32_t nSosHuffTblSelDc_Td;
            uint32_t nSosHuffTblSelAc_Ta;

            // Max range of components indices is between 1..4
            for (uint32_t nScanCompInd = 1; ((nScanCompInd <= m_nSosNumCompScan_Ns) &&
                                             (!_stateAbort)); nScanCompInd++) {
                strFull = QString("    Component[%1]: ").arg(nScanCompInd);
                nSosCompSel_Cs = getByte(_pos++); // Cs, range 0..255
                nSosHuffTblSel = getByte(_pos++);
                nSosHuffTblSelDc_Td = (nSosHuffTblSel & 0xf0) >> 4;     // Td, range 0..3
                nSosHuffTblSelAc_Ta = (nSosHuffTblSel & 0x0f);  // Ta, range 0..3
                strTmp = QString("selector=0x%1, table=%2(DC),%3(AC)")
                    .arg(nSosCompSel_Cs, 2, 16, QChar('0'))
                    .arg(nSosHuffTblSelDc_Td)
                    .arg(nSosHuffTblSelAc_Ta);
                strFull += strTmp;
                _log.info(strFull);

                bRet = _imgDec.SetDhtTables(nScanCompInd, nSosHuffTblSelDc_Td, nSosHuffTblSelAc_Ta);

                decodeErrCheck(bRet);
            }

            m_nSosSpectralStart_Ss = getByte(_pos++);
            m_nSosSpectralEnd_Se = getByte(_pos++);
            m_nSosSuccApprox_A = getByte(_pos++);

            strTmp = QString("  Spectral selection = %1 .. %2")
                .arg(m_nSosSpectralStart_Ss)
                .arg(m_nSosSpectralEnd_Se);
            _log.info(strTmp);
            strTmp = QString("  Successive approximation = 0x%1").arg(m_nSosSuccApprox_A, 2, 16, QChar('0'));
            _log.info(strTmp);

            if (_appConfig.scanDump()) {
                _log.info("");
                _log.info("  Scan Data: (after bitstuff removed)");
            }

            // Save the scan data segment position
            nPosScanStart = _pos;

            // Skip over the Scan Data segment
            //   Pass 1) Quick, allowing for bOutputScanDump to dump first 640B.
            //   Pass 2) If bDecodeScanImg, we redo the process but in detail decoding.

            // FIXME: Not sure why, but if I skip over Pass 1 (eg if I leave in the
            // following line uncommented), then I get an error at the end of the
            // pass 2 decode (indicating that EOI marker not seen, and expecting
            // marker).
            //              if (m_pAppConfig->bOutputScanDump) {

            // --- PASS 1 ---
            bool bSkipDone;

            uint32_t nSkipCount;
            uint32_t nSkipData;
            uint32_t nSkipPos;

            bool bScanDumpTrunc;

            bSkipDone = false;
            nSkipCount = 0;
            nSkipPos = 0;
            bScanDumpTrunc = false;

            strFull = "";

            while (!bSkipDone) {
                nSkipCount++;
                nSkipPos++;
                nSkipData = getByte(_pos++);

                if (nSkipData == 0xFF) {
                    // this could either be a marker or a byte stuff
                    nSkipData = getByte(_pos++);
                    nSkipCount++;

                    if (nSkipData == 0x00) {
                        // Byte stuff
                        nSkipData = 0xFF;
                    } else if ((nSkipData >= JFIF_RST0) && (nSkipData <= JFIF_RST7)) {
                        // Skip over
                    } else {
                        // Marker
                        bSkipDone = true;
                        _pos -= 2;
                    }
                }

                if (_appConfig.scanDump() && (!bSkipDone)) {
                    // Only display 20 lines of scan data
                    if (nSkipPos > 640) {
                        if (!bScanDumpTrunc) {
                            _log.warn("    WARNING: Dump truncated.");
                            bScanDumpTrunc = true;
                        }
                    } else {
                        if (((nSkipPos - 1) == 0) || (((nSkipPos - 1) % 32) == 0)) {
                            strFull = "    ";
                        }

                        strTmp = QString("%1 ").arg(nSkipData, 2, 16, QChar('0'));
                        strFull += strTmp;

                        if (((nSkipPos - 1) % 32) == 31) {
                            _log.info(strFull);
                            strFull = "";
                        }
                    }
                }

                // Did we run out of bytes?

                // FIXME:
                // NOTE: This line here doesn't allow us to attempt to
                // decode images that are missing EOI. Maybe this is
                // not the best solution here? Instead, we should be
                // checking m_nPos against file length? .. and not
                // return but "break".
                if (!_wbuf.isBufferOk()) {
                    strTmp = QString("Ran out of buffer before EOI during phase 1 of Scan decode @ 0x%1").arg(
                        _pos,
                        8,
                        16,
                        QChar('0'));
                    _log.error(strTmp);
                    break;
                }

            }

            _log.info(strFull);

            //              }

            // --- PASS 2 ---
            // If the option is set, start parsing!
            if (_appConfig.decodeImage() && m_bImgSofUnsupported) {
                // SOF marker was of type we don't support, so skip decoding
                _log.warn("  Scan parsing doesn't support this SOF mode.");
#ifndef DEBUG_YCCK
            } else if (_appConfig.decodeImage() && (m_nSofNumComps_Nf == 4)) {
                _log.warn("  Scan parsing doesn't support CMYK files yet.");
#endif
            } else if (_appConfig.decodeImage() && !m_bImgSofUnsupported) {
                if (!_stateSofOk) {
                    _log.warn("  Scan decode disabled as SOF not decoded.");
                } else if (!_stateDqtOk) {
                    _log.warn("  Scan decode disabled as DQT not decoded.");
                } else if (!_stateDhtOk) {
                    _log.warn("  Scan decode disabled as DHT not decoded.");
                } else {
                    _log.info("");

                    // Set the primary image details
                    _imgDec.setImageDetails(m_nSofSampsPerLine_X, m_nSofNumLines_Y,
                                            m_nSofNumComps_Nf, m_nSosNumCompScan_Ns, m_nImgRstEn, m_nImgRstInterval);

                    // Only recalculate the scan decoding if we need to (i.e. file
                    // changed, offset changed, scan option changed)
                    // TODO: In order to decode multiple scans, we will need to alter the
                    // way that m_pImgSrcDirty is set
                    if (_imgSrcDirty) {
                        _imgDec.decodeScanImg(nPosScanStart, true, false);
                        _imgSrcDirty = false;
                    }
                }
            }

            _stateSosOk = true;

            break;

        case JFIF_DRI:
            uint32_t nVal;

            nLength = getByte(_pos) * 256 + getByte(_pos + 1);
            strTmp = QString("  Length     = %1").arg(nLength);
            _log.info(strTmp);
            nVal = getByte(_pos + 2) * 256 + getByte(_pos + 3);

            // According to ITU-T spec B.2.4.4, we only expect
            // restart markers if DRI value is non-zero!
            m_nImgRstInterval = nVal;
            if (nVal != 0) {
                m_nImgRstEn = true;
            } else {
                m_nImgRstEn = false;
            }

            strTmp = QString("  interval   = %1").arg(m_nImgRstInterval);
            _log.info(strTmp);
            _pos += 4;

            if (!expectMarkerEnd(nPosMarkerStart, nLength))
                return DECMARK_ERR;

            break;

        case JFIF_EOI:             // EOI
            _log.info("");

            // Save the EOI file position
            // NOTE: If the file is missing the EOI, then this variable will be
            //       set to mark the end of file.
            _posEmbedEnd = _pos;
            _posEoi = _pos;
            _stateEoi = true;

            return DECMARK_EOI;

            break;

            // Markers that are not yet supported in JPEGsnoop
        case JFIF_DHP:
            // Markers defined for future use / extensions
        case JFIF_JPG:
        case JFIF_JPG0:
        case JFIF_JPG1:
        case JFIF_JPG2:
        case JFIF_JPG3:
        case JFIF_JPG4:
        case JFIF_JPG5:
        case JFIF_JPG6:
        case JFIF_JPG7:
        case JFIF_JPG8:
        case JFIF_JPG9:
        case JFIF_JPG10:
        case JFIF_JPG11:
        case JFIF_JPG12:
        case JFIF_JPG13:
        case JFIF_TEM:
            // Unsupported marker
            // - Provide generic decode based on length
            nLength = getByte(_pos) * 256 + getByte(_pos + 1);    // Length
            strTmp = QString("  Header length = %1").arg(nLength);
            _log.info(strTmp);
            _log.warn("  Skipping unsupported marker");
            _pos += nLength;
            break;

        case JFIF_RST0:
        case JFIF_RST1:
        case JFIF_RST2:
        case JFIF_RST3:
        case JFIF_RST4:
        case JFIF_RST5:
        case JFIF_RST6:
        case JFIF_RST7:
            // We don't expect to see restart markers outside the entropy coded segment.
            // NOTE: RST# are standalone markers, so no length indicator exists
            // But for the sake of robustness, we can check here to see if treating
            // as a standalone marker will arrive at another marker (ie. OK). If not,
            // proceed to assume there is a length indicator.
            strTmp = QString("  WARNING: Restart marker [0xFF%1] detected outside scan").arg(nCode, 2, 16, QChar('0'));
            _log.warn(strTmp);

            if (!_appConfig.relaxedParsing()) {
                // Abort
                _log.error("  Stopping decode");
                _log.info("  Use [Img Search Fwd/Rev] to locate other valid embedded JPEGs");
                return DECMARK_ERR;
            } else {
                // Ignore
                // Check to see if standalone marker treatment looks OK
                if (getByte(_pos + 2) == 0xFF) {
                    // Looks like standalone
                    _log.warn("  Ignoring standalone marker. Proceeding with decode.");
                    _pos += 2;
                } else {
                    // Looks like marker with length

                    nLength = getByte(_pos) * 256 + getByte(_pos + 1);
                    strTmp = QString("  Header length = %1").arg(nLength);
                    _log.info(strTmp);
                    _log.warn("  Skipping marker");
                    _pos += nLength;
                }
            }
            break;

        default:
            strTmp = QString("  WARNING: Unknown marker [0xFF%1]").arg(nCode, 2, 16, QChar('0'));
            _log.warn(strTmp);

            if (!_appConfig.relaxedParsing()) {
                // Abort
                _log.error("  Stopping decode");
                _log.info("  Use [Img Search Fwd/Rev] to locate other valid embedded JPEGs");
                return DECMARK_ERR;
            } else {
                // Skip
                nLength = getByte(_pos) * 256 + getByte(_pos + 1);
                strTmp = QString("  Header length = %1").arg(nLength);
                _log.info(strTmp);
                _log.warn("  Skipping marker");
                _pos += nLength;
            }
    }

    // Add white-space between each marker
    _log.info(" ");

    // If we decided to abort for any reason, make sure we trap it now.
    // This will stop the ProcessFile() while loop. We can set m_bStateAbort
    // if user says that they want to stop.
    if (_stateAbort) {
        return DECMARK_ERR;
    }

    return DECMARK_OK;
}

// Print out a header for the current JFIF marker code
void JfifDecode::addHeader(uint32_t nCode) {
    QString strTmp;

    switch (nCode) {
        case JFIF_SOI:
            _log.info("*** Marker: SOI (xFFD8) ***");
            break;

        case JFIF_APP0:
            _log.info("*** Marker: APP0 (xFFE0) ***");
            break;

        case JFIF_APP1:
            _log.info("*** Marker: APP1 (xFFE1) ***");
            break;

        case JFIF_APP2:
            _log.info("*** Marker: APP2 (xFFE2) ***");
            break;

        case JFIF_APP3:
            _log.info("*** Marker: APP3 (xFFE3) ***");
            break;

        case JFIF_APP4:
            _log.info("*** Marker: APP4 (xFFE4) ***");
            break;

        case JFIF_APP5:
            _log.info("*** Marker: APP5 (xFFE5) ***");
            break;

        case JFIF_APP6:
            _log.info("*** Marker: APP6 (xFFE6) ***");
            break;

        case JFIF_APP7:
            _log.info("*** Marker: APP7 (xFFE7) ***");
            break;

        case JFIF_APP8:
            _log.info("*** Marker: APP8 (xFFE8) ***");
            break;

        case JFIF_APP9:
            _log.info("*** Marker: APP9 (xFFE9) ***");
            break;

        case JFIF_APP10:
            _log.info("*** Marker: APP10 (xFFEA) ***");
            break;

        case JFIF_APP11:
            _log.info("*** Marker: APP11 (xFFEB) ***");
            break;

        case JFIF_APP12:
            _log.info("*** Marker: APP12 (xFFEC) ***");
            break;

        case JFIF_APP13:
            _log.info("*** Marker: APP13 (xFFED) ***");
            break;

        case JFIF_APP14:
            _log.info("*** Marker: APP14 (xFFEE) ***");
            break;

        case JFIF_APP15:
            _log.info("*** Marker: APP15 (xFFEF) ***");
            break;

        case JFIF_SOF0:
            _log.info("*** Marker: SOF0 (Baseline DCT) (xFFC0) ***");
            break;

        case JFIF_SOF1:
            _log.info("*** Marker: SOF1 (Extended Sequential DCT, Huffman) (xFFC1) ***");
            break;

        case JFIF_SOF2:
            _log.info("*** Marker: SOF2 (Progressive DCT, Huffman) (xFFC2) ***");
            break;

        case JFIF_SOF3:
            _log.info("*** Marker: SOF3 (Lossless Process, Huffman) (xFFC3) ***");
            break;

        case JFIF_SOF5:
            _log.info("*** Marker: SOF5 (Differential Sequential DCT, Huffman) (xFFC4) ***");
            break;

        case JFIF_SOF6:
            _log.info("*** Marker: SOF6 (Differential Progressive DCT, Huffman) (xFFC5) ***");
            break;

        case JFIF_SOF7:
            _log.info("*** Marker: SOF7 (Differential Lossless Process, Huffman) (xFFC6) ***");
            break;

        case JFIF_SOF9:
            _log.info("*** Marker: SOF9 (Sequential DCT, Arithmetic) (xFFC9) ***");
            break;

        case JFIF_SOF10:
            _log.info("*** Marker: SOF10 (Progressive DCT, Arithmetic) (xFFCA) ***");
            break;

        case JFIF_SOF11:
            _log.info("*** Marker: SOF11 (Lossless Process, Arithmetic) (xFFCB) ***");
            break;

        case JFIF_SOF13:
            _log.info("*** Marker: SOF13 (Differential Sequential, Arithmetic) (xFFCD) ***");
            break;

        case JFIF_SOF14:
            _log.info("*** Marker: SOF14 (Differential Progressive DCT, Arithmetic) (xFFCE) ***");
            break;

        case JFIF_SOF15:
            _log.info("*** Marker: SOF15 (Differential Lossless Process, Arithmetic) (xFFCF) ***");
            break;

        case JFIF_JPG:
            _log.info("*** Marker: JPG (xFFC8) ***");
            break;

        case JFIF_DAC:
            _log.info("*** Marker: DAC (xFFCC) ***");
            break;

        case JFIF_RST0:
        case JFIF_RST1:
        case JFIF_RST2:
        case JFIF_RST3:
        case JFIF_RST4:
        case JFIF_RST5:
        case JFIF_RST6:
        case JFIF_RST7:
            _log.info("*** Marker: RST# ***");
            break;

        case JFIF_DQT:             // Define quantization tables
            _log.info("*** Marker: DQT (xFFDB) ***");
            _log.info("  Define a Quantization Table.");
            break;

        case JFIF_COM:             // COM
            _log.info("*** Marker: COM (Comment) (xFFFE) ***");
            break;

        case JFIF_DHT:             // DHT
            _log.info("*** Marker: DHT (Define Huffman Table) (xFFC4) ***");
            break;

        case JFIF_DHT_FAKE:        // DHT from standard table (MotionJPEG)
            _log.info("*** Marker: DHT from MotionJPEG standard (Define Huffman Table) ***");
            break;

        case JFIF_SOS:             // SOS
            _log.info("*** Marker: SOS (Start of Scan) (xFFDA) ***");
            break;

        case JFIF_DRI:             // DRI
            _log.info("*** Marker: DRI (Restart Interval) (xFFDD) ***");
            break;

        case JFIF_EOI:             // EOI
            _log.info("*** Marker: EOI (End of Image) (xFFD9) ***");
            break;

        case JFIF_DNL:
            _log.info("*** Marker: DNL (Define Number of Lines) (xFFDC) ***");
            break;
        case JFIF_DHP:
            _log.info("*** Marker: DHP (Define Hierarchical Progression) (xFFDE) ***");
            break;
        case JFIF_EXP:
            _log.info("*** Marker: EXP (Expand Reference Components) (xFFDF) ***");
            break;
        case JFIF_JPG0:
            _log.info("*** Marker: JPG0 (JPEG Extension) (xFFF0) ***");
            break;
        case JFIF_JPG1:
            _log.info("*** Marker: JPG1 (JPEG Extension) (xFFF1) ***");
            break;
        case JFIF_JPG2:
            _log.info("*** Marker: JPG2 (JPEG Extension) (xFFF2) ***");
            break;
        case JFIF_JPG3:
            _log.info("*** Marker: JPG3 (JPEG Extension) (xFFF3) ***");
            break;
        case JFIF_JPG4:
            _log.info("*** Marker: JPG4 (JPEG Extension) (xFFF4) ***");
            break;
        case JFIF_JPG5:
            _log.info("*** Marker: JPG5 (JPEG Extension) (xFFF5) ***");
            break;
        case JFIF_JPG6:
            _log.info("*** Marker: JPG6 (JPEG Extension) (xFFF6) ***");
            break;
        case JFIF_JPG7:
            _log.info("*** Marker: JPG7 (JPEG Extension) (xFFF7) ***");
            break;
        case JFIF_JPG8:
            _log.info("*** Marker: JPG8 (JPEG Extension) (xFFF8) ***");
            break;
        case JFIF_JPG9:
            _log.info("*** Marker: JPG9 (JPEG Extension) (xFFF9) ***");
            break;
        case JFIF_JPG10:
            _log.info("*** Marker: JPG10 (JPEG Extension) (xFFFA) ***");
            break;
        case JFIF_JPG11:
            _log.info("*** Marker: JPG11 (JPEG Extension) (xFFFB) ***");
            break;
        case JFIF_JPG12:
            _log.info("*** Marker: JPG12 (JPEG Extension) (xFFFC) ***");
            break;
        case JFIF_JPG13:
            _log.info("*** Marker: JPG13 (JPEG Extension) (xFFFD) ***");
            break;
        case JFIF_TEM:
            _log.info("*** Marker: TEM (Temporary) (xFF01) ***");
            break;

        default:
            strTmp = QString("*** Marker: ??? (Unknown) (xFF%1) ***").arg(nCode, 2, 16, QChar('0'));
            _log.info(strTmp);
            break;
    }

    // Adjust position to account for the word used in decoding the marker!
    strTmp = QString("  OFFSET: 0x%1").arg(_pos - 2, 8, 16, QChar('0'));
    _log.info(strTmp);
}

void JfifDecode::setStatusText(const QString &msg) {
    // emit(msg, 0);
    // QCoreApplication::processEvents();
}

//-----------------------------------------------------------------------------
// Generate a special output form of the current image's
// compression signature and other characteristics. This is only
// used during development and batch import to build the MySQL repository.
void JfifDecode::outputSpecial() {
    QString strTmp;
    QString strFull;

    Q_ASSERT(m_eImgLandscape != ENUM_LANDSCAPE_UNSET);

    // This mode of operation is currently only used
    // to import the local signature database into a MySQL database
    // backend. It simply reports the MySQL commands which can be input
    // into a MySQL client application.
    if (_outputDb) {
        _log.info("*** DB OUTPUT START ***");
        _log.info("INSERT INTO `quant` (`key`, `make`, `model`, ");
        _log.info("`qual`, `subsamp`, `lum_00`, `lum_01`, `lum_02`, `lum_03`, `lum_04`, ");
        _log.info("`lum_05`, `lum_06`, `lum_07`, `chr_00`, `chr_01`, `chr_02`, ");
        _log.info("`chr_03`, `chr_04`, `chr_05`, `chr_06`, `chr_07`, `qual_lum`, `qual_chr`) VALUES (");

        strFull = "'*KEY*', ";      // key -- need to override

        // Might need to change m_strImgExifMake to be lowercase
        strTmp = QString("'%1', ").arg(m_strImgExifMake);
        strFull += strTmp;          // make

        strTmp = QString("'%1', ").arg(m_strImgExifModel);
        strFull += strTmp;          // model

        strTmp = QString("'%1', ").arg(m_strImgQualExif);
        strFull += strTmp;          // quality

        strTmp = QString("'%1', ").arg(m_strImgQuantCss);
        strFull += strTmp;          // subsampling

        _log.info(strFull);

        // Step through both quantization tables (0=lum,1=chr)
        uint32_t nMatrixInd;

        for (uint32_t nDqtInd = 0; nDqtInd < 2; nDqtInd++) {

            strFull = "";

            for (uint32_t nY = 0; nY < 8; nY++) {
                strFull += "'";

                for (uint32_t nX = 0; nX < 8; nX++) {
                    // Rotate the matrix if necessary!
                    nMatrixInd = (m_eImgLandscape != ENUM_LANDSCAPE_NO) ? (nY * 8 + nX) : (nX * 8 + nY);
                    strTmp = QString("%1").arg(m_anImgDqtTbl[nDqtInd][nMatrixInd]);
                    strFull += strTmp;

                    if (nX != 7) {
                        strFull += ",";
                    }
                }

                strFull += "', ";

                if (nY == 3) {
                    _log.info(strFull);
                    strFull = "";
                }
            }

            _log.info(strFull);
        }

        strFull = "";
        // Output quality ratings
        strTmp = QString("'%1', ").arg(m_adImgDqtQual[0]);
        strFull += strTmp;
        // Don't put out comma separator on last line!
        strTmp = QString("'%1'").arg(m_adImgDqtQual[1]);
        strFull += strTmp;
        strFull += ");";
        _log.info(strFull);

        _log.info("*** DB OUTPUT END ***");
    }
}

uint32_t JfifDecode::writeBuf(QFile &file, uint32_t startOffset, uint32_t endOffset, bool overlayEnabled) {
    if (endOffset < startOffset) return 0;

    auto size = endOffset - startOffset + 1;
    if (size > MAX_SEGMENT_SIZE) {
        size = MAX_SEGMENT_SIZE;
        _log.warn("Segment size");
    }

    const auto bufPtr = reinterpret_cast<const char *>(&_writeBuf);

    auto index = startOffset;
    const auto tmpEndOffset = startOffset + size - 1;
    while (index <= tmpEndOffset) {
        auto copyLength = tmpEndOffset - index + 1;
        if (copyLength > EXPORT_BUF_SIZE) copyLength = EXPORT_BUF_SIZE;

        for (auto tmpIndex = 0; tmpIndex < copyLength; tmpIndex++) {
            _writeBuf[tmpIndex] = getByte(index + tmpIndex, !overlayEnabled);
        }

        file.write(bufPtr, copyLength);
        index += copyLength;
    }

    return size;
}

//-----------------------------------------------------------------------------
// Generate the compression signatures (both unrotated and
// rotated) in advance of submitting to the database.
void JfifDecode::prepareSignature() {
    // Set m_strHash
    prepareSignatureSingle(false);
    // Set m_strHashRot
    prepareSignatureSingle(true);
}

// Prepare the image signature for later submission
// NOTE: ASCII vars are used (instead of unicode) to support usage of MD5 library
void JfifDecode::prepareSignatureSingle(bool bRotate) {
    QString strTmp;
    QString strSet;
    QString strHashIn;

    //  unsigned char pHashIn[2000];
    unsigned char *pHashIn;

    QString strDqt;

    MD5_CTX sMd5;

    int32_t nLenHashIn;
    uint32_t nInd;

    Q_ASSERT(m_eImgLandscape != ENUM_LANDSCAPE_UNSET);

    pHashIn = new unsigned char[2000];
    // -----------------------------------------------------------
    // Calculate the MD5 hash for online/internal database!
    // signature "00" : DQT0,DQT1,CSS
    // signature "01" : salt,DQT0,DQT1,..DQTx(if defined)

    // Build the source string
    // NOTE: For the purposes of the hash, we need to rotate the DQT tables
    // if we detect that the photo is in portrait orientation! This keeps everything consistent.

    // If no DQT tables have been defined (e.g. could have loaded text file!) then override the sig generation!
    bool bDqtDefined = false;

    for (uint32_t nSet = 0; nSet < 4; nSet++) {
        if (m_abImgDqtSet[nSet]) {
            bDqtDefined = true;
        }
    }

    if (!bDqtDefined) {
        m_strHash = "NONE";
        m_strHashRot = "NONE";
        return;
    }

    // NOTE:
    // The following MD5 code depends on an ASCII string for input
    // We are therefore using QStringA for the hash input instead
    // of the generic text functions. No special (non-ASCII)
    // characters are expected in this string.

    if (DB_SIG_VER == 0x00) {
        strHashIn = "";
    } else {
        strHashIn = "JPEGsnoop";
    }

    // Need to duplicate DQT0 if we only have one DQT table
    for (uint32_t nSet = 0; nSet < 4; nSet++) {
        if (m_abImgDqtSet[nSet]) {
            strSet = "";
            strSet = QString("*DQT%1,").arg(nSet);
            strHashIn += strSet;

            for (uint32_t i = 0; i < 64; i++) {
                nInd = (!bRotate) ? i : glb_anQuantRotate[i];
                strTmp = QString("%1,").arg(m_anImgDqtTbl[nSet][nInd], 3, 10, QChar('0'));
                strHashIn += strTmp;
            }
        }                           // if DQTx defined
    }                             // loop through sets (DQT0..DQT3)

    // Removed CSS from signature after version 0x00
    if (DB_SIG_VER == 0x00) {
        strHashIn += "*CSS,";
        strHashIn += m_strImgQuantCss;
        strHashIn += ",";
    }

    strHashIn += "*END";
    nLenHashIn = strHashIn.length();

    // Display hash input
    for (int32_t i = 0; i < nLenHashIn; i += 80) {
        strTmp = "";
        strTmp = QString("In%1: [").arg(i / 80);
        strTmp += strHashIn.mid(i, 80);
        strTmp += "]";
#ifdef DEBUG_SIG
        m_pLog->AddLine(strTmp);
#endif
    }

    // Copy into buffer
    Q_ASSERT(nLenHashIn < 2000);

    for (int32_t i = 0; i < nLenHashIn; i++) {
        pHashIn[i] = strHashIn[i].toLatin1();
    }

    // Calculate the hash
    MD5Init(&sMd5, 0);
    MD5Update(&sMd5, pHashIn, nLenHashIn);
    MD5Final(&sMd5);

    // Overwrite top 8 bits for signature version number
    sMd5.digest32[0] = (sMd5.digest32[0] & 0x00FFFFFF) + (DB_SIG_VER << 24);

    // Convert hash to string format
    // The hexadecimal string is converted to Unicode (if that is build directive)
    if (!bRotate) {
        m_strHash = QString("%1%2%3%4")
            .arg(sMd5.digest32[0], 8, 16, QChar('0'))
            .arg(sMd5.digest32[1], 8, 16, QChar('0'))
            .arg(sMd5.digest32[2], 8, 16, QChar('0'))
            .arg(sMd5.digest32[3], 8, 16, QChar('0'));
        m_strHash = m_strHash.toUpper();
    } else {
        m_strHashRot = QString("%1%2%3%4")
            .arg(sMd5.digest32[0], 8, 16, QChar('0'))
            .arg(sMd5.digest32[1], 8, 16, QChar('0'))
            .arg(sMd5.digest32[2], 8, 16, QChar('0'))
            .arg(sMd5.digest32[3], 8, 16, QChar('0'));
        m_strHashRot = m_strHashRot.toUpper();
    }

    //  QByteArray in(reinterpret_cast<char *>(pHashIn), nLenHashIn);
    //  m_strHash = QCryptographicHash::hash(reinterpret_cast<char *>(pHashIn), QCryptographicHash::Md5).toHex();
}

//-----------------------------------------------------------------------------
// Generate the compression signatures for the thumbnails
void JfifDecode::prepareSignatureThumb() {
    // Generate m_strHashThumb
    prepareSignatureThumbSingle(false);
    // Generate m_strHashThumbRot
    prepareSignatureThumbSingle(true);
}

//-----------------------------------------------------------------------------
// Prepare the image signature for later submission
// NOTE: ASCII vars are used (instead of unicode) to support usage of MD5 library
void JfifDecode::prepareSignatureThumbSingle(bool bRotate) {
    QString strTmp;
    QString strSet;
    QString strHashIn;

    unsigned char pHashIn[2000];

    QString strDqt;

    MD5_CTX sMd5;

    int32_t nLenHashIn;
    uint32_t nInd;

    // -----------------------------------------------------------
    // Calculate the MD5 hash for online/internal database!
    // signature "00" : DQT0,DQT1,CSS
    // signature "01" : salt,DQT0,DQT1

    // Build the source string
    // NOTE: For the purposes of the hash, we need to rotate the DQT tables
    // if we detect that the photo is in portrait orientation! This keeps everything
    // consistent.

    // If no DQT tables have been defined (e.g. could have loaded text file!)
    // then override the sig generation!
    bool bDqtDefined = false;

    for (uint32_t nSet = 0; nSet < 4; nSet++) {
        if (m_abImgDqtThumbSet[nSet]) {
            bDqtDefined = true;
        }
    }

    if (!bDqtDefined) {
        m_strHashThumb = "NONE";
        m_strHashThumbRot = "NONE";
        return;
    }

    if (DB_SIG_VER == 0x00) {
        strHashIn = "";
    } else {
        strHashIn = "JPEGsnoop";
    }

    //tblSelY = m_anSofQuantTblSel_Tqi[0]; // Y
    //tblSelC = m_anSofQuantTblSel_Tqi[1]; // Cb (should be same as for Cr)

    // Need to duplicate DQT0 if we only have one DQT table

    for (uint32_t nSet = 0; nSet < 4; nSet++) {
        if (m_abImgDqtThumbSet[nSet]) {
            strSet = "";
            strSet = QString("*DQT%1,").arg(nSet);
            strHashIn += strSet;

            for (uint32_t i = 0; i < 64; i++) {
                nInd = (!bRotate) ? i : glb_anQuantRotate[i];
                strTmp = QString("%1,").arg(m_anImgThumbDqt[nSet][nInd], 3, 10, QChar('0'));
                strHashIn += strTmp;
            }
        }                           // if DQTx defined
    }                             // loop through sets (DQT0..DQT3)

    // Removed CSS from signature after version 0x00
    if (DB_SIG_VER == 0x00) {
        strHashIn += "*CSS,";
        strHashIn += m_strImgQuantCss;
        strHashIn += ",";
    }

    strHashIn += "*END";
    nLenHashIn = strHashIn.length();

    //  qDebug() << "Hash" << strHashIn << strHashIn.length() << strHashIn.toLatin1();
    //  QByteArray s;
    //  s = QCryptographicHash::hash(strHashIn.toLatin1(), QCryptographicHash::Md5);
    //  qDebug() << s;

    // Display hash input
    for (int32_t i = 0; i < nLenHashIn; i += 80) {
        strTmp = "";
        strTmp = QString("In%1: [").arg(i / 80);
        strTmp += strHashIn.mid(i, 80);
        strTmp += "]";
#ifdef DEBUG_SIG
        //m_pLog->AddLine(strTmp);
#endif
    }

    // Copy into buffer
    Q_ASSERT(nLenHashIn < 2000);

    for (int32_t i = 0; i < nLenHashIn; i++) {
        pHashIn[i] = strHashIn[i].toLatin1();
    }

    // Calculate the hash
    MD5Init(&sMd5, 0);
    MD5Update(&sMd5, pHashIn, nLenHashIn);
    MD5Final(&sMd5);

    // Overwrite top 8 bits for signature version number
    sMd5.digest32[0] = (sMd5.digest32[0] & 0x00FFFFFF) + (DB_SIG_VER << 24);

    // Convert hash to string format
    if (!bRotate) {
        m_strHashThumb =
            QString("%1%2%3%4")
                .arg(sMd5.digest32[0], 8, 16, QChar('0'))
                .arg(sMd5.digest32[1], 8, 16, QChar('0'))
                .arg(sMd5.digest32[2], 8, 16, QChar('0'))
                .arg(sMd5.digest32[3], 8, 16, QChar('0'));
    } else {
        m_strHashThumbRot =
            QString("%1%2%3%4")
                .arg(sMd5.digest32[0], 8, 16, QChar('0'))
                .arg(sMd5.digest32[1], 8, 16, QChar('0'))
                .arg(sMd5.digest32[2], 8, 16, QChar('0'))
                .arg(sMd5.digest32[3], 8, 16, QChar('0'));
    }
}

//-----------------------------------------------------------------------------
// Parse the embedded JPEG thumbnail. This routine is a much-reduced
// version of the main JFIF parser, in that it focuses primarily on the
// DQT tables.
void JfifDecode::decodeEmbeddedThumb() {
    QString strTmp;
    QString strMarker;

    uint32_t nPosSaved;
    uint32_t nPosSaved_sof;
    uint32_t nPosEnd;

    bool bDone;

    uint32_t nCode;

    bool bRet;

    QString strFull;

    uint32_t nDqtPrecision_Pq;
    uint32_t nDqtQuantDestId_Tq;
    uint32_t nImgPrecision;
    uint32_t nLength;
    uint32_t nTmpVal;

    bool bScanSkipDone;
    bool bErrorAny = false;
    bool bErrorThumbLenZero = false;

    uint32_t nSkipCount;

    nPosSaved = _pos;

    // Examine the EXIF embedded thumbnail (if it exists)
    if (m_nImgExifThumbComp == 6) {
        _log.info("");
        _log.info("*** Embedded JPEG Thumbnail ***");
        strTmp = QString("  Offset: 0x%1").arg(m_nImgExifThumbOffset, 8, 16, QChar('0'));
        _log.info(strTmp);
        strTmp = QString("  Length: 0x%1 (%2)").arg(m_nImgExifThumbLen, 8, 16, QChar('0')).arg(m_nImgExifThumbLen);
        _log.info(strTmp);

        // Quick scan for DQT tables
        _pos = m_nImgExifThumbOffset;
        bDone = false;

        while (!bDone) {
            // For some reason, I have found files that have a nLength of 0
            if (m_nImgExifThumbLen != 0) {
                if ((_pos - m_nImgExifThumbOffset) > m_nImgExifThumbLen) {
                    strTmp = QString("Read more than specified EXIF thumb nLength (%1 bytes) before EOI").arg(
                        m_nImgExifThumbLen);
                    _log.error(strTmp);
                    bErrorAny = true;
                    bDone = true;
                }
            } else {
                // Don't try to process if nLength is 0!
                // Seen this in a Canon 1ds file (processed by photoshop)
                bDone = true;
                bErrorAny = true;
                bErrorThumbLenZero = true;
            }

            if ((!bDone) && (getByte(_pos++) != 0xFF)) {
                strTmp =
                    QString("Expected marker 0xFF, got 0x%1 @ offset 0x%2")
                        .arg(getByte(_pos - 1), 2, 16, QChar('0'))
                        .arg(_pos - 1, 8, 16, QChar('0'));
                _log.error(strTmp);
                bErrorAny = true;
                bDone = true;
            }

            if (!bDone) {
                nCode = getByte(_pos++);

                _log.info("");

                switch (nCode) {
                    case JFIF_SOI:       // SOI
                        _log.info("  * Embedded Thumb Marker: SOI");
                        break;

                    case JFIF_DQT:       // Define quantization tables
                        _log.info("  * Embedded Thumb Marker: DQT");

                        nLength = getByte(_pos) * 256 + getByte(_pos + 1);
                        nPosEnd = _pos + nLength;
                        _pos += 2;
                        strTmp = QString("    Length = %1").arg(nLength);
                        _log.info(strTmp);

                        while (nPosEnd > _pos) {
                            strTmp = QString("    ----");
                            _log.info(strTmp);

                            nTmpVal = getByte(_pos++);
                            nDqtPrecision_Pq = (nTmpVal & 0xF0) >> 4;
                            nDqtQuantDestId_Tq = nTmpVal & 0x0F;
                            QString strPrecision = "";

                            if (nDqtPrecision_Pq == 0) {
                                strPrecision = "8 bits";
                            } else if (nDqtPrecision_Pq == 1) {
                                strPrecision = "16 bits";
                            } else {
                                strPrecision = QString("??? unknown [value=%1]").arg(nDqtPrecision_Pq);
                            }

                            strTmp = QString("    Precision=%1").arg(strPrecision);
                            _log.info(strTmp);
                            strTmp = QString("    Destination ID=%1").arg(nDqtQuantDestId_Tq);

                            // NOTE: The mapping between destination IDs and the actual
                            // usage is defined in the SOF marker which is often later.
                            // In nearly all images, the following is true. However, I have
                            // seen some test images that set Tbl 3 = Lum, Tbl 0=Chr,
                            // Tbl1=Chr, and Tbl2 undefined
                            if (nDqtQuantDestId_Tq == 0) {
                                strTmp += " (Luminance, typically)";
                            } else if (nDqtQuantDestId_Tq == 1) {
                                strTmp += " (Chrominance, typically)";
                            } else if (nDqtQuantDestId_Tq == 2) {
                                strTmp += " (Chrominance, typically)";
                            } else {
                                strTmp += " (???)";
                            }

                            _log.info(strTmp);

                            if (nDqtQuantDestId_Tq >= 4) {
                                strTmp = QString("nDqtQuantDestId_Tq = %1, >= 4").arg(nDqtQuantDestId_Tq);
                                _log.error(strTmp);
                                bDone = true;
                                bErrorAny = true;
                                break;
                            }

                            for (uint32_t nInd = 0; nInd <= 63; nInd++) {
                                nTmpVal = getByte(_pos++);
                                m_anImgThumbDqt[nDqtQuantDestId_Tq][glb_anZigZag[nInd]] = nTmpVal;
                            }

                            m_abImgDqtThumbSet[nDqtQuantDestId_Tq] = true;

                            // Now display the table
                            for (uint32_t nY = 0; nY < 8; nY++) {
                                strFull = QString("      DQT, Row #%1: ").arg(nY);

                                for (uint32_t nX = 0; nX < 8; nX++) {
                                    strTmp = QString("%1 ").arg(m_anImgThumbDqt[nDqtQuantDestId_Tq][nY * 8 + nX], 3);
                                    strFull += strTmp;

                                    // Store the DQT entry into the Image DenCoder
                                    bRet = _imgDec.setDqtEntry(nDqtQuantDestId_Tq,
                                                               nY * 8 + nX,
                                                               glb_anUnZigZag[nY * 8 + nX],
                                                               m_anImgDqtTbl[nDqtQuantDestId_Tq][nY * 8 + nX]);
                                    decodeErrCheck(bRet);
                                }

                                _log.info(strFull);
                            }
                        }

                        break;

                    case JFIF_SOF0:
                        _log.info("  * Embedded Thumb Marker: SOF");
                        nLength = getByte(_pos) * 256 + getByte(_pos + 1);
                        nPosSaved_sof = _pos;
                        _pos += 2;
                        strTmp = QString("    Frame header length = %1").arg(nLength);
                        _log.info(strTmp);

                        nImgPrecision = getByte(_pos++);
                        strTmp = QString("    Precision = %1").arg(nImgPrecision);
                        _log.info(strTmp);

                        m_nImgThumbNumLines = getByte(_pos) * 256 + getByte(_pos + 1);
                        _pos += 2;
                        strTmp = QString("    Number of Lines = %1").arg(m_nImgThumbNumLines);
                        _log.info(strTmp);

                        m_nImgThumbSampsPerLine = getByte(_pos) * 256 + getByte(_pos + 1);
                        _pos += 2;
                        strTmp = QString("    Samples per Line = %1").arg(m_nImgThumbSampsPerLine);
                        _log.info(strTmp);
                        strTmp = QString("    Image Size = %1 x %2").arg(m_nImgThumbSampsPerLine).arg(
                            m_nImgThumbNumLines);
                        _log.info(strTmp);

                        _pos = nPosSaved_sof + nLength;

                        break;

                    case JFIF_SOS:       // SOS
                        _log.info("  * Embedded Thumb Marker: SOS");
                        _log.info("    Skipping scan data");
                        bScanSkipDone = false;
                        nSkipCount = 0;

                        while (!bScanSkipDone) {
                            if ((getByte(_pos) == 0xFF) && (getByte(_pos + 1) != 0x00)) {
                                // Was it a restart marker?
                                if ((getByte(_pos + 1) >= JFIF_RST0) && (getByte(_pos + 1) <= JFIF_RST7)) {
                                    _pos++;
                                } else {
                                    // No... it's a real marker
                                    bScanSkipDone = true;
                                }
                            } else {
                                _pos++;
                                nSkipCount++;
                            }
                        }

                        strTmp = QString("    Skipped %1 bytes").arg(nSkipCount);
                        _log.info(strTmp);
                        break;

                    case JFIF_EOI:
                        _log.info("  * Embedded Thumb Marker: EOI");
                        bDone = true;
                        break;

                    case JFIF_RST0:
                    case JFIF_RST1:
                    case JFIF_RST2:
                    case JFIF_RST3:
                    case JFIF_RST4:
                    case JFIF_RST5:
                    case JFIF_RST6:
                    case JFIF_RST7:
                        break;

                    default:
                        getMarkerName(nCode, strMarker);
                        strTmp = QString("  * Embedded Thumb Marker: %1").arg(strMarker);
                        _log.info(strTmp);
                        nLength = getByte(_pos) * 256 + getByte(_pos + 1);
                        strTmp = QString("    Length = %1").arg(nLength);
                        _log.info(strTmp);
                        _pos += nLength;
                        break;
                }
            }                         // if !bDone
        }                           // while !bDone

        // Now calculate the signature
        if (!bErrorAny) {
            prepareSignatureThumb();
            _log.info("");
            strTmp = QString("  * Embedded Thumb Signature: %1").arg(m_strHashThumb);
            _log.info(strTmp);
        }

        if (bErrorThumbLenZero) {
            m_strHashThumb = "ERR: Len=0";
            m_strHashThumbRot = "ERR: Len=0";
        }
    }                             // if JPEG compressed

    _pos = nPosSaved;
}

//-----------------------------------------------------------------------------
// Lookup the EXIF marker name from the code value
bool JfifDecode::getMarkerName(uint32_t nCode, QString &markerStr) {
    bool bDone = false;
    bool bFound = false;

    uint32_t nInd = 0;

    while (!bDone) {
        if (_markerNames[nInd].nCode == 0) {
            bDone = true;
        } else if (_markerNames[nInd].nCode == nCode) {
            bDone = true;
            bFound = true;
            markerStr = _markerNames[nInd].strName;
            return true;
        } else {
            nInd++;
        }
    }

    if (!bFound) {
        markerStr = "";
        markerStr = QString("(0xFF%1)").arg(nCode, 2, 16, QChar('0'));
        return false;
    }

    return true;
}

//-----------------------------------------------------------------------------
// Determine if the file is an AVI MJPEG.
// If so, parse the headers.
// TODO: Expand this function to use sub-functions for each block type
bool JfifDecode::decodeAvi() {
    _log.debug("JfifDecode::decodeAvi() Begin");

    QString strTmp;

    uint32_t nPosSaved;

    _avi = false;
    _aviMjpeg = false;

    // Perhaps start from file position 0?
    nPosSaved = _pos;

    // Start from file position 0
    _pos = 0;

    bool bSwap = true;

    QString strRiff;

    uint32_t nRiffLen;

    QString strForm;

    _log.debug("JfifDecode::decodeAvi() Checkpoint 1");

    strRiff = _wbuf.readStrN(_pos, 4);
    _pos += 4;
    nRiffLen = _wbuf.getDataX(_pos, 4, bSwap);
    _pos += 4;
    strForm = _wbuf.readStrN(_pos, 4);
    _pos += 4;

    _log.debug("JfifDecode::decodeAvi() Checkpoint 2");

    if ((strRiff == "RIFF") && (strForm == "AVI ")) {
        _avi = true;
        _log.info("");
        _log.info("*** AVI File Decoding ***");
        _log.info("Decoding RIFF AVI format...");
        _log.info("");
    } else {
        // Reset file position
        _pos = nPosSaved;
        return false;
    }

    QString strHeader;

    uint32_t nChunkSize;
    uint32_t nChunkDataStart;

    bool done = false;

    while (!done) {
        if (_pos >= _wbuf.fileSize()) {
            done = true;
            break;
        }

        strHeader = _wbuf.readStrN(_pos, 4);
        _pos += 4;
        strTmp = QString("  %1").arg(strHeader);
        _log.info(strTmp);

        nChunkSize = _wbuf.getDataX(_pos, 4, bSwap);
        _pos += 4;
        nChunkDataStart = _pos;

        if (strHeader == "LIST") {
            // --- LIST ---
            QString strListType;

            strListType = _wbuf.readStrN(_pos, 4);
            _pos += 4;

            strTmp = QString("    %1").arg(strListType);
            _log.info(strTmp);

            if (strListType == "hdrl") {
                // --- hdrl ---

                uint32_t nPosHdrlStart;

                QString strHdrlId;

                strHdrlId = _wbuf.readStrN(_pos, 4);
                _pos += 4;
                uint32_t nHdrlLen;

                nHdrlLen = _wbuf.getDataX(_pos, 4, bSwap);
                _pos += 4;
                nPosHdrlStart = _pos;

                // nHdrlLen should be 14*4 bytes

                _pos = nPosHdrlStart + nHdrlLen;

            } else if (strListType == "strl") {
                // --- strl ---

                // strhHEADER
                uint32_t nPosStrlStart;

                QString strStrlId;

                strStrlId = _wbuf.readStrN(_pos, 4);
                _pos += 4;
                uint32_t nStrhLen;

                nStrhLen = _wbuf.getDataX(_pos, 4, bSwap);
                _pos += 4;
                nPosStrlStart = _pos;

                QString fccType;

                QString fccHandler;

                uint32_t dwFlags, dwReserved1, dwInitialFrames, dwScale, dwRate;

                uint32_t dwStart, dwLength, dwSuggestedBufferSize, dwQuality;

                uint32_t dwSampleSize, xdwQuality, xdwSampleSize;

                fccType = _wbuf.readStrN(_pos, 4);
                _pos += 4;
                fccHandler = _wbuf.readStrN(_pos, 4);
                _pos += 4;
                dwFlags = _wbuf.getDataX(_pos, 4, bSwap);
                _pos += 4;
                dwReserved1 = _wbuf.getDataX(_pos, 4, bSwap);
                _pos += 4;
                dwInitialFrames = _wbuf.getDataX(_pos, 4, bSwap);
                _pos += 4;
                dwScale = _wbuf.getDataX(_pos, 4, bSwap);
                _pos += 4;
                dwRate = _wbuf.getDataX(_pos, 4, bSwap);
                _pos += 4;
                dwStart = _wbuf.getDataX(_pos, 4, bSwap);
                _pos += 4;
                dwLength = _wbuf.getDataX(_pos, 4, bSwap);
                _pos += 4;
                dwSuggestedBufferSize = _wbuf.getDataX(_pos, 4, bSwap);
                _pos += 4;
                dwQuality = _wbuf.getDataX(_pos, 4, bSwap);
                _pos += 4;
                dwSampleSize = _wbuf.getDataX(_pos, 4, bSwap);
                _pos += 4;
                xdwQuality = _wbuf.getDataX(_pos, 4, bSwap);
                _pos += 4;
                xdwSampleSize = _wbuf.getDataX(_pos, 4, bSwap);
                _pos += 4;

                QString fccTypeDecode = "";

                if (fccType == "vids") {
                    fccTypeDecode = "[vids] Video";
                } else if (fccType == "auds") {
                    fccTypeDecode = "[auds] Audio";
                } else if (fccType == "txts") {
                    fccTypeDecode = "[txts] Subtitle";
                } else {
                    fccTypeDecode = QString("[%1]").arg(fccType);
                }

                strTmp = QString("      -[FourCC Type]  = %1").arg(fccTypeDecode);
                _log.info(strTmp);

                strTmp = QString("      -[FourCC Codec] = [%1]").arg(fccHandler);
                _log.info(strTmp);

                double fSampleRate = 0;

                if (dwScale != 0) {
                    fSampleRate = static_cast<double>(dwRate) / static_cast<double>(dwScale);
                }

                strTmp = QString("      -[Sample Rate]  = [%.2f]").arg(fSampleRate);

                if (fccType == "vids") {
                    strTmp.append(" frames/sec");
                } else if (fccType == "auds") {
                    strTmp.append(" samples/sec");
                }

                _log.info(strTmp);

                _pos = nPosStrlStart + nStrhLen;      // Skip

                strTmp = QString("      %1").arg(fccType);
                _log.info(strTmp);

                if (fccType == "vids") {
                    // --- vids ---

                    // Is it MJPEG?
                    //strTmp = QString("      -[Video Stream FourCC]=[%s]"),fccHandler;
                    //m_pLog->AddLine(strTmp);
                    if (fccHandler == "mjpg") {
                        _aviMjpeg = true;
                    }

                    if (fccHandler == "MJPG") {
                        _aviMjpeg = true;
                    }

                    // strfHEADER_BIH
                    QString strSkipId;

                    uint32_t nSkipLen;
                    uint32_t nSkipStart;

                    strSkipId = _wbuf.readStrN(_pos, 4);
                    _pos += 4;
                    nSkipLen = _wbuf.getDataX(_pos, 4, bSwap);
                    _pos += 4;
                    nSkipStart = _pos;

                    _pos = nSkipStart + nSkipLen;       // Skip
                } else if (fccType == "auds") {
                    // --- auds ---

                    // strfHEADER_WAVE

                    QString strSkipId;

                    uint32_t nSkipLen;
                    uint32_t nSkipStart;

                    strSkipId = _wbuf.readStrN(_pos, 4);
                    _pos += 4;
                    nSkipLen = _wbuf.getDataX(_pos, 4, bSwap);
                    _pos += 4;
                    nSkipStart = _pos;

                    _pos = nSkipStart + nSkipLen;       // Skip
                } else {
                    // strfHEADER

                    QString strSkipId;

                    uint32_t nSkipLen;

                    uint32_t nSkipStart;

                    strSkipId = _wbuf.readStrN(_pos, 4);
                    _pos += 4;
                    nSkipLen = _wbuf.getDataX(_pos, 4, bSwap);
                    _pos += 4;
                    nSkipStart = _pos;

                    _pos = nSkipStart + nSkipLen;       // Skip
                }

                // strnHEADER
                uint32_t nPosStrnStart;

                QString strStrnId;

                strStrnId = _wbuf.readStrN(_pos, 4);
                _pos += 4;
                uint32_t nStrnLen;

                nStrnLen = _wbuf.getDataX(_pos, 4, bSwap);
                _pos += 4;
                nPosStrnStart = _pos;

                // FIXME: Can we rewrite in terms of ChunkSize and ChunkDataStart?
                //Q_ASSERT ((nPosStrnStart + nStrnLen + (nStrnLen%2)) == (nChunkDataStart + nChunkSize + (nChunkSize%2)));
                //m_nPos = nChunkDataStart + nChunkSize + (nChunkSize%2);
                _pos = nPosStrnStart + nStrnLen + (nStrnLen % 2);     // Skip
            } else if (strListType == "movi") {
                // movi

                _pos = nChunkDataStart + nChunkSize + (nChunkSize % 2);
            } else if (strListType == "INFO") {
                // INFO
                uint32_t nInfoStart;

                nInfoStart = _pos;

                QString strInfoId;

                uint32_t nInfoLen;

                strInfoId = _wbuf.readStrN(_pos, 4);
                _pos += 4;
                nInfoLen = _wbuf.getDataX(_pos, 4, bSwap);
                _pos += 4;

                if (strInfoId == "ISFT") {
                    QString strIsft = "";

                    strIsft = _wbuf.readStrN(_pos, nChunkSize);
                    strIsft = strIsft.trimmed();  //!! trim right
                    strTmp = QString("      -[Software] = [%1]").arg(strIsft);
                    _log.info(strTmp);
                }

                _pos = nChunkDataStart + nChunkSize + (nChunkSize % 2);
            } else {
                // ?
                _pos = nChunkDataStart + nChunkSize + (nChunkSize % 2);
            }
        } else if (strHeader == "JUNK") {
            // Junk

            _pos = nChunkDataStart + nChunkSize + (nChunkSize % 2);
        } else if (strHeader == "IDIT") {
            // Timestamp info (Canon, etc.)

            QString strIditTimestamp = "";

            strIditTimestamp = _wbuf.readStrN(_pos, nChunkSize);
            strIditTimestamp = strIditTimestamp.trimmed();    //!!
            strTmp = QString("    -[Timestamp] = [1s]").arg(strIditTimestamp);
            _log.info(strTmp);

            _pos = nChunkDataStart + nChunkSize + (nChunkSize % 2);

        } else if (strHeader == "indx") {
            // Index

            _pos = nChunkDataStart + nChunkSize + (nChunkSize % 2);
        } else if (strHeader == "idx1") {
            // Index
            //uint32_t nIdx1Entries = nChunkSize / (4*4);

            _pos = nChunkDataStart + nChunkSize + (nChunkSize % 2);
        } else {
            // Unsupported
            _pos = nChunkDataStart + nChunkSize + (nChunkSize % 2);
        }
    }

    _log.info("");

    if (_aviMjpeg) {
        m_strImgExtras += "[AVI]:[mjpg],";
        _log.info("  AVI is MotionJPEG");
        _log.warn("  Use [Tools->Img Search Fwd] to locate next frame");
    } else {
        m_strImgExtras += "[AVI]:[????],";
        _log.warn("  AVI is not MotionJPEG. [Img Search Fwd/Rev] unlikely to find frames.");
    }

    _log.info("");

    // Reset file position
    _pos = nPosSaved;

    _log.debug("JfifDecode::decodeAvi() End");

    return _aviMjpeg;
}

//-----------------------------------------------------------------------------
// This is the primary JFIF parsing routine.
// The main loop steps through all of the JFIF markers and calls
// DecodeMarker() each time until we reach the end of file or an error.
// Finally, we invoke the compression signature search function.
//
// Processing starts at the file offset m_pAppConfig->startPos()
//
// INPUT:
// - inFile                                             = Input file pointer
//
// PRE:
// - m_pAppConfig->startPos()    = Starting file offset for decode
//
void JfifDecode::processFile(uint32_t position) {
    // Reset the JFIF decoder state as we may be redoing another file
    reset();

    // Reset the IMG Decoder state
    if (_imgSrcDirty) {
        _imgDec.resetState();
    }

    // Ensure the status bar has been allocated
    // NOTE: The stat bar is NULL if we drag & drop a file onto
    //       the JPEGsnoop app icon.

    setStatusText("Processing...");

    // Note that we don't clear out the logger (with m_pLog->Reset())
    // as we want top-level caller to do this. This way we can
    // still insert extra lines from top level.

    // GetLength returns ULONGLONG. Abort on large files (>=4GB)
    if (_wbuf.fileSize() > 0xFFFFFFFF) {
        _log.error("File too large. Skipping.");
        return;
    }

    _posFileEnd = static_cast<uint32_t>(_wbuf.fileSize());

    auto startPos = position;
    _pos = startPos;
    _posEmbedStart = startPos; // Save the embedded file start position

    _log.info(QString("Start Offset: 0x%1").arg(startPos, 8, 16, QChar('0')));

    // ----------------------------------------------------------------
    // Test for AVI file
    // - Detect header
    // - start from beginning of file
    // decodeAvi();
    // TODO: Should we skip decode of file if not MJPEG?
    // ----------------------------------------------------------------

    // Decode as JPEG JFIF file

    // If we are in a non-zero offset, add this to extras
    // if (_appConfig.startPos() != 0) {
    //     strTmp = QString("[Offset]=[%1],").arg(_appConfig.startPos());
    //     m_strImgExtras += strTmp;
    // }

    uint32_t dataAfterEof = 0;

    auto done = false;
    while (!done) {
        // Allow some other threads to jump in

        // Return value 0 - OK
        //              1 - Error
        //              2 - EOI
        if (decodeMarker() != DECMARK_OK) {
            done = true;

            if (_posFileEnd >= _posEoi) {
                dataAfterEof = _posFileEnd - _posEoi;
            }
        } else {
            if (_pos > _wbuf.fileSize()) {
                _log.error("Early EOF - file may be missing EOI");
                done = true;
            }
        }
    }

    // -----------------------------------------------------------
    // Perform any other informational calculations that require all tables
    // to be present.

    // Determine the CSS Ratio
    // Save the subsampling string. Assume component 2 is representative of the overall chrominance.

    // NOTE: Ensure that we don't execute the following code if we haven't
    //       completed our read (ie. get bad marker earlier in processing).
    // TODO: What is the best way to determine all is OK?

    m_strImgQuantCss = "?x?";
    m_strHash = "NONE";
    m_strHashRot = "NONE";

    if (_imgOk) {
        Q_ASSERT(m_eImgLandscape != ENUM_LANDSCAPE_UNSET);

        if (m_nSofNumComps_Nf == NUM_CHAN_YCC) {
            // We only try to determine the chroma subsampling ratio if we have 3 components (assume YCC)
            // In general, we should be able to use the 2nd or 3rd component

            // NOTE: The following assumes m_anSofHorzSampFact_Hi and m_anSofVertSampFact_Vi
            // are non-zero as otherwise we'll have a divide-by-0 exception.
            uint32_t nCompIdent = m_anSofQuantCompId[SCAN_COMP_CB];
            uint32_t nCssFactH = m_nSofHorzSampFactMax_Hmax / m_anSofHorzSampFact_Hi[nCompIdent];
            uint32_t nCssFactV = m_nSofVertSampFactMax_Vmax / m_anSofVertSampFact_Vi[nCompIdent];

            if (m_eImgLandscape != ENUM_LANDSCAPE_NO) {
                // Landscape orientation
                m_strImgQuantCss = QString("%1x%2").arg(nCssFactH).arg(nCssFactV);
            } else {
                // Portrait orientation (flip subsampling ratio)
                m_strImgQuantCss = QString("%1x%1").arg(nCssFactV).arg(nCssFactH);
            }
        } else if (m_nSofNumComps_Nf == NUM_CHAN_GRAYSCALE) {
            m_strImgQuantCss = "Gray";
        }

        decodeEmbeddedThumb();

        // Generate the signature
        prepareSignature();

        // Compare compression signature
        if (_appConfig.searchSig()) {
            // In the case of lossless files, there won't be any DQT and
            // hence no compression signatures to compare. Therefore, skip this process.
            if (m_strHash == "NONE") {
                _log.warn("Skipping compression signature search as no DQT");
            } else {
                // compareSignature();
            }
        }

        if (dataAfterEof > 0) {
            _log.info("");
            _log.info("*** Additional Info ***");
            _log.info(QString("Data exists after EOF, range: 0x%1-0x%2 (%3 bytes)")
                          .arg(_posEoi, 8, 16, QChar('0'))
                          .arg(_posFileEnd, 8, 16, QChar('0'))
                          .arg(dataAfterEof));
        }

        // Print out the special-purpose outputs
        outputSpecial();
    }

    // Reset the status bar text
    setStatusText("Done");

    // Mark the file as closed
    //m_pWBuf->BufFileUnset();
}

//-----------------------------------------------------------------------------
// Determine if the analyzed file is in a state ready for image
// extraction. Confirms that the important JFIF markers have been
// detected in the previous analysis.
//
// PRE:
// - m_nPosEmbedStart
// - m_nPosEmbedEnd
// - m_nPosFileEnd
//
// RETURN:
// - True if image is ready for extraction
//
bool JfifDecode::exportJpegPrepare(bool forceSoi, bool forceEoi, bool ignoreEoi) {
    // Extract from current file
    //   [m_nPosEmbedStart ... m_nPosEmbedEnd]
    // If state is valid (i.e. file opened)

    _log.info("");
    _log.info("*** Exporting JPEG ***");

    if (!_stateEoi) {
        if (!forceEoi && !ignoreEoi) {
            _log.error(QString("Missing marker: %1").arg("EOI"));
            _log.error("Aborting export. Consider enabling [Force EOI] or [Ignore Missing EOI] option");
            return false;
        } else if (ignoreEoi) {
            _posEmbedEnd = _posFileEnd;
        }
    }

    if ((_posEmbedStart == 0) && (_posEmbedEnd == 0)) {
        _log.error("No frame found at this position in file. Consider using [Img Search]");
        return false;
    }

    if (!_stateSoi) {
        if (!forceSoi) {
            _log.error(QString("Missing marker: %1").arg("SOI"));
            _log.error("Aborting export. Consider enabling [Force SOI] option");
            return false;
        } else {
            // We're missing the SOI but the user has requested
            // that we force an SOI, so let's fix things up
        }
    }

    if (!_stateSos) {
        _log.error(QString("Missing marker: %1").arg("SOS"));
        _log.error("Aborting export");
        return false;
    }

    QString missing;
    if (!_stateDqt) missing += "DQT ";
    if (!_stateDht) missing += "DHT ";
    if (!_stateSof) missing += "SOF ";

    if (!missing.isEmpty()) {
        _log.warn(QString("Missing marker: %1").arg(missing));
        _log.warn("Exported JPEG may not be valid");
    }

    if (_posEmbedEnd < _posEmbedStart) {
        _log.error("Invalid SOI-EOI order. Export aborted.");
        return false;
    }

    return true;
}

// Export the embedded JPEG image at the current position in the file (with overlays)
// (maybe the primary image or even an embedded thumbnail).
bool JfifDecode::exportJpegDo(const QString &outFilePath, bool overlayEnabled, bool dhtAviInsert, bool forceSoi, bool forceEoi) {
    _log.info(QString("Exporting to: [%1]").arg(outFilePath));

    // Open specified file
    // Added in shareDenyNone as this apparently helps resolve some people's troubles
    // with an error showing: Couldn't open file "Sharing Violation"
    QFile outFile(outFilePath);
    if (!outFile.open(QIODevice::WriteOnly)) {
        _log.error(QString("Couldn't open file for write [%1]: [%2]").arg(outFilePath, outFile.errorString()));
        return false;
    }

    // Need to insert fake DHT. Assume we have enough buffer allocated.
    //
    // Step 1: Copy from SOI -> SOS (not incl)
    // Step 2: Insert Fake DHT
    // Step 3: Copy from SOS -> EOI

    // Step 1

    // If we need to force an SOI, do it now
    if (!_stateSoi && forceSoi) {
        _log.info("Forcing SOI Marker");

        uint8_t anBufSoi[2] = { 0xFF, JFIF_SOI };
        outFile.write(reinterpret_cast<const char *>(&anBufSoi), 2);
    }

    writeBuf(outFile, _posEmbedStart, _posSos - 1, overlayEnabled);

    if (dhtAviInsert) {
        // Step 2. The following struct includes the JFIF marker too
        _log.info("Inserting standard AVI DHT huffman table");
        outFile.write(reinterpret_cast<const char *>(&_motionJpegDhtSeg), JFIF_DHT_FAKE_SZ);
    }

    // Step 3
    writeBuf(outFile, _posSos, _posEmbedEnd, overlayEnabled);

    // Now optionally insert the EOI Marker
    if (forceEoi) {
        _log.info("Forcing EOI Marker");

        uint8_t anBufEoi[2] = { 0xFF, JFIF_EOI };
        outFile.write(reinterpret_cast<const char *>(&anBufEoi), 2);
    }

    outFile.close();

    _log.info("Export done");

    return true;
}

// Export a subset of the file with no overlays or mods
bool JfifDecode::exportJpegDoRange(const QString &strFileIn, QString strFileOut, uint32_t nStart, uint32_t nEnd) {
    QFile *pFileOutput;

    QString strTmp = "";

    strTmp = QString("  Exporting range to:   [%1]").arg(strFileOut);
    _log.info(strTmp);

    if (strFileIn == strFileOut) {
        _log.error("Can't overwrite source file. Aborting export.");

        return false;
    }

    Q_ASSERT(strFileIn != "");

    if (strFileIn == "") {
        _log.error("Export but source filename empty");

        return false;
    }

    pFileOutput = new QFile(strFileOut);

    if ((pFileOutput->open(QIODevice::WriteOnly)) == false) {
        QString strError;

        strError = QString("Couldn't open file for write [%1]: [%2]").arg(strFileOut).arg(pFileOutput->errorString());
        _log.error(strError);

        pFileOutput = nullptr;

        return false;
    }

    uint32_t nCopyStart;
    uint32_t nCopyEnd;
    uint32_t nCopyLeft;
    uint32_t ind;

    uint8_t *pBuf;

    pBuf = new uint8_t[EXPORT_BUF_SIZE + 10];

    if (!pBuf) {
        if (pFileOutput) {
            delete pFileOutput;
            pFileOutput = nullptr;
        }

        return false;
    }

    // Step 1
    nCopyStart = nStart;
    nCopyEnd = nEnd;
    ind = nCopyStart;

    while (ind < nCopyEnd) {
        nCopyLeft = nCopyEnd - ind + 1;

        if (nCopyLeft > EXPORT_BUF_SIZE) {
            nCopyLeft = EXPORT_BUF_SIZE;
        }

        for (uint32_t ind1 = 0; ind1 < nCopyLeft; ind1++) {
            pBuf[ind1] = getByte(ind + ind1, false);
        }

        pFileOutput->write(reinterpret_cast<char *>(pBuf), nCopyLeft);
        ind += nCopyLeft;
        strTmp = QString("Exporting %1%%...").arg(ind * 100 / (nCopyEnd - nCopyStart), 3);
        setStatusText(strTmp);
    }

    // Free up space
    pFileOutput->close();

    if (pBuf) {
        delete[]pBuf;
        pBuf = nullptr;
    }

    if (pFileOutput) {
        delete pFileOutput;
        pFileOutput = nullptr;
    }

    setStatusText("");
    strTmp = "  Export range done";
    _log.info(strTmp);

    return true;
}

// ====================================================================================
// JFIF Decoder Constants
// ====================================================================================

// List of the JFIF markers
const MarkerNameTable JfifDecode::_markerNames[] = {
    { JFIF_SOF0,  "SOF0" },
    { JFIF_SOF1,  "SOF1" },
    { JFIF_SOF2,  "SOF2" },
    { JFIF_SOF3,  "SOF3" },
    { JFIF_SOF5,  "SOF5" },
    { JFIF_SOF6,  "SOF6" },
    { JFIF_SOF7,  "SOF7" },
    { JFIF_JPG,   "JPG" },
    { JFIF_SOF9,  "SOF9" },
    { JFIF_SOF10, "SOF10" },
    { JFIF_SOF11, "SOF11" },
    { JFIF_SOF13, "SOF13" },
    { JFIF_SOF14, "SOF14" },
    { JFIF_SOF15, "SOF15" },
    { JFIF_DHT,   "DHT" },
    { JFIF_DAC,   "DAC" },
    { JFIF_RST0,  "RST0" },
    { JFIF_RST1,  "RST1" },
    { JFIF_RST2,  "RST2" },
    { JFIF_RST3,  "RST3" },
    { JFIF_RST4,  "RST4" },
    { JFIF_RST5,  "RST5" },
    { JFIF_RST6,  "RST6" },
    { JFIF_RST7,  "RST7" },
    { JFIF_SOI,   "SOI" },
    { JFIF_EOI,   "EOI" },
    { JFIF_SOS,   "SOS" },
    { JFIF_DQT,   "DQT" },
    { JFIF_DNL,   "DNL" },
    { JFIF_DRI,   "DRI" },
    { JFIF_DHP,   "DHP" },
    { JFIF_EXP,   "EXP" },
    { JFIF_APP0,  "APP0" },
    { JFIF_APP1,  "APP1" },
    { JFIF_APP2,  "APP2" },
    { JFIF_APP3,  "APP3" },
    { JFIF_APP4,  "APP4" },
    { JFIF_APP5,  "APP5" },
    { JFIF_APP6,  "APP6" },
    { JFIF_APP7,  "APP7" },
    { JFIF_APP8,  "APP8" },
    { JFIF_APP9,  "APP9" },
    { JFIF_APP10, "APP10" },
    { JFIF_APP11, "APP11" },
    { JFIF_APP12, "APP12" },
    { JFIF_APP13, "APP13" },
    { JFIF_APP14, "APP14" },
    { JFIF_APP15, "APP15" },
    { JFIF_JPG0,  "JPG0" },
    { JFIF_JPG1,  "JPG1" },
    { JFIF_JPG2,  "JPG2" },
    { JFIF_JPG3,  "JPG3" },
    { JFIF_JPG4,  "JPG4" },
    { JFIF_JPG5,  "JPG5" },
    { JFIF_JPG6,  "JPG6" },
    { JFIF_JPG7,  "JPG7" },
    { JFIF_JPG8,  "JPG8" },
    { JFIF_JPG9,  "JPG9" },
    { JFIF_JPG10, "JPG10" },
    { JFIF_JPG11, "JPG11" },
    { JFIF_JPG12, "JPG12" },
    { JFIF_JPG13, "JPG13" },
    { JFIF_COM,   "COM" },
    { JFIF_TEM,   "TEM" },
    //{JFIF_RES*,"RES"},
    { 0x00,       "*" },
};

// For Motion JPEG, define the DHT tables that we use since they won't exist
// in each frame within the AVI. This table will be read in during
// DecodeDHT()'s call to Buf().
const quint8 JfifDecode::_motionJpegDhtSeg[JFIF_DHT_FAKE_SZ] = {
    /* JPEG DHT Segment for YCrCb omitted from MJPG data */
    0xFF, 0xC4, 0x01, 0xA2,
    0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x01, 0x00, 0x03, 0x01, 0x01, 0x01, 0x01,
    0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B, 0x10, 0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04, 0x04, 0x00,
    0x00, 0x01, 0x7D, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61,
    0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08, 0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52, 0xD1, 0xF0, 0x24,
    0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x34,
    0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56,
    0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
    0x79, 0x7A, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
    0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9,
    0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9,
    0xDA, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7,
    0xF8, 0xF9, 0xFA, 0x11, 0x00, 0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04, 0x04, 0x00, 0x01,
    0x02, 0x77, 0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
    0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33, 0x52, 0xF0, 0x15, 0x62,
    0x72, 0xD1, 0x0A, 0x16, 0x24, 0x34, 0xE1, 0x25, 0xF1, 0x17, 0x18, 0x19, 0x1A, 0x26, 0x27, 0x28, 0x29, 0x2A,
    0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56,
    0x57, 0x58, 0x59, 0x5A, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
    0x79, 0x7A, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
    0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8,
    0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8,
    0xD9, 0xDA, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8,
    0xF9, 0xFA
};

// TODO: Add ITU-T Example DQT & DHT
//       These will be useful for GeoRaster decode (ie. JPEG-B)

QString glb_strMsgStopDecode = "  Stopping decode. Use [Relaxed Parsing] to continue.";
