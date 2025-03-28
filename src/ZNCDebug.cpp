/*
 * Copyright (C) 2004-2025 ZNC, see the NOTICE file for details.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <znc/ZNCDebug.h>
#include <znc/Utils.h>
#include <iostream>
#include <sys/time.h>
#include <stdio.h>
#include <time.h>

bool CDebug::stdoutIsTTY = true;
bool CDebug::debug = false;

CString CDebug::Filter(const CString& sUnfilteredLine) {
    CString sFilteredLine = sUnfilteredLine;

    // If the line is a PASS command to authenticate to a server / znc
    if (sUnfilteredLine.StartsWith("PASS ")) {
        CString sPrefix = sUnfilteredLine.substr(0, sUnfilteredLine[5] == ':' ? 6 : 5);
        CString sRest = sUnfilteredLine.substr(sPrefix.length());

        VCString vsSafeCopy;
        sRest.Split(":", vsSafeCopy);

        if (vsSafeCopy.size() > 1) {
            sFilteredLine = sPrefix + vsSafeCopy[0] + ":<censored>";
        } else {
            sFilteredLine = sPrefix + "<censored>";
        }
    }

    return sFilteredLine;
}

CDebugStream::~CDebugStream() {
    timeval tTime = CUtils::GetTime();
    time_t tSec = (time_t)tTime.tv_sec;  // some systems (e.g. openbsd) define
                                         // tv_sec as long int instead of time_t
    tm tM;
    tzset();  // localtime_r requires this
    localtime_r(&tSec, &tM);
    char sTime[20] = {};
    strftime(sTime, sizeof(sTime), "%Y-%m-%d %H:%M:%S", &tM);
    char sUsec[7] = {};
    snprintf(sUsec, sizeof(sUsec), "%06lu", (unsigned long int)tTime.tv_usec);
    std::cout << "[" << sTime << "." << sUsec << "] "
              << CString(this->str()).Escape_n(CString::EDEBUG) << std::endl;
}
