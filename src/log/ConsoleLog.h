#pragma once

#ifndef JPEGSNOOP_CONSOLELOG_H
#define JPEGSNOOP_CONSOLELOG_H

#include <QTextStream>

#include "ILog.h"

class ConsoleLog : public ILog {
    Q_DISABLE_COPY(ConsoleLog)
public:
    ConsoleLog() = default;

    void debug(const QString &text) override;
    void trace(const QString &text) override;
    void info(const QString &text) override;

    void warn(const QString &text) override;
    void error(const QString &text) override;
};

#endif //JPEGSNOOP_CONSOLELOG_H
