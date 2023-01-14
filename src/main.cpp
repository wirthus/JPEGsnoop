#include <QDirIterator>
#include <QDebug>
#include <QFileInfo>

#include "log/ConsoleLog.h"
#include "SnoopConfig.h"
#include "SnoopCore.h"

QStringList GetFilePathsFromDir(const QString &dir) {
    if (dir.isEmpty()) return {};

    QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
    QStringList result;
    while (it.hasNext()) {
        result += it.next();
    }

    return result;
}

QString GetFilePath(const QString &dirPath, const QString &srcFilePath, int index) {
    QFileInfo info(srcFilePath);

    const auto newFileName = QString("%1_%2.jpg")
        .arg(info.baseName(), QString::number(index).rightJustified(4, '0'));

    QDir dir(dirPath);
    return dir.filePath(newFileName);
}

int main(int argc, char *argv[]) {
    if (argc < 3) return 0;

    ConsoleLog log;
    log.setTraceEnabled(false);
    log.setDebugEnabled(false);
    log.setInfoEnabled(false);

    const QString inputDir(argv[1]);
    const QString outputDir(argv[2]);

    const auto filePaths = GetFilePathsFromDir(inputDir);

    SnoopConfig appConfig;
    SnoopCore core(log, appConfig);

    for (const auto &filePath: filePaths) {
        try {
            core.openFile(filePath);

            auto index = 1;

            do {
                if (core.analyze()) {
                    const auto newFilePath = GetFilePath(outputDir, filePath, index++);
                    core.exportJpeg(newFilePath);
                }
            } while (core.searchForward());
        } catch (const std::exception &ex) {
            log.error(ex.what());
        }
    }

    return 0;
}
