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
    _decodeScanImg = false;
    _decodeScanImgAc = false;     // Coach message will be shown just in case
    _sigSearch = false;

    _outputScanDump = false;      // Print snippet of scan data
    _outputDhtExpand = false;     // Print expanded huffman tables
    _decodeMaker = true;

    _exifHideUnknown = true;      // Default to hiding unknown EXIF tags
    _relaxedParsing = true;       // Normal parsing stops on bad marker

    _errMaxDecodeScan = 20;

    // _decodeColorConvert = true;   // Perform color convert after scan decode
}
