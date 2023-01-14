#pragma once

#ifndef JPEGSNOOP_SNOOPCORE_H
#define JPEGSNOOP_SNOOPCORE_H

#include <QObject>
#include <QFile>

#include <memory>

// #include "DbSigs.h"
#include "log/ILog.h"
#include "ImgDecode.h"
#include "JfifDecode.h"
#include "SnoopConfig.h"
#include "WindowBuf.h"
#include "log/ILog.h"

class SnoopCore {
    Q_DISABLE_COPY(SnoopCore)
public:
    SnoopCore(ILog &log, SnoopConfig &appConfig);
    ~SnoopCore();

    qint64 offset() const;
    void setOffset(qint64 offset);

    bool decodeStatus() const;

    void openFile(const QString &filePath, qint64 offset = 0);
    void closeFile();

    bool analyze();
    bool searchForward();
    bool exportJpeg(const QString &outFilePath);

private:
    ILog &_log;
    SnoopConfig &_appConfig;

    std::unique_ptr<WindowBuf> _wbuf;
    // std::unique_ptr<DbSigs> _dbSigs;
    std::unique_ptr<ImgDecode> _imgDec;
    std::unique_ptr<JfifDecode> _jfifDec;

    QString _filePath;
    std::unique_ptr<QFile> _file;
    qint64 _offset = 0;
    bool _hasAnalysis = false;

    static std::unique_ptr<QFile> internalOpenFile(const QString &filePath, qint64 offset);
};

#endif //JPEGSNOOP_SNOOPCORE_H
