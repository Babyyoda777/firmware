#include "SimRadio.h"
#include "MeshService.h"
#include "Router.h"

SimRadio::SimRadio()
{
    instance = this;
}

SimRadio *SimRadio::instance;

ErrorCode SimRadio::send(MeshPacket *p)
{
    printPacket("enqueuing for send", p);

    ErrorCode res = txQueue.enqueue(p) ? ERRNO_OK : ERRNO_UNKNOWN;

    if (res != ERRNO_OK) { // we weren't able to queue it, so we must drop it to prevent leaks
        packetPool.release(p);
        return res;
    }

    // set (random) transmit delay to let others reconfigure their radio,
    // to avoid collisions and implement timing-based flooding
    LOG_DEBUG("Set random delay before transmitting.\n");
    setTransmitDelay();
    return res;
}

void SimRadio::setTransmitDelay()
{
    MeshPacket *p = txQueue.getFront();
    // We want all sending/receiving to be done by our daemon thread.
    // We use a delay here because this packet might have been sent in response to a packet we just received.
    // So we want to make sure the other side has had a chance to reconfigure its radio.

    /* We assume if rx_snr = 0 and rx_rssi = 0, the packet was generated locally.
        *   This assumption is valid because of the offset generated by the radio to account for the noise
        *   floor.
        */
    if (p->rx_snr == 0 && p->rx_rssi == 0) {
        startTransmitTimer(true);
    } else {
        // If there is a SNR, start a timer scaled based on that SNR.
        LOG_DEBUG("rx_snr found. hop_limit:%d rx_snr:%f\n", p->hop_limit, p->rx_snr);
        startTransmitTimerSNR(p->rx_snr);
    }
}

void SimRadio::startTransmitTimer(bool withDelay)
{
    // If we have work to do and the timer wasn't already scheduled, schedule it now
    if (!txQueue.empty()) {
        uint32_t delayMsec = !withDelay ? 1 : getTxDelayMsec();
        // LOG_DEBUG("xmit timer %d\n", delay);
        delay(delayMsec);
        onNotify(TRANSMIT_DELAY_COMPLETED);
    } else {
        LOG_DEBUG("TX QUEUE EMPTY!\n");
    }
}

void SimRadio::startTransmitTimerSNR(float snr)
{
    // If we have work to do and the timer wasn't already scheduled, schedule it now
    if (!txQueue.empty()) {
        uint32_t delayMsec = getTxDelayMsecWeighted(snr);
        // LOG_DEBUG("xmit timer %d\n", delay);
        delay(delayMsec);
        onNotify(TRANSMIT_DELAY_COMPLETED);
    }
}

void SimRadio::handleTransmitInterrupt()
{
    // This can be null if we forced the device to enter standby mode.  In that case
    // ignore the transmit interrupt
    if (sendingPacket)
        completeSending();
}

void SimRadio::completeSending()
{
    // We are careful to clear sending packet before calling printPacket because
    // that can take a long time
    auto p = sendingPacket;
    sendingPacket = NULL;

    if (p) {
        txGood++;
        printPacket("Completed sending", p);

        // We are done sending that packet, release it
        packetPool.release(p);
        // LOG_DEBUG("Done with send\n");
    }
}


/** Could we send right now (i.e. either not actively receving or transmitting)? */
bool SimRadio::canSendImmediately()
{
    // We wait _if_ we are partially though receiving a packet (rather than just merely waiting for one).
    // To do otherwise would be doubly bad because not only would we drop the packet that was on the way in,
    // we almost certainly guarantee no one outside will like the packet we are sending.
    bool busyTx = sendingPacket != NULL;
    bool busyRx = isReceiving && isActivelyReceiving();

    if (busyTx || busyRx) {
        if (busyTx)
            LOG_WARN("Can not send yet, busyTx\n");
        if (busyRx)
            LOG_WARN("Can not send yet, busyRx\n");
        return false;
    } else
        return true;
}

bool SimRadio::isActivelyReceiving() 
{
    return false;  // TODO check how this should be simulated
}

bool SimRadio::isChannelActive()
{
    return false;   // TODO ask simulator
}

/** Attempt to cancel a previously sent packet.  Returns true if a packet was found we could cancel */
bool SimRadio::cancelSending(NodeNum from, PacketId id)
{
    auto p = txQueue.remove(from, id);
    if (p)
        packetPool.release(p); // free the packet we just removed

    bool result = (p != NULL);
    LOG_DEBUG("cancelSending id=0x%x, removed=%d\n", id, result);
    return result;
}


void SimRadio::onNotify(uint32_t notification)
{
    switch (notification) {
    case ISR_TX:
        handleTransmitInterrupt();
        LOG_DEBUG("tx complete - starting timer\n");
        startTransmitTimer(); 
        break;
    case ISR_RX:
        LOG_DEBUG("rx complete - starting timer\n");
        break;
    case TRANSMIT_DELAY_COMPLETED:
        LOG_DEBUG("delay done\n");

        // If we are not currently in receive mode, then restart the random delay (this can happen if the main thread
        // has placed the unit into standby)  FIXME, how will this work if the chipset is in sleep mode?
        if (!txQueue.empty()) {
            if (!canSendImmediately()) {
                // LOG_DEBUG("Currently Rx/Tx-ing: set random delay\n");
                setTransmitDelay(); // currently Rx/Tx-ing: reset random delay
            } else {
                if (isChannelActive()) { // check if there is currently a LoRa packet on the channel
                    // LOG_DEBUG("Channel is active: set random delay\n");
                    setTransmitDelay(); // reset random delay
                } else {
                    // Send any outgoing packets we have ready
                    MeshPacket *txp = txQueue.dequeue();
                    assert(txp);
                    startSend(txp);
                    // Packet has been sent, count it toward our TX airtime utilization.
                    uint32_t xmitMsec = getPacketTime(txp);
                    airTime->logAirtime(TX_LOG, xmitMsec);

                    delay(xmitMsec); // Model the time it is busy sending
                    completeSending();
                }
            }
        } else {
            // LOG_DEBUG("done with txqueue\n");
        }
        break; 
    default:
        assert(0); // We expected to receive a valid notification from the ISR
    }
}

/** start an immediate transmit */
void SimRadio::startSend(MeshPacket * txp)
{
    printPacket("Starting low level send", txp);
    size_t numbytes = beginSending(txp);
    MeshPacket* p = packetPool.allocCopy(*txp);
    perhapsDecode(p);
    Compressed c = Compressed_init_default;
    c.portnum = p->decoded.portnum; 
    // LOG_DEBUG("Sending back to simulator with portNum %d\n", p->decoded.portnum); 
    if (p->decoded.payload.size <= sizeof(c.data.bytes)) {
        memcpy(&c.data.bytes, p->decoded.payload.bytes, p->decoded.payload.size);
        c.data.size = p->decoded.payload.size; 
    } else {
        LOG_WARN("Payload size is larger than compressed message allows! Sending empty payload.\n");
    }
    p->decoded.payload.size = pb_encode_to_bytes(p->decoded.payload.bytes, sizeof(p->decoded.payload.bytes), &Compressed_msg, &c);
    p->decoded.portnum = PortNum_SIMULATOR_APP;
    service.sendToPhone(p); // Sending back to simulator
}


void SimRadio::startReceive(MeshPacket *p) {
    isReceiving = true;
    size_t length = getPacketLength(p);
    uint32_t xmitMsec = getPacketTime(length);
    delay(xmitMsec); // Model the time it is busy receiving
    handleReceiveInterrupt(p);
}

QueueStatus SimRadio::getQueueStatus()
{
    QueueStatus qs;

    qs.res = qs.mesh_packet_id = 0;
    qs.free = txQueue.getFree();
    qs.maxlen = txQueue.getMaxLen();

    return qs;
}

void SimRadio::handleReceiveInterrupt(MeshPacket *p)
{
    LOG_DEBUG("HANDLE RECEIVE INTERRUPT\n");
    uint32_t xmitMsec;
    assert(isReceiving);
    isReceiving = false;

    // read the number of actually received bytes
    size_t length = getPacketLength(p);
    xmitMsec = getPacketTime(length);
    // LOG_DEBUG("Payload size %d vs length (includes header) %d\n", p->decoded.payload.size, length);

    MeshPacket *mp = packetPool.allocCopy(*p); // keep a copy in packtPool
    mp->which_payload_variant = MeshPacket_decoded_tag; // Mark that the payload is already decoded 

    printPacket("Lora RX", mp);

    airTime->logAirtime(RX_LOG, xmitMsec);

    deliverToReceiver(mp);
}

size_t SimRadio::getPacketLength(MeshPacket *mp) {
    auto &p = mp->decoded;
    return (size_t)p.payload.size+sizeof(PacketHeader);  
}

int16_t SimRadio::readData(uint8_t* data, size_t len) {
    int16_t state = RADIOLIB_ERR_NONE;

    if(state == RADIOLIB_ERR_NONE) {
        // add null terminator
        data[len] = 0;
    }

    return state;
}