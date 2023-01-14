#include "ConsoleLog.h"

#include <QDebug>

void ConsoleLog::debug(const QString &text) {
    if (!isEnabled() || !isDebugEnabled()) return;

    qDebug() << text;
}

void ConsoleLog::trace(const QString &text) {
    if (!isEnabled() || !isTraceEnabled()) return;

    qDebug() << text;
}

void ConsoleLog::info(const QString &text) {
    if (!isEnabled() || !isInfoEnabled()) return;

    qInfo() << text;
}

void ConsoleLog::warn(const QString &text) {
    if (!isEnabled() || !isWarnEnabled()) return;

    qWarning() << "WARN:" << text;
}

void ConsoleLog::error(const QString &text) {
    if (!isEnabled() || !isErrorEnabled()) return;

    qCritical() << "ERROR:" << text;
}
