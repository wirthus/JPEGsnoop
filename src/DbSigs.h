// JPEGsnoop - JPEG Image Decoder & Analysis Utility
// Copyright (C) 2018 - Calvin Hass
// http://www.impulseadventure.com/photo/jpeg-snoop.html
//
//    This program is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 2 of the License, or
//    (at your option) any later version.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

// ==========================================================================
// CLASS DESCRIPTION:
// - Class provides management of the signatures database
// - Supports both built-in and user database entries
//
// ==========================================================================
#pragma once

#ifndef JPEGSNOOP_DBSIGS_H
#define JPEGSNOOP_DBSIGS_H

#define DBEX_ENTRIES_MAX 300
#define DB_VER_STR "03"

#include <QString>

#include "log/ILog.h"
#include "SnoopConfig.h"
#include "Snoop.h"

// Signature exception structure with metadata fields
struct CompExcMm {
    QString strXMake;             // EXIF Make
    QString strXModel;            // EXIF Model
};

// Signature structure for hardcoded table
struct CompSigConst {
    teEditor eEditor;             // Digicam vs software/editor
    QString strXMake;             // Blank for editors (set to strMSwDisp)
    QString strXModel;            // Blank for editors
    QString strUmQual;
    QString strCSig;              // Signature
    QString strCSigRot;           // Signature of rotated DQTs
    QString strXSubsamp;          // Blank for editors
    QString strMSwTrim;           // Blank for digicam
    QString strMSwDisp;           // Blank for digicam
};

// Signature structure for runtime table (can use QStrings)
struct CompSig {
    bool bValid;                  // Set to FALSE for removal
    teEditor eEditor;
    QString strXMake;             // Blank for editors
    QString strXModel;            // Blank for editors
    QString strUmQual;
    QString strCSig;
    QString strCSigRot;
    QString strXSubsamp;          // Blank for editors
    QString strMSwTrim;           // Blank for digicam
    QString strMSwDisp;           // Blank for digicam
};

class DbSigs {
public:
    DbSigs(ILog &log, SnoopConfig &appConfig);
    ~DbSigs();

    int32_t GetNumSigsInternal();
    int32_t GetNumSigsExtra();
    int32_t GetDBNumEntries();

    bool GetDBEntry(int32_t nInd, CompSig *pEntry);
    uint32_t IsDBEntryUser(uint32_t nInd);

    uint32_t DatabaseExtraGetNum();
    CompSig DatabaseExtraGet(uint32_t nInd);

    void DatabaseExtraAdd(const QString &strExifMake, const QString &strExifModel,
                          const QString &strQual, const QString &strSig, const QString &strSigRot, const QString &strCss,
                          teSource eUserSource, const QString &strUserSoftware);

    bool SearchCom(const QString &strCom);

    uint32_t GetIjgNum();
    QString GetIjgEntry(uint32_t nInd);

    void SetDbDir(const QString &strDbDir);
    void SetFirstRun(bool bFirstRun);

    bool LookupExcMmNoMkr(const QString &strMake, const QString &strModel);
    bool LookupExcMmIsEdit(const QString &strMake, const QString &strModel);

private:
    void SetEntryValid(uint32_t nInd, bool bValid);

    void DatabaseExtraClean();
    void DatabaseExtraLoad();
    void DatabaseExtraStore();

    bool BufReadNum(quint8 *pBuf, uint32_t &nOut, uint32_t nMaxBytes, uint32_t &nOffsetBytes);
    bool BufReadStr(quint8 *pBuf, QString &strOut, uint32_t nMaxBytes, bool bUni, uint32_t &nOffsetBytes);
    bool BufWriteNum(quint8 *pBuf, uint32_t nIn, uint32_t nMaxBytes, uint32_t &nOffsetBytes);
    bool BufWriteStr(quint8 *pBuf, const QString &strIn, uint32_t nMaxBytes, bool bUni, uint32_t &nOffsetBytes);

    bool SearchSignatureExactInternal(const QString &strMake, const QString &strModel, const QString &strSig);

    ILog &_log;
    SnoopConfig &_appConfig;

    CompSig m_sSigListExtra[DBEX_ENTRIES_MAX];  // Extra entries
    int32_t m_nSigListExtraNum;

    int32_t m_nSigListNum;
    static const CompSigConst m_sSigList[];       // Built-in entries
    QList<CompSigConst> m_qSigList;

    int32_t m_nExcMmNoMkrListNum;
    static const CompExcMm m_sExcMmNoMkrList[];

    int32_t m_nExcMmIsEditListNum;
    static const CompExcMm m_sExcMmIsEditList[];

    int32_t m_nSwIjgListNum;
    int32_t m_nXcomSwListNum;

    QString m_strDbDir;           // Database directory
    QStringList m_sXComSwList;
    QStringList m_sSwIjgList;

    bool m_bFirstRun;             // First time running app?
};

#endif
