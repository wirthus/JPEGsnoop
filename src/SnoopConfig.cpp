#include "SnoopConfig.h"

SnoopConfig::SnoopConfig() {
    _decodeScanImg = false;

    _outputScanDump = false;      // Print snippet of scan data
    _outputDhtExpand = false;     // Print expanded huffman tables
    _decodeMaker = true;

    _exifHideUnknown = true;      // Default to hiding unknown EXIF tags
    _relaxedParsing = true;       // Normal parsing stops on bad marker

    _errMaxDecodeScan = 20;

    // _decodeColorConvert = true;   // Perform color convert after scan decode
}
