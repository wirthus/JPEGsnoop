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
// - This module decodes the JPEG JFIF marker segments
// - Decoding the scan segment (SOS) is not handled here as that is
//   done in the CimgDecode class.
//
// ==========================================================================

#pragma once

#ifndef JPEGSNOOP_JFIFDECODE_H
#define JPEGSNOOP_JFIFDECODE_H

#include <QObject>
#include <QString>

#include <memory>

// #include "DbSigs.h"
#include "DecodePs.h"
#include "ImgDecode.h"
#include "SnoopConfig.h"
#include "WindowBuf.h"
#include "log/ILog.h"

static constexpr uint32_t EXPORT_BUF_SIZE = 128 * 1024;

static const int32_t MAX_IFD_COMPS = 150;        // Maximum number of IFD entry components to display

static const uint32_t JFIF_SOF0 = 0xC0;
static const uint32_t JFIF_SOF1 = 0xC1;
static const uint32_t JFIF_SOF2 = 0xC2;
static const uint32_t JFIF_SOF3 = 0xC3;
static const uint32_t JFIF_SOF5 = 0xC5;
static const uint32_t JFIF_SOF6 = 0xC6;
static const uint32_t JFIF_SOF7 = 0xC7;
static const uint32_t JFIF_JPG = 0xC8;
static const uint32_t JFIF_SOF9 = 0xC9;
static const uint32_t JFIF_SOF10 = 0xCA;
static const uint32_t JFIF_SOF11 = 0xCB;
static const uint32_t JFIF_SOF13 = 0xCD;
static const uint32_t JFIF_SOF14 = 0xCE;
static const uint32_t JFIF_SOF15 = 0xCF;
static const uint32_t JFIF_DHT = 0xC4;
static const uint32_t JFIF_DAC = 0xCC;
//static const uint32_t JFIF_RST0 = 0xD0;
//static const uint32_t JFIF_RST1 = 0xD1;
//static const uint32_t JFIF_RST2 = 0xD2;
//static const uint32_t JFIF_RST3 = 0xD3;
//static const uint32_t JFIF_RST4 = 0xD4;
//static const uint32_t JFIF_RST5 = 0xD5;
//static const uint32_t JFIF_RST6 = 0xD6;
//static const uint32_t JFIF_RST7 = 0xD7;
static const uint32_t JFIF_SOI = 0xD8;
//static const uint32_t JFIF_EOI = 0xD9;
static const uint32_t JFIF_SOS = 0xDA;
static const uint32_t JFIF_DQT = 0xDB;
static const uint32_t JFIF_DNL = 0xDC;
static const uint32_t JFIF_DRI = 0xDD;
static const uint32_t JFIF_DHP = 0xDE;
static const uint32_t JFIF_EXP = 0xDF;
static const uint32_t JFIF_APP0 = 0xE0;
static const uint32_t JFIF_APP1 = 0xE1;
static const uint32_t JFIF_APP2 = 0xE2;
static const uint32_t JFIF_APP3 = 0xE3;
static const uint32_t JFIF_APP4 = 0xE4;
static const uint32_t JFIF_APP5 = 0xE5;
static const uint32_t JFIF_APP6 = 0xE6;
static const uint32_t JFIF_APP7 = 0xE7;
static const uint32_t JFIF_APP8 = 0xE8;
static const uint32_t JFIF_APP9 = 0xE9;
static const uint32_t JFIF_APP10 = 0xEA;
static const uint32_t JFIF_APP11 = 0xEB;
static const uint32_t JFIF_APP12 = 0xEC;
static const uint32_t JFIF_APP13 = 0xED;
static const uint32_t JFIF_APP14 = 0xEE;
static const uint32_t JFIF_APP15 = 0xEF;
static const uint32_t JFIF_JPG0 = 0xF0;
static const uint32_t JFIF_JPG1 = 0xF1;
static const uint32_t JFIF_JPG2 = 0xF2;
static const uint32_t JFIF_JPG3 = 0xF3;
static const uint32_t JFIF_JPG4 = 0xF4;
static const uint32_t JFIF_JPG5 = 0xF5;
static const uint32_t JFIF_JPG6 = 0xF6;
static const uint32_t JFIF_JPG7 = 0xF7;
static const uint32_t JFIF_JPG8 = 0xF8;
static const uint32_t JFIF_JPG9 = 0xF9;
static const uint32_t JFIF_JPG10 = 0xFA;
static const uint32_t JFIF_JPG11 = 0xFB;
static const uint32_t JFIF_JPG12 = 0xFC;
static const uint32_t JFIF_JPG13 = 0xFD;
static const uint32_t JFIF_COM = 0xFE;
static const uint32_t JFIF_TEM = 0x01;
static const uint32_t JFIF_DHT_FAKE = 0x999999C4;
static const uint32_t JFIF_DHT_FAKE_SZ = 0x1A4;

#define APP14_COLXFM_UNSET    -1
#define APP14_COLXFM_UNK_RGB   0
#define APP14_COLXFM_YCC       1
#define APP14_COLXFM_YCCK      2

static const int32_t MAX_IDENTIFIER = 256;       // Max length for identifier strings (include terminator)

struct CStr2 {
    QString strTag;
    QString strVal;
    bool bUnknown;                // Tag is not known
};

struct MarkerNameTable {
    uint32_t nCode;
    QString strName;
};

class JfifDecode final {
    Q_DISABLE_COPY(JfifDecode)
public:
    JfifDecode(ILog &log, WindowBuf &buf, ImgDecode &imgDec, SnoopConfig &appConfig);
    ~JfifDecode();

    void reset();

    // Public accesssor & mutator functions
    void getAviMode(bool &isAvi, bool &isMjpeg) const;
    void setAviMode(bool isAvi, bool isMjpeg);
    uint32_t getPosEmbedStart() const;
    uint32_t getPosEmbedEnd() const;
    void getDecodeSummary(QString &strHash, QString &strHashRot, QString &strImgExifMake, QString &strImgExifModel, QString &strImgQualExif, QString &strSoftware, teDbAdd &eDbReqSuggest);
    uint32_t getDqtZigZagIndex(uint32_t nInd, bool bZigZag);
    uint32_t getDqtQuantStd(uint32_t nInd);

    bool getDecodeStatus() const;

    // void ExportRangeSet(uint32_t nStart, uint32_t nEnd);
    bool exportJpegPrepare(bool forceSoi, bool forceEoi, bool ignoreEoi);
    bool exportJpegDo(const QString &outFilePath, bool overlayEnabled, bool dhtAviInsert, bool forceSoi, bool forceEoi);

    void processFile(uint32_t position);

private:
    uint32_t writeBuf(QFile &file, uint32_t startOffset, uint32_t endOffset, bool overlayEnabled);

    // Display routines
    void dbgAddLine(const QString &strLine);
    void addHeader(uint32_t code);
    QString printAsHexUc(uint8_t *anBytes, uint32_t nCount);
    QString printAsHex8(uint32_t *anBytes, uint32_t nCount);
    QString printAsHex32(uint32_t *anWords, uint32_t nCount);

    // Buffer access
    quint8 getByte(uint32_t nOffset, bool bClean);
    void unByteSwap4(uint32_t nVal, uint32_t &nByte0, uint32_t &nByte1, uint32_t &nByte2, uint32_t &nByte3);
    uint32_t byteSwap4(uint32_t nByte0, uint32_t nByte1, uint32_t nByte2, uint32_t nByte3);
    uint32_t byteSwap2(uint32_t nByte0, uint32_t nByte1);
    uint32_t readSwap2(uint32_t nPos);
    uint32_t readSwap4(uint32_t nPos);
    uint32_t readBe4(uint32_t nPos);

    uint32_t decodeMarker();
    bool expectMarkerEnd(uint32_t nMarkerStart, uint32_t nMarkerLen);
    void decodeEmbeddedThumb();
    bool decodeAvi();

    bool validateValue(uint32_t &nVal, uint32_t nMin, uint32_t nMax, const QString& strName, bool bOverride, uint32_t nOverrideVal);

    // Marker specific parsing
    static bool getMarkerName(uint32_t code, QString &marker);
    uint32_t decodeExifIfd(const QString &strIfd, uint32_t nPosExifStart, uint32_t nStartIfdPtr);
    // uint32_t DecodeMakerIfd(uint32_t ifd_tag,uint32_t ptr,uint32_t len);
    bool decodeMakerSubType();
    void decodeDht(bool bInject);
    uint32_t decodeApp13Ps();
    uint32_t decodeApp2FlashPix();
    uint32_t decodeApp2IccProfile(uint32_t nLen);
    uint32_t decodeIccHeader(uint32_t nPos);

    // DQT / DHT
    void clearDqt();
    void setDqtQuick(uint16_t anDqt0[], uint16_t anDqt1[]);
    void genLookupHuffMask();

    // Field parsing
    bool decodeValRational(uint32_t nPos, double &nVal);
    QString decodeValFraction(uint32_t nPos);
    bool decodeValGps(uint32_t nPos, QString &strCoord);
    bool printValGps(uint32_t nCount, double fCoord1, double fCoord2, double fCoord3, QString &coord);
    QString decodeIccDateTime(uint32_t anVal[3]);
    QString lookupExifTag(const QString &strSect, uint32_t nTag, bool &bUnknown);
    CStr2 lookupMakerCanonTag(uint32_t nMainTag, uint32_t nSubTag, uint32_t nVal);

    void decodeErrCheck(bool bRet);

    ILog &_log;
    // DbSigs &_dbSigs;
    WindowBuf &_wbuf;
    ImgDecode &_imgDec;
    SnoopConfig &_appConfig;
    std::unique_ptr<DecodePs> _psDec;

    uint8_t _writeBuf[EXPORT_BUF_SIZE];
    bool _verbose;
    bool _bufFakeDht;           // Flag to redirect DHT read to AVI DHT over Buffer content

    // Constants
    static const uint8_t _motionJpegDhtSeg[JFIF_DHT_FAKE_SZ]; // Motion JPEG DHT
    static const MarkerNameTable _markerNames[];

    // Status
    bool _imgOk;                // Img decode encounter SOF
    bool _avi;                  // Is it an AVI file?
    bool _aviMjpeg;             // Is it a MotionJPEG AVI file?
    bool _psd;                  // Is it a Photoshop file?

    bool _imgSrcDirty;          // Do we need to recalculate the scan decode?

    // File position records
    uint32_t _pos;           // Current file/buffer position
    uint32_t _posEoi;        // Position of EOI (0xFFD9) marker
    uint32_t _posSos;
    uint32_t _posEmbedStart; // Embedded/offset start
    uint32_t _posEmbedEnd;   // Embedded/offset end
    uint32_t _posFileEnd;    // End of file position

    // Decoder state
    char _app0Identifier[MAX_IDENTIFIER];      // APP0 type: JFIF, AVI1, etc.

    double m_afStdQuantLumCompare[64];
    double m_afStdQuantChrCompare[64];

    uint32_t m_anMaskLookup[32];

    uint32_t m_nImgVersionMajor;
    uint32_t m_nImgVersionMinor;
    uint32_t m_nImgUnits;
    uint32_t m_nImgDensityX;
    uint32_t m_nImgDensityY;
    uint32_t m_nImgThumbSizeX;
    uint32_t m_nImgThumbSizeY;

    bool m_bImgProgressive;       // Progressive scan?
    bool m_bImgSofUnsupported;    // SOF mode unsupported - skip SOI content

    QString m_strComment;         // Comment string

    uint32_t m_nSosNumCompScan_Ns;
    uint32_t m_nSosSpectralStart_Ss;
    uint32_t m_nSosSpectralEnd_Se;
    uint32_t m_nSosSuccApprox_A;

    bool m_nImgRstEn;             // DRI seen
    uint32_t m_nImgRstInterval;

    uint16_t m_anImgDqtTbl[MAX_DQT_DEST_ID][MAX_DQT_COEFF];
    double m_adImgDqtQual[MAX_DQT_DEST_ID];
    bool m_abImgDqtSet[MAX_DQT_DEST_ID];  // Has this table been configured?
    uint32_t m_anDhtNumCodesLen_Li[17];

    uint32_t m_nSofPrecision_P;
    uint32_t m_nSofNumLines_Y;
    uint32_t m_nSofSampsPerLine_X;
    uint32_t m_nSofNumComps_Nf;   // Number of components in frame (might not equal m_nSosNumCompScan_Ns)

    // Define Quantization table details for the indexed Component Identifier
    // - Component identifier (SOF:Ci) has a range of 0..255
    uint32_t m_anSofQuantCompId[MAX_SOF_COMP_NF]; // SOF:Ci, index is i-1
    uint32_t m_anSofQuantTblSel_Tqi[MAX_SOF_COMP_NF];
    uint32_t m_anSofHorzSampFact_Hi[MAX_SOF_COMP_NF];
    uint32_t m_anSofVertSampFact_Vi[MAX_SOF_COMP_NF];
    uint32_t m_nSofHorzSampFactMax_Hmax;
    uint32_t m_nSofVertSampFactMax_Vmax;

    // FIXME: Move to CDecodePs
    uint32_t m_nImgQualPhotoshopSa;
    uint32_t m_nImgQualPhotoshopSfw;

    int m_nApp14ColTransform;     // Color transform from JFIF APP14 Adobe marker (-1 if not set)

    teLandscape m_eImgLandscape;  // Landscape vs Portrait
    QString m_strImgQuantCss;     // Chroma subsampling (e.g. "2x1")

    uint32_t m_nImgExifEndian;    // 0=Intel 1=Motorola
    uint32_t m_nImgExifSubIfdPtr;
    uint32_t m_nImgExifGpsIfdPtr;
    uint32_t m_nImgExifInteropIfdPtr;
    uint32_t m_nImgExifMakerPtr;

    bool m_bImgExifMakeSupported; // Mark makes that we support decode for
    uint32_t m_nImgExifMakeSubtype;       // Used for Nikon (e.g. type3)

    QString m_strImgExtras;       // Extra strings used for DB submission

    // Embedded EXIF Thumbnail
    uint32_t m_nImgExifThumbComp;
    uint32_t m_nImgExifThumbOffset;
    uint32_t m_nImgExifThumbLen;
    uint32_t m_anImgThumbDqt[4][64];
    bool m_abImgDqtThumbSet[4];
    QString m_strHashThumb;
    QString m_strHashThumbRot;
    uint32_t m_nImgThumbNumLines;
    uint32_t m_nImgThumbSampsPerLine;

    // State of decoder -- have we seen each marker?
    bool _stateAbort;           // Do we abort decoding? (eg. user hits cancel after errs)

    bool _stateSoi;
    bool _stateDht;
    bool _stateDhtOk;
    bool _stateDhtFake;         // Fake DHT required for AVI
    bool _stateDqt;
    bool _stateDqtOk;
    bool _stateSof;
    bool _stateSofOk;
    bool _stateSos;
    bool _stateSosOk;
    bool _stateEoi;

    teDbAdd m_eDbReqSuggest;
    QString m_strHash;
    QString m_strHashRot;
    QString m_strImgExifMake;
    QString m_strImgExifModel;
    QString m_strImgQualExif;     // Quality (e.g. "fine") from makernotes
    QString m_strSoftware;        // EXIF Software field
    bool m_bImgExifMakernotes;    // Are any Makernotes present?
};

#endif
