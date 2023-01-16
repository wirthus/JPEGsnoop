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
// - Application configuration structures
// - Registry management
// - Note that the majority of this class is defined with public members
//
// ==========================================================================

#pragma once

#ifndef JPEGSNOOP_SNOOPCONFIG_H
#define JPEGSNOOP_SNOOPCONFIG_H

#include "Snoop.h"

#include <QSettings>
#include <QTextStream>

class SnoopConfig : public QObject {
Q_OBJECT
    Q_DISABLE_COPY(SnoopConfig)

public:
    explicit SnoopConfig(QObject *parent = nullptr);
    ~SnoopConfig() override = default;

    int32_t maxDecodeError() const { return _errMaxDecodeScan; }

    bool decodeAc() const { return _decodeScanImgAc; }

    bool decodeImage() const { return _decodeScanImg; }

    bool decodeMaker() const { return _decodeMaker; }

    bool expandDht() const { return _outputDhtExpand; }

    bool hideUnknownExif() const { return _exifHideUnknown; }

    bool relaxedParsing() const { return _relaxedParsing; }

    bool scanDump() const { return _outputScanDump; }

    bool searchSig() const { return _sigSearch; }

private:
    QString _cmdLineOpenFileName;   // input filename
    QString _cmdLineOutputFileName; // output filename
    QString _cmdLineBatchDirName;   // directory path for batch job

    // Registry Configuration options
    QString _dbDir;                // Directory for User DB
    int _errMaxDecodeScan;         // Max # errs to show in scan decode
    bool _sigSearch;               // Automatically search for comp signatures
    bool _decodeScanImg;           // Scan image decode enabled
    bool _decodeScanImgAc;         // When scan image decode, do full AC
    bool _outputScanDump;          // Do we dump a portion of scan data?
    bool _outputDhtExpand;
    bool _decodeMaker;
    bool _exifHideUnknown;         // Hide unknown exif tags?
    bool _relaxedParsing;          // Proceed despite bad marker / format?

    // Extra config (not in registry)
    // bool _decodeColorConvert;     // Do we do color convert after scan decode?

    // Debug log
    // - Used if DEBUG_LOG_OUT
    // bool _debugLogEnable;
    // QString _debugLogFileName;
    // QTextStream *_debugLogStream;
};

#endif
