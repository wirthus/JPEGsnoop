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

#include "ImgDecode.h"

#include <cmath>

#include "SnoopConfig.h"

// ------------------------------------------------------
// Settings

// Flag: Use fixed point arithmetic for IDCT?
//#define IDCT_FIXEDPT

// Flag: Do we stop during scan decode if 0xFF (but not pad)?
// TODO: Make this a config option
//#define SCAN_BAD_MARKER_STOP

// ------------------------------------------------------
// Main code

// Constructor for the Image Decoder
// - This constructor is called only once by Document class
ImgDecode::ImgDecode(ILog &log, WindowBuf &wbuf, SnoopConfig &appConfig) :
    _log(log),
    _wbuf(wbuf),
    _appConfig(appConfig) {

    _verbose = false;

    m_pMcuFileMap = nullptr;
    m_pBlkDcValY = nullptr;
    m_pBlkDcValCb = nullptr;
    m_pBlkDcValCr = nullptr;
    m_pPixValY = nullptr;
    m_pPixValCb = nullptr;
    m_pPixValCr = nullptr;

    // Reset the image decoding state
    reset();

    m_nImgSizeX = 0;
    m_nImgSizeY = 0;

    // FIXME: Temporary hack to avoid divide-by-0 when displaying PSD (instead of JPEG)
    m_nMcuWidth = 1;
    m_nMcuHeight = 1;

    // Detailed VLC Decode mode
    m_bDetailVlc = false;
    m_nDetailVlcX = 0;
    m_nDetailVlcY = 0;
    m_nDetailVlcLen = 1;

    // Set up the IDCT lookup tables
    PrecalcIdct();

    GenLookupHuffMask();

    // The following contain information that is set by
    // the JFIF Decoder. We can only reset them here during
    // the constructor and later by explicit call by JFIF Decoder.
    resetState();

    _decodeScanAc = true;
}

// Destructor for Image Decode class
// - Deallocate any image-related dynamic storage
ImgDecode::~ImgDecode() {
    deleteAndNullBlk(m_pMcuFileMap);
    deleteAndNullBlk(m_pBlkDcValY);
    deleteAndNullBlk(m_pBlkDcValCb);
    deleteAndNullBlk(m_pBlkDcValCr);
    deleteAndNullBlk(m_pPixValY);
    deleteAndNullBlk(m_pPixValCb);
    deleteAndNullBlk(m_pPixValCr);
}

// Reset decoding state for start of new decode
// Note that we don't touch the DQT or DHT entries as
// those are set at different times versus reset (sometimes
// before Reset() ).
void ImgDecode::reset() {
    _log.debug("ImgDecode::reset() Start");

    DecodeRestartScanBuf(0, false);
    DecodeRestartDcState();

    m_bRestartRead = false;       // No restarts seen yet
    m_nRestartRead = 0;

    m_nImgSizeX = 0;
    m_nImgSizeY = 0;
    m_nMcuXMax = 0;
    m_nMcuYMax = 0;
    m_nBlkXMax = 0;
    m_nBlkYMax = 0;

    deleteAndNullBlk(m_pMcuFileMap);
    deleteAndNullBlk(m_pBlkDcValY);
    deleteAndNullBlk(m_pBlkDcValCb);
    deleteAndNullBlk(m_pBlkDcValCr);
    deleteAndNullBlk(m_pPixValY);
    deleteAndNullBlk(m_pPixValCb);
    deleteAndNullBlk(m_pPixValCr);

    // Haven't warned about anything yet
    if (!m_bScanErrorsDisable) {
        m_nWarnBadScanNum = 0;
    }
}

// Reset the major parameters
// - Called by JFIF Decoder when we begin a new file
// TODO: Consider merging with Reset()
// POST:
// - m_anSofSampFactH[]
// - m_anSofSampFactV[]
// - m_bImgDetailsSet
// - m_nNumSofComps
// - m_nPrecision
// - m_bScanErrorsDisable
// - m_nMarkersBlkNum
//
void ImgDecode::resetState() {
    resetDhtLookup();
    resetDqtTables();

    for (uint32_t nCompInd = 0; nCompInd < MAX_SOF_COMP_NF; nCompInd++) {
        m_anSofSampFactH[nCompInd] = 0;
        m_anSofSampFactV[nCompInd] = 0;
    }

    m_bImgDetailsSet = false;
    m_nNumSofComps = 0;

    m_nPrecision = 0;             // Default to "precision not set"

    m_bScanErrorsDisable = false;

    // Reset the markers
    m_nMarkersBlkNum = 0;
}

//void ImgDecode::paintEvent(QPaintEvent *)
//{
//  int32_t top = 0;

//  QRect bRect;

//  QStyleOption opt;
//  opt.init(this);
//  QPainter p(this);
//  style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
//  p.setFont(QFont("Courier New", 14));

//  p.fillRect(0, 0, width(), height(), QColor(Qt::white));

//  if(m_bDibTempReady)
//  {
//    if(!m_bPreviewIsJpeg)
//    {
//      // For all non-JPEG images, report with simple title
//      m_strTitle = "Image";
//    }
//    else
//    {
//      m_strTitle = "Image (";

//      switch (m_nPreviewMode)
//      {
//        case PREVIEW_RGB:
//          m_strTitle += "RGB";
//          break;

//        case PREVIEW_YCC:
//          m_strTitle += "YCC";
//          break;

//        case PREVIEW_R:
//          m_strTitle += "R";
//          break;

//        case PREVIEW_G:
//          m_strTitle += "G";
//          break;

//        case PREVIEW_B:
//          m_strTitle += "B";
//          break;

//        case PREVIEW_Y:
//          m_strTitle += "Y";
//          break;

//        case PREVIEW_CB:
//          m_strTitle += "Cb";
//          break;

//        case PREVIEW_CR:
//          m_strTitle += "Cr";
//          break;

//        default:
//          m_strTitle += "???";
//          break;
//      }

//      if(_decodeScanAc)
//      {
//        m_strTitle += ", DC+AC)";
//      }
//      else
//      {
//        m_strTitle += ", DC)";
//      }
//    }

//    switch (m_nZoomMode)
//    {
//      case PRV_ZOOM_12:
//        m_strTitle += " @ 12.5% (1/8)";
//        break;

//      case PRV_ZOOM_25:
//        m_strTitle += " @ 25% (1/4)";
//        break;

//      case PRV_ZOOM_50:
//        m_strTitle += " @ 50% (1/2)";
//        break;

//      case PRV_ZOOM_100:
//        m_strTitle += " @ 100% (1:1)";
//        break;

//      case PRV_ZOOM_150:
//        m_strTitle += " @ 150% (3:2)";
//        break;

//      case PRV_ZOOM_200:
//        m_strTitle += " @ 200% (2:1)";
//        break;

//      case PRV_ZOOM_300:
//        m_strTitle += " @ 300% (3:1)";
//        break;

//      case PRV_ZOOM_400:
//        m_strTitle += " @ 400% (4:1)";
//        break;

//      case PRV_ZOOM_800:
//        m_strTitle += " @ 800% (8:1)";
//        break;

//      default:
//        m_strTitle += "";
//        break;
//    }

//    // Draw image title
//    bRect = p.boundingRect(QRect(nTitleIndent, top, width(), 100), Qt::AlignLeft | Qt::AlignTop, m_strTitle);
//    p.drawText(bRect, Qt::AlignLeft | Qt::AlignCenter, m_strTitle);

//    top = bRect.height() + nTitleLowGap;
////    p.drawImage(nTitleIndent, top, m_pDibTemp->scaled(m_pDibTemp->width() / 4, m_pDibTemp->height() / 4, Qt::KeepAspectRatio));

////    top += m_pDibTemp->height() + nTitleLowGap;
//    top += nTitleLowGap;
//  }

//  if(m_bHistEn)
//  {
//    if(m_bDibHistRgbReady)
//    {
//      bRect = p.boundingRect(QRect(nTitleIndent, top, width(), 100), Qt::AlignLeft | Qt::AlignTop, "Histogram (RGB)");
//      p.drawText(bRect, Qt::AlignLeft | Qt::AlignCenter, "Histogram (RGB)");

//      top += bRect.height() + nTitleLowGap;
//      p.drawImage(nTitleIndent, top, *m_pDibHistRgb);
//      top += m_pDibHistRgb->height() + nTitleLowGap;
//    }

//    if(m_bDibHistYReady)
//    {
//      bRect = p.boundingRect(QRect(nTitleIndent, top, width(), 100), Qt::AlignLeft | Qt::AlignTop, "Histogram (Y)");
//      p.drawText(bRect, Qt::AlignLeft | Qt::AlignCenter, "Histogram (Y)");

//      top += bRect.height() + nTitleLowGap;
//      p.drawImage(nTitleIndent, top, *m_pDibHistY);
//    }
//  }
//}

// Update the status bar text
//
// INPUT:
// - str                                = New text to display on status bar
// PRE:
// - m_pStatBar
//
void ImgDecode::setStatusText(const QString &text) {
    // Make sure that we have been connected to the status
    // bar of the main window first! Note that it is jpegsnoopDoc
    // that sets this variable.
    // if (m_pStatBar) {
    //     m_pStatBar->showMessage(str);
    // }
}

// Clears the DQT entries
// POST:
// - m_anDqtTblSel[]
// - m_anDqtCoeff[][]
// - m_anDqtCoeffZz[][]
void ImgDecode::resetDqtTables() {
    for (uint32_t nDqtComp = 0; nDqtComp < MAX_DQT_COMP; nDqtComp++) {
        // Force entries to an invalid value. This makes
        // sure that we have to get a valid SetDqtTables() call
        // from JfifDecode first.
        m_anDqtTblSel[nDqtComp] = -1;
    }

    for (uint32_t nDestId = 0; nDestId < MAX_DQT_DEST_ID; nDestId++) {
        for (uint32_t nCoeff = 0; nCoeff < MAX_DQT_COEFF; nCoeff++) {
            m_anDqtCoeff[nDestId][nCoeff] = 0;
            m_anDqtCoeffZz[nDestId][nCoeff] = 0;
        }
    }

    m_nNumSofComps = 0;
}

// Reset the DHT lookup tables
// - These tables are used to speed up VLC lookups
// - This should be called by the JFIF decoder any time we start a new file
// POST:
// - m_anDhtLookupSetMax[]
// - m_anDhtLookupSize[][]
// - m_anDhtLookup_bitlen[][][]
// - m_anDhtLookup_bits[][][]
// - m_anDhtLookup_code[][][]
// - m_anDhtLookupfast[][][]
//
void ImgDecode::resetDhtLookup() {
    memset(m_anDhtHisto, 0, sizeof(m_anDhtHisto));

    // Use explicit loop ranges instead of memset
    for (uint32_t nClass = DHT_CLASS_DC; nClass <= DHT_CLASS_AC; nClass++) {
        m_anDhtLookupSetMax[nClass] = 0;
        // DHT table destination ID is range 0..3
        for (uint32_t nDestId = 0; nDestId < MAX_DHT_DEST_ID; nDestId++) {
            m_anDhtLookupSize[nClass][nDestId] = 0;

            for (uint32_t nCodeInd = 0; nCodeInd < MAX_DHT_CODES; nCodeInd++) {
                m_anDhtLookup_bitlen[nClass][nDestId][nCodeInd] = 0;
                m_anDhtLookup_bits[nClass][nDestId][nCodeInd] = 0;
                m_anDhtLookup_mask[nClass][nDestId][nCodeInd] = 0;
                m_anDhtLookup_code[nClass][nDestId][nCodeInd] = 0;
            }

            for (uint32_t nElem = 0; nElem < (2 << DHT_FAST_SIZE); nElem++) {
                // Mark with invalid value
                m_anDhtLookupfast[nClass][nDestId][nElem] = DHT_CODE_UNUSED;
            }
        }

        for (uint32_t nCompInd = 0; nCompInd < 1 + MAX_SOS_COMP_NS; nCompInd++) {
            // Force entries to an invalid value. This makes
            // sure that we have to get a valid SetDhtTables() call
            // from JfifDecode first.
            // Even though nCompInd is supposed to be 1-based numbering,
            // we start at index 0 to ensure it is marked as invalid.
            m_anDhtTblSel[nClass][nCompInd] = -1;
        }
    }

    m_nNumSosComps = 0;
}

// Configure an entry in a quantization table
//
// INPUT:
// - nSet                       = Quant table dest ID (from DQT:Tq)
// - nInd                       = Coeff index (normal order)
// - nIndzz                     = Coeff index (zigzag order)
// - nCoeff                     = Coeff value
// POST:
// - m_anDqtCoeff[]
// - m_anDqtCoeffZz[]
// RETURN:
// - True if params in range, false otherwise
//
// NOTE: Asynchronously called by JFIF Decoder
//
bool ImgDecode::setDqtEntry(uint32_t nTblDestId, uint32_t nCoeffInd, uint32_t nCoeffIndZz, uint16_t nCoeffVal) {
    if ((nTblDestId < MAX_DQT_DEST_ID) && (nCoeffInd < MAX_DQT_COEFF)) {
        m_anDqtCoeff[nTblDestId][nCoeffInd] = nCoeffVal;

        // Save a copy that represents the original zigzag order
        // This is used by the IDCT logic
        m_anDqtCoeffZz[nTblDestId][nCoeffIndZz] = nCoeffVal;

    } else {
        const auto strTmp = QString("ERROR: Attempt to set DQT entry out of range (nTblDestId = %1, nCoeffInd = %2, nCoeffVal = %3")
            .arg(nTblDestId)
            .arg(nCoeffInd)
            .arg(nCoeffVal);

#ifdef DEBUG_LOG
        const auto strDebug = QString("## File = %1 Block = %2 Error = %3")
            .arg(_appConfig.curFileName, -100)
            .arg("ImgDecode", -10)
            .arg(strTmp);

        _log.addLineDebug(strDebug);
#else
        Q_ASSERT(false);
#endif

        _log.error(strTmp);

        // if (m_pAppConfig->isInteractive()) {
        //     msgBox.setText(strTmp);
        //     msgBox.exec();
        // }

        return false;
    }

    return true;
}

// Fetch a DQT table entry
//
// INPUT:
// - nTblDestId                         = DQT Table Destination ID
// - nCoeffInd                          = Coefficient index in 8x8 matrix
// PRE:
// - m_anDqtCoeff[][]
// RETURN:
// - Returns the indexed DQT matrix entry
//
uint32_t ImgDecode::GetDqtEntry(uint32_t nTblDestId, uint32_t nCoeffInd) {
    if ((nTblDestId < MAX_DQT_DEST_ID) && (nCoeffInd < MAX_DQT_COEFF)) {
        return m_anDqtCoeff[nTblDestId][nCoeffInd];
    } else {
        // Should never get here!
        QString strTmp;

        strTmp = QString("ERROR: GetDqtEntry(nTblDestId = %1, nCoeffInd = %2").arg(nTblDestId).arg(nCoeffInd);
        _log.error(strTmp);

        // if (m_pAppConfig->isInteractive()) {
        //     msgBox.setText(strTmp);
        //     msgBox.exec();
        // }

#ifdef DEBUG_LOG
        _log.debug(QString("## File = %1 Block = %2 Error = %3").arg(_appConfig.curFileName).arg("ImgDecode").arg(strTmp));
#else
        Q_ASSERT(false);
#endif

        return 0;
    }
}

// Set a DQT table for a frame image component identifier
//
// INPUT:
// - nCompInd                   = Component index. Based on m_nSofNumComps_Nf-1 (ie. 0..254)
// - nTbl                               = DQT Table number. Based on SOF:Tqi (ie. 0..3)
// POST:
// - m_anDqtTblSel[]
// RETURN:
// - Success if index and table are in range
// NOTE:
// - Asynchronously called by JFIF Decoder
//
bool ImgDecode::SetDqtTables(uint32_t nCompId, uint32_t nTbl) {
    QString strTmp;

    if ((nCompId < MAX_SOF_COMP_NF) && (nTbl < MAX_DQT_DEST_ID)) {
        m_anDqtTblSel[nCompId] = static_cast<int32_t>(nTbl);
    } else {
        // Should never get here unless the JFIF SOF table has a bad entry!
        strTmp = QString("ERROR: SetDqtTables(Comp ID = %1, Table = %2")
            .arg(nCompId)
            .arg(nTbl);
        _log.error(strTmp);

        // if (m_pAppConfig->isInteractive()) {
        //     msgBox.setText(strTmp);
        //     msgBox.exec();
        // }

        return false;
    }

    return true;
}

// Set a DHT table for a scan image component index
// - The DHT Table select array is stored as:
//   m_anDhtTblSel[0][1,2,3] for DC
//   m_anDhtTblSel[1][1,2,3] for AC
//
// INPUT:
// - nCompInd                   = Component index (1-based). Range 1..4
// - nTblDc                             = DHT table index for DC elements of component
// - nTblAc                             = DHT table index for AC elements of component
// POST:
// - m_anDhtTblSel[][]
// RETURN:
// - Success if indices are in range
//
bool ImgDecode::SetDhtTables(uint32_t nCompInd, uint32_t nTblDc, uint32_t nTblAc) {
    QString strTmp;

    // Note use of (nCompInd < MAX_SOS_COMP_NS+1) as nCompInd is 1-based notation
    if ((nCompInd >= 1) && (nCompInd < MAX_SOS_COMP_NS + 1) && (nTblDc < MAX_DHT_DEST_ID) &&
        (nTblAc < MAX_DHT_DEST_ID)) {
        m_anDhtTblSel[DHT_CLASS_DC][nCompInd] = static_cast<int32_t>(nTblDc);
        m_anDhtTblSel[DHT_CLASS_AC][nCompInd] = static_cast<int32_t>(nTblAc);
    } else {
        // Should never get here!
        strTmp = QString("ERROR: SetDhtTables(comp = %1, TblDC = %2 TblAC = %3) out of indexed range")
            .arg(nCompInd)
            .arg(nTblDc)
            .arg(nTblAc);
        _log.error(strTmp);

        // if (m_pAppConfig->isInteractive()) {
        //     msgBox.setText(strTmp);
        //     msgBox.exec();
        // }

        return false;
    }

    return true;
}

// Get the precision field
//
// INPUT:
// - nPrecision                 = DCT sample precision (typically 8 or 12)
// POST:
// - m_nPrecision
//
void ImgDecode::SetPrecision(uint32_t nPrecision) {
    m_nPrecision = nPrecision;
}

// Set the general image details for the image decoder
//
// INPUT:
// - nDimX                              = Image dimension (X)
// - nDimY                              = Image dimension (Y)
// - nCompsSOF                  = Number of components in Frame
// - nCompsSOS                  = Number of components in Scan
// - bRstEn                             = Restart markers present?
// - nRstInterval               = Restart marker interval
// POST:
// - m_bImgDetailsSet
// - m_nDimX
// - m_nDimY
// - m_nNumSofComps
// - m_nNumSosComps
// - m_bRestartEn
// - m_nRestartInterval
// NOTE:
// - Called asynchronously by the JFIF decoder
//
void ImgDecode::setImageDetails(uint32_t nDimX, uint32_t nDimY, uint32_t nCompsSOF, uint32_t nCompsSOS, bool bRstEn,
                                uint32_t nRstInterval) {
    m_bImgDetailsSet = true;
    m_nDimX = nDimX;
    m_nDimY = nDimY;
    m_nNumSofComps = nCompsSOF;
    m_nNumSosComps = nCompsSOS;
    m_bRestartEn = bRstEn;
    m_nRestartInterval = nRstInterval;
}

// Set the sampling factor for an image component
//
// INPUT:
// - nCompInd                   = Component index from Nf (ie. 1..255)
// - nSampFactH                 = Sampling factor in horizontal direction
// - nSampFactV                 = Sampling factor in vertical direction
// POST:
// - m_anSofSampFactH[]
// - m_anSofSampFactV[]
// NOTE:
// - Called asynchronously by the JFIF decoder in SOF
//
void ImgDecode::SetSofSampFactors(uint32_t nCompInd, uint32_t nSampFactH, uint32_t nSampFactV) {
    // TODO: Check range
    m_anSofSampFactH[nCompInd] = nSampFactH;
    m_anSofSampFactV[nCompInd] = nSampFactV;
}

// Set a DHT table entry and associated lookup table
// - Configures the fast lookup table for short code bitstrings
//
// INPUT:
// - nDestId                    = DHT destination table ID (0..3)
// - nClass                             = Select between DC and AC tables (0=DC, 1=AC)
// - nInd                               = Index into table
// - nLen                               = Huffman code length
// - nBits                              = Huffman code bitstring (left justified)
// - nMask                              = Huffman code bit mask (left justified)
// - nCode                              = Huffman code value
// POST:
// - m_anDhtLookup_bitlen[][][]
// - m_anDhtLookup_bits[][][]
// - m_anDhtLookup_mask[][][]
// - m_anDhtLookup_code[][][]
// - m_anDhtLookupSetMax[]
// - m_anDhtLookupfast[][][]
// RETURN:
// - Success if indices are in range
// NOTE:
// - Asynchronously called by JFIF Decoder
//
bool ImgDecode::SetDhtEntry(uint32_t nDestId, uint32_t nClass, uint32_t nInd, uint32_t nLen,
                            uint32_t nBits, uint32_t nMask, uint32_t nCode) {
    if ((nDestId >= MAX_DHT_DEST_ID) || (nClass >= MAX_DHT_CLASS) || (nInd >= MAX_DHT_CODES)) {
        QString strTmp = "Attempt to set DHT entry out of range";
        _log.error(strTmp);

        // if (m_pAppConfig->isInteractive()) {
        //     msgBox.setText(strTmp);
        //     msgBox.exec();
        // }
        // #ifdef DEBUG_LOG
        _log.debug(QString("## Block = %1 Error = %2").arg("ImgDecode", strTmp));
        // #else
        //         Q_ASSERT(false);
        // #endif
        return false;
    }

    m_anDhtLookup_bitlen[nClass][nDestId][nInd] = nLen;
    m_anDhtLookup_bits[nClass][nDestId][nInd] = nBits;
    m_anDhtLookup_mask[nClass][nDestId][nInd] = nMask;
    m_anDhtLookup_code[nClass][nDestId][nInd] = nCode;

    // Record the highest numbered DHT set.
    // TODO: Currently assuming that there are no missing tables in the sequence
    if (nDestId > m_anDhtLookupSetMax[nClass]) {
        m_anDhtLookupSetMax[nClass] = nDestId;
    }

    uint32_t nBitsMsb;
    uint32_t nBitsExtraLen;
    uint32_t nBitsExtraVal;
    uint32_t nBitsMax;
    uint32_t nFastVal;

    // If the variable-length code is short enough, add it to the
    // fast lookup table.
    if (nLen <= DHT_FAST_SIZE) {
        // nBits is a left-justified number (assume right-most bits are zero)
        // nLen is number of leading bits to compare

        //   nLen     = 5
        //   nBits    = 32'b1011_1xxx_xxxx_xxxx_xxxx_xxxx_xxxx_xxxx  (0xB800_0000)
        //   nBitsMsb =  8'b1011_1xxx (0xB8)
        //   nBitsMsb =  8'b1011_1000 (0xB8)
        //   nBitsMax =  8'b1011_1111 (0xBF)

        //   nBitsExtraLen = 8-len = 8-5 = 3

        //   nBitsExtraVal = (1<<nBitsExtraLen) -1 = 1<<3 -1 = 8'b0000_1000 -1 = 8'b0000_0111
        //
        //   nBitsMax = nBitsMsb + nBitsExtraVal
        //   nBitsMax =  8'b1011_1111
        nBitsMsb = (nBits & nMask) >> (32 - DHT_FAST_SIZE);
        nBitsExtraLen = DHT_FAST_SIZE - nLen;
        nBitsExtraVal = (1 << nBitsExtraLen) - 1;
        nBitsMax = nBitsMsb + nBitsExtraVal;

        for (uint32_t ind1 = nBitsMsb; ind1 <= nBitsMax; ind1++) {
            // The arrangement of the dht_lookupfast[] values:
            //                         0xFFFF_FFFF = No entry, resort to slow search
            //    m_anDhtLookupfast[nClass][nDestId] [15: 8] = nLen
            //    m_anDhtLookupfast[nClass][nDestId] [ 7: 0] = nCode
            //
            //Q_ASSERT(code <= 0xFF);
            //Q_ASSERT(ind1 <= 0xFF);
            nFastVal = nCode + (nLen << 8);
            m_anDhtLookupfast[nClass][nDestId][ind1] = nFastVal;
        }
    }

    return true;
}

// Assign the size of the DHT table
//
// INPUT:
// - nDestId                    = Destination DHT table ID (From DHT:Th, range 0..3)
// - nClass                             = Select between DC and AC tables (0=DC, 1=AC) (From DHT:Tc, range 0..1)
// - nSize                              = Number of entries in the DHT table
// POST:
// - m_anDhtLookupSize[][]
// RETURN:
// - Success if indices are in range
//
bool ImgDecode::SetDhtSize(uint32_t nDestId, uint32_t nClass, uint32_t nSize) {
    if ((nDestId >= MAX_DHT_DEST_ID) || (nClass >= MAX_DHT_CLASS) || (nSize >= MAX_DHT_CODES)) {
        QString strTmp = "ERROR: Attempt to set DHT table size out of range";

        _log.error(strTmp);

        // if (m_pAppConfig->isInteractive()) {
        //     msgBox.setText(strTmp);
        //     msgBox.exec();
        // }

        return false;
    } else {
        m_anDhtLookupSize[nClass][nDestId] = nSize;
    }

    return true;
}

// Convert huffman code (DC) to signed value
// - Convert according to ITU-T.81 Table 5
//
// INPUT:
// - nVal                               = Huffman DC value (left justified)
// - nBits                              = Bitstring length of value
// RETURN:
// - Signed integer representing Huffman DC value
//
int32_t ImgDecode::HuffmanDc2Signed(uint32_t nVal, uint32_t nBits) {
    if (nVal >= (1 << (nBits - 1))) {
        return static_cast<int32_t>(nVal);
    } else {
        return static_cast<int32_t>(nVal - ((1 << nBits) - 1));
    }
}

// Generate the Huffman code lookup table mask
//
// POST:
// - m_anHuffMaskLookup[]
//
void ImgDecode::GenLookupHuffMask() {
    uint32_t nMask;

    for (uint32_t nLen = 0; nLen < 32; nLen++) {
        nMask = (1 << (nLen)) - 1;
        nMask <<= 32 - nLen;
        m_anHuffMaskLookup[nLen] = nMask;
    }
}

// Extract a specified number of bits from a 32-bit holding register
//
// INPUT:
// - nWord                              = The 32-bit holding register
// - nBits                              = Number of bits (leftmost) to extract from the holding register
// PRE:
// - m_anHuffMaskLookup[]
// RETURN:
// - The subset of bits extracted from the holding register
// NOTE:
// - This routine is inlined for speed
//
inline uint32_t ImgDecode::ExtractBits(uint32_t nWord, uint32_t nBits) {
    uint32_t nVal;

    nVal = (nWord & m_anHuffMaskLookup[nBits]) >> (32 - nBits);
    return nVal;
}

// Removes bits from the holding buffer
// - Discard the leftmost "nNumBits" of m_nScanBuff
// - And then realign file position pointers
//
// INPUT:
// - nNumBits                           = Number of left-most bits to consume
// POST:
// - m_nScanBuff
// - m_nScanBuff_vacant
// - m_nScanBuffPtr_align
// - m_anScanBuffPtr_pos[]
// - m_anScanBuffPtr_err[]
// - m_nScanBuffLatchErr
// - m_nScanBuffPtr_num
//
inline void ImgDecode::ScanBuffConsume(uint32_t nNumBits) {
    m_nScanBuff <<= nNumBits;
    m_nScanBuff_vacant += nNumBits;

    // Need to latch any errors during consumption of multi-bytes
    // otherwise we'll miss the error notification if we skip over
    // it before we exit this routine! e.g if num_bytes=2 and error
    // appears on first byte, then we want to retain it in pos[0]

    // Now realign the file position pointers if necessary
    uint32_t nNumBytes = (m_nScanBuffPtr_align + nNumBits) / 8;

    for (uint32_t nInd = 0; nInd < nNumBytes; nInd++) {
        m_anScanBuffPtr_pos[0] = m_anScanBuffPtr_pos[1];
        m_anScanBuffPtr_pos[1] = m_anScanBuffPtr_pos[2];
        m_anScanBuffPtr_pos[2] = m_anScanBuffPtr_pos[3];
        // Don't clear the last ptr position because during an overread
        // this will be the only place that the last position was preserved
        //m_anScanBuffPtr_pos[3] = 0;

        m_anScanBuffPtr_err[0] = m_anScanBuffPtr_err[1];
        m_anScanBuffPtr_err[1] = m_anScanBuffPtr_err[2];
        m_anScanBuffPtr_err[2] = m_anScanBuffPtr_err[3];
        m_anScanBuffPtr_err[3] = SCANBUF_OK;

        if (m_anScanBuffPtr_err[0] != SCANBUF_OK) {
            m_nScanBuffLatchErr = m_anScanBuffPtr_err[0];
        }

        m_nScanBuffPtr_num--;
    }

    m_nScanBuffPtr_align = (m_nScanBuffPtr_align + nNumBits) % 8;
}

// Augment the current scan buffer with another byte
// - Extra bits are added to right side of existing bitstring
//
// INPUT:
// - nNewByte                 = 8-bit byte to add to buffer
// - nPtr                               = UNUSED
// PRE:
// - m_nScanBuff
// - m_nScanBuff_vacant
// - m_nScanBuffPtr_num
// POST:
// - m_nScanBuff
// - m_nScanBuff_vacant
// - m_anScanBuffPtr_err[]
// - m_anScanBuffPtr_pos[]
//
inline void ImgDecode::ScanBuffAdd(uint32_t nNewByte, uint32_t nPtr) {
    // Add the new byte to the buffer
    // Assume that m_nScanBuff has already been shifted to be
    // aligned to bit 31 as first bit.
    m_nScanBuff += (nNewByte << (m_nScanBuff_vacant - 8));
    m_nScanBuff_vacant -= 8;

    Q_ASSERT(m_nScanBuffPtr_num < 4);

    if (m_nScanBuffPtr_num >= 4) {
        return;
    }                             // Unexpected by design

    m_anScanBuffPtr_err[m_nScanBuffPtr_num] = SCANBUF_OK;
    m_anScanBuffPtr_pos[m_nScanBuffPtr_num++] = nPtr;

    // m_nScanBuffPtr_align stays the same as we add 8 bits
}

// Augment the current scan buffer with another byte (but mark as error)
//
// INPUT:
// - nNewByte                 = 8-bit byte to add to buffer
// - nPtr                               = UNUSED
// - nErr                               = Error code to associate with this buffer byte
// POST:
// - m_anScanBuffPtr_err[]
//
inline void ImgDecode::ScanBuffAddErr(uint32_t nNewByte, uint32_t nPtr, uint32_t nErr) {
    ScanBuffAdd(nNewByte, nPtr);
    m_anScanBuffPtr_err[m_nScanBuffPtr_num - 1] = nErr;
}

// Disable any further reporting of scan errors
//
// PRE:
// - _scanErrMax
// POST:
// - m_nWarnBadScanNum
// - m_bScanErrorsDisable
//
void ImgDecode::ScanErrorsDisable() {
    m_nWarnBadScanNum = _scanErrMax;
    m_bScanErrorsDisable = true;
}

// Enable reporting of scan errors
//
// POST:
// - m_nWarnBadScanNum
// - m_bScanErrorsDisable
//
void ImgDecode::ScanErrorsEnable() {
    m_nWarnBadScanNum = 0;
    m_bScanErrorsDisable = false;
}

// Read in bits from the buffer and find matching huffman codes
// - Input buffer is m_nScanBuff
// - Perform shift in buffer afterwards
// - Abort if there aren't enough bits (note that the scan buffer
//   is almost empty when we reach the end of a scan segment)
//
// INPUT:
// - nClass                                     = DHT Table class (0..1)
// - nTbl                                       = DHT Destination ID (0..3)
// PRE:
// - Assume that dht_lookup_size[nTbl] != 0 (already checked)
// - m_bRestartRead
// - m_nScanBuff_vacant
// - _scanErrMax
// - m_nScanBuff
// - m_anDhtLookupFast[][][]
// - m_anDhtLookupSize[][]
// - m_anDhtLookup_mask[][][]
// - m_anDhtLookup_bits[][][]
// - m_anDhtLookup_bitlen[][][]
// - m_anDhtLookup_code[][][]
// - m_nPrecision
// POST:
// - m_nScanBitsUsed# is calculated
// - m_bScanEnd
// - m_bScanBad
// - m_nWarnBadScanNum
// - m_anDhtHisto[][][]
// OUTPUT:
// - rZrl                               = Zero run amount (if any)
// - rVal                               = Coefficient value
// RETURN:
// - Status from attempting to decode the current value
//
// PERFORMANCE:
// - Tried to unroll all function calls listed below,
//   but performance was same before & after (27sec).
// - Calls unrolled: BuffTopup(), ScanBuffConsume(),
//                   ExtractBits(), HuffmanDc2Signed()
teRsvRet ImgDecode::ReadScanVal(uint32_t nClass, uint32_t nTbl, uint32_t &rZrl, int32_t &rVal) {
    bool bDone = false;

    uint32_t nInd = 0;

    uint32_t nCode = DHT_CODE_UNUSED;     // Not a valid nCode

    uint32_t nVal;

    Q_ASSERT(nClass < MAX_DHT_CLASS);
    Q_ASSERT(nTbl < MAX_DHT_DEST_ID);

    m_nScanBitsUsed1 = 0;         // bits consumed for nCode
    m_nScanBitsUsed2 = 0;

    rZrl = 0;
    rVal = 0;

    // First check to see if we've entered here with a completely empty
    // scan buffer with a restart marker already observed. In that case
    // we want to exit with condition 3 (restart terminated)
    if ((m_nScanBuff_vacant == 32) && (m_bRestartRead)) {
        return RSV_RST_TERM;
    }

    // Has the scan buffer been depleted?
    if (m_nScanBuff_vacant >= 32) {
        // Trying to overread end of scan segment

        if (m_nWarnBadScanNum < _scanErrMax) {
            QString strTmp;

            strTmp = QString("*** ERROR: Overread scan segment (before nCode)! @ Offset: %1").arg(getScanBufPos());
            _log.error(strTmp);

            m_nWarnBadScanNum++;

            if (m_nWarnBadScanNum >= _scanErrMax) {
                strTmp = QString("    Only reported first %1 instances of this message...").arg(_scanErrMax);
                _log.error(strTmp);
            }
        }

        m_bScanEnd = true;
        m_bScanBad = true;
        return RSV_UNDERFLOW;
    }

    // Top up the buffer just in case
    BuffTopup();

    bDone = false;
    bool bFound = false;

    // Fast search for variable-length huffman nCode
    // Do direct lookup for any codes DHT_FAST_SIZE bits or shorter

    uint32_t nCodeMsb;

    uint32_t nCodeFastSearch;

    // Only enable this fast search if m_nScanBuff_vacant implies
    // that we have at least DHT_FAST_SIZE bits available in the buffer!
    if ((32 - m_nScanBuff_vacant) >= DHT_FAST_SIZE) {
        nCodeMsb = m_nScanBuff >> (32 - DHT_FAST_SIZE);
        nCodeFastSearch = m_anDhtLookupfast[nClass][nTbl][nCodeMsb];

        if (nCodeFastSearch != DHT_CODE_UNUSED) {
            // We found the code!
            m_nScanBitsUsed1 += (nCodeFastSearch >> 8);
            nCode = (nCodeFastSearch & 0xFF);
            bDone = true;
            bFound = true;
        }
    }

    // Slow search for variable-length huffman nCode
    while (!bDone) {
        uint32_t nBitLen;

        if ((m_nScanBuff & m_anDhtLookup_mask[nClass][nTbl][nInd]) == m_anDhtLookup_bits[nClass][nTbl][nInd]) {

            nBitLen = m_anDhtLookup_bitlen[nClass][nTbl][nInd];

            // Just in case this VLC bit string is longer than the number of
            // bits we have left in the buffer (due to restart marker or end
            // of scan data), we need to double-check
            if (nBitLen <= 32 - m_nScanBuff_vacant) {
                nCode = m_anDhtLookup_code[nClass][nTbl][nInd];
                m_nScanBitsUsed1 += nBitLen;
                bDone = true;
                bFound = true;
            }
        }

        nInd++;

        if (nInd >= m_anDhtLookupSize[nClass][nTbl]) {
            bDone = true;
        }
    }

    // Could not find huffman nCode in table!
    if (!bFound) {
        // If we didn't find the huffman nCode, it might be due to a
        // restart marker that prematurely stopped the scan buffer filling.
        // If so, return with an indication so that DecodeScanComp() can
        // handle the restart marker, refill the scan buffer and then
        // re-do ReadScanVal()
        if (m_bRestartRead) {
            return RSV_RST_TERM;
        }

        // FIXME:
        // What should we be consuming here? we need to make forward progress
        // in file. Options:
        // 1) Move forward to next byte in file
        // 2) Move forward to next bit in file
        // Currently moving 1 bit so that we have slightly more opportunities
        // to re-align earlier.
        m_nScanBitsUsed1 = 1;
        nCode = DHT_CODE_UNUSED;
    }

    // Log an entry into a histogram
    if (m_nScanBitsUsed1 < MAX_DHT_CODELEN + 1) {
        m_anDhtHisto[nClass][nTbl][m_nScanBitsUsed1]++;
    } else {
        Q_ASSERT(false);
        // Somehow we got out of range
    }

    ScanBuffConsume(m_nScanBitsUsed1);

    // Did we overread the scan buffer?
    if (m_nScanBuff_vacant > 32) {
        // The nCode consumed more bits than we had!
        QString strTmp;

        strTmp = QString("*** ERROR: Overread scan segment (after nCode)! @ Offset: %1").arg(getScanBufPos());
        _log.error(strTmp);
        m_bScanEnd = true;
        m_bScanBad = true;
        return RSV_UNDERFLOW;
    }

    // Replenish buffer after nCode extraction and before variable extra bits
    // This is required because we may have a 12 bit nCode followed by a 16 bit var length bitstring
    BuffTopup();

    // Did we find the nCode?
    if (nCode != DHT_CODE_UNUSED) {
        rZrl = (nCode & 0xF0) >> 4;
        m_nScanBitsUsed2 = nCode & 0x0F;

        if ((rZrl == 0) && (m_nScanBitsUsed2 == 0)) {
            // EOB (was bits_extra=0)
            return RSV_EOB;
        } else if (m_nScanBitsUsed2 == 0) {
            // Zero rValue
            rVal = 0;
            return RSV_OK;

        } else {
            // Normal nCode
            nVal = ExtractBits(m_nScanBuff, m_nScanBitsUsed2);
            rVal = HuffmanDc2Signed(nVal, m_nScanBitsUsed2);

            // Now handle the different precision values
            // Treat 12-bit like 8-bit but scale values first (ie. drop precision down to 8-bit)
            int32_t nPrecisionDivider = 1;

            if (m_nPrecision >= 8) {
                nPrecisionDivider = 1 << (m_nPrecision - 8);
                rVal /= nPrecisionDivider;
            } else {
                // Precision value seems out of range!
            }

            ScanBuffConsume(m_nScanBitsUsed2);

            // Did we overread the scan buffer?
            if (m_nScanBuff_vacant > 32) {
                // The nCode consumed more bits than we had!
                QString strTmp;

                strTmp = QString("*** ERROR: Overread scan segment (after bitstring)! @ Offset: %1").arg(getScanBufPos());
                _log.error(strTmp);
                m_bScanEnd = true;
                m_bScanBad = true;
                return RSV_UNDERFLOW;
            }

            return RSV_OK;
        }
    } else {
        // ERROR: Invalid huffman nCode!

        // FIXME: We may also enter here if we are about to encounter a
        // restart marker! Need to see if ScanBuf is terminated by
        // restart marker; if so, then we simply flush the ScanBuf,
        // consume the 2-byte RST marker, clear the ScanBuf terminate
        // reason, then indicate to caller that they need to call ReadScanVal
        // again.

        if (m_nWarnBadScanNum < _scanErrMax) {
            QString strTmp;

            strTmp =
                QString("*** ERROR: Can't find huffman bitstring @ %1, table %2, value 0x%3").arg(getScanBufPos()).arg(
                    nTbl).
                    arg(m_nScanBuff, 8, 16, QChar('0'));
            _log.error(strTmp);

            m_nWarnBadScanNum++;

            if (m_nWarnBadScanNum >= _scanErrMax) {
                strTmp = QString("    Only reported first %1 instances of this message...").arg(_scanErrMax);
                _log.error(strTmp);
            }
        }

        // TODO: What rValue and ZRL should we return?
        m_bScanBad = true;
        return RSV_UNDERFLOW;
    }

    // NOTE: Can't reach here
    // return RSV_UNDERFLOW;
}

// Refill the scan buffer as needed
//
void ImgDecode::BuffTopup() {
    uint32_t nRetVal;

    // Do we have space to load another byte?
    bool bDone = (m_nScanBuff_vacant < 8);

    // Have we already reached the end of the scan segment?
    if (m_bScanEnd) {
        bDone = true;
    }

    while (!bDone) {
        nRetVal = BuffAddByte();

        // NOTE: If we have read in a restart marker, the above call will not
        // read in any more bits into the scan buffer, so we should just simply
        // say that we've done the best we can for the top up.
        if (m_bRestartRead) {
            bDone = true;
        }

        if (m_nScanBuff_vacant < 8) {
            bDone = true;
        }

        // If the buffer read returned an error or end of scan segment
        // then stop filling buffer
        if (nRetVal != 0) {
            bDone = true;
        }
    }
}

// Check for restart marker and compare the index against expected
//
// PRE:
// - m_nScanBuffPtr
// RETURN:
// - Restart marker found at the current buffer position
// NOTE:
// - This routine expects that we did not remove restart markers
//   from the bytestream (in BuffAddByte).
//
bool ImgDecode::ExpectRestart() {
    uint32_t nMarker;
    uint32_t nBuf0 = _wbuf.getByte(m_nScanBuffPtr);
    uint32_t nBuf1 = _wbuf.getByte(m_nScanBuffPtr + 1);

    // Check for restart marker first. Assume none back-to-back.
    if (nBuf0 == 0xFF) {
        nMarker = nBuf1;

        // FIXME: Later, check that we are on the right marker!
        if ((nMarker >= JFIF_RST0) && (nMarker <= JFIF_RST7)) {
            if (_verbose) {
                QString strTmp;

                strTmp =
                    QString("  RESTART marker: @ 0x%1.0 : RST%2")
                        .arg(m_nScanBuffPtr, 8, 16, QChar('0'))
                        .arg(nMarker - JFIF_RST0, 2, 10, QChar('0'));
                _log.info(strTmp);
            }

            m_nRestartRead++;

            // FIXME:
            //     Later on, we should be checking for RST out of
            //     sequence!

            // Now we need to skip to the next bytes
            m_nScanBuffPtr += 2;

            // Now refill in the buffer if we need to
            BuffAddByte();

            return true;
        }
    }

    return false;
}

// Add a byte to the scan buffer from the file
// - Handle stuff bytes
// - Handle restart markers
//
// PRE:
// - m_nScanBuffPtr
// POST:
// - m_nRestartRead
// - m_nRestartLastInd
//
uint32_t ImgDecode::BuffAddByte() {
    uint32_t nMarker = 0x00;
    uint32_t nBuf0, nBuf1;

    // If we have already read in a restart marker but not
    // handled it yet, then simply return without reading any
    // more bytes
    if (m_bRestartRead) {
        return 0;
    }

    nBuf0 = _wbuf.getByte(m_nScanBuffPtr);
    nBuf1 = _wbuf.getByte(m_nScanBuffPtr + 1);

    // Check for restart marker first. Assume none back-to-back.
    if (nBuf0 == 0xFF) {
        nMarker = nBuf1;

        if ((nMarker >= JFIF_RST0) && (nMarker <= JFIF_RST7)) {
            if (_verbose) {
                QString strTmp;

                strTmp =
                    QString("  RESTART marker: @ 0x%1.0 : RST%2")
                        .arg(m_nScanBuffPtr, 8, 16, QChar('0'))
                        .arg(nMarker - JFIF_RST0, 2, 10, QChar('0'));
                _log.info(strTmp);
            }

            m_nRestartRead++;
            m_nRestartLastInd = nMarker - JFIF_RST0;

            if (m_nRestartLastInd != m_nRestartExpectInd) {
                if (!m_bScanErrorsDisable) {
                    QString strTmp;

                    strTmp =
                        QString("  ERROR: Expected RST marker index RST%1 got RST%2 @ 0x%3.0")
                            .arg(m_nRestartExpectInd)
                            .arg(m_nRestartLastInd)
                            .arg(m_nScanBuffPtr, 8, 16, QChar('0'));
                    _log.error(strTmp);
                }
            }

            m_nRestartExpectInd = (m_nRestartLastInd + 1) % 8;

            // FIXME: Later on, we should be checking for RST out of sequence

            // END BUFFER READ HERE!
            // Indicate that a Restart marker has been seen, which
            // will prevent further reading of scan buffer until it
            // has been handled.
            m_bRestartRead = true;

            return 0;

            /*
      // Now we need to skip to the next bytes
			m_nScanBuffPtr+=2;

			// Update local saved buffer vars
			nBuf0 = m_pWBuf->Buf(m_nScanBuffPtr);
			nBuf1 = m_pWBuf->Buf(m_nScanBuffPtr+1);

      // Use ScanBuffAddErr() to mark this byte as being after Restart marker!
			m_anScanBuffPtr_err[m_nScanBuffPtr_num] = SCANBUF_RST;

			// FIXME: We should probably discontinue filling the scan
			// buffer if we encounter a restart marker. This will stop us
			// from reading past the restart marker and missing the necessary
			// step in resetting the decoder accumulation state.

      // When we stop adding bytes to the buffer, we should also
			// mark this scan buffer with a flag to indicate that it was
			// ended by RST not by EOI or an Invalid Marker.

			// If the main decoder (in ReadScanVal) cannot find a VLC code
			// match with the few bits left in the scan buffer
			// (presumably padded with 1's), then it should check to see
			// if the buffer is terminated by RST. If so, then it
      // purges the scan buffer, advances to the next byte (after the
			// RST marker) and does a fill, then re-invokes the ReadScanVal
			// routine. At the level above, the Decoder that is calling
			// ReadScanVal should be counting MCU rows and expect this error
			// from ReadScanVal (no code match and buf terminated by RST).
			// If it happened out of place, we have corruption somehow!

			// See IJG Code:
			//   jdmarker.c:
			//     read_restart_marker()
			//     jpeg_resync_to_restart()
*/
        }
    }

    // Check for byte Stuff
    if ((nBuf0 == 0xFF) && (nBuf1 == 0x00)) {
        // Add byte to m_nScanBuff & record file position
        ScanBuffAdd(nBuf0, m_nScanBuffPtr);
        m_nScanBuffPtr += 2;
    } else if ((nBuf0 == 0xFF) && (nBuf1 == 0xFF)) {
        // NOTE:
        // We should be checking for a run of 0xFF before EOI marker.
        // It is possible that we could get marker padding on the end
        // of the scan segment, so we'd want to handle it here, otherwise
        // we'll report an error that we got a non-EOI Marker in scan
        // segment.

        // The downside of this is that we don't detect errors if we have
        // a run of 0xFF in the stream, until we leave the string of FF's.
        // If it were followed by an 0x00, then we may not notice it at all.
        // Probably OK.

        /*
       if (m_nWarnBadScanNum < _scanErrMax) {
       QString strTmp;
       strTmp = QString("  Scan Data encountered sequence 0xFFFF @ 0x%08X.0 - Assume start of marker pad at end of scan segment",
       m_nScanBuffPtr);
       m_pLog->AddLineWarn(strTmp);

       m_nWarnBadScanNum++;
       if (m_nWarnBadScanNum >= _scanErrMax) {
       strTmp = QString("    Only reported first %u instances of this message..."),_scanErrMax;
       m_pLog->AddLineErr(strTmp);
       }
       }

       // Treat as single byte of byte stuff for now, since we don't
       // know if FF bytes will arrive in pairs or not.
       m_nScanBuffPtr+=1;
     */

        // NOTE:
        // If I treat the 0xFFFF as a potential marker pad, we may not stop correctly
        // upon error if we see this inside the image somewhere (not at end).
        // Therefore, let's simply add these bytes to the buffer and let the DecodeScanImg()
        // routine figure out when we're at the end, etc.

        ScanBuffAdd(nBuf0, m_nScanBuffPtr);
        m_nScanBuffPtr += 1;
    } else if ((nBuf0 == 0xFF) && (nMarker != 0x00)) {
        // We have read a marker... don't assume that this is bad as it will
        // always happen at the end of the scan segment. Therefore, we will
        // assume this marker is valid (ie. not bit error in scan stream)
        // and mark the end of the scan segment.

        if (m_nWarnBadScanNum < _scanErrMax) {
            QString strTmp;

            strTmp = QString("  Scan Data encountered marker   0xFF%1 @ 0x%2.0")
                .arg(nMarker, 2, 16, QChar('0'))
                .arg(m_nScanBuffPtr, 8, 16, QChar('0'));
            _log.info(strTmp);

            if (nMarker != JFIF_EOI) {
                _log.error("  NOTE: Marker wasn't EOI (0xFFD9)");
            }

            m_nWarnBadScanNum++;

            if (m_nWarnBadScanNum >= _scanErrMax) {
                strTmp = QString("    Only reported first %1 instances of this message...").arg(_scanErrMax);
                _log.error(strTmp);
            }
        }

        // Optionally stop immediately upon a bad marker
#ifdef SCAN_BAD_MARKER_STOP
                                                                                                                                m_bScanEnd = true;
    return 1;
#else
        // Add byte to m_nScanBuff & record file position
        ScanBuffAddErr(nBuf0, m_nScanBuffPtr, SCANBUF_BADMARK);

        m_nScanBuffPtr += 1;
#endif
    } else {
        // Normal byte
        // Add byte to m_nScanBuff & record file position
        ScanBuffAdd(nBuf0, m_nScanBuffPtr);

        m_nScanBuffPtr += 1;
    }

    return 0;
}

// Define minimum value before we include DCT entry in
// the IDCT calcs.
// NOTE: Currently disabled
#define IDCT_COEF_THRESH 4

// Decode a single component for one block of an MCU
// - Pull bits from the main buffer
// - Find matching huffman codes
// - Fill in the IDCT matrix (m_anDctBlock[])
// - Perform the IDCT to create spatial domain
//
// INPUT:
// - nTblDhtDc                                  = DHT table ID for DC component
// - nTblDhtAc                                  = DHT talbe ID for AC component
// - nMcuX                                              = UNUSED
// - nMcuY                                              = UNUSED
// RETURN:
// - Indicate if the decode was successful
// POST:
// - m_nScanCurDc = DC component value
// Performance:
// - This routine is called on every block so it must be
//   reasonably efficient. Only call string routines when we
//   are sure that we have an error.
//
// FIXME: Consider adding checks for DHT table like in ReadScanVal()
//
bool ImgDecode::DecodeScanComp(uint32_t nTblDhtDc, uint32_t nTblDhtAc, uint32_t nTblDqt, uint32_t, uint32_t) {
    uint32_t nZrl;

    int32_t nVal;

    bool bDone = false;
    bool bDC = true;              // Start with DC coeff

    teRsvRet eRsvRet;             // Return value from ReadScanVal()

    uint32_t nNumCoeffs = 0;

    //uint32_t nDctMax = 0;                 // Maximum DCT coefficient to use for IDCT
    uint32_t nSavedBufPos = 0;

    uint32_t nSavedBufErr = SCANBUF_OK;

    uint32_t nSavedBufAlign = 0;

    // Profiling: No difference noted
    DecodeIdctClear();

    while (!bDone) {
        BuffTopup();

        // Note that once we perform ReadScanVal(), then GetScanBufPos() will be
        // after the decoded VLC
        // Save old file position info in case we want accurate error positioning
        nSavedBufPos = m_anScanBuffPtr_pos[0];
        nSavedBufErr = m_nScanBuffLatchErr;
        nSavedBufAlign = m_nScanBuffPtr_align;

        // ReadScanVal return values:
        // - RSV_OK                     OK
        // - RSV_EOB            End of block
        // - RSV_UNDERFLOW      Ran out of data in buffer
        // - RSV_RST_TERM       No huffman code found, but restart marker seen
        // Assume nTblDht just points to DC tables, adjust for AC
        // e.g. nTblDht = 0,2,4
        eRsvRet = ReadScanVal(bDC ? 0 : 1, bDC ? nTblDhtDc : nTblDhtAc, nZrl, nVal);

        // Handle Restart marker first.
        if (eRsvRet == RSV_RST_TERM) {
            // Assume that m_bRestartRead is TRUE
            // No huffman code found because either we ran out of bits
            // in the scan buffer or the bits padded with 1's didn't result
            // in a valid VLC code.

            // Steps:
            //   1) Reset the decoder state (DC values)
            //   2) Advance the buffer pointer (might need to handle the
            //      case of perfect alignment to byte boundary separately)
            //   3) Flush the Scan Buffer
            //   4) Clear m_bRestartRead
            //   5) Refill Scan Buffer with BuffTopUp()
            //   6) Re-invoke ReadScanVal()

            // Step 1:
            DecodeRestartDcState();

            // Step 2
            m_nScanBuffPtr += 2;

            // Step 3
            DecodeRestartScanBuf(m_nScanBuffPtr, true);

            // Step 4
            m_bRestartRead = false;

            // Step 5
            BuffTopup();

            // Step 6
            // Q_ASSERT is because we assume that we don't get 2 restart
            // markers in a row!
            eRsvRet = ReadScanVal(bDC ? 0 : 1, bDC ? nTblDhtDc : nTblDhtAc, nZrl, nVal);
            Q_ASSERT(eRsvRet != RSV_RST_TERM);
        }

        // In case we encountered a restart marker or bad scan marker
        if (nSavedBufErr == SCANBUF_BADMARK) {
            // Mark as scan error
            m_nScanCurErr = true;

            m_bScanBad = true;

            if (m_nWarnBadScanNum < _scanErrMax) {
                QString strPos = getScanBufPos(nSavedBufPos, nSavedBufAlign);

                QString strTmp;

                strTmp = QString("*** ERROR: Bad marker @ %1").arg(strPos);
                _log.error(strTmp);

                m_nWarnBadScanNum++;

                if (m_nWarnBadScanNum >= _scanErrMax) {
                    strTmp = QString("    Only reported first %1 instances of this message...").arg(_scanErrMax);
                    _log.error(strTmp);
                }
            }

            // Reset the latched error now that we've dealt with it
            m_nScanBuffLatchErr = SCANBUF_OK;
        }

        int16_t nVal2;

        nVal2 = static_cast<int16_t>(nVal & 0xFFFF);

        if (eRsvRet == RSV_OK) {
            // DC entry is always one value only
            if (bDC) {
                DecodeIdctSet(nTblDqt, nNumCoeffs, nZrl, nVal2);        //CALZ
                bDC = false;            // Now we will be on AC comps
            } else {
                // We're on AC entry, so keep looping until
                // we have finished up to 63 entries
                // Set entry in table
                // PERFORMANCE:
                //   No noticeable difference if following is skipped
                if (_decodeScanAc) {
                    DecodeIdctSet(nTblDqt, nNumCoeffs, nZrl, nVal2);
                }
            }
        } else if (eRsvRet == RSV_EOB) {
            if (bDC) {
                DecodeIdctSet(nTblDqt, nNumCoeffs, nZrl, nVal2);        //CALZ
                // Now that we have finished the DC coefficient, start on AC coefficients
                bDC = false;
            } else {
                // Now that we have finished the AC coefficients, we are done
                bDone = true;
            }
        } else if (eRsvRet == RSV_UNDERFLOW) {
            // ERROR

            if (m_nWarnBadScanNum < _scanErrMax) {
                QString strPos = getScanBufPos(nSavedBufPos, nSavedBufAlign);

                QString strTmp;

                strTmp = QString("*** ERROR: Bad huffman code @ %1").arg(strPos);
                _log.error(strTmp);

                m_nWarnBadScanNum++;

                if (m_nWarnBadScanNum >= _scanErrMax) {
                    strTmp = QString("    Only reported first %1 instances of this message...").arg(_scanErrMax);
                    _log.error(strTmp);
                }
            }

            m_nScanCurErr = true;
            bDone = true;

            return false;
        }

        // Increment the number of coefficients
        nNumCoeffs += 1 + nZrl;

        // If we have filled out an entire 64 entries, then we move to
        // the next block without an EOB
        // NOTE: This is only 63 entries because we assume that we
        //       are doing the AC (DC was already bDone in a different pass)

        // FIXME: Would like to combine DC & AC in one pass so that
        // we don't end up having to use 2 tables. The check below will
        // also need to be changed to == 64.
        //
        // Currently, we will have to correct AC nNumCoeffs entries (in IDCT) to
        // be +1 to get real index, as we are ignoring DC position 0.

        if (nNumCoeffs == 64) {
            bDone = true;
        } else if (nNumCoeffs > 64) {
            // ERROR

            if (m_nWarnBadScanNum < _scanErrMax) {
                QString strTmp;

                QString strPos = getScanBufPos(nSavedBufPos, nSavedBufAlign);

                strTmp = QString("*** ERROR: @ %1, nNumCoeffs>64 [%2]").arg(strPos).arg(nNumCoeffs);
                _log.error(strTmp);

                m_nWarnBadScanNum++;

                if (m_nWarnBadScanNum >= _scanErrMax) {
                    strTmp = QString("    Only reported first %1 instances of this message...").arg(_scanErrMax);
                    _log.error(strTmp);
                }
            }

            m_nScanCurErr = true;
            m_bScanBad = true;
            bDone = true;

            nNumCoeffs = 64;          // Just to ensure we don't use an overrun value anywhere
        }
    }

    // We finished the MCU
    // Now calc the IDCT matrix

    // The following code needs to be very efficient.
    // A number of experiments have been carried out to determine
    // the magnitude of speed improvements through various settings
    // and IDCT methods:
    //
    // PERFORMANCE:
    //   Example file: canon_1dsmk2_
    //
    //   0:06       Turn off _decodeScanAc (so no array memset, etc.)
    //   0:10   _decodeScanAc=true, but DecodeIdctCalc() skipped
    //   0:26       _decodeScanAc=true and DecodeIdctCalcFixedpt()
    //   0:27       _decodeScanAc=true and DecodeIdctCalcFloat()

    if (_decodeScanAc) {
#ifdef IDCT_FIXEDPT
        DecodeIdctCalcFixedpt();
#else

        // TODO: Select appropriate conversion routine based on performance
        //              DecodeIdctCalcFloat(nDctMax);
        //              DecodeIdctCalcFloat(nNumCoeffs);
        //              DecodeIdctCalcFloat(m_nDctCoefMax);
        DecodeIdctCalcFloat(64);
        //              DecodeIdctCalcFloat(32);
#endif
    }

    return true;
}

// Decode a single component for one block of an MCU with printing
// used for the Detailed Decode functionality
// - Same as DecodeScanComp() but adds reporting of variable length codes (VLC)
//
// INPUT:
// - nTblDhtDc                                  = DHT table ID for DC component
// - nTblDhtAc                                  = DHT talbe ID for AC component
// - nMcuX                                              = Current MCU X coordinate (for reporting only)
// - nMcuY                                              = Current MCU Y coordinate (for reporting only)
// RETURN:
// - Indicate if the decode was successful
// POST:
// - m_nScanCurDc = DC component value
// PERFORMANCE:
// - As this routine is called for every MCU, it is important
//   for it to be efficient. However, we are in print mode, so
//   we can afford to be slower.
//
// FIXME: need to fix like DecodeScanComp() (ordering of exit conditions, etc.)
// FIXME: Consider adding checks for DHT table like in ReadScanVal()
//
bool ImgDecode::DecodeScanCompPrint(uint32_t nTblDhtDc, uint32_t nTblDhtAc, uint32_t nTblDqt, uint32_t nMcuX,
                                    uint32_t nMcuY) {
    bool bPrint = true;

    teRsvRet eRsvRet;

    QString strTmp;
    QString strTbl;
    QString strSpecial;
    QString strPos;

    uint32_t nZrl;
    int32_t nVal;

    bool bDone = false;
    bool bDC = true;              // Start with DC component

    if (bPrint) {
        switch (nTblDqt) {
            case 0:
                strTbl = "Lum";
                break;

            case 1:
                strTbl = "Chr(0)";      // Usually Cb
                break;

            case 2:
                strTbl = "Chr(1)";      // Usually Cr
                break;

            default:
                strTbl = "???";
                break;
        }

        strTmp = QString("    %1 (Tbl #%2), MCU=[%3,%4]").arg(strTbl).arg(nTblDqt).arg(nMcuX).arg(nMcuY);
        _log.info(strTmp);
    }

    uint32_t nNumCoeffs = 0;
    uint32_t nSavedBufPos = 0;
    uint32_t nSavedBufErr = SCANBUF_OK;
    uint32_t nSavedBufAlign = 0;

    DecodeIdctClear();

    while (!bDone) {
        BuffTopup();

        // Note that once we perform ReadScanVal(), then GetScanBufPos() will be
        // after the decoded VLC

        // Save old file position info in case we want accurate error positioning
        nSavedBufPos = m_anScanBuffPtr_pos[0];
        nSavedBufErr = m_nScanBuffLatchErr;
        nSavedBufAlign = m_nScanBuffPtr_align;

        // Return values:
        //      0 - OK
        //  1 - EOB
        //  2 - Overread error
        //  3 - No huffman code found, but restart marker seen
        eRsvRet = ReadScanVal(bDC ? 0 : 1, bDC ? nTblDhtDc : nTblDhtAc, nZrl, nVal);

        // Handle Restart marker first.
        if (eRsvRet == RSV_RST_TERM) {
            // Assume that m_bRestartRead is TRUE
            // No huffman code found because either we ran out of bits
            // in the scan buffer or the bits padded with 1's didn't result
            // in a valid VLC code.

            // Steps:
            //   1) Reset the decoder state (DC values)
            //   2) Advance the buffer pointer (might need to handle the
            //      case of perfect alignment to byte boundary separately)
            //   3) Flush the Scan Buffer
            //   4) Clear m_bRestartRead
            //   5) Refill Scan Buffer with BuffTopUp()
            //   6) Re-invoke ReadScanVal()

            // Step 1:
            DecodeRestartDcState();

            // Step 2
            m_nScanBuffPtr += 2;

            // Step 3
            DecodeRestartScanBuf(m_nScanBuffPtr, true);

            // Step 4
            m_bRestartRead = false;

            // Step 5
            BuffTopup();

            // Step 6
            // Q_ASSERT is because we assume that we don't get 2 restart
            // markers in a row!
            eRsvRet = ReadScanVal(bDC ? 0 : 1, bDC ? nTblDhtDc : nTblDhtAc, nZrl, nVal);
            Q_ASSERT(eRsvRet != RSV_RST_TERM);
        }

        // In case we encountered a restart marker or bad scan marker
        if (nSavedBufErr == SCANBUF_BADMARK) {
            // Mark as scan error
            m_nScanCurErr = true;

            m_bScanBad = true;

            if (m_nWarnBadScanNum < _scanErrMax) {
                strPos = getScanBufPos(nSavedBufPos, nSavedBufAlign);
                strTmp = QString("*** ERROR: Bad marker @ %1").arg(strPos);
                _log.error(strTmp);

                m_nWarnBadScanNum++;

                if (m_nWarnBadScanNum >= _scanErrMax) {
                    strTmp = QString("    Only reported first %1 instances of this message...").arg(_scanErrMax);
                    _log.error(strTmp);
                }
            }

            // Reset the latched error now that we've dealt with it
            m_nScanBuffLatchErr = SCANBUF_OK;
        }

        // Should this be before or after restart checks?
        uint32_t nCoeffStart = nNumCoeffs;
        uint32_t nCoeffEnd = nNumCoeffs + nZrl;

        int16_t nVal2;

        nVal2 = static_cast<int16_t>(nVal & 0xFFFF);

        if (eRsvRet == RSV_OK) {
            strSpecial.clear();
            // DC entry is always one value only
            // FIXME: Do I need nTblDqt == 4 as well?
            if (bDC) {
                DecodeIdctSet(nTblDqt, nNumCoeffs, nZrl, nVal2);
                bDC = false;            // Now we will be on AC comps
            } else {
                // We're on AC entry, so keep looping until
                // we have finished up to 63 entries
                // Set entry in table
                DecodeIdctSet(nTblDqt, nNumCoeffs, nZrl, nVal2);
            }
        } else if (eRsvRet == RSV_EOB) {
            if (bDC) {
                DecodeIdctSet(nTblDqt, nNumCoeffs, nZrl, nVal2);
                bDC = false;            // Now we will be on AC comps
            } else {
                bDone = true;
            }
            strSpecial = "EOB";
        } else if (eRsvRet == RSV_UNDERFLOW) {
            if (m_nWarnBadScanNum < _scanErrMax) {
                strSpecial = "ERROR";
                strPos = getScanBufPos(nSavedBufPos, nSavedBufAlign);

                strTmp = QString("*** ERROR: Bad huffman code @ %1").arg(strPos);
                _log.error(strTmp);

                m_nWarnBadScanNum++;

                if (m_nWarnBadScanNum >= _scanErrMax) {
                    strTmp = QString("    Only reported first %1 instances of this message...").arg(_scanErrMax);
                    _log.error(strTmp);
                }
            }

            m_nScanCurErr = true;
            bDone = true;

            // Print out before we leave
            if (bPrint) {
                reportVlc(nSavedBufPos, nSavedBufAlign, nZrl, nVal2, nCoeffStart, nCoeffEnd, strSpecial);
            }

            return false;
        }

        // Increment the number of coefficients
        nNumCoeffs += 1 + nZrl;
        // If we have filled out an entire 64 entries, then we move to
        // the next block without an EOB
        // NOTE: This is only 63 entries because we assume that we
        //       are doing the AC (DC was already done in a different pass)
        if (nNumCoeffs == 64) {
            strSpecial = "EOB64";
            bDone = true;
        } else if (nNumCoeffs > 64) {
            // ERROR

            if (m_nWarnBadScanNum < _scanErrMax) {
                strPos = getScanBufPos(nSavedBufPos, nSavedBufAlign);
                strTmp = QString("*** ERROR: @ %1, nNumCoeffs>64 [%2]").arg(strPos).arg(nNumCoeffs);
                _log.error(strTmp);

                m_nWarnBadScanNum++;

                if (m_nWarnBadScanNum >= _scanErrMax) {
                    strTmp = QString("    Only reported first %1 instances of this message...").arg(_scanErrMax);
                    _log.error(strTmp);
                }
            }

            m_nScanCurErr = true;
            m_bScanBad = true;
            bDone = true;

            nNumCoeffs = 64;          // Just to ensure we don't use an overrun value anywhere
        }

        if (bPrint) {
            reportVlc(nSavedBufPos, nSavedBufAlign, nZrl, nVal2, nCoeffStart, nCoeffEnd, strSpecial);
        }
    }

    // We finished the MCU component

    // Now calc the IDCT matrix
#ifdef IDCT_FIXEDPT
    DecodeIdctCalcFixedpt();
#else
    //      DecodeIdctCalcFloat(nNumCoeffs);
    DecodeIdctCalcFloat(64);
#endif

    // Now report the coefficient matrix (after zigzag reordering)
    if (bPrint) {
        reportDctMatrix();
    }

    return true;
}

// Print out the DCT matrix for a given block
//
// PRE:
// - m_anDctBlock[]
//
void ImgDecode::reportDctMatrix() {
    QString strTmp;
    QString strLine;

    int32_t nCoefVal;

    for (uint32_t nY = 0; nY < 8; nY++) {
        if (nY == 0) {
            strLine = "                      DCT Matrix=[";
        } else {
            strLine = "                                 [";
        }

        for (uint32_t nX = 0; nX < 8; nX++) {
            strTmp.clear();
            nCoefVal = m_anDctBlock[nY * 8 + nX];
            strTmp = QString("%1").arg(nCoefVal, 5);
            strLine.append(strTmp);

            if (nX != 7) {
                strLine.append(" ");
            }
        }

        strLine.append("]");
        _log.info(strLine);
    }

    _log.info("");
}

// Report out the variable length codes (VLC)
//
// Need to consider impact of padding bytes... When pads get extracted, they
// will not appear in the binary display shown below. Might want the get buffer
// to do post-pad removal first.
//
// eg. file Dsc0019.jpg
// Overlay 0x4215 = 7FFF0000 len=4
//
// INPUT:
// - nVlcPos                            =
// - nVlcAlign                          =
// - nZrl                                       =
// - nVal                                       =
// - nCoeffStart                        =
// - nCoeffEnd                          =
// - specialStr                         =
//
void ImgDecode::reportVlc(uint32_t nVlcPos, uint32_t nVlcAlign, uint32_t nZrl, int32_t nVal, uint32_t nCoeffStart, uint32_t nCoeffEnd, const QString &specialStr) {
    QString strPos;
    QString strTmp;

    uint32_t nBufByte[4];
    uint32_t nBufPosInd = nVlcPos;

    QString strData = "";
    QString strByte1 = "";
    QString strByte2 = "";
    QString strByte3 = "";
    QString strByte4 = "";
    QString strBytes = "";
    QString strBytesOrig = "";
    QString strBinMarked = "";

    strPos = getScanBufPos(nVlcPos, nVlcAlign);

    // Read in the buffer bytes, but skip pad bytes (0xFF00 -> 0xFF)

    // We need to look at previous byte as it might have been
    // start of stuff byte! If so, we need to ignore the byte
    // and advance the pointers.
    uint8_t nBufBytePre = _wbuf.getByte(nBufPosInd - 1);

    nBufByte[0] = _wbuf.getByte(nBufPosInd++);

    if ((nBufBytePre == 0xFF) && (nBufByte[0] == 0x00)) {
        nBufByte[0] = _wbuf.getByte(nBufPosInd++);
    }

    nBufByte[1] = _wbuf.getByte(nBufPosInd++);

    if ((nBufByte[0] == 0xFF) && (nBufByte[1] == 0x00)) {
        nBufByte[1] = _wbuf.getByte(nBufPosInd++);
    }

    nBufByte[2] = _wbuf.getByte(nBufPosInd++);

    if ((nBufByte[1] == 0xFF) && (nBufByte[2] == 0x00)) {
        nBufByte[2] = _wbuf.getByte(nBufPosInd++);
    }

    nBufByte[3] = _wbuf.getByte(nBufPosInd++);

    if ((nBufByte[2] == 0xFF) && (nBufByte[3] == 0x00)) {
        nBufByte[3] = _wbuf.getByte(nBufPosInd++);
    }

    strByte1 = Dec2Bin(nBufByte[0], 8, true);
    strByte2 = Dec2Bin(nBufByte[1], 8, true);
    strByte3 = Dec2Bin(nBufByte[2], 8, true);
    strByte4 = Dec2Bin(nBufByte[3], 8, true);
    strBytesOrig = strByte1 + " " + strByte2 + " " + strByte3 + " " + strByte4;
    strBytes = strByte1 + strByte2 + strByte3 + strByte4;

    for (uint32_t ind = 0; ind < nVlcAlign; ind++) {
        strBinMarked += "-";
    }

    strBinMarked += strBytes.mid(nVlcAlign, m_nScanBitsUsed1 + m_nScanBitsUsed2);

    for (uint32_t ind = nVlcAlign + m_nScanBitsUsed1 + m_nScanBitsUsed2; ind < 32; ind++) {
        strBinMarked += "-";
    }

    strBinMarked.insert(24, " ");
    strBinMarked.insert(16, " ");
    strBinMarked.insert(8, " ");

    strData = QString("0x %1 %2 %3 %4 = 0b (%5)")
        .arg(nBufByte[0], 2, 16, QChar('0'))
        .arg(nBufByte[1], 2, 16, QChar('0'))
        .arg(nBufByte[2], 2, 16, QChar('0'))
        .arg(nBufByte[3], 2, 16, QChar('0'))
        .arg(strBinMarked);

    if ((nCoeffStart == 0) && (nCoeffEnd == 0)) {
        strTmp = QString("      [%1]: ZRL=[%2] Val=[%3] Coef=[%4= DC] Data=[%5] %6")
            .arg(strPos)
            .arg(nZrl, 2)
            .arg(nVal, 5)
            .arg(nCoeffStart, 2, 10, QChar('0'))
            .arg(strData)
            .arg(specialStr);
    } else {
        strTmp = QString("      [%1]: ZRL=[%2] Val=[%3] Coef=[%4..%5] Data=[%6] %7")
            .arg(strPos)
            .arg(nZrl, 2)
            .arg(nVal, 5)
            .arg(nCoeffStart, 2, 10, QChar('0'))
            .arg(nCoeffEnd, 2, 10, QChar('0'))
            .arg(strData)
            .arg(specialStr);
    }

    _log.info(strTmp);
}

// Clear input and output matrix
//
// POST:
// - m_anDctBlock[]
// - m_afIdctBlock[]
// - m_anIdctBlock[]
// - m_nDctCoefMax
//
void ImgDecode::DecodeIdctClear() {
    memset(m_anDctBlock, 0, sizeof m_anDctBlock);
    memset(m_afIdctBlock, 0, sizeof m_afIdctBlock);
    memset(m_anIdctBlock, 0, sizeof m_anIdctBlock);

    m_nDctCoefMax = 0;
}

// Set the DCT matrix entry
// - Fills in m_anDctBlock[] with the unquantized coefficients
// - Reversing the quantization is done using m_anDqtCoeffZz[][]
//
// INPUT:
// - nDqtTbl                            =
// - num_coeffs                         =
// - zrl                                        =
// - val                                        =
// PRE:
// - glb_anZigZag[]
// - m_anDqtCoeffZz[][]
// - m_nDctCoefMax
// POST:
// - m_anDctBlock[]
// NOTE:
// - We need to convert between the zigzag order and the normal order
//
void ImgDecode::DecodeIdctSet(uint32_t nDqtTbl, uint32_t num_coeffs, uint32_t zrl, int16_t val) {
    uint32_t ind = num_coeffs + zrl;

    if (ind >= 64) {
        // We have an error! Don't set the block. Skip this comp for now
        // After this call, we will likely trap the error.
    } else {
        uint32_t nDctInd = glb_anZigZag[ind];

        int16_t nValUnquant = val * m_anDqtCoeffZz[nDqtTbl][ind];

        /*
       // NOTE:
       //  To test steganography analysis, we can experiment with dropping
       //  specific components of the image.
       uint32_t nRow = nDctInd/8;
       uint32_t nCol = nDctInd - (nRow*8);
       if ((nRow == 0) && (nCol>=0 && nCol<=7)) {
       nValUnquant = 0;
       }
     */

        m_anDctBlock[nDctInd] = nValUnquant;

        // Update max DCT coef # (after unzigzag) so that we can save
        // some work when performing IDCT.
        // FIXME: The following doesn't seem to work when we later
        // restrict DecodeIdctCalc() to only m_nDctCoefMax coefs!

        //              if ( (nDctInd > m_nDctCoefMax) && (abs(nValUnquant) >= IDCT_COEF_THRESH) ) {
        if (nDctInd > m_nDctCoefMax) {
            m_nDctCoefMax = nDctInd;
        }
    }
}

// Precalculate the IDCT lookup tables
//
// POST:
// - m_afIdctLookup[]
// - m_anIdctLookup[]
// NOTE:
// - This is 4k entries @ 4B each = 16KB
//
void ImgDecode::PrecalcIdct() {
    uint32_t nX, nY, nU, nV;

    uint32_t nYX, nVU;

    double fCu, fCv;
    double fCosProd;
    double fInsideProd;
    double fPi = 3.141592654;
    double fSqrtHalf = 0.707106781;

    for (nY = 0; nY < DCT_SZ_Y; nY++) {
        for (nX = 0; nX < DCT_SZ_X; nX++) {
            nYX = nY * DCT_SZ_X + nX;

            for (nV = 0; nV < DCT_SZ_Y; nV++) {
                for (nU = 0; nU < DCT_SZ_X; nU++) {

                    nVU = nV * DCT_SZ_X + nU;

                    fCu = (nU == 0) ? fSqrtHalf : 1;
                    fCv = (nV == 0) ? fSqrtHalf : 1;
                    fCosProd = cos((2 * nX + 1) * nU * fPi / 16) * cos((2 * nY + 1) * nV * fPi / 16);
                    // Note that the only part we are missing from
                    // the "Inside Product" is the "m_afDctBlock[nV*8+nU]" term
                    fInsideProd = fCu * fCv * fCosProd;

                    // Store the Lookup result
                    m_afIdctLookup[nYX][nVU] = fInsideProd;

                    // Store a fixed point Lookup as well
                    m_anIdctLookup[nYX][nVU] = static_cast<int32_t>(fInsideProd * (1 << 10));
                }
            }
        }
    }
}

// Perform IDCT
//
// Formula:
//  See itu-t81.pdf, section A.3.3
//
// s(yx) = 1/4*Sum(u=0..7)[ Sum(v=0..7)[ C(u) * C(v) * S(vu) *
//                     cos( (2x+1)*u*Pi/16 ) * cos( (2y+1)*v*Pi/16 ) ] ]
// Cu, Cv = 1/sqrt(2) for u,v=0
// Cu, Cv = 1 else
//
// INPUT:
// - nCoefMax                           = Maximum number of coefficients to calculate
// PRE:
// - m_afIdctLookup[][]
// - m_anDctBlock[]
// POST:
// - m_afIdctBlock[]
//
void ImgDecode::DecodeIdctCalcFloat(uint32_t nCoefMax) {
    uint32_t nYX, nVU;

    double fSum;

    for (nYX = 0; nYX < DCT_SZ_ALL; nYX++) {
        fSum = 0;

        // Skip DC coefficient!
        for (nVU = 1; nVU < nCoefMax; nVU++) {
            fSum += m_afIdctLookup[nYX][nVU] * m_anDctBlock[nVU];
        }

        fSum *= 0.25;

        // Store the result
        // FIXME: Note that float->int is very slow!
        //   Should consider using fixed point instead!
        m_afIdctBlock[nYX] = fSum;
    }
}

// Fixed point version of DecodeIdctCalcFloat()
//
// PRE:
// - m_anIdctLookup[][]
// - m_anDctBlock[]
// POST:
// - m_anIdctBlock[]
//
void ImgDecode::DecodeIdctCalcFixedpt() {
    uint32_t nYX, nVU;

    int32_t nSum;

    for (nYX = 0; nYX < DCT_SZ_ALL; nYX++) {
        nSum = 0;

        // Skip DC coefficient!
        for (nVU = 1; nVU < DCT_SZ_ALL; nVU++) {
            nSum += m_anIdctLookup[nYX][nVU] * m_anDctBlock[nVU];
        }

        nSum /= 4;

        // Store the result
        // FIXME: Note that float->int is very slow!
        //   Should consider using fixed point instead!
        m_anIdctBlock[nYX] = nSum >> 10;
    }
}

// Clear the entire pixel image arrays for all three components (YCC)
//
// INPUT:
// - nWidth                                     = Current allocated image width
// - nHeight                            = Current allocated image height
// POST:
// - m_pPixValY
// - m_pPixValCb
// - m_pPixValCr
//
void ImgDecode::ClrFullRes(int32_t nWidth, int32_t nHeight) {
    Q_ASSERT(m_pPixValY);

    if (m_nNumSosComps == NUM_CHAN_YCC) {
        Q_ASSERT(m_pPixValCb);
        Q_ASSERT(m_pPixValCr);
    }

    // FIXME: Add in range checking here
    memset(m_pPixValY, 0, (nWidth * nHeight * sizeof(int16_t)));

    if (m_nNumSosComps == NUM_CHAN_YCC) {
        memset(m_pPixValCb, 0, (nWidth * nHeight * sizeof(int16_t)));
        memset(m_pPixValCr, 0, (nWidth * nHeight * sizeof(int16_t)));
    }
}

// Generate a single component's pixel content for one MCU
// - Fetch content from the 8x8 IDCT block (m_afIdctBlock[])
//   for the specified component (nComp)
// - Transfer the pixel content to the specified component's
//   pixel map (m_pPixValY[],m_pPixValCb[],m_pPixValCr[])
// - DC level shifting is performed (nDcOffset)
// - Replication of pixels according to Chroma Subsampling (sampling factors)
//
// INPUT:
// - nMcuX                                      =
// - nMcuY                                      =
// - nComp                                      = Component index (1,2,3)
// - nCssXInd                           =
// - nCssYInd                           =
// - nDcOffset                          =
// PRE:
// - DecodeIdctCalc() already called on Lum AC, and Lum DC already done
//
void ImgDecode::SetFullRes(int32_t nMcuX, int32_t nMcuY, int32_t nComp, uint32_t nCssXInd, uint32_t nCssYInd,
                           int16_t nDcOffset) {
    uint32_t nYX;

    double fVal;

    int16_t nVal;

    int32_t nChan;

    // Convert from Component index (1-based) to Channel index (0-based)
    // Component index is direct from SOF/SOS
    // Channel index is used for internal display representation
    if (nComp <= 0) {
#ifdef DEBUG_LOG
        QString strTmp;
        QString strDebug;

        strTmp = QString("SetFullRes() with nComp <= 0 [%1]").arg(nComp);
        strDebug = QString("## File=[%1] Block=[%2] Error=[%3]\n")
            .arg(_appConfig.curFileName, -100)
            .arg("ImgDecode", -10)
            .arg(strTmp);
        _log.debug(strDebug);
#else
        Q_ASSERT(false);
#endif
        return;
    }

    nChan = nComp - 1;

    int32_t nPixMapW = m_nBlkXMax * BLK_SZ_X;    // Width of pixel map
    int32_t nOffsetBlkCorner;    // Linear offset to top-left corner of block
    int32_t nOffsetPixCorner;    // Linear offset to top-left corner of pixel (start point for expansion)

    // Calculate the linear pixel offset for the top-left corner of the block in the MCU
    nOffsetBlkCorner =
        ((nMcuY * m_nMcuHeight) + nCssYInd * BLK_SZ_X) * nPixMapW + ((nMcuX * m_nMcuWidth) + nCssXInd * BLK_SZ_Y);

    // Use the expansion factor to determine how many bits to replicate
    // Typically for luminance (Y) this will be 1 & 1
    // The replication factor is available in m_anExpandBitsMcuH[] and m_anExpandBitsMcuV[]

    // Step through all pixels in the block
    for (uint32_t nY = 0; nY < BLK_SZ_Y; nY++) {
        for (uint32_t nX = 0; nX < BLK_SZ_X; nX++) {
            nYX = nY * BLK_SZ_X + nX;

            // Fetch the pixel value from the IDCT 8x8 block and perform DC level shift
#ifdef IDCT_FIXEDPT
                                                                                                                                    nVal = m_anIdctBlock[nYX];
      // TODO: Why do I need AC value x8 multiplier?
      nVal = (nVal * 8) + nDcOffset;
#else
            fVal = m_afIdctBlock[nYX];
            // TODO: Why do I need AC value x8 multiplier?
            nVal = (static_cast<int16_t>(fVal * 8) + nDcOffset);
#endif

            // NOTE: These range checks were already done in DecodeScanImg()
            Q_ASSERT(nCssXInd < MAX_SAMP_FACT_H);
            Q_ASSERT(nCssYInd < MAX_SAMP_FACT_V);
            Q_ASSERT(nY < BLK_SZ_Y);
            Q_ASSERT(nX < BLK_SZ_X);

            // Set the pixel value for the component

            // We start with the linear offset into the pixel map for the top-left
            // corner of the block. Then we adjust to determine the top-left corner
            // of the pixel that we may optionally expand in subsampling scenarios.

            // Calculate the top-left corner pixel linear offset after taking
            // into account any expansion in the X direction
            nOffsetPixCorner = nOffsetBlkCorner + nX * m_anExpandBitsMcuH[nComp];

            // Replication the pixels as specified in the sampling factor
            // This is typically done for the chrominance channels when
            // chroma subsamping is used.
            for (uint32_t nIndV = 0; nIndV < m_anExpandBitsMcuV[nComp]; nIndV++) {
                for (uint32_t nIndH = 0; nIndH < m_anExpandBitsMcuH[nComp]; nIndH++) {
                    if (nChan == CHAN_Y) {
                        m_pPixValY[nOffsetPixCorner + (nIndV * nPixMapW) + nIndH] = nVal;
                    } else if (nChan == CHAN_CB) {
                        m_pPixValCb[nOffsetPixCorner + (nIndV * nPixMapW) + nIndH] = nVal;
                    } else if (nChan == CHAN_CR) {
                        m_pPixValCr[nOffsetPixCorner + (nIndV * nPixMapW) + nIndH] = nVal;
                    } else {
                        Q_ASSERT(false);
                    }
                }                       // nIndH
            }                         // nIndV
        }                           // nX

        nOffsetBlkCorner += (nPixMapW * m_anExpandBitsMcuV[nComp]);
    }                             // nY
}

// Calculates the actual byte offset (from start of file) for
// the current position in the m_nScanBuff.
//
// PRE:
// - m_anScanBuffPtr_pos[]
// - m_nScanBuffPtr_align
// RETURN:
// - File position
//
QString ImgDecode::getScanBufPos() {
    return getScanBufPos(m_anScanBuffPtr_pos[0], m_nScanBuffPtr_align);
}

// Generate a file position string that also indicates bit alignment
//
// INPUT:
// - pos                        = File position (byte)
// - align                      = File position (bit)
// RETURN:
// - Formatted string
//
QString ImgDecode::getScanBufPos(uint32_t pos, uint32_t align) {
    return QString("0x%1.%2")
        .arg(pos, 8, 16, QChar('0'))
        .arg(align);;
}

// Test the scan error flag and, if set, report out the position
//
// INPUT:
// - nMcuX                              = MCU x coordinate
// - nMcuY                              = MCU y coordinate
// - nCssIndH                   = Chroma subsampling (horizontal)
// - nCssIndV                   = Chroma subsampling (vertical)
// - nComp                              = Image component
//
void ImgDecode::CheckScanErrors(uint32_t nMcuX, uint32_t nMcuY, uint32_t nCssIndH, uint32_t nCssIndV, uint32_t nComp) {
    //uint32_t mcu_x_max = (m_nDimX/m_nMcuWidth);
    //uint32_t mcu_y_max = (m_nDimY/m_nMcuHeight);

    // Determine pixel position, taking into account sampling quadrant as well
    uint32_t err_pos_x = m_nMcuWidth * nMcuX + nCssIndH * BLK_SZ_X;

    uint32_t err_pos_y = m_nMcuHeight * nMcuY + nCssIndV * BLK_SZ_Y;

    if (m_nScanCurErr) {
        QString strTmp, errStr;

        // Report component and subsampling quadrant
        switch (nComp) {
            case SCAN_COMP_Y:
                strTmp = QString("Lum CSS(%1,%2)")
                    .arg(nCssIndH)
                    .arg(nCssIndV);
                break;

            case SCAN_COMP_CB:
                strTmp = QString("Chr(Cb) CSS(%1,%2)")
                    .arg(nCssIndH)
                    .arg(nCssIndV);
                break;

            case SCAN_COMP_CR:
                strTmp = QString("Chr(Cr) CSS(%1,%2)")
                    .arg(nCssIndH)
                    .arg(nCssIndV);
                break;

            default:
                // Unknown component
                strTmp = QString("??? CSS(%1,%2)")
                    .arg(nCssIndH)
                    .arg(nCssIndV);
                break;
        }

        if (m_nWarnBadScanNum < _scanErrMax) {
            errStr = QString("*** ERROR: Bad scan data in MCU(%1,%2): %3 @ Offset %4")
                .arg(nMcuX)
                .arg(nMcuY)
                .arg(strTmp)
                .arg(getScanBufPos());
            _log.error(errStr);
            errStr = QString("           MCU located at pixel=(%1, %2)")
                .arg(err_pos_x)
                .arg(err_pos_y);
            _log.error(errStr);

            //errStr = QString("*** Resetting Error state to continue ***");
            //m_pLog->AddLineErr(errStr);

            m_nWarnBadScanNum++;

            if (m_nWarnBadScanNum >= _scanErrMax) {
                strTmp = QString("    Only reported first %1 instances of this message...").arg(_scanErrMax);
                _log.error(strTmp);
            }
        }

        // TODO: Should we reset m_nScanCurErr?
        m_nScanCurErr = false;

        //errStr = QString("*** Resetting Error state to continue ***");
        //m_pLog->AddLineErr(errStr);
    }                             // Error?
}

// Reset the DC values in the decoder (e.g. at start and
// after restart markers)
//
// POST:
// - m_nDcLum
// - m_nDcChrCb
// - m_nDcChrCr
// - m_anDcLumCss[]
// - m_anDcChrCbCss[]
// - m_anDcChrCrCss[]
//
void ImgDecode::DecodeRestartDcState() {
    m_nDcLum = 0;
    m_nDcChrCb = 0;
    m_nDcChrCr = 0;

    for (uint32_t nInd = 0; nInd < MAX_SAMP_FACT_V * MAX_SAMP_FACT_H; nInd++) {
        m_anDcLumCss[nInd] = 0;
        m_anDcChrCbCss[nInd] = 0;
        m_anDcChrCrCss[nInd] = 0;
    }
}

// Process the entire scan segment and optionally render the image
// - Reset and clear the output structures
// - Loop through each MCU and read each component
// - Maintain running DC level accumulator
// - Call SetFullRes() to transfer IDCT output to YCC Pixel Map
//
// INPUT:
// - startPosition                             = File position at start of scan
// - display                                   = Generate a preview image?
// - quiet                                     = Disable output of certain messages during decode?
//
void ImgDecode::decodeScanImg(uint32_t startPosition, bool display, bool quiet) {
    _log.debug("ImgDecode::decodeScanImg Start");

    QString strTmp;

    auto dieOnFirstErr = false;  // FIXME: do we want this? It makes it less useful for corrupt jpegs

    int32_t nPixMapW = 0;
    int32_t nPixMapH = 0;

    // Reset the decoder state variables
    reset();

    _scanErrMax = _appConfig.maxDecodeError();
    _decodeScanAc = false;

    // Detect the scenario where the image component details haven't been set yet
    // The image details are set via SetImageDetails()
    if (!m_bImgDetailsSet) {
        _log.error("*** ERROR: Decoding image before Image components defined ***");
        return;
    }

    // Even though we support decoding of MAX_SOS_COMP_NS we limit the component flexibility further
    if ((m_nNumSosComps != NUM_CHAN_GRAYSCALE) && (m_nNumSosComps != NUM_CHAN_YCC)) {
        strTmp = QString("  NOTE: Number of SOS components not supported [%1]").arg(m_nNumSosComps);
        _log.warn(strTmp);
#ifndef DEBUG_YCCK
        return;
#endif
    }

    // Determine the maximum sampling factor and min sampling factor for this scan
    m_nSosSampFactHMax = 0;
    m_nSosSampFactVMax = 0;
    m_nSosSampFactHMin = 0xFF;
    m_nSosSampFactVMin = 0xFF;

    for (uint32_t nComp = 1; nComp <= m_nNumSosComps; nComp++) {
        m_nSosSampFactHMax = qMax(m_nSosSampFactHMax, m_anSofSampFactH[nComp]);
        m_nSosSampFactVMax = qMax(m_nSosSampFactVMax, m_anSofSampFactV[nComp]);
        m_nSosSampFactHMin = qMin(m_nSosSampFactHMin, m_anSofSampFactH[nComp]);
        m_nSosSampFactVMin = qMin(m_nSosSampFactVMin, m_anSofSampFactV[nComp]);
        Q_ASSERT(m_nSosSampFactHMin != 0);
        Q_ASSERT(m_nSosSampFactVMin != 0);
    }

    // ITU-T.81 clause A.2.2 "Non-interleaved order (Ns=1)"
    // - In some cases an image may have a single component in a scan but with sampling factors other than 1:
    //     Number of Img components = 1
    //       Component[1]: ID=0x01, Samp Fac=0x22 (Subsamp 1 x 1), Quant Tbl Sel=0x00 (Lum: Y)
    // - This could either be in a 3-component SOF with multiple 1-component SOS or a 1-component SOF (monochrome image)
    // - In general, grayscale images exhibit a sampling factor of 0x11
    // - Per ITU-T.81 A.2.2:
    //     When Ns = 1 (where Ns is the number of components in a scan), the order of data units
    //     within a scan shall be left-to-right and top-to-bottom, as shown in Figure A.2. This
    //     ordering applies whenever Ns = 1, regardless of the values of H1 and V1.
    // - Thus, instead of the usual decoding sequence for 0x22:
    //   [ 0 1 ] [ 4 5 ]
    //   [ 2 3 ] [ 6 7 ]
    // - The sequence for decode should be:
    //   [ 0 ] [ 1 ] [ 2 ] [ 3 ] [ 4 ] ...
    // - Which is equivalent to the non-subsampled ordering (ie. 0x11)
    // - Apply a correction for such images to remove the sampling factor
    if (m_nNumSosComps == 1) {
        // TODO: Need to confirm if component index needs to be looked up
        // in the case of multiple SOS or if [1] is the correct index
        if ((m_anSofSampFactH[1] != 1) || (m_anSofSampFactV[1] != 1)) {
            _log.warn("    Altering sampling factor for single component scan to 0x11");
        }

        m_anSofSampFactH[1] = 1;
        m_anSofSampFactV[1] = 1;
        m_nSosSampFactHMax = 1;
        m_nSosSampFactVMax = 1;
        m_nSosSampFactHMin = 1;
        m_nSosSampFactVMin = 1;
    }

    // Perform additional range checks
    if ((m_nSosSampFactHMax == 0) || (m_nSosSampFactVMax == 0) || (m_nSosSampFactHMax > MAX_SAMP_FACT_H)
        || (m_nSosSampFactVMax > MAX_SAMP_FACT_V)) {
        strTmp = QString("  NOTE: Degree of subsampling factor not supported [HMax=%1, VMax=%2]")
            .arg(m_nSosSampFactHMax)
            .arg(m_nSosSampFactVMax);
        _log.warn(strTmp);
        return;
    }

    // Calculate the MCU size for this scan. We do it here rather
    // than at the time of SOF (ie. SetImageDetails) for the reason
    // that under some circumstances we need to override the sampling
    // factor in single-component scans. This is done earlier.
    m_nMcuWidth = m_nSosSampFactHMax * BLK_SZ_X;
    m_nMcuHeight = m_nSosSampFactVMax * BLK_SZ_Y;

    // Calculate the number of bits to replicate when we generate the pixel map
    for (uint32_t nComp = 1; nComp <= m_nNumSosComps; nComp++) {
        m_anExpandBitsMcuH[nComp] = m_nSosSampFactHMax / m_anSofSampFactH[nComp];
        m_anExpandBitsMcuV[nComp] = m_nSosSampFactVMax / m_anSofSampFactV[nComp];
    }

    // Calculate the number of component samples per MCU
    for (uint32_t nComp = 1; nComp <= m_nNumSosComps; nComp++) {
        m_anSampPerMcuH[nComp] = m_anSofSampFactH[nComp];
        m_anSampPerMcuV[nComp] = m_anSofSampFactV[nComp];
    }

    // Determine the MCU ranges
    m_nMcuXMax = (m_nDimX / m_nMcuWidth);
    m_nMcuYMax = (m_nDimY / m_nMcuHeight);

    // Detect incomplete (partial) MCUs and round-up the MCU ranges if necessary.
    if ((m_nDimX % m_nMcuWidth) != 0) {
        m_nMcuXMax++;
    }

    if ((m_nDimY % m_nMcuHeight) != 0) {
        m_nMcuYMax++;
    }

    // Save the maximum 8x8 block dimensions
    m_nBlkXMax = m_nMcuXMax * m_nSosSampFactHMax;
    m_nBlkYMax = m_nMcuYMax * m_nSosSampFactVMax;

    // Ensure the image has a size
    if ((m_nBlkXMax == 0) || (m_nBlkYMax == 0)) {
        return;
    }

    // Set the decoded size and before scaling
    m_nImgSizeX = m_nMcuXMax * m_nMcuWidth;
    m_nImgSizeY = m_nMcuYMax * m_nMcuHeight;
    _log.debug(QString("ImgDecode::decodeScanImg ImgSizeX=%1 ImgSizeY=%2").arg(m_nImgSizeX).arg(m_nImgSizeY));

    // Determine decoding range
    int32_t nDecMcuRowStart;
    int32_t nDecMcuRowEnd;       // End to AC scan decoding
    int32_t nDecMcuRowEndFinal;  // End to general decoding

    nDecMcuRowStart = 0;
    nDecMcuRowEnd = m_nMcuYMax;
    nDecMcuRowEndFinal = m_nMcuYMax;

    // Limit the decoding range to valid range
    nDecMcuRowEnd = qMin(nDecMcuRowEnd, m_nMcuYMax);
    nDecMcuRowEndFinal = qMin(nDecMcuRowEndFinal, m_nMcuYMax);

    // Allocate the MCU File Map
    Q_ASSERT(m_pMcuFileMap == 0);
    m_pMcuFileMap = new uint32_t[m_nMcuYMax * m_nMcuXMax];
    memset(m_pMcuFileMap, 0, (m_nMcuYMax * m_nMcuXMax * sizeof(int32_t)));

    // Allocate the 8x8 Block DC Map
    m_pBlkDcValY = new int16_t[m_nBlkYMax * m_nBlkXMax];
    memset(m_pBlkDcValY, 0, (m_nBlkYMax * m_nBlkXMax * sizeof(int16_t)));

    if (m_nNumSosComps == NUM_CHAN_YCC) {
        m_pBlkDcValCb = new int16_t[m_nBlkYMax * m_nBlkXMax];
        memset(m_pBlkDcValCb, 0, (m_nBlkYMax * m_nBlkXMax * sizeof(int16_t)));

        m_pBlkDcValCr = new int16_t[m_nBlkYMax * m_nBlkXMax];
        memset(m_pBlkDcValCr, 0, (m_nBlkYMax * m_nBlkXMax * sizeof(int16_t)));
    }

    // Allocate the real YCC pixel Map
    nPixMapH = m_nBlkYMax * BLK_SZ_Y;
    nPixMapW = m_nBlkXMax * BLK_SZ_X;

    // Ensure no image allocated yet
    Q_ASSERT(m_pPixValY == nullptr);

    if (m_nNumSosComps == NUM_CHAN_YCC) {
        Q_ASSERT(m_pPixValCb == nullptr);
        Q_ASSERT(m_pPixValCr == nullptr);
    }

    // Allocate image (YCC)
    m_pPixValY = new int16_t[nPixMapW * nPixMapH];

    if (m_nNumSosComps == NUM_CHAN_YCC) {
        m_pPixValCb = new int16_t[nPixMapW * nPixMapH];
        m_pPixValCr = new int16_t[nPixMapW * nPixMapH];
    }

    // Reset pixel map
    if (display) {
        ClrFullRes(nPixMapW, nPixMapH);
    }

    // Reset the DC cumulative state
    DecodeRestartDcState();

    // Reset the scan buffer
    DecodeRestartScanBuf(startPosition, false);

    // Load the buffer
    if (!_wbuf.loadWindow(startPosition)) return;

    // Expect that we start with RST0
    m_nRestartExpectInd = 0;
    m_nRestartLastInd = 0;

    // Load the first data into the scan buffer
    // This is done so that the MCU map will already
    // have access to the data.
    BuffTopup();

    if (!quiet) {
        _log.info("*** Decoding SCAN Data ***");
        _log.info(QString("  OFFSET: 0x%1").arg(startPosition, 8, 16, QChar('0')));
    }

    // TODO: Might be more appropriate to check against m_nNumSosComps instead?
    if ((m_nNumSofComps != NUM_CHAN_GRAYSCALE) && (m_nNumSofComps != NUM_CHAN_YCC)) {
        _log.warn(QString("  Number of Image Components not supported [%1]").arg(m_nNumSofComps));
#ifndef DEBUG_YCCK
        return;
#endif
    }

    // Check DQT tables
    // We need to ensure that the DQT Table selection has already
    // been done (via a call from JfifDec to SetDqtTables() ).
    uint32_t nDqtTblY = 0;
    uint32_t nDqtTblCr = 0;
    uint32_t nDqtTblCb = 0;

#ifdef DEBUG_YCCK
    uint32_t nDqtTblK = 0;
#endif
    bool bDqtReady = true;

    for (uint32_t ind = 1; ind <= m_nNumSosComps; ind++) {
        if (m_anDqtTblSel[ind] < 0) {
            bDqtReady = false;
        }
    }

    if (!bDqtReady) {
        _log.error("*** ERROR: Decoding image before DQT Table Selection via JFIF_SOF ***");
        // TODO: Is more error handling required?
        return;
    } else {
        // FIXME: Not sure that we can always depend on the indices to appear in this order.
        // May need another layer of indirection to get at the frame image component index.
        nDqtTblY = m_anDqtTblSel[DQT_DEST_Y];
        nDqtTblCb = m_anDqtTblSel[DQT_DEST_CB];
        nDqtTblCr = m_anDqtTblSel[DQT_DEST_CR];
#ifdef DEBUG_YCCK
                                                                                                                                if(m_nNumSosComps == 4)
    {
      nDqtTblK = m_anDqtTblSel[DQT_DEST_K];
    }
#endif
    }

    // Now check DHT tables
    bool bDhtReady = true;

    uint32_t nDhtTblDcY, nDhtTblDcCb, nDhtTblDcCr;
    uint32_t nDhtTblAcY, nDhtTblAcCb, nDhtTblAcCr;

#ifdef DEBUG_YCCK
    uint32_t nDhtTblDcK, nDhtTblAcK;
#endif
    for (uint32_t nClass = DHT_CLASS_DC; nClass <= DHT_CLASS_AC; nClass++) {
        for (uint32_t nCompInd = 1; nCompInd <= m_nNumSosComps; nCompInd++) {
            if (m_anDhtTblSel[nClass][nCompInd] < 0) {
                bDhtReady = false;
            }
        }
    }

    // Ensure that the table has been defined already!
    uint32_t nSel;

    for (uint32_t nCompInd = 1; nCompInd <= m_nNumSosComps; nCompInd++) {
        // Check for DC DHT table
        nSel = m_anDhtTblSel[DHT_CLASS_DC][nCompInd];

        if (m_anDhtLookupSize[DHT_CLASS_DC][nSel] == 0) {
            bDhtReady = false;
        }

        // Check for AC DHT table
        nSel = m_anDhtTblSel[DHT_CLASS_AC][nCompInd];

        if (m_anDhtLookupSize[DHT_CLASS_AC][nSel] == 0) {
            bDhtReady = false;
        }
    }

    if (!bDhtReady) {
        _log.error("*** ERROR: Decoding image before DHT Table Selection via JFIF_SOS ***");
        // TODO: Is more error handling required here?
        return;
    } else {
        // Define the huffman table indices that are selected for each
        // image component index and class (AC,DC).
        //
        // No need to check if a table is valid here since we have
        // previously checked to ensure that all required tables exist.
        // NOTE: If the table has not been defined, then the index
        // will be 0xFFFFFFFF. ReadScanVal() will trap this with Q_ASSERT
        // should it ever be used.
        nDhtTblDcY = m_anDhtTblSel[DHT_CLASS_DC][COMP_IND_YCC_Y];
        nDhtTblAcY = m_anDhtTblSel[DHT_CLASS_AC][COMP_IND_YCC_Y];
        nDhtTblDcCb = m_anDhtTblSel[DHT_CLASS_DC][COMP_IND_YCC_CB];
        nDhtTblAcCb = m_anDhtTblSel[DHT_CLASS_AC][COMP_IND_YCC_CB];
        nDhtTblDcCr = m_anDhtTblSel[DHT_CLASS_DC][COMP_IND_YCC_CR];
        nDhtTblAcCr = m_anDhtTblSel[DHT_CLASS_AC][COMP_IND_YCC_CR];
#ifdef DEBUG_YCCK
                                                                                                                                nDhtTblDcK = m_anDhtTblSel[DHT_CLASS_DC][COMP_IND_YCC_K];
    nDhtTblAcK = m_anDhtTblSel[DHT_CLASS_AC][COMP_IND_YCC_K];
#endif
    }

    // Done checks

    // Inform if they are in AC+DC/DC mode
    if (!quiet) {
        if (_decodeScanAc) {
            _log.info("  Scan Decode Mode: Full IDCT (AC + DC)");
        } else {
            _log.info("  Scan Decode Mode: No IDCT (DC only)");
            _log.warn("Low-resolution DC component shown. Can decode full-res with [Options->Scan Segment->Full IDCT]");
        }

        _log.info("");
    }

    // Report any Buffer overlays
    _wbuf.reportOverlays(_log);

    m_nNumPixels = 0;

    // -----------------------------------------------------------------------
    // Process all scan MCUs
    // -----------------------------------------------------------------------

    for (uint32_t nMcuY = nDecMcuRowStart; nMcuY < nDecMcuRowEndFinal; nMcuY++) {
        // Set the statusbar text to Processing...
        strTmp = QString("Decoding Scan Data... Row %1 of %2 (%3%%)")
            .arg(nMcuY, 4, 10, QChar('0'))
            .arg(m_nMcuYMax, 4, 10, QChar('0'))
            .arg(nMcuY * 100.0 / m_nMcuYMax, 3, 'f', 0);
        setStatusText(strTmp);

        // TODO: Trap escape keypress here (or run as thread)

        bool bDscRet;               // Return value for DecodeScanComp()
        bool bScanStop = false;

        for (uint32_t nMcuX = 0; (nMcuX < m_nMcuXMax) && (!bScanStop); nMcuX++) {
            // Check to see if we should expect a restart marker!
            // FIXME: Should actually check to ensure that we do in
            // fact get a restart marker, and that it was the right one!
            if ((m_bRestartEn) && (m_nRestartMcusLeft == 0)) {
                /*
           if (m_bVerbose) {
           strTmp = QString("  Expect Restart interval elapsed @ %s"),GetScanBufPos();
           m_pLog->AddLine(strTmp);
           }
         */

                if (m_bRestartRead) {
                    /*
             // FIXME: Check for restart counter value match
             if (m_bVerbose) {
             strTmp = QString("  Restart marker matched");
             m_pLog->AddLine(strTmp);
             }
           */
                } else {
                    strTmp = QString("  Expect Restart interval elapsed @ %1").arg(getScanBufPos());
                    _log.info(strTmp);
                    strTmp = QString("    ERROR: Restart marker not detected");
                    _log.error(strTmp);
                }
                /*
           if (ExpectRestart()) {
           if (m_bVerbose) {
           strTmp = QString("  Restart marker detected");
           m_pLog->AddLine(strTmp);
           }
           } else {
           strTmp = QString("  ERROR: Restart marker expected but not found @ %s"),GetScanBufPos();
           m_pLog->AddLineErr(strTmp);
           }
         */

            }

            // To support a fast decode mode, allow for a subset of the
            // image to have DC+AC decoding, while the remainder is only DC decoding
            if ((nMcuY < nDecMcuRowStart) || (nMcuY > nDecMcuRowEnd)) {
                _decodeScanAc = false;
            } else {
                _decodeScanAc = false;
            }

            // Precalculate MCU matrix index
            uint32_t nMcuXY = nMcuY * m_nMcuXMax + nMcuX;

            // Mark the start of the MCU in the file map
            m_pMcuFileMap[nMcuXY] = packFileOffset(m_anScanBuffPtr_pos[0], m_nScanBuffPtr_align);

            // Is this an MCU that we want full printing of decode process?
            bool bVlcDump = false;

            uint32_t nRangeBase;
            uint32_t nRangeCur;

            if (m_bDetailVlc) {
                nRangeBase = (m_nDetailVlcY * m_nMcuXMax) + m_nDetailVlcX;
                nRangeCur = nMcuXY;

                if ((nRangeCur >= nRangeBase) && (nRangeCur < nRangeBase + m_nDetailVlcLen)) {
                    bVlcDump = true;
                }
            }

            // Luminance
            // If there is chroma subsampling, then this block will have (css_x * css_y) luminance blocks to process
            // We store them all in an array m_anDcLumCss[]

            // Give separator line between MCUs
            if (bVlcDump) {
                _log.info("");
            }

            // CSS array indices
            uint32_t nCssIndH;
            uint32_t nCssIndV;
            uint32_t nComp;

            // No need to reset the IDCT output matrix for this MCU
            // since we are going to be generating it here. This help
            // maintain performance.

            // --------------------------------------------------------------
            nComp = SCAN_COMP_Y;

            // Step through the sampling factors per image component
            // TODO: Could rewrite this to use single loop across each image component
            for (nCssIndV = 0; nCssIndV < m_anSampPerMcuV[nComp]; nCssIndV++) {
                for (nCssIndH = 0; nCssIndH < m_anSampPerMcuH[nComp]; nCssIndH++) {
                    if (!bVlcDump) {
                        bDscRet = DecodeScanComp(nDhtTblDcY, nDhtTblAcY, nDqtTblY, nMcuX, nMcuY);   // Lum DC+AC
                    } else {
                        bDscRet = DecodeScanCompPrint(nDhtTblDcY, nDhtTblAcY, nDqtTblY, nMcuX, nMcuY);      // Lum DC+AC
                    }

                    if (m_nScanCurErr) {
                        CheckScanErrors(nMcuX, nMcuY, nCssIndH, nCssIndV, nComp);
                    }

                    if (!bDscRet && dieOnFirstErr) {
                        return;
                    }

                    // The DCT Block matrix has already been dezigzagged
                    // and multiplied against quantization table entry
                    m_nDcLum += m_anDctBlock[DCT_COEFF_DC];

                    if (bVlcDump) {
                        //                                              PrintDcCumVal(nMcuX,nMcuY,m_nDcLum);
                    }

                    // Now take a snapshot of the current cumulative DC value
                    m_anDcLumCss[nCssIndV * MAX_SAMP_FACT_H + nCssIndH] = m_nDcLum;

                    // At this point we have one of the luminance comps
                    // fully decoded (with IDCT if enabled). The result is
                    // currently in the array: m_afIdctBlock[]
                    // The next step would be to move these elements into
                    // the 3-channel MCU image map

                    // Store the pixels associated with this channel into
                    // the full-res pixel map. IDCT has already been computed
                    // on the 8x8 (or larger) MCU block region.

#if 1
                    if (display) {
                        SetFullRes(nMcuX, nMcuY, nComp, nCssIndH, nCssIndV, m_nDcLum);
                    }
#else
                                                                                                                                            // FIXME
          // Temporarily experiment with trying to handle multiple scans
          // by converting sampling factor of luminance scan back to 1x1
          uint32_t nNewMcuX, nNewMcuY, nNewCssX, nNewCssY;

          if(nCssIndV == 0)
          {
            if(nMcuX < m_nMcuXMax / 2)
            {
              nNewMcuX = nMcuX;
              nNewMcuY = nMcuY;
              nNewCssY = 0;
            }
            else
            {
              nNewMcuX = nMcuX - (m_nMcuXMax / 2);
              nNewMcuY = nMcuY;
              nNewCssY = 1;
            }
            nNewCssX = nCssIndH;
            SetFullRes(nNewMcuX, nNewMcuY, SCAN_COMP_Y, nNewCssX, nNewCssY, m_nDcLum);
          }
          else
          {
            nNewMcuX = (nMcuX / 2) + 1;
            nNewMcuY = (nMcuY / 2);
            nNewCssX = nCssIndH;
            nNewCssY = nMcuY % 2;
          }
#endif

                    // ---------------

                    // TODO: Counting pixels makes assumption that luminance is
                    // not subsampled, so we increment by 64.
                    m_nNumPixels += BLK_SZ_X * BLK_SZ_Y;
                }
            }

            // In a grayscale image, we don't do this part!
            //if (m_nNumSofComps == NUM_CHAN_YCC) {
            if (m_nNumSosComps == NUM_CHAN_YCC) {
                // --------------------------------------------------------------
                nComp = SCAN_COMP_CB;

                // Chrominance Cb
                for (nCssIndV = 0; nCssIndV < m_anSampPerMcuV[nComp]; nCssIndV++) {
                    for (nCssIndH = 0; nCssIndH < m_anSampPerMcuH[nComp]; nCssIndH++) {
                        if (!bVlcDump) {
                            bDscRet = DecodeScanComp(nDhtTblDcCb,
                                                     nDhtTblAcCb,
                                                     nDqtTblCb,
                                                     nMcuX,
                                                     nMcuY);      // Chr Cb DC+AC
                        } else {
                            bDscRet = DecodeScanCompPrint(nDhtTblDcCb,
                                                          nDhtTblAcCb,
                                                          nDqtTblCb,
                                                          nMcuX,
                                                          nMcuY); // Chr Cb DC+AC
                        }

                        if (m_nScanCurErr) {
                            CheckScanErrors(nMcuX, nMcuY, nCssIndH, nCssIndV, nComp);
                        }

                        if (!bDscRet && dieOnFirstErr) {
                            return;
                        }

                        m_nDcChrCb += m_anDctBlock[DCT_COEFF_DC];

                        if (bVlcDump) {
                            //PrintDcCumVal(nMcuX,nMcuY,m_nDcChrCb);
                        }

                        // Now take a snapshot of the current cumulative DC value
                        m_anDcChrCbCss[nCssIndV * MAX_SAMP_FACT_H + nCssIndH] = m_nDcChrCb;

                        // Store fullres value
                        if (display) {
                            SetFullRes(nMcuX, nMcuY, nComp, nCssIndH, nCssIndV, m_nDcChrCb);
                        }
                    }
                }

                // --------------------------------------------------------------
                nComp = SCAN_COMP_CR;

                // Chrominance Cr
                for (nCssIndV = 0; nCssIndV < m_anSampPerMcuV[nComp]; nCssIndV++) {
                    for (nCssIndH = 0; nCssIndH < m_anSampPerMcuH[nComp]; nCssIndH++) {
                        if (!bVlcDump) {
                            bDscRet = DecodeScanComp(nDhtTblDcCr,
                                                     nDhtTblAcCr,
                                                     nDqtTblCr,
                                                     nMcuX,
                                                     nMcuY);      // Chr Cr DC+AC
                        } else {
                            bDscRet = DecodeScanCompPrint(nDhtTblDcCr,
                                                          nDhtTblAcCr,
                                                          nDqtTblCr,
                                                          nMcuX,
                                                          nMcuY); // Chr Cr DC+AC
                        }

                        if (m_nScanCurErr) {
                            CheckScanErrors(nMcuX, nMcuY, nCssIndH, nCssIndV, nComp);
                        }

                        if (!bDscRet && dieOnFirstErr) {
                            return;
                        }

                        m_nDcChrCr += m_anDctBlock[DCT_COEFF_DC];

                        if (bVlcDump) {
                            //PrintDcCumVal(nMcuX,nMcuY,m_nDcChrCr);
                        }

                        // Now take a snapshot of the current cumulative DC value
                        m_anDcChrCrCss[nCssIndV * MAX_SAMP_FACT_H + nCssIndH] = m_nDcChrCr;

                        // Store fullres value
                        if (display) {
                            SetFullRes(nMcuX, nMcuY, nComp, nCssIndH, nCssIndV, m_nDcChrCr);
                        }
                    }
                }
            }

#ifdef DEBUG_YCCK
                                                                                                                                    else if(m_nNumSosComps == NUM_CHAN_YCCK)
      {
        // --------------------------------------------------------------
        nComp = SCAN_COMP_CB;

        // Chrominance Cb
        for(nCssIndV = 0; nCssIndV < m_anSampPerMcuV[nComp]; nCssIndV++)
        {
          for(nCssIndH = 0; nCssIndH < m_anSampPerMcuH[nComp]; nCssIndH++)
          {
            if(!bVlcDump)
            {
              bDscRet = DecodeScanComp(nDhtTblDcCb, nDhtTblAcCb, nDqtTblCb, nMcuX, nMcuY);      // Chr Cb DC+AC
            }
            else
            {
              bDscRet = DecodeScanCompPrint(nDhtTblDcCb, nDhtTblAcCb, nDqtTblCb, nMcuX, nMcuY); // Chr Cb DC+AC
            }

            if(m_nScanCurErr)
            {
              CheckScanErrors(nMcuX, nMcuY, nCssIndH, nCssIndV, nComp);
            }

            if(!bDscRet && bDieOnFirstErr)
            {
              return;
            }

            m_nDcChrCb += m_anDctBlock[DCT_COEFF_DC];

            if(bVlcDump)
            {
              //PrintDcCumVal(nMcuX,nMcuY,m_nDcChrCb);
            }

            // Now take a snapshot of the current cumulative DC value
            m_anDcChrCbCss[nCssIndV * MAX_SAMP_FACT_H + nCssIndH] = m_nDcChrCb;

            // Store fullres value
            if(display)
            {
              SetFullRes(nMcuX, nMcuY, nComp, 0, 0, m_nDcChrCb);
            }
          }
        }

        // --------------------------------------------------------------
        nComp = SCAN_COMP_CR;

        // Chrominance Cr
        for(nCssIndV = 0; nCssIndV < m_anSampPerMcuV[nComp]; nCssIndV++)
        {
          for(nCssIndH = 0; nCssIndH < m_anSampPerMcuH[nComp]; nCssIndH++)
          {
            if(!bVlcDump)
            {
              bDscRet = DecodeScanComp(nDhtTblDcCr, nDhtTblAcCr, nDqtTblCr, nMcuX, nMcuY);      // Chr Cr DC+AC
            }
            else
            {
              bDscRet = DecodeScanCompPrint(nDhtTblDcCr, nDhtTblAcCr, nDqtTblCr, nMcuX, nMcuY); // Chr Cr DC+AC
            }

            if(m_nScanCurErr)
              CheckScanErrors(nMcuX, nMcuY, nCssIndH, nCssIndV, nComp);

            if(!bDscRet && bDieOnFirstErr)
              return;

            m_nDcChrCr += m_anDctBlock[DCT_COEFF_DC];

            if(bVlcDump)
            {
              //PrintDcCumVal(nMcuX,nMcuY,m_nDcChrCr);
            }

            // Now take a snapshot of the current cumulative DC value
            m_anDcChrCrCss[nCssIndV * MAX_SAMP_FACT_H + nCssIndH] = m_nDcChrCr;

            // Store fullres value
            if(display)
              SetFullRes(nMcuX, nMcuY, nComp, 0, 0, m_nDcChrCr);
          }
        }

        // --------------------------------------------------------------
        // IGNORED
        nComp = SCAN_COMP_K;

        // Black K
        for(nCssIndV = 0; nCssIndV < m_anSampPerMcuV[nComp]; nCssIndV++)
        {
          for(nCssIndH = 0; nCssIndH < m_anSampPerMcuH[nComp]; nCssIndH++)
          {

            if(!bVlcDump)
            {
              bDscRet = DecodeScanComp(nDhtTblDcK, nDhtTblAcK, nDqtTblK, nMcuX, nMcuY); // K DC+AC
            }
            else
            {
              bDscRet = DecodeScanCompPrint(nDhtTblDcK, nDhtTblAcK, nDqtTblK, nMcuX, nMcuY);    // K DC+AC
            }

            if(m_nScanCurErr)
              CheckScanErrors(nMcuX, nMcuY, nCssIndH, nCssIndV, nComp);

            if(!bDscRet && bDieOnFirstErr)
              return;

/*
						m_nDcChrK += m_anDctBlock[DCT_COEFF_DC];

						if (bVlcDump) {
							//PrintDcCumVal(nMcuX,nMcuY,m_nDcChrCb);
						}

						// Now take a snapshot of the current cumulative DC value
						m_anDcChrKCss[nCssIndV*MAX_SAMP_FACT_H+nCssIndH] = m_nDcChrK;

						// Store fullres value
						if (display)
							SetFullRes(nMcuX,nMcuY,nComp,0,0,m_nDcChrK);
*/

          }
        }
      }
#endif
            // --------------------------------------------------------------------

            uint32_t nBlkXY;

            // Now save the DC YCC values (expanded per 8x8 block)
            // without ranging or translation into RGB.
            //
            // We enter this code once per MCU so we need to expand
            // out to cover all blocks in this MCU.

            // --------------------------------------------------------------
            nComp = SCAN_COMP_Y;

            // Calculate top-left corner of MCU in block map
            // and then linear offset into block map
            uint32_t nBlkCornerMcuX, nBlkCornerMcuY, nBlkCornerMcuLinear;

            nBlkCornerMcuX = nMcuX * m_anExpandBitsMcuH[nComp];
            nBlkCornerMcuY = nMcuY * m_anExpandBitsMcuV[nComp];
            nBlkCornerMcuLinear = (nBlkCornerMcuY * m_nBlkXMax) + nBlkCornerMcuX;

            // Now step through each block in the MCU per subsampling
            for (nCssIndV = 0; nCssIndV < m_anSampPerMcuV[nComp]; nCssIndV++) {
                for (nCssIndH = 0; nCssIndH < m_anSampPerMcuH[nComp]; nCssIndH++) {
                    // Calculate upper-left Blk index
                    // FIXME: According to code analysis the following write assignment
                    // to m_pBlkDcValY[] can apparently exceed the buffer bounds (C6386).
                    // I have not yet determined what scenario can lead to
                    // this. So for now, add in specific clause to trap and avoid.
                    nBlkXY = nBlkCornerMcuLinear + (nCssIndV * m_nBlkXMax) + nCssIndH;

                    // FIXME: Temporarily catch any range issue
                    if (nBlkXY >= m_nBlkXMax * m_nBlkYMax) {
#ifdef DEBUG_LOG
                        QString strDebug;

                        strTmp = QString(
                            "decodeScanImg() with nBlkXY out of range. nBlkXY=[%1] m_nBlkXMax=[%2] m_nBlkYMax=[%3]")
                            .arg(nBlkXY)
                            .arg(m_nBlkXMax)
                            .arg(m_nBlkYMax);
                        strDebug = QString("## File=[%1] Block=[%2] Error=[%3]\n")
                            .arg(_appConfig.curFileName, -100)
                            .arg("ImgDecode", -10).arg(strTmp);
                        _log.debug(strDebug);
#else
                        Q_ASSERT(false);
#endif
                    } else {
                        m_pBlkDcValY[nBlkXY] = m_anDcLumCss[nCssIndV * MAX_SAMP_FACT_H + nCssIndH];
                    }
                }
            }

            // Only process the chrominance if it is YCC
            if (m_nNumSosComps == NUM_CHAN_YCC) {
                // --------------------------------------------------------------
                nComp = SCAN_COMP_CB;

                for (nCssIndV = 0; nCssIndV < m_anSampPerMcuV[nComp]; nCssIndV++) {
                    for (nCssIndH = 0; nCssIndH < m_anSampPerMcuH[nComp]; nCssIndH++) {
                        // Calculate upper-left Blk index
                        nBlkXY = (nMcuY * m_anExpandBitsMcuV[nComp] + nCssIndV) * m_nBlkXMax +
                                 (nMcuX * m_anExpandBitsMcuH[nComp] + nCssIndH);

                        // FIXME: Temporarily catch any range issue
                        if (nBlkXY >= m_nBlkXMax * m_nBlkYMax) {
#ifdef DEBUG_LOG
                            QString strDebug;

                            strTmp = QString(
                                "decodeScanImg() with nBlkXY out of range. nBlkXY=[%1] m_nBlkXMax=[%2] m_nBlkYMax=[%3]").arg(
                                nBlkXY).arg(m_nBlkXMax).arg(m_nBlkYMax);
                            strDebug = QString("## File=[%1] Block=[%2] Error=[%3]\n").arg(_appConfig.curFileName,
                                                                                           -100).arg("ImgDecode",
                                                                                                     -10).arg(strTmp);
                            _log.debug(strDebug);
#else
                            Q_ASSERT(false);
#endif
                        } else {
                            m_pBlkDcValCb[nBlkXY] = m_anDcChrCbCss[nCssIndV * MAX_SAMP_FACT_H + nCssIndH];
                        }
                    }
                }

                // --------------------------------------------------------------
                nComp = SCAN_COMP_CR;

                for (nCssIndV = 0; nCssIndV < m_anSampPerMcuV[nComp]; nCssIndV++) {
                    for (nCssIndH = 0; nCssIndH < m_anSampPerMcuH[nComp]; nCssIndH++) {
                        // Calculate upper-left Blk index
                        nBlkXY = (nMcuY * m_anExpandBitsMcuV[nComp] + nCssIndV) * m_nBlkXMax +
                                 (nMcuX * m_anExpandBitsMcuH[nComp] + nCssIndH);

                        // FIXME: Temporarily catch any range issue
                        if (nBlkXY >= m_nBlkXMax * m_nBlkYMax) {
#ifdef DEBUG_LOG
                            QString strDebug;

                            strTmp = QString(
                                "decodeScanImg() with nBlkXY out of range. nBlkXY=[%1] m_nBlkXMax=[%2] m_nBlkYMax=[%3]").arg(
                                nBlkXY).arg(m_nBlkXMax).arg(m_nBlkYMax);
                            strDebug = QString("## File=[%1] Block=[%2] Error=[%3]\n").arg(_appConfig.curFileName,
                                                                                           -100).arg("ImgDecode",
                                                                                                     -10).arg(strTmp);
                            _log.debug(strDebug);
#else
                            Q_ASSERT(false);
#endif
                        } else {
                            m_pBlkDcValCr[nBlkXY] = m_anDcChrCrCss[nCssIndV * MAX_SAMP_FACT_H + nCssIndH];
                        }
                    }
                }
            }

            // Now that we finished an MCU, decrement the restart interval counter
            if (m_bRestartEn) {
                m_nRestartMcusLeft--;
            }

            // Check to see if we need to abort for some reason.
            // Note that only check m_bScanEnd if we have a failure.
            // m_bScanEnd is Q_ASSERTed during normal out-of-data when scan
            // segment ends with marker. We don't want to abort early
            // or else we'll not decode the last MCU or two!
            if (m_bScanEnd && m_bScanBad) {
                bScanStop = true;
            }
        }                           // nMcuX
    }                             // nMcuY

    if (!quiet) {
        _log.info("");
    }

    // ------------------------------------
    // Report statistics

    if (!quiet) {
        // Report Compression stats
        // TODO: Should we use m_nNumSofComps?
        strTmp = QString("  Compression stats:");
        _log.info(strTmp);
        double nCompressionRatio =
            static_cast<double>(m_nDimX * m_nDimY * m_nNumSosComps * 8) /
            static_cast<double>((m_anScanBuffPtr_pos[0] - m_nScanBuffPtr_first) * 8);
        strTmp = QString("    Compression Ratio: %1:1").arg(nCompressionRatio, 5, 'f', 2);
        _log.info(strTmp);

        double nBitsPerPixel =
            static_cast<double>((m_anScanBuffPtr_pos[0] - m_nScanBuffPtr_first) * 8) /
            static_cast<double>(m_nDimX * m_nDimY);

        strTmp = QString("    Bits per pixel:    %1:1").arg(nBitsPerPixel, 5, 'f', 2);
        _log.info(strTmp);
        _log.info("");

        // Report Huffman stats
        strTmp = QString("  Huffman code histogram stats:");
        _log.info(strTmp);

        uint32_t nDhtHistoTotal;

        for (uint32_t nClass = DHT_CLASS_DC; nClass <= DHT_CLASS_AC; nClass++) {
            for (uint32_t nDhtDestId = 0; nDhtDestId <= m_anDhtLookupSetMax[nClass]; nDhtDestId++) {
                nDhtHistoTotal = 0;

                for (uint32_t nBitLen = 1; nBitLen <= MAX_DHT_CODELEN; nBitLen++) {
                    nDhtHistoTotal += m_anDhtHisto[nClass][nDhtDestId][nBitLen];
                }

                strTmp = QString("    Huffman Table: (Dest ID: %1, Class: %2)").arg(nDhtDestId).arg(nClass ? "AC"
                                                                                                           : "DC");
                _log.info(strTmp);

                for (uint32_t nBitLen = 1; nBitLen <= MAX_DHT_CODELEN; nBitLen++) {
                    strTmp = QString("      # codes of length %1 bits: %2 (%3%)")
                        .arg(nBitLen, 2, 10, QChar('0'))
                        .arg(m_anDhtHisto[nClass][nDhtDestId][nBitLen], 8)
                        .arg((m_anDhtHisto[nClass][nDhtDestId][nBitLen] * 100.0) / nDhtHistoTotal, 3, 'f', 0);
                    _log.info(strTmp);
                }

                _log.info("");
            }
        }
    }

    if (!quiet) {
        _log.info("  Finished Decoding SCAN Data");
        strTmp = QString("    Number of RESTART markers decoded: %1").arg(m_nRestartRead);
        _log.info(strTmp);
        strTmp = QString("    Next position in scan buffer: Offset %1").arg(getScanBufPos());
        _log.info(strTmp);
        _log.info("");
    }
}

// Reset the decoder Scan Buff (at start of scan and
// after any restart markers)
//
// INPUT:
// - nFilePos                                   = File position at start of scan
// - bRestart                                   = Is this a reset due to RSTn marker?
// PRE:
// - m_nRestartInterval
// POST:
// - m_bScanEnd
// - m_bScanBad
// - m_nScanBuff
// - m_nScanBuffPtr_first
// - m_nScanBuffPtr_start
// - m_nScanBuffPtr_align
// - m_anScanBuffPtr_pos[]
// - m_anScanBuffPtr_err[]
// - m_nScanBuffLatchErr
// - m_nScanBuffPtr_num
// - m_nScanBuff_vacant
// - m_nScanCurErr
// - m_bRestartRead
// - m_nRestartMcusLeft
//
void ImgDecode::DecodeRestartScanBuf(uint32_t nFilePos, bool bRestart) {
    // Reset the state
    m_bScanEnd = false;
    m_bScanBad = false;
    m_nScanBuff = 0x00000000;
    m_nScanBuffPtr = nFilePos;

    if (!bRestart) {
        // Only reset the scan buffer pointer at the start of the file,
        // not after any RSTn markers. This is only used for the compression
        // ratio calculations.
        m_nScanBuffPtr_first = nFilePos;
    }

    m_nScanBuffPtr_start = nFilePos;
    m_nScanBuffPtr_align = 0;     // Start with byte alignment (0)
    m_anScanBuffPtr_pos[0] = 0;
    m_anScanBuffPtr_pos[1] = 0;
    m_anScanBuffPtr_pos[2] = 0;
    m_anScanBuffPtr_pos[3] = 0;
    m_anScanBuffPtr_err[0] = SCANBUF_OK;
    m_anScanBuffPtr_err[1] = SCANBUF_OK;
    m_anScanBuffPtr_err[2] = SCANBUF_OK;
    m_anScanBuffPtr_err[3] = SCANBUF_OK;
    m_nScanBuffLatchErr = SCANBUF_OK;

    m_nScanBuffPtr_num = 0;       // Empty m_nScanBuff
    m_nScanBuff_vacant = 32;

    m_nScanCurErr = false;

    //
    m_nScanBuffPtr = nFilePos;

    // Reset RST Interval checking
    m_bRestartRead = false;
    m_nRestartMcusLeft = m_nRestartInterval;
}

// Create a file offset notation that represents bytes and bits
// - Essentially a fixed-point notation
//
// INPUT:
// - nByte                            = File byte position
// - nBit                               = File bit position
// RETURN:
// - Fixed-point file offset (29b for bytes, 3b for bits)
//
uint32_t ImgDecode::packFileOffset(uint32_t nByte, uint32_t nBit) {
    uint32_t nTmp;

    // Note that we only really need 3 bits, but I'll keep 4
    // so that the file offset is human readable. We will only
    // handle files up to 2^28 bytes (256MB), so this is probably
    // fine!
    nTmp = (nByte << 4) + nBit;
    return nTmp;
}

// Convert from file offset notation to bytes and bits
//
// INPUT:
// - nPacked                    = Fixed-point file offset (29b for bytes, 3b for bits)
// OUTPUT:
// - nByte                            = File byte position
// - nBit                               = File bit position
//
void ImgDecode::unpackFileOffset(uint32_t nPacked, uint32_t &nByte, uint32_t &nBit) {
    nBit = nPacked & 0x7;
    nByte = nPacked >> 4;
}

// Fetch the number of block markers assigned
//
// RETURN:
// - Number of marker blocks
//
uint32_t ImgDecode::GetMarkerCount() {
    return m_nMarkersBlkNum;
}

void ImgDecode::setStatusFilePosText(const QString &text) {
    m_strStatusFilePos = text;
}

const QString &ImgDecode::getStatusFilePosText() const {
    return m_strStatusFilePos;
}
