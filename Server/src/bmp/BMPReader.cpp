/*
 * Copyright (c) 2013-2015 Cisco Systems, Inc. and others.  All rights reserved.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v1.0 which accompanies this distribution,
 * and is available at http://www.eclipse.org/legal/epl-v10.html
 *
 */

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstdio>
#include <unistd.h>

#include <iostream>
#include <cstring>
#include <cstdlib>
#include <string>
#include <cerrno>

#include "BMPListener.h"
#include "BMPReader.h"
#include "parseBMP.h"
#include "parseBGP.h"
#include "MsgBusInterface.hpp"
#include "Logger.h"


using namespace std;

/**
 * Class constructor
 *
 *  \param [in] logPtr  Pointer to existing Logger for app logging
 *  \param [in] config  Pointer to the loaded configuration
 *
 */
BMPReader::BMPReader(Logger *logPtr, Config *config) {
    debug = false;

    cfg = config;

    logger = logPtr;

    if (cfg->debug_bmp)
        enableDebug();
}

/**
 * Destructor
 */
BMPReader::~BMPReader() {

}


/**
 * Read messages from BMP stream in a loop
 *
 * \param [in]  run         Reference to bool to indicate if loop should continue or not
 * \param [in]  client      Client information pointer
 * \param [in]  mbus_ptr     The database pointer referencer - DB should be already initialized
 *
 * \return true if more to read, false if the connection is done/closed
 *
 * \throw (char const *str) message indicate error
 */
void BMPReader::readerThreadLoop(bool &run, BMPListener::ClientInfo *client, MsgBusInterface *mbus_ptr) {
    while (run) {

        try {
            if (not ReadIncomingMsg(client, mbus_ptr))
                break;

        } catch (char const *str) {
            run = false;
            break;
        }
    }
}

/**
 * Read messages from BMP stream
 *
 * BMP routers send BMP/BGP messages, this method reads and parses those.
 *
 * \param [in]  client      Client information pointer
 * \param [in]  mbus_ptr     The database pointer referencer - DB should be already initialized
 *
 * \return true if more to read, false if the connection is done/closed
 *
 * \throw (char const *str) message indicate error
 */
bool BMPReader::ReadIncomingMsg(BMPListener::ClientInfo *client, MsgBusInterface *mbus_ptr) {
    bool rval = true;
    parseBGP *pBGP;                                 // Pointer to BGP parser

    int read_fd = client->pipe_sock > 0 ? client->pipe_sock : client->c_sock;

    // Data storage structures
    MsgBusInterface::obj_bgp_peer p_entry;

    // Initialize the parser for BMP messages
    parseBMP *pBMP = new parseBMP(logger, &p_entry);    // handler for BMP messages

    if (cfg->debug_bmp) {
        enableDebug();
        pBMP->enableDebug();
    }

    char bmp_type = 0;

    MsgBusInterface::obj_router r_object;
    memcpy(router_hash_id, client->hash_id, sizeof(router_hash_id));    // Cache the router hash ID (hash is generated by BMPListener)
    bzero(&r_object, sizeof(r_object));
    memcpy(r_object.hash_id, router_hash_id, sizeof(r_object.hash_id));

    // Setup the router record table object
    memcpy(r_object.ip_addr, client->c_ip, sizeof(client->c_ip));

    try {
        bmp_type = pBMP->handleMessage(read_fd);

        /*
         * Now that we have parsed the BMP message...
         *  add record to the database
         */

        if (bmp_type != parseBMP::TYPE_INIT_MSG)
            mbus_ptr->update_Router(r_object, mbus_ptr->ROUTER_ACTION_FIRST);              // add the router entry

        // only process the peering info if the message includes it
        if (bmp_type < 4) {
            // Update p_entry hash_id now that add_Router updated it.
            memcpy(p_entry.router_hash_id, r_object.hash_id, sizeof(r_object.hash_id));

            if (bmp_type != parseBMP::TYPE_PEER_UP)
                mbus_ptr->update_Peer(p_entry, NULL, NULL, mbus_ptr->PEER_ACTION_FIRST);     // add the peer entry
        }

        /*
         * At this point we only have the BMP header message, what happens next depends
         *      on the BMP message type.
         */
        switch (bmp_type) {
            case parseBMP::TYPE_PEER_DOWN : { // Peer down type

                MsgBusInterface::obj_peer_down_event down_event = {};

                if (pBMP->parsePeerDownEventHdr(read_fd,down_event)) {
                    pBMP->bufferBMPMessage(read_fd);


                    // Prepare the BGP parser
                    pBGP = new parseBGP(logger, mbus_ptr, &p_entry, (char *)r_object.ip_addr,
                                        &peer_info_map[string(reinterpret_cast<char*>(p_entry.hash_id))]);

                    if (cfg->debug_bgp)
                       pBGP->enableDebug();

                    // Check if the reason indicates we have a BGP message that follows
                    switch (down_event.bmp_reason) {
                        case 1 : { // Local system close with BGP notify
                            snprintf(down_event.error_text, sizeof(down_event.error_text),
                                    "Local close by (%s) for peer (%s) : ", r_object.ip_addr,
                                    p_entry.peer_addr);
                            pBGP->handleDownEvent(pBMP->bmp_data, pBMP->bmp_data_len, down_event);
                            break;
                        }
                        case 2 : // Local system close, no bgp notify
                        {
                            // Read two byte code corresponding to the FSM event
                            uint16_t fsm_event = 0 ;
                            memcpy(&fsm_event, pBMP->bmp_data, 2);
                            bgp::SWAP_BYTES(&fsm_event);

                            snprintf(down_event.error_text, sizeof(down_event.error_text),
                                    "Local (%s) closed peer (%s) session: fsm_event=%d, No BGP notify message.",
                                    r_object.ip_addr,p_entry.peer_addr, fsm_event);
                            break;
                        }
                        case 3 : { // remote system close with bgp notify
                            snprintf(down_event.error_text, sizeof(down_event.error_text),
                                    "Remote peer (%s) closed local (%s) session: ", r_object.ip_addr,
                                    p_entry.peer_addr);

                            pBGP->handleDownEvent(pBMP->bmp_data, pBMP->bmp_data_len, down_event);
                            break;
                        }
                    }

                    delete pBGP;            // Free the bgp parser after each use.

                    // Add event to the database
                    mbus_ptr->update_Peer(p_entry, NULL, &down_event, mbus_ptr->PEER_ACTION_DOWN);

                } else {
                    LOG_ERR("Error with client socket %d", read_fd);
                    // Make sure to free the resource
                    throw "BMPReader: Unable to read from client socket";
                }
                break;
            }

            case parseBMP::TYPE_PEER_UP : // Peer up type
            {
                MsgBusInterface::obj_peer_up_event up_event = {};

                if (pBMP->parsePeerUpEventHdr(read_fd, up_event)) {
                    LOG_INFO("%s: PEER UP Received, local addr=%s:%hu remote addr=%s:%hu", client->c_ip,
                            up_event.local_ip, up_event.local_port, p_entry.peer_addr, up_event.remote_port);

                    pBMP->bufferBMPMessage(read_fd);

                    // Prepare the BGP parser
                    pBGP = new parseBGP(logger, mbus_ptr, &p_entry, (char *)r_object.ip_addr,
                                        &peer_info_map[string(reinterpret_cast<char*>(p_entry.hash_id))]);

                    if (cfg->debug_bgp)
                       pBGP->enableDebug();

                    // Parse the BGP sent/received open messages
                    pBGP->handleUpEvent(pBMP->bmp_data, pBMP->bmp_data_len, &up_event);

                    // Free the bgp parser
                    delete pBGP;

                    // Add the up event to the DB
                    mbus_ptr->update_Peer(p_entry, &up_event, NULL, mbus_ptr->PEER_ACTION_UP);

                } else {
                    LOG_NOTICE("%s: PEER UP Received but failed to parse the BMP header.", client->c_ip);
                }
                break;
            }

            case parseBMP::TYPE_ROUTE_MON : { // Route monitoring type
                pBMP->bufferBMPMessage(read_fd);

                /*
                 * Read and parse the the BGP message from the client.
                 *     parseBGP will update mysql directly
                 */
                pBGP = new parseBGP(logger, mbus_ptr, &p_entry, (char *)r_object.ip_addr,
                                    &peer_info_map[string(reinterpret_cast<char*>(p_entry.hash_id))]);
                if (cfg->debug_bgp)
                    pBGP->enableDebug();

                pBGP->handleUpdate(pBMP->bmp_data, pBMP->bmp_data_len);
                delete pBGP;

                break;
            }

            case parseBMP::TYPE_STATS_REPORT : { // Stats Report
                MsgBusInterface::obj_stats_report stats = {};
                if (! pBMP->handleStatsReport(read_fd, stats))
                    // Add to mysql
                    mbus_ptr->add_StatReport(p_entry, stats);

                break;
            }

            case parseBMP::TYPE_INIT_MSG : { // Initiation Message
                LOG_INFO("%s: Init message received with length of %u", client->c_ip, pBMP->getBMPLength());
                pBMP->handleInitMsg(read_fd, r_object);

                // Update the router entry with the details
                mbus_ptr->update_Router(r_object, mbus_ptr->ROUTER_ACTION_INIT);
                break;
            }

            case parseBMP::TYPE_TERM_MSG : { // Termination Message
                LOG_INFO("%s: Term message received with length of %u", client->c_ip, pBMP->getBMPLength());


                pBMP->handleTermMsg(read_fd, r_object);

                LOG_INFO("Proceeding to disconnect router");
                mbus_ptr->update_Router(r_object, mbus_ptr->ROUTER_ACTION_TERM);
                close(client->c_sock);

                rval = false;                           // Indicate connection is closed
                break;
            }

        }

    } catch (char const *str) {
        // Mark the router as disconnected and update the error to be a local disconnect (no term message received)
        LOG_INFO("%s: Caught: %s", client->c_ip, str);
        disconnect(client, mbus_ptr, parseBMP::TERM_REASON_OPENBMP_CONN_ERR, str);

        delete pBMP;                    // Make sure to free the resource
        throw str;
    }

    // Send BMP RAW packet data
    mbus_ptr->send_bmp_raw(router_hash_id, p_entry, pBMP->bmp_packet, pBMP->bmp_packet_len);

    // Free the bmp parser
    delete pBMP;

    return rval;
}

/**
 * disconnect/close bmp stream
 *
 * Closes the BMP stream and disconnects router as needed
 *
 * \param [in]  client      Client information pointer
 * \param [in]  mbus_ptr     The database pointer referencer - DB should be already initialized
 * \param [in]  reason_code The reason code for closing the stream/feed
 * \param [in]  reason_text String detailing the reason for close
 *
 */
void BMPReader::disconnect(BMPListener::ClientInfo *client, MsgBusInterface *mbus_ptr, int reason_code, char const *reason_text) {

    MsgBusInterface::obj_router r_object;
    bzero(&r_object, sizeof(r_object));
    memcpy(r_object.hash_id, router_hash_id, sizeof(r_object.hash_id));
    memcpy(r_object.ip_addr, client->c_ip, sizeof(client->c_ip));

    r_object.term_reason_code = reason_code;
    if (reason_text != NULL)
        snprintf(r_object.term_reason_text, sizeof(r_object.term_reason_text), "%s", reason_text);

    mbus_ptr->update_Router(r_object, mbus_ptr->ROUTER_ACTION_TERM);

    close(client->c_sock);
}

/*
 * Enable/Disable debug
 */
void BMPReader::enableDebug() {
    debug = true;
}

void BMPReader::disableDebug() {
    debug = false;
}
