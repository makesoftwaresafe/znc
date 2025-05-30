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

#include <znc/Chan.h>
#include <znc/IRCSock.h>
#include <znc/User.h>
#include <znc/IRCNetwork.h>
#include <znc/Config.h>
#include <znc/znc.h>
#include <znc/Message.h>

using std::set;
using std::vector;
using std::map;

CChan::CChan(const CString& sName, CIRCNetwork* pNetwork, bool bInConfig,
             CConfig* pConfig)
    : m_bDetached(false),
      m_bIsOn(false),
      m_bAutoClearChanBuffer(pNetwork->GetUser()->AutoClearChanBuffer()),
      m_bInConfig(bInConfig),
      m_bDisabled(false),
      m_bHasBufferCountSet(false),
      m_bHasAutoClearChanBufferSet(false),
      m_sName(sName.Token(0)),
      m_sKey(sName.Token(1)),
      m_sTopic(""),
      m_sTopicOwner(""),
      m_ulTopicDate(0),
      m_ulCreationDate(0),
      m_pNetwork(pNetwork),
      m_Nick(),
      m_uJoinTries(0),
      m_sDefaultModes(""),
      m_msNicks(),
      m_Buffer(),
      m_bModeKnown(false),
      m_mcsModes() {
    if (!m_pNetwork->IsChan(m_sName)) {
        m_sName = "#" + m_sName;
    }

    m_Nick.SetNetwork(m_pNetwork);
    m_Buffer.SetLineCount(m_pNetwork->GetUser()->GetChanBufferSize(), true);

    if (pConfig) {
        CString sValue;
        if (pConfig->FindStringEntry("buffer", sValue))
            SetBufferCount(sValue.ToUInt(), true);
        if (pConfig->FindStringEntry("autoclearchanbuffer", sValue))
            SetAutoClearChanBuffer(sValue.ToBool());
        if (pConfig->FindStringEntry("keepbuffer", sValue))
            // XXX Compatibility crap, added in 0.207
            SetAutoClearChanBuffer(!sValue.ToBool());
        if (pConfig->FindStringEntry("detached", sValue))
            SetDetached(sValue.ToBool());
        if (pConfig->FindStringEntry("disabled", sValue))
            if (sValue.ToBool()) Disable();
        if (pConfig->FindStringEntry("autocycle", sValue))
            if (sValue.Equals("true"))
                CUtils::PrintError(
                    "WARNING: AutoCycle has been removed, instead try -> "
                    "LoadModule = autocycle " +
                    sName);
        if (pConfig->FindStringEntry("key", sValue)) SetKey(sValue);
        if (pConfig->FindStringEntry("modes", sValue)) SetDefaultModes(sValue);
    }
}

CChan::~CChan() { ClearNicks(); }

void CChan::Reset() {
    m_bIsOn = false;
    m_bModeKnown = false;
    m_mcsModes.clear();
    m_sTopic = "";
    m_sTopicOwner = "";
    m_ulTopicDate = 0;
    m_ulCreationDate = 0;
    m_Nick.Reset();
    ClearNicks();
    ResetJoinTries();
}

CConfig CChan::ToConfig() const {
    CConfig config;

    if (m_bHasBufferCountSet)
        config.AddKeyValuePair("Buffer", CString(GetBufferCount()));
    if (m_bHasAutoClearChanBufferSet)
        config.AddKeyValuePair("AutoClearChanBuffer",
                               CString(AutoClearChanBuffer()));
    if (IsDetached()) config.AddKeyValuePair("Detached", "true");
    if (IsDisabled()) config.AddKeyValuePair("Disabled", "true");
    if (!GetKey().empty()) config.AddKeyValuePair("Key", GetKey());
    if (!GetDefaultModes().empty())
        config.AddKeyValuePair("Modes", GetDefaultModes());

    return config;
}

void CChan::Clone(CChan& chan) {
    // We assume that m_sName and m_pNetwork are equal
    SetBufferCount(chan.GetBufferCount(), true);
    SetAutoClearChanBuffer(chan.AutoClearChanBuffer());
    SetKey(chan.GetKey());
    SetDefaultModes(chan.GetDefaultModes());

    if (IsDetached() != chan.IsDetached()) {
        // Only send something if it makes sense
        // (= Only detach if client is on the channel
        //    and only attach if we are on the channel)
        if (IsOn()) {
            if (IsDetached()) {
                AttachUser();
            } else {
                DetachUser();
            }
        }
        SetDetached(chan.IsDetached());
    }
}

void CChan::Cycle() const {
    m_pNetwork->PutIRC("PART " + GetName() + "\r\nJOIN " + GetName() + " " +
                       GetKey());
}

void CChan::JoinUser(const CString& sKey) {
    if (!IsOn() && !sKey.empty()) {
        SetKey(sKey);
    }
    if (m_pNetwork->IsIRCConnected()) {
        m_pNetwork->PutIRC("JOIN " + GetName() + " " + GetKey());
    }
}

void CChan::AttachUser(CClient* pClient) {
    m_pNetwork->PutUser(
        ":" + m_pNetwork->GetIRCNick().GetNickMask() + " JOIN :" + GetName(),
        pClient);

    if (!GetTopic().empty()) {
        m_pNetwork->PutUser(":" + m_pNetwork->GetIRCServer() + " 332 " +
                                m_pNetwork->GetIRCNick().GetNick() + " " +
                                GetName() + " :" + GetTopic(),
                            pClient);
        if (!GetTopicOwner().empty()) {
            m_pNetwork->PutUser(":" + m_pNetwork->GetIRCServer() + " 333 " +
                                    m_pNetwork->GetIRCNick().GetNick() + " " +
                                    GetName() + " " + GetTopicOwner() + " " +
                                    CString(GetTopicDate()),
                                pClient);
        }
    }

    CString sPre = ":" + m_pNetwork->GetIRCServer() + " 353 " +
                   m_pNetwork->GetIRCNick().GetNick() + " " +
                   GetModeForNames() + " " + GetName() + " :";
    CString sLine = sPre;
    CString sPerm, sNick;

    const vector<CClient*>& vpClients = m_pNetwork->GetClients();
    for (CClient* pEachClient : vpClients) {
        CClient* pThisClient;
        if (!pClient)
            pThisClient = pEachClient;
        else
            pThisClient = pClient;

        for (map<CString, CNick>::iterator a = m_msNicks.begin();
             a != m_msNicks.end(); ++a) {
            if (pThisClient->HasNamesx()) {
                sPerm = a->second.GetPermStr();
            } else {
                char c = a->second.GetPermChar();
                sPerm = "";
                if (c != '\0') {
                    sPerm += c;
                }
            }
            if (pThisClient->HasUHNames() && !a->second.GetIdent().empty() &&
                !a->second.GetHost().empty()) {
                sNick = a->first + "!" + a->second.GetIdent() + "@" +
                        a->second.GetHost();
            } else {
                sNick = a->first;
            }

            sLine += sPerm + sNick;

            if (sLine.size() >= 490 || a == (--m_msNicks.end())) {
                m_pNetwork->PutUser(sLine, pThisClient);
                sLine = sPre;
            } else {
                sLine += " ";
            }
        }

        if (pClient)  // We only want to do this for one client
            break;
    }

    m_pNetwork->PutUser(":" + m_pNetwork->GetIRCServer() + " 366 " +
                            m_pNetwork->GetIRCNick().GetNick() + " " +
                            GetName() + " :End of /NAMES list.",
                        pClient);
    m_bDetached = false;

    // Send Buffer
    SendBuffer(pClient);
}

void CChan::DetachUser() {
    if (!m_bDetached) {
        m_pNetwork->PutUser(":" + m_pNetwork->GetIRCNick().GetNickMask() +
                            " PART " + GetName());
        m_bDetached = true;
    }
}

CString CChan::GetModeString() const {
    CString sModes, sArgs;

    for (const auto& it : m_mcsModes) {
        sModes += it.first;
        if (it.second.size()) {
            sArgs += " " + it.second;
        }
    }

    return sModes.empty() ? sModes : CString("+" + sModes + sArgs);
}

CString CChan::GetModeForNames() const {
    CString sMode;

    for (const auto& it : m_mcsModes) {
        if (it.first == 's') {
            sMode = "@";
        } else if ((it.first == 'p') && sMode.empty()) {
            sMode = "*";
        }
    }

    return (sMode.empty() ? "=" : sMode);
}

void CChan::SetModes(const CString& sModes) {
    m_mcsModes.clear();
    ModeChange(sModes);
}

void CChan::SetModes(const CString& modes, const VCString& vsModeParams) {
    m_mcsModes.clear();
    ModeChange(modes, vsModeParams);
}

void CChan::SetAutoClearChanBuffer(bool b) {
    m_bHasAutoClearChanBufferSet = true;
    m_bAutoClearChanBuffer = b;

    if (m_bAutoClearChanBuffer && !IsDetached() && m_pNetwork->IsUserOnline()) {
        ClearBuffer();
    }
}

void CChan::InheritAutoClearChanBuffer(bool b) {
    if (!m_bHasAutoClearChanBufferSet) {
        m_bAutoClearChanBuffer = b;

        if (m_bAutoClearChanBuffer && !IsDetached() &&
            m_pNetwork->IsUserOnline()) {
            ClearBuffer();
        }
    }
}

void CChan::ResetAutoClearChanBuffer() {
    SetAutoClearChanBuffer(m_pNetwork->GetUser()->AutoClearChanBuffer());
    m_bHasAutoClearChanBufferSet = false;
}

void CChan::OnWho(const CString& sNick, const CString& sIdent,
                  const CString& sHost) {
    CNick* pNick = FindNick(sNick);

    if (pNick) {
        pNick->SetIdent(sIdent);
        pNick->SetHost(sHost);
    }
}

void CChan::ModeChange(const CString& sModes, const VCString& vsModes,
                       const CNick* pOpNick) {
    bool bAdd = true;

    /* Try to find a CNick* from this channel so that pOpNick->HasPerm()
     * works as expected. */
    if (pOpNick) {
        CNick* OpNick = FindNick(pOpNick->GetNick());
        /* If nothing was found, use the original pOpNick, else use the
         * CNick* from FindNick() */
        if (OpNick) pOpNick = OpNick;
    }

    {
        CString sArgs = CString(" ").Join(vsModes.begin(), vsModes.end());
        NETWORKMODULECALL(OnRawMode2(pOpNick, *this, sModes, sArgs),
                          m_pNetwork->GetUser(), m_pNetwork, nullptr, NOTHING);
    }

    VCString::const_iterator argIter = vsModes.begin();
    const CString sEmpty;
    auto nextArg = [&]() -> const CString& {
        if (argIter == vsModes.end()) {
            return sEmpty;
        }
        return *argIter++;
    };
    for (unsigned int a = 0; a < sModes.size(); a++) {
        const char& cMode = sModes[a];

        if (cMode == '+') {
            bAdd = true;
        } else if (cMode == '-') {
            bAdd = false;
        } else if (m_pNetwork->GetIRCSock()->IsPermMode(cMode)) {
            const CString& sArg = nextArg();
            CNick* pNick = FindNick(sArg);
            if (pNick) {
                char cPerm = m_pNetwork->GetIRCSock()->GetPermFromMode(cMode);

                if (cPerm) {
                    bool bNoChange = (pNick->HasPerm(cPerm) == bAdd);

                    if (bAdd) {
                        pNick->AddPerm(cPerm);

                        if (pNick->NickEquals(m_pNetwork->GetCurNick())) {
                            AddPerm(cPerm);
                        }
                    } else {
                        pNick->RemPerm(cPerm);

                        if (pNick->NickEquals(m_pNetwork->GetCurNick())) {
                            RemPerm(cPerm);
                        }
                    }

                    NETWORKMODULECALL(OnChanPermission3(pOpNick, *pNick, *this,
                                                        cMode, bAdd, bNoChange),
                                      m_pNetwork->GetUser(), m_pNetwork,
                                      nullptr, NOTHING);

                    if (cMode == CChan::M_Op) {
                        if (bAdd) {
                            NETWORKMODULECALL(
                                OnOp2(pOpNick, *pNick, *this, bNoChange),
                                m_pNetwork->GetUser(), m_pNetwork, nullptr,
                                NOTHING);
                        } else {
                            NETWORKMODULECALL(
                                OnDeop2(pOpNick, *pNick, *this, bNoChange),
                                m_pNetwork->GetUser(), m_pNetwork, nullptr,
                                NOTHING);
                        }
                    } else if (cMode == CChan::M_Voice) {
                        if (bAdd) {
                            NETWORKMODULECALL(
                                OnVoice2(pOpNick, *pNick, *this, bNoChange),
                                m_pNetwork->GetUser(), m_pNetwork, nullptr,
                                NOTHING);
                        } else {
                            NETWORKMODULECALL(
                                OnDevoice2(pOpNick, *pNick, *this, bNoChange),
                                m_pNetwork->GetUser(), m_pNetwork, nullptr,
                                NOTHING);
                        }
                    }
                }
            }
        } else {
            bool bList = false;
            CString sArg;

            switch (m_pNetwork->GetIRCSock()->GetModeType(cMode)) {
                case CIRCSock::ListArg:
                    bList = true;
                    sArg = nextArg();
                    break;
                case CIRCSock::HasArg:
                    sArg = nextArg();
                    break;
                case CIRCSock::NoArg:
                    break;
                case CIRCSock::ArgWhenSet:
                    if (bAdd) {
                        sArg = nextArg();
                    }

                    break;
            }

            bool bNoChange;
            if (bList) {
                bNoChange = false;
            } else if (bAdd) {
                bNoChange = HasMode(cMode) && GetModeArg(cMode) == sArg;
            } else {
                bNoChange = !HasMode(cMode);
            }
            NETWORKMODULECALL(
                OnMode2(pOpNick, *this, cMode, sArg, bAdd, bNoChange),
                m_pNetwork->GetUser(), m_pNetwork, nullptr, NOTHING);

            if (!bList) {
                (bAdd) ? AddMode(cMode, sArg) : RemMode(cMode);
            }

            // This is called when we join (ZNC requests the channel modes
            // on join) *and* when someone changes the channel keys.
            // We ignore channel key "*" because of some broken nets.
            if (cMode == M_Key && !bNoChange && bAdd && sArg != "*") {
                SetKey(sArg);
            }
        }
    }
}

void CChan::ModeChange(const CString& sModes, const CNick* pOpNick) {
    VCString vsModes;
    CString sModeArg = sModes.Token(0);
    bool colon = sModeArg.TrimPrefix(":");

    // Only handle parameters if sModes doesn't start with a colon
    // because if it does, we only have the mode string with no parameters
    if (!colon) {
        CString sArgs = sModes.Token(1, true);

        while (!sArgs.empty()) {
            // Check if this parameter is a trailing parameter
            // If so, treat the rest of sArgs as one parameter
            if (sArgs.TrimPrefix(":")) {
                vsModes.push_back(sArgs);
                sArgs.clear();
            } else {
                vsModes.push_back(sArgs.Token(0));
                sArgs = sArgs.Token(1, true);
            }
        }
    }

    ModeChange(sModeArg, vsModes, pOpNick);
}

CString CChan::GetOptions() const {
    VCString vsRet;

    if (IsDetached()) {
        vsRet.push_back("Detached");
    }

    if (AutoClearChanBuffer()) {
        if (HasAutoClearChanBufferSet()) {
            vsRet.push_back("AutoClearChanBuffer");
        } else {
            vsRet.push_back("AutoClearChanBuffer (default)");
        }
    }

    return CString(", ").Join(vsRet.begin(), vsRet.end());
}

CString CChan::GetModeArg(char cMode) const {
    if (cMode) {
        map<char, CString>::const_iterator it = m_mcsModes.find(cMode);

        if (it != m_mcsModes.end()) {
            return it->second;
        }
    }

    return "";
}

bool CChan::HasMode(char cMode) const {
    return (cMode && m_mcsModes.find(cMode) != m_mcsModes.end());
}

bool CChan::AddMode(char cMode, const CString& sArg) {
    m_mcsModes[cMode] = sArg;
    return true;
}

bool CChan::RemMode(char cMode) {
    if (!HasMode(cMode)) {
        return false;
    }

    m_mcsModes.erase(cMode);
    return true;
}

CString CChan::GetModeArg(CString& sArgs) const {
    CString sRet = sArgs.substr(0, sArgs.find(' '));
    sArgs = (sRet.size() < sArgs.size()) ? sArgs.substr(sRet.size() + 1) : "";
    return sRet;
}

void CChan::ClearNicks() { m_msNicks.clear(); }

int CChan::AddNicks(const CString& sNicks) {
    int iRet = 0;
    VCString vsNicks;

    sNicks.Split(" ", vsNicks, false);

    for (const CString& sNick : vsNicks) {
        if (AddNick(sNick)) {
            iRet++;
        }
    }

    return iRet;
}

bool CChan::AddNick(const CString& sNick) {
    const char* p = sNick.c_str();
    CString sPrefix, sTmp, sIdent, sHost;

    while (m_pNetwork->GetIRCSock()->IsPermChar(*p)) {
        sPrefix += *p;

        if (!*++p) {
            return false;
        }
    }

    sTmp = p;

    // The UHNames extension gets us nick!ident@host instead of just plain nick
    sIdent = sTmp.Token(1, true, "!");
    sHost = sIdent.Token(1, true, "@");
    sIdent = sIdent.Token(0, false, "@");
    // Get the nick
    sTmp = sTmp.Token(0, false, "!");

    CNick tmpNick(sTmp);
    CNick* pNick = FindNick(sTmp);
    if (!pNick) {
        pNick = &tmpNick;
        pNick->SetNetwork(m_pNetwork);
    }

    if (!sIdent.empty()) pNick->SetIdent(sIdent);
    if (!sHost.empty()) pNick->SetHost(sHost);

    for (CString::size_type i = 0; i < sPrefix.length(); i++) {
        pNick->AddPerm(sPrefix[i]);
    }

    if (pNick->NickEquals(m_pNetwork->GetCurNick())) {
        for (CString::size_type i = 0; i < sPrefix.length(); i++) {
            AddPerm(sPrefix[i]);
        }
    }

    m_msNicks[pNick->GetNick()] = *pNick;

    return true;
}

map<char, unsigned int> CChan::GetPermCounts() const {
    map<char, unsigned int> mRet;

    for (const auto& it : m_msNicks) {
        CString sPerms = it.second.GetPermStr();

        for (unsigned int p = 0; p < sPerms.size(); p++) {
            mRet[sPerms[p]]++;
        }
    }

    return mRet;
}

bool CChan::RemNick(const CString& sNick) {
    map<CString, CNick>::iterator it;

    it = m_msNicks.find(sNick);
    if (it == m_msNicks.end()) {
        return false;
    }

    m_msNicks.erase(it);

    return true;
}

bool CChan::ChangeNick(const CString& sOldNick, const CString& sNewNick) {
    map<CString, CNick>::iterator it = m_msNicks.find(sOldNick);

    if (it == m_msNicks.end()) {
        return false;
    }

    // Rename this nick
    it->second.SetNick(sNewNick);

    // Insert a new element into the map then erase the old one, do this to
    // change the key to the new nick
    m_msNicks[sNewNick] = it->second;
    m_msNicks.erase(it);

    return true;
}

const CNick* CChan::FindNick(const CString& sNick) const {
    map<CString, CNick>::const_iterator it = m_msNicks.find(sNick);
    return (it != m_msNicks.end()) ? &it->second : nullptr;
}

CNick* CChan::FindNick(const CString& sNick) {
    map<CString, CNick>::iterator it = m_msNicks.find(sNick);
    return (it != m_msNicks.end()) ? &it->second : nullptr;
}

void CChan::SendBuffer(CClient* pClient) {
    SendBuffer(pClient, m_Buffer);
    if (AutoClearChanBuffer()) {
        ClearBuffer();
    }
}

void CChan::SendBuffer(CClient* pClient, const CBuffer& Buffer) {
    if (m_pNetwork && m_pNetwork->IsUserAttached()) {
        // in the event that pClient is nullptr, need to send this to all
        // clients for the user I'm presuming here that pClient is listed
        // inside vClients thus vClients at this point can't be empty.
        //
        // This loop has to be cycled twice to maintain the existing behavior
        // which is
        // 1. OnChanBufferStarting
        // 2. OnChanBufferPlayLine
        // 3. ClearBuffer() if not keeping the buffer
        // 4. OnChanBufferEnding
        //
        // With the exception of ClearBuffer(), this needs to happen per
        // client, and if pClient is not nullptr, the loops break after the
        // first iteration.
        //
        // Rework this if you like ...
        if (!Buffer.IsEmpty()) {
            const vector<CClient*>& vClients = m_pNetwork->GetClients();
            for (CClient* pEachClient : vClients) {
                CClient* pUseClient = (pClient ? pClient : pEachClient);

                bool bWasPlaybackActive = pUseClient->IsPlaybackActive();
                pUseClient->SetPlaybackActive(true);

                bool bSkipStatusMsg = pUseClient->HasServerTime();
                NETWORKMODULECALL(OnChanBufferStarting(*this, *pUseClient),
                                  m_pNetwork->GetUser(), m_pNetwork, nullptr,
                                  &bSkipStatusMsg);

                if (!bSkipStatusMsg) {
                    m_pNetwork->PutUser(":***!znc@znc.in PRIVMSG " + GetName() +
                                            " :" + t_s("Buffer Playback..."),
                                        pUseClient);
                }

                bool bBatch = pUseClient->HasBatch();
                CString sBatchName = GetName().MD5();

                if (bBatch) {
                    m_pNetwork->PutUser(":znc.in BATCH +" + sBatchName +
                                            " znc.in/playback " + GetName(),
                                        pUseClient);
                }

                size_t uSize = Buffer.Size();
                for (size_t uIdx = 0; uIdx < uSize; uIdx++) {
                    const CBufLine& BufLine = Buffer.GetBufLine(uIdx);
                    CMessage Message =
                        BufLine.ToMessage(*pUseClient, MCString::EmptyMap);
                    Message.SetChan(this);
                    Message.SetNetwork(m_pNetwork);
                    Message.SetClient(pUseClient);
                    if (bBatch) {
                        Message.SetTag("batch", sBatchName);
                    }
                    bool bNotShowThisLine = false;
                    NETWORKMODULECALL(OnChanBufferPlayMessage(Message),
                                      m_pNetwork->GetUser(), m_pNetwork,
                                      nullptr, &bNotShowThisLine);
                    if (bNotShowThisLine) continue;
                    m_pNetwork->PutUser(Message, pUseClient);
                }

                bSkipStatusMsg = pUseClient->HasServerTime();
                NETWORKMODULECALL(OnChanBufferEnding(*this, *pUseClient),
                                  m_pNetwork->GetUser(), m_pNetwork, nullptr,
                                  &bSkipStatusMsg);
                if (!bSkipStatusMsg) {
                    m_pNetwork->PutUser(":***!znc@znc.in PRIVMSG " + GetName() +
                                            " :" + t_s("Playback Complete."),
                                        pUseClient);
                }

                if (bBatch) {
                    m_pNetwork->PutUser(":znc.in BATCH -" + sBatchName,
                                        pUseClient);
                }

                pUseClient->SetPlaybackActive(bWasPlaybackActive);

                if (pClient) break;
            }
        }
    }
}

void CChan::Enable() {
    ResetJoinTries();
    m_bDisabled = false;
}

void CChan::SetKey(const CString& s) {
    if (m_sKey != s) {
        m_sKey = s;
        if (m_bInConfig) {
            CZNC::Get().SetConfigState(CZNC::ECONFIG_DELAYED_WRITE);
        }
    }
}

void CChan::SetInConfig(bool b) {
    if (m_bInConfig != b) {
        m_bInConfig = b;
        CZNC::Get().SetConfigState(CZNC::ECONFIG_DELAYED_WRITE);
    }
}

void CChan::ResetBufferCount() {
    SetBufferCount(m_pNetwork->GetUser()->GetBufferCount());
    m_bHasBufferCountSet = false;
}
