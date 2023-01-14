#pragma once

#ifndef JPEGSNOOP_ILOG_H
#define JPEGSNOOP_ILOG_H

#include <QtGlobal>

class ILog {
    Q_DISABLE_COPY(ILog)
public:
    virtual ~ILog() = default;

    virtual void debug(const QString &text) = 0;
    virtual void trace(const QString &text) = 0;
    virtual void info(const QString &text) = 0;
    virtual void warn(const QString &text) = 0;
    virtual void error(const QString &text) = 0;

    virtual bool isEnabled() const {
        return _enabled;
    }

    virtual void setEnabled(bool enabled) {
        _enabled = enabled;
    }

    virtual bool isDebugEnabled() const {
        return _debugEnabled;
    }

    virtual void setDebugEnabled(bool enabled) {
        _debugEnabled = enabled;
    }

    virtual bool isTraceEnabled() const {
        return _traceEnabled;
    }

    virtual void setTraceEnabled(bool enabled)  {
        _traceEnabled = enabled;
    }

    virtual bool isInfoEnabled() const {
        return _infoEnabled;
    }

    virtual void setInfoEnabled(bool enabled) {
        _infoEnabled = enabled;
    }

    virtual bool isWarnEnabled() const {
        return _warnEnabled;
    }

    virtual void setWarnEnabled(bool enabled) {
        _warnEnabled = enabled;
    }

    virtual bool isErrorEnabled() const {
        return _errorEnabled;
    }

    virtual void setErrorEnabled(bool enabled) {
        _errorEnabled = enabled;
    }

protected:
    ILog() = default;

private:
    bool _enabled = true;
#ifdef DEBUG
    bool _debugEnabled = true;
    bool _traceEnabled = true;
#else
    bool _debugEnabled = false;
    bool _traceEnabled = false;
#endif
    bool _infoEnabled = true;
    bool _warnEnabled = true;
    bool _errorEnabled = true;
};

#endif //JPEGSNOOP_ILOG_H
