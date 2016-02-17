/**
 * \file ProtocolState.cpp
 * \brief 
 *
 * The hdlc-tools implement the HDLC protocol to easily talk to devices connected via serial communications
 * Copyright (C) 2016  Florian Evers, florian-evers@gmx.de
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "ProtocolState.h"
#include <assert.h>
#include "../ComPortHandler.h"
#include "FrameParser.h"
#include "FrameGenerator.h"

ProtocolState::ProtocolState(std::shared_ptr<ComPortHandler> a_ComPortHandler, boost::asio::io_service& a_IOService) {
    m_bAwaitsNextHDLCFrame = true;
    m_SSeqOutgoing = 0;
    m_RSeqOutgoing = 0;
    m_SSeqIncoming = 0;
    m_RSeqIncoming = 0;
    m_bPeerRequiresAck = false;
    m_HDLCType = HDLC_TYPE_UNKNOWN;
    m_ComPortHandler = a_ComPortHandler;
}

void ProtocolState::Start() {
    m_FrameParser = std::make_shared<FrameParser>(shared_from_this());
}

void ProtocolState::Stop() {
    m_ComPortHandler.reset();
    m_FrameParser.reset();
}

void ProtocolState::SendPayload(const std::vector<unsigned char> &a_Payload) {
    // Fresh Payload to be sent is available.
    m_ComPortHandler->DeliverPayloadToClients(a_Payload, false);
    
    // Queue payload for later framing
    m_PayloadWaitQueue.emplace_back(std::move(a_Payload));
    if (m_bAwaitsNextHDLCFrame) {
        OpportunityForTransmission();
    } // if
}

void ProtocolState::TriggerNextHDLCFrame() {
    // The ComPortHandler is ready to transmit the next HDLC frame
    assert(m_bAwaitsNextHDLCFrame == false);
    m_bAwaitsNextHDLCFrame = true;
    OpportunityForTransmission();
}

void ProtocolState::AddReceivedRawBytes(const char* a_Buffer, size_t a_Bytes) {
    m_FrameParser->AddReceivedRawBytes(a_Buffer, a_Bytes);
}

void ProtocolState::InterpretDeserializedFrame(const std::vector<unsigned char> &a_Payload, const Frame& a_Frame, bool a_bMessageValid) {
    // Deliver raw frame to clients that have interest
    m_ComPortHandler->DeliverRawFrameToClients(a_Payload, true, a_bMessageValid); // not escaped
    m_ComPortHandler->DeliverDissectedFrameToClients(">>> Rcvd: " + a_Frame.GetReadableDescription(), true, a_bMessageValid);
    
    // Stop here if the frame was considered broken
    if (a_bMessageValid == false) {
        return;
    } // if
    
    // Go ahead interpreting the frame we received
    if (a_Frame.HasPayload()) {
        // I-Frame or U-Frame with UI
        m_ComPortHandler->DeliverPayloadToClients(a_Frame.GetPayload(), true);
        
        // If it is an I-Frame, the data may have to be acked
        if (a_Frame.IsIFrame()) {
            // FIXME: may be a repeated packet
            if (m_RSeqIncoming != a_Frame.GetSSeq()) {
                // TODO: does not respect gaps yet
                m_bPeerRequiresAck = true;
                m_RSeqIncoming = a_Frame.GetSSeq();
            } // if
        } // if
    } // if
    
    // Check the various types of ACKs and NACKs
    if ((a_Frame.IsIFrame()) || (a_Frame.IsSFrame())) {
        if ((a_Frame.IsIFrame()) || (a_Frame.GetHDLCFrameType() == Frame::HDLC_FRAMETYPE_S_RR)) {
            // Now we know the start of the window the receiver expects and which segments it allows us to send
            m_RSeqOutgoing = a_Frame.GetRSeq();
        } else if (a_Frame.GetHDLCFrameType() == Frame::HDLC_FRAMETYPE_S_RNR) {
            // The peer wants us to stop sending subsequent data
            // TODO: not implemented yet
        } else if (a_Frame.GetHDLCFrameType() == Frame::HDLC_FRAMETYPE_S_REJ) {
            // The peer requests for go-back-N. We have to retransmit all affected packets
            // TODO: not implemented yet
        } else {
            assert(a_Frame.GetHDLCFrameType() == Frame::HDLC_FRAMETYPE_S_SREJ);
            // The peer requests for the retransmission of a single segment with a specific sequence number
            // TODO: not implemented yet
        } // else
    } // if
    
    if (m_bAwaitsNextHDLCFrame) {
        // Check if we have to send something now
        OpportunityForTransmission();
    } // if
}

void ProtocolState::OpportunityForTransmission() {
    // Checks
    assert(m_bAwaitsNextHDLCFrame);
    
    // TODO: Here we can add all that neat retransmission / RR / REJ stuff :-)
    if ((m_PayloadWaitQueue.empty()) && (!m_bPeerRequiresAck)) {
        return;
    } // if

    m_bAwaitsNextHDLCFrame = false;
    Frame l_Frame;
    if (m_PayloadWaitQueue.empty() == false) {
        l_Frame = PrepareIFrame();
        m_bPeerRequiresAck = false;
    } else {
        l_Frame = PrepareSFrameRR();
        m_bPeerRequiresAck = false;
    } // else
    
    // Deliver unescaped frame to clients that have interest
    const std::vector<unsigned char> l_HDLCFrameBuffer = FrameGenerator::SerializeFrame(l_Frame);
    m_ComPortHandler->DeliverRawFrameToClients(l_HDLCFrameBuffer, false, true); // not escaped
    m_ComPortHandler->DeliverDissectedFrameToClients("<<< Sent: " + l_Frame.GetReadableDescription(), false, true);
    m_ComPortHandler->DeliverHDLCFrame(std::move(FrameGenerator::EscapeFrame(l_HDLCFrameBuffer)));
    
    // Increase outgoing SSeq
    m_SSeqOutgoing = ((m_SSeqOutgoing + 1) & 0x07);
}

Frame ProtocolState::PrepareIFrame() {
    Frame l_Frame;
    l_Frame.SetAddress(0x30);
    l_Frame.SetHDLCFrameType(Frame::HDLC_FRAMETYPE_I);
    l_Frame.SetPF(false);
    l_Frame.SetSSeq(m_SSeqOutgoing);
    l_Frame.SetRSeq(m_RSeqIncoming);
    l_Frame.SetPayload(std::move(m_PayloadWaitQueue.front()));
    m_PayloadWaitQueue.pop_front();
    return(std::move(l_Frame));
}

Frame ProtocolState::PrepareSFrameRR() {
    Frame l_Frame;
    l_Frame.SetAddress(0x30);
    l_Frame.SetHDLCFrameType(Frame::HDLC_FRAMETYPE_S_RR);
    l_Frame.SetPF(false);
    l_Frame.SetRSeq(m_RSeqIncoming);
    return(std::move(l_Frame));
}
