#include "SnoopConfig.h"

#include <QStandardPaths>

SnoopConfig::SnoopConfig(QObject *parent) : QObject(parent) {
    // Debug log
    // _debugLogFileName = ".\\JPEGsnoop-debug.log";
    // _debugLogStream = nullptr;
    // _debugLogEnable = false;

    // Command-line modes
    _cmdLineOpenFileName = "";
    _cmdLineOutputFileName = "";
    _cmdLineBatchDirName = "";

    // --------------------------------
    // Registry settings
    _dbDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    _decodeScanImg = true;
    _decodeScanImgAc = true;     // Coach message will be shown just in case
    _sigSearch = false;

    _outputScanDump = false;      // Print snippet of scan data
    _outputDhtExpand = false;     // Print expanded huffman tables
    _decodeMaker = true;
    _dumpHistogramY = false;

    // Difference in performance: 1dsmk2 image:
    // Performance boost ~ 25%
    _histogramEnabled = false;    // Histogram & clipping stats enabled?
    _statClipEnabled = false;     // UNUSED: Enable Scan Decode clip stats?

    _exifHideUnknown = true;      // Default to hiding unknown EXIF tags
    _relaxedParsing = true;       // Normal parsing stops on bad marker

    _errMaxDecodeScan = 20;

    // _decodeColorConvert = true;   // Perform color convert after scan decode
}

SnoopConfig::~SnoopConfig(void) = default;
