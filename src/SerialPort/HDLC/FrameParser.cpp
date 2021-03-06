/**
 * \file FrameParser.cpp
 * \brief 
 *
 * Copyright (c) 2016, Florian Evers, florian-evers@gmx.de
 * All rights reserved.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * 
 *     (1) Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer. 
 * 
 *     (2) Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in
 *     the documentation and/or other materials provided with the
 *     distribution.  
 *     
 *     (3)The name of the author may not be used to
 *     endorse or promote products derived from this software without
 *     specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT,
 * INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT,
 * STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include "FrameParser.h"
#include "ProtocolState.h"
#include "FCS16.h"

FrameParser::FrameParser(ProtocolState& a_ProtocolState): m_ProtocolState(a_ProtocolState) {
    Reset();
}

void FrameParser::Reset() {
    // Prepare assembly buffer
    m_Buffer.clear();
    m_Buffer.reserve(max_length);
    m_Buffer.emplace_back(0x7E);
    m_bStartTokenSeen = false;
}

void FrameParser::AddReceivedRawBytes(const unsigned char* a_Buffer, size_t a_Bytes) {
    while (a_Bytes) {
        size_t l_ConsumedBytes = AddChunk(a_Buffer, a_Bytes);
        a_Buffer += l_ConsumedBytes;
        a_Bytes  -= l_ConsumedBytes;
    } // while
}

size_t FrameParser::AddChunk(const unsigned char* a_Buffer, size_t a_Bytes) {
    if (m_bStartTokenSeen == false) {
        // No start token seen yet. Check if there is the start token available in the input buffer.
        const void* l_pStartTokenPtr = memchr((const void*)a_Buffer, 0x7E, a_Bytes);
        if (l_pStartTokenPtr) {
            // The start token was found in the input buffer. 
            m_bStartTokenSeen = true;
            if (l_pStartTokenPtr == a_Buffer) {
                // The start token is at the beginning of the buffer. Clip it.
                return 1;
            } else {
                // Clip front of buffer containing junk, including the start token.
                return ((const unsigned char*)l_pStartTokenPtr - a_Buffer + 1);
            } // else
        } else {
            // No token found, and no token was seen yet. Dropping received buffer now.
            return a_Bytes;
        } // else
    } else {
        // We already have seen the start token. Check if there is the end token available in the input buffer.
        const void* l_pEndTokenPtr = memchr((const void*)a_Buffer, 0x7E, a_Bytes);
        if (l_pEndTokenPtr) {
            // The end token was found in the input buffer. At first, check if we receive to much data.
            size_t l_NbrOfBytes = ((const unsigned char*)l_pEndTokenPtr - a_Buffer + 1);
            if ((m_Buffer.size() + l_NbrOfBytes) <= (2 * max_length)) {
                // We did not exceed the maximum frame size yet. Copy all bytes including the end token.
                m_Buffer.insert(m_Buffer.end(), a_Buffer, a_Buffer + l_NbrOfBytes);
                if (RemoveEscapeCharacters()) {
                    // The complete frame was valid and was consumed.
                    m_bStartTokenSeen = false;
                } // if
            } // else

            m_Buffer.resize(1); // Already contains start token 0x7E
            return (l_NbrOfBytes);
        } else {
            // No end token found. Copy all bytes if we do not exceed the maximum frame size.
            if ((m_Buffer.size() + a_Bytes) > (2 * max_length)) {
                // Even if all these bytes were escaped, we have exceeded the maximum frame size.
                m_bStartTokenSeen = false;
                m_Buffer.resize(1); // Already contains start token 0x7E
            } else {
                // Add all bytes
                m_Buffer.insert(m_Buffer.end(), a_Buffer, a_Buffer + a_Bytes);
            } // else
            
            return a_Bytes;
        } // else
    } // else
}

bool FrameParser::RemoveEscapeCharacters() {
    // Checks
    assert(m_Buffer.front() == 0x7E);
    assert(m_Buffer.back()  == 0x7E);
    assert(m_Buffer.size() >= 2);
    assert(m_bStartTokenSeen == true);

    if (m_Buffer.size() == 2) {
        // Remove junk, start again
        return false;
    } // if
    
    // Check for illegal escape sequence at the end of the buffer
    bool l_bMessageInvalid = false;
    if (m_Buffer[m_Buffer.size() - 2] == 0x7D) {
        l_bMessageInvalid = true;
    } else {
        // Remove escape sequences
        std::vector<unsigned char> l_UnescapedBuffer;
        l_UnescapedBuffer.reserve(m_Buffer.size());
        for (auto it = m_Buffer.begin(); it != m_Buffer.end(); ++it) {
            if (*it == 0x7D) {
                // This was the escape character
                ++it;
                if (*it == 0x5E) {
                    l_UnescapedBuffer.emplace_back(0x7E);
                } else if (*it == 0x5D) {
                    l_UnescapedBuffer.emplace_back(0x7D);
                } else {
                    // Invalid character. Go ahead with an invalid frame.
                    l_bMessageInvalid = true;
                    l_UnescapedBuffer.emplace_back(*it);
                } // else
            } else {
                // Normal non-escaped character, or one of the frame delimiters
                l_UnescapedBuffer.emplace_back(*it);
            } // else
        } // while
        
        // Go ahead with the unescaped buffer
        m_Buffer = std::move(l_UnescapedBuffer);
    } // if
    
    // We now have the unescaped frame at hand.
    if ((m_Buffer.size() < 6) || (m_Buffer.size() > max_length)) {
        // To short or too long for a valid HDLC frame. We consider it as junk.
        return false;
    } // if

    if (l_bMessageInvalid == false) {
        // Check FCS
        l_bMessageInvalid = (pppfcs16(PPPINITFCS16, (m_Buffer.data() + 1), (m_Buffer.size() - 2)) != PPPGOODFCS16);
    } // if

    m_ProtocolState.InterpretDeserializedFrame(m_Buffer, DeserializeFrame(m_Buffer), l_bMessageInvalid);
    return (l_bMessageInvalid == false);
}

HdlcFrame FrameParser::DeserializeFrame(const std::vector<unsigned char> &a_UnescapedBuffer) const {
    // Parse byte buffer to get the HDLC frame
    HdlcFrame l_HdlcFrame;
    l_HdlcFrame.SetAddress(a_UnescapedBuffer[1]);
    unsigned char l_ucCtrl = a_UnescapedBuffer[2];
    l_HdlcFrame.SetPF((l_ucCtrl & 0x10) >> 4);
    bool l_bAppendPayload = false;
    if ((l_ucCtrl & 0x01) == 0) {
        // I-Frame
        l_HdlcFrame.SetHDLCFrameType(HdlcFrame::HDLC_FRAMETYPE_I);
        l_HdlcFrame.SetSSeq((l_ucCtrl & 0x0E) >> 1);
        l_HdlcFrame.SetRSeq((l_ucCtrl & 0xE0) >> 5);
        l_bAppendPayload = true;
    } else {
        // S-Frame or U-Frame
        if ((l_ucCtrl & 0x02) == 0x00) {
            // S-Frame    
            l_HdlcFrame.SetRSeq((l_ucCtrl & 0xE0) >> 5);
            unsigned char l_ucType = ((l_ucCtrl & 0x0c) >> 2);
            if (l_ucType == 0x00) {
                // Receive-Ready (RR)
                l_HdlcFrame.SetHDLCFrameType(HdlcFrame::HDLC_FRAMETYPE_S_RR);
            } else if (l_ucType == 0x01) {
                // Receive-Not-Ready (RNR)
                l_HdlcFrame.SetHDLCFrameType(HdlcFrame::HDLC_FRAMETYPE_S_RNR);
            } else if (l_ucType == 0x02) {
                // Reject (REJ)
                l_HdlcFrame.SetHDLCFrameType(HdlcFrame::HDLC_FRAMETYPE_S_REJ);
            } else {
                // Selective Reject (SREJ)
                l_HdlcFrame.SetHDLCFrameType(HdlcFrame::HDLC_FRAMETYPE_S_SREJ);
            } // else
        } else {
            // U-Frame
            unsigned char l_ucType = (((l_ucCtrl & 0x0c) >> 2) | ((l_ucCtrl & 0xe0) >> 3));
            switch (l_ucType) {
                case 0b00000: {
                    // Unnumbered information (UI)
                    l_HdlcFrame.SetHDLCFrameType(HdlcFrame::HDLC_FRAMETYPE_U_UI);
                    l_bAppendPayload = true;
                    break;
                }
                case 0b00001: {
                    // Set Init. Mode (SIM)
                    l_HdlcFrame.SetHDLCFrameType(HdlcFrame::HDLC_FRAMETYPE_U_SIM);
                    break;
                }
                case 0b00011: {
                    // Set Async. Response Mode (SARM)
                    l_HdlcFrame.SetHDLCFrameType(HdlcFrame::HDLC_FRAMETYPE_U_SARM);
                    break;
                }
                case 0b00100: {
                    // Unnumbered Poll (UP)
                    l_HdlcFrame.SetHDLCFrameType(HdlcFrame::HDLC_FRAMETYPE_U_UP);
                    break;
                }
                case 0b00111: {
                    // Set Async. Balance Mode (SABM)
                    l_HdlcFrame.SetHDLCFrameType(HdlcFrame::HDLC_FRAMETYPE_U_SABM);
                    break;
                }
                case 0b01000: {
                    // Disconnect (DISC)
                    l_HdlcFrame.SetHDLCFrameType(HdlcFrame::HDLC_FRAMETYPE_U_DISC);
                    break;
                }
                case 0b01100: {
                    // Unnumbered Ack. (UA)
                    l_HdlcFrame.SetHDLCFrameType(HdlcFrame::HDLC_FRAMETYPE_U_UA);
                    break;
                }
                case 0b10000: {
                    // Set normal response mode (SNRM)
                    l_HdlcFrame.SetHDLCFrameType(HdlcFrame::HDLC_FRAMETYPE_U_SNRM);
                    break;
                }
                case 0b10001: {
                    // Command reject (FRMR / CMDR)
                    l_HdlcFrame.SetHDLCFrameType(HdlcFrame::HDLC_FRAMETYPE_U_CMDR);
                    l_bAppendPayload = true;
                    break;
                }
                case 0b11100: {
                    // Test (TEST)
                    l_HdlcFrame.SetHDLCFrameType(HdlcFrame::HDLC_FRAMETYPE_U_TEST);
                    l_bAppendPayload = true;
                    break;
                }
                case 0b11101: {
                    // Exchange Identification (XID)
                    l_HdlcFrame.SetHDLCFrameType(HdlcFrame::HDLC_FRAMETYPE_U_XID);
                    l_bAppendPayload = true;
                    break;
                }
                default: {
                    l_HdlcFrame.SetHDLCFrameType(HdlcFrame::HDLC_FRAMETYPE_UNSET);
                    break;
                }
            } // switch
        } // else
    } // else
    
    if (l_bAppendPayload) {
        // I-Frames and UI-Frames have additional payload
        std::vector<unsigned char> l_Payload;
        l_Payload.assign(&a_UnescapedBuffer[3], (&a_UnescapedBuffer[3] + (a_UnescapedBuffer.size() - 6)));
        l_HdlcFrame.SetPayload(std::move(l_Payload));
    } // if
    
    return l_HdlcFrame;
}
