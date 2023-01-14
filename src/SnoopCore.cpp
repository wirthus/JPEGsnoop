#include "SnoopCore.h"

#include <stdexcept>

SnoopCore::SnoopCore(ILog &log, SnoopConfig &appConfig) :
    _log(log),
    _appConfig(appConfig) {

    _wbuf = std::make_unique<WindowBuf>(_log);
    // _dbSigs = std::make_unique<DbSigs>(_log, _appConfig);
    _imgDec = std::make_unique<ImgDecode>(_log, *_wbuf, _appConfig);
    _jfifDec = std::make_unique<JfifDecode>(_log, *_wbuf, *_imgDec, _appConfig);
}

SnoopCore::~SnoopCore() {
    closeFile();
}

qint64 SnoopCore::offset() const {
    return _offset;
}

void SnoopCore::setOffset(qint64 offset) {
    if (_offset == offset) return;

    if (!_file) return;
    if (!_file->seek(offset)) throw std::logic_error("Can't seek file.");

    _offset = offset;
    _hasAnalysis = false;
}

bool SnoopCore::decodeStatus() const {
    if (!_hasAnalysis) return false;

    return _jfifDec->getDecodeStatus();
}

void SnoopCore::openFile(const QString &filePath, qint64 offset) {
    if (_filePath == filePath) return;
    _filePath = filePath;

    _file = internalOpenFile(filePath, offset);
    _offset = offset;
    _wbuf->setFile(_file.get());
    _hasAnalysis = false;
}

void SnoopCore::closeFile() {
    _filePath.clear();
    _file = nullptr;
    _hasAnalysis = false;
    _offset = 0;
}

bool SnoopCore::analyze() {
    if (!_hasAnalysis) {
        _jfifDec->processFile(_offset);
        _hasAnalysis = true;
    }

    return _jfifDec->getDecodeStatus();
}

bool SnoopCore::searchForward() {
    const auto offset = _hasAnalysis ? _offset + 1 : _offset;

    uint32_t foundPosition;
    const auto found = _wbuf->search(offset, 0xFFD8FF, 3, true, foundPosition);
    if (!found) return false;

    _offset = foundPosition;
    _hasAnalysis = false;

    return true;
}

bool SnoopCore::exportJpeg(const QString &outFilePath) {
    if (outFilePath.isEmpty()) return false;

    const auto forceSoi = false;
    const auto forceEoi = false;
    if (_jfifDec->getDecodeStatus() && _jfifDec->exportJpegPrepare(forceSoi, forceEoi, true)) {
        if (_jfifDec->exportJpegDo(outFilePath, false, true, forceSoi, forceEoi)) {
            return true;
        }
    }

    return false;
}

std::unique_ptr<QFile> SnoopCore::internalOpenFile(const QString &filePath, qint64 offset) {
    if (filePath.isEmpty()) throw std::logic_error("File path is empty.");

    auto file = std::make_unique<QFile>(filePath);
    if (!file->open(QFile::OpenModeFlag::ReadOnly)) throw std::logic_error("File not open.");;
    if (file->size() == 0) throw std::logic_error("File size is zero.");;
    if (!file->seek(offset)) throw std::logic_error("Can't seek file.");

    return file;
}
