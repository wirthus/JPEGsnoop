#pragma once

#ifndef JPEGSNOOP_SNOOPCONFIG_H
#define JPEGSNOOP_SNOOPCONFIG_H

#include <string>

#include "Snoop.h"

class SnoopConfig final {
    Q_DISABLE_COPY(SnoopConfig)
public:
    explicit SnoopConfig();
    ~SnoopConfig() = default;

    int32_t maxDecodeError() const { return _errMaxDecodeScan; }

    bool decodeImage() const { return _decodeScanImg; }

    bool decodeMaker() const { return _decodeMaker; }

    bool expandDht() const { return _outputDhtExpand; }

    bool hideUnknownExif() const { return _exifHideUnknown; }

    bool relaxedParsing() const { return _relaxedParsing; }

    bool scanDump() const { return _outputScanDump; }

private:
    int _errMaxDecodeScan;         // Max # errs to show in scan decode
    bool _decodeScanImg;           // Scan image decode enabled
    bool _outputScanDump;          // Do we dump a portion of scan data?
    bool _outputDhtExpand;
    bool _decodeMaker;
    bool _exifHideUnknown;         // Hide unknown exif tags?
    bool _relaxedParsing;          // Proceed despite bad marker / format?
};

#endif
