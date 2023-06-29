#include "seader_worker_i.h"

#include <flipper_format/flipper_format.h>

#define TAG "SeaderWorker"

#define APDU_HEADER_LEN 5
#define ASN1_PREFIX 6
#define ASN1_DEBUG true

#define RFAL_PICOPASS_TXRX_FLAGS                                                    \
    (FURI_HAL_NFC_LL_TXRX_FLAGS_CRC_TX_MANUAL | FURI_HAL_NFC_LL_TXRX_FLAGS_AGC_ON | \
     FURI_HAL_NFC_LL_TXRX_FLAGS_PAR_RX_REMV | FURI_HAL_NFC_LL_TXRX_FLAGS_CRC_RX_KEEP)

// TODO: const
uint8_t GET_RESPONSE[] = {0x00, 0xc0, 0x00, 0x00, 0xff};

char payloadDebug[384] = {0};
char display[SEADER_UART_RX_BUF_SIZE * 2 + 1] = {0};
char asn1_log[SEADER_UART_RX_BUF_SIZE] = {0};
bool requestPacs = true;

// Forward declaration
void sendCardDetected(SeaderUartBridge* seader_uart, CardDetails_t* cardDetails);

static void seader_worker_enable_field() {
    furi_hal_nfc_ll_txrx_on();
    furi_hal_nfc_exit_sleep();
    furi_hal_nfc_ll_poll();
}

static ReturnCode seader_worker_disable_field(ReturnCode rc) {
    furi_hal_nfc_ll_txrx_off();
    furi_hal_nfc_start_sleep();
    return rc;
}

/***************************** Seader Worker API *******************************/

SeaderWorker* seader_worker_alloc() {
    SeaderWorker* seader_worker = malloc(sizeof(SeaderWorker));

    // Worker thread attributes
    seader_worker->thread =
        furi_thread_alloc_ex("SeaderWorker", 8192, seader_worker_task, seader_worker);

    seader_worker->callback = NULL;
    seader_worker->context = NULL;
    seader_worker->storage = furi_record_open(RECORD_STORAGE);

    seader_worker_change_state(seader_worker, SeaderWorkerStateReady);

    return seader_worker;
}

void seader_worker_free(SeaderWorker* seader_worker) {
    furi_assert(seader_worker);

    furi_thread_free(seader_worker->thread);

    furi_record_close(RECORD_STORAGE);

    free(seader_worker);
}

SeaderWorkerState seader_worker_get_state(SeaderWorker* seader_worker) {
    return seader_worker->state;
}

void seader_worker_start(
    SeaderWorker* seader_worker,
    SeaderWorkerState state,
    SeaderUartBridge* uart,
    SeaderCredential* credential,
    SeaderWorkerCallback callback,
    void* context) {
    furi_assert(seader_worker);
    furi_assert(uart);
    furi_assert(credential);

    seader_worker->callback = callback;
    seader_worker->context = context;
    seader_worker->uart = uart;
    seader_worker->credential = credential;
    seader_worker_change_state(seader_worker, state);
    furi_thread_start(seader_worker->thread);
}

void seader_worker_stop(SeaderWorker* seader_worker) {
    furi_assert(seader_worker);
    if(seader_worker->state == SeaderWorkerStateBroken ||
       seader_worker->state == SeaderWorkerStateReady) {
        return;
    }
    seader_worker_disable_field(ERR_NONE);

    seader_worker_change_state(seader_worker, SeaderWorkerStateStop);
    furi_thread_join(seader_worker->thread);
}

void seader_worker_change_state(SeaderWorker* seader_worker, SeaderWorkerState state) {
    seader_worker->state = state;
}

/***************************** Seader Worker Thread *******************************/

void* calloc(size_t count, size_t size) {
    return malloc(count * size);
}

void nfc_scene_field_on_enter() {
    furi_hal_nfc_field_on();
    furi_hal_nfc_exit_sleep();
}

void nfc_scene_field_on_exit() {
    furi_hal_nfc_sleep();
    furi_hal_nfc_field_off();
}

bool sendAPDU(
    SeaderUartBridge* seader_uart,
    uint8_t CLA,
    uint8_t INS,
    uint8_t P1,
    uint8_t P2,
    uint8_t* payload,
    uint8_t length) {
    if(APDU_HEADER_LEN + length > SEADER_UART_RX_BUF_SIZE) {
        FURI_LOG_E(TAG, "Cannot send message, too long: %d", APDU_HEADER_LEN + length);
        return false;
    }

    uint8_t* apdu = malloc(APDU_HEADER_LEN + length);
    apdu[0] = CLA;
    apdu[1] = INS;
    apdu[2] = P1;
    apdu[3] = P2;
    apdu[4] = length;
    memcpy(apdu + APDU_HEADER_LEN, payload, length);

    PC_to_RDR_XfrBlock(seader_uart, apdu, APDU_HEADER_LEN + length);
    free(apdu);
    return true;
}

static int toString(const void* buffer, size_t size, void* app_key) {
    if(app_key) {
        char* str = (char*)app_key;
        size_t next = strlen(str);
        strncpy(str + next, buffer, size);
    } else {
        uint8_t next = strlen(asn1_log);
        strncpy(asn1_log + next, buffer, size);
    }
    return 0;
}

bool mf_df_check_card_type(uint8_t ATQA0, uint8_t ATQA1, uint8_t SAK) {
    return ATQA0 == 0x44 && ATQA1 == 0x03 && SAK == 0x20;
}

bool mf_classic_check_card_type(uint8_t ATQA0, uint8_t ATQA1, uint8_t SAK) {
    UNUSED(ATQA1);
    if((ATQA0 == 0x44 || ATQA0 == 0x04) && (SAK == 0x08 || SAK == 0x88 || SAK == 0x09)) {
        return true;
    } else if((ATQA0 == 0x01) && (ATQA1 == 0x0F) && (SAK == 0x01)) {
        //skylanders support
        return true;
    } else if((ATQA0 == 0x42 || ATQA0 == 0x02) && (SAK == 0x18)) {
        return true;
    } else {
        return false;
    }
}

bool read_nfc(SeaderUartBridge* seader_uart) {
    FuriHalNfcDevData nfc_data = {};
    bool rtn = false;
    if(furi_hal_nfc_detect(&nfc_data, 300)) {
        // Process first found device
        if(nfc_data.type == FuriHalNfcTypeA) {
            FURI_LOG_D(TAG, "NFC-A detected");
            CardDetails_t* cardDetails = 0;
            cardDetails = calloc(1, sizeof *cardDetails);
            assert(cardDetails);

            OCTET_STRING_fromBuf(&cardDetails->csn, (const char*)nfc_data.uid, nfc_data.uid_len);
            uint8_t protocolBytes[] = {0x00, FrameProtocol_nfc};
            OCTET_STRING_fromBuf(
                &cardDetails->protocol, (const char*)protocolBytes, sizeof(protocolBytes));

            OCTET_STRING_t sak = {.buf = &(nfc_data.sak), .size = 1};
            cardDetails->sak = &sak;

            uint8_t fake_seos_ats[] = {0x78, 0x77, 0x80, 0x02};
            uint8_t fake_desfire_ats[] = {0x75, 0x77, 0x81, 0x02, 0x80};
            if(mf_df_check_card_type(nfc_data.atqa[0], nfc_data.atqa[1], nfc_data.sak)) {
                FURI_LOG_D(TAG, "Desfire");
                OCTET_STRING_t atqa = {.buf = fake_desfire_ats, .size = sizeof(fake_desfire_ats)};
                cardDetails->atqa = &atqa;
                sendCardDetected(seader_uart, cardDetails);
                rtn = true;
            } else if(mf_classic_check_card_type(
                          nfc_data.atqa[0], nfc_data.atqa[1], nfc_data.sak)) {
                FURI_LOG_D(TAG, "MFC");
                OCTET_STRING_t atqa = {.buf = nfc_data.atqa, .size = sizeof(nfc_data.atqa)};
                cardDetails->atqa = &atqa;
                sendCardDetected(seader_uart, cardDetails);
                rtn = true;
            } else if(nfc_data.interface == FuriHalNfcInterfaceIsoDep) {
                FURI_LOG_D(TAG, "ISO-DEP");
                OCTET_STRING_t atqa = {.buf = fake_seos_ats, .size = sizeof(fake_seos_ats)};
                cardDetails->atqa = &atqa;
                sendCardDetected(seader_uart, cardDetails);
                rtn = true;
            }
            ASN_STRUCT_FREE(asn_DEF_CardDetails, cardDetails);
        }
    }
    return rtn;
}

bool detect_nfc(SeaderWorker* seader_worker) {
    SeaderUartBridge* seader_uart = seader_worker->uart;

    while(seader_worker->state == SeaderWorkerStateRead14a) {
        // Card found
        if(read_nfc(seader_uart)) {
            return true;
        }

        furi_delay_ms(100);
    }
    return false;
}

void sendPayload(
    SeaderUartBridge* seader_uart,
    Payload_t* payload,
    uint8_t to,
    uint8_t from,
    uint8_t replyTo) {
    uint8_t rBuffer[SEADER_UART_RX_BUF_SIZE] = {0};

    asn_enc_rval_t er = der_encode_to_buffer(
        &asn_DEF_Payload, payload, rBuffer + ASN1_PREFIX, sizeof(rBuffer) - ASN1_PREFIX);

#ifdef ASN1_DEBUG
    if(er.encoded > -1) {
        memset(payloadDebug, 0, sizeof(payloadDebug));
        (&asn_DEF_Payload)->op->print_struct(&asn_DEF_Payload, payload, 1, toString, payloadDebug);
        if(strlen(payloadDebug) > 0) {
            FURI_LOG_D(TAG, "Sending payload: %s", payloadDebug);
        }
    }
#endif
    //0xa0, 0xda, 0x02, 0x63, 0x00, 0x00, 0x0a,
    //0x44, 0x0a, 0x44, 0x00, 0x00, 0x00, 0xa0, 0x02, 0x96, 0x00
    rBuffer[0] = to;
    rBuffer[1] = from;
    rBuffer[2] = replyTo;

    sendAPDU(seader_uart, 0xA0, 0xDA, 0x02, 0x63, rBuffer, 6 + er.encoded);
}

void sendResponse(
    SeaderUartBridge* seader_uart,
    Response_t* response,
    uint8_t to,
    uint8_t from,
    uint8_t replyTo) {
    Payload_t* payload = 0;
    payload = calloc(1, sizeof *payload);
    assert(payload);

    payload->present = Payload_PR_response;
    payload->choice.response = *response;

    sendPayload(seader_uart, payload, to, from, replyTo);

    ASN_STRUCT_FREE(asn_DEF_Payload, payload);
}

void sendRequestPacs(SeaderUartBridge* seader_uart) {
    RequestPacs_t* requestPacs = 0;
    requestPacs = calloc(1, sizeof *requestPacs);
    assert(requestPacs);

    requestPacs->contentElementTag = ContentElementTag_implicitFormatPhysicalAccessBits;

    SamCommand_t* samCommand = 0;
    samCommand = calloc(1, sizeof *samCommand);
    assert(samCommand);

    samCommand->present = SamCommand_PR_requestPacs;
    samCommand->choice.requestPacs = *requestPacs;

    Payload_t* payload = 0;
    payload = calloc(1, sizeof *payload);
    assert(payload);

    payload->present = Payload_PR_samCommand;
    payload->choice.samCommand = *samCommand;

    sendPayload(seader_uart, payload, 0x44, 0x0a, 0x44);

    ASN_STRUCT_FREE(asn_DEF_RequestPacs, requestPacs);
    ASN_STRUCT_FREE(asn_DEF_SamCommand, samCommand);
    ASN_STRUCT_FREE(asn_DEF_Payload, payload);
}

void sendCardDetected(SeaderUartBridge* seader_uart, CardDetails_t* cardDetails) {
    CardDetected_t* cardDetected = 0;
    cardDetected = calloc(1, sizeof *cardDetected);
    assert(cardDetected);

    cardDetected->detectedCardDetails = *cardDetails;

    SamCommand_t* samCommand = 0;
    samCommand = calloc(1, sizeof *samCommand);
    assert(samCommand);

    samCommand->present = SamCommand_PR_cardDetected;
    samCommand->choice.cardDetected = *cardDetected;

    Payload_t* payload = 0;
    payload = calloc(1, sizeof *payload);
    assert(payload);

    payload->present = Payload_PR_samCommand;
    payload->choice.samCommand = *samCommand;

    sendPayload(seader_uart, payload, 0x44, 0x0a, 0x44);

    ASN_STRUCT_FREE(asn_DEF_CardDetected, cardDetected);
    ASN_STRUCT_FREE(asn_DEF_SamCommand, samCommand);
    ASN_STRUCT_FREE(asn_DEF_Payload, payload);
}

bool unpack_pacs(SeaderCredential* seader_credential, uint8_t* buf, size_t size) {
    PAC_t* pac = 0;
    pac = calloc(1, sizeof *pac);
    assert(pac);
    bool rtn = false;

    asn_dec_rval_t rval = asn_decode(0, ATS_DER, &asn_DEF_PAC, (void**)&pac, buf, size);

    if(rval.code == RC_OK) {
        char pacDebug[384] = {0};
        (&asn_DEF_PAC)->op->print_struct(&asn_DEF_PAC, pac, 1, toString, pacDebug);
        if(strlen(pacDebug) > 0) {
            FURI_LOG_D(TAG, "Received pac: %s", pacDebug);
        }

        if(pac->size <= sizeof(seader_credential->credential)) {
            seader_credential->bit_length = pac->size * 8 - pac->bits_unused;
            memcpy(&seader_credential->credential, pac->buf, pac->size);
            seader_credential->credential = __builtin_bswap64(seader_credential->credential);
            seader_credential->credential = seader_credential->credential >>
                                            (64 - seader_credential->bit_length);
            rtn = true;
        }
        // TODO: make credential into a 12 byte array
    }

    ASN_STRUCT_FREE(asn_DEF_PAC, pac);
    return rtn;
}

bool parseSamResponse(SeaderWorker* seader_worker, SamResponse_t* samResponse) {
    SeaderUartBridge* seader_uart = seader_worker->uart;
    SeaderCredential* credential = seader_worker->credential;

    if(samResponse->size == 0) {
        if(requestPacs) {
            // FURI_LOG_D(TAG, "samResponse %d => requesting PACS", samResponse->size);
            sendRequestPacs(seader_uart);
            requestPacs = false;
        } else {
            // FURI_LOG_D(TAG, "samResponse %d, no action", samResponse->size);
            if(seader_worker->callback) {
                seader_worker->callback(SeaderWorkerEventFail, seader_worker->context);
            }
        }
    } else if(unpack_pacs(credential, samResponse->buf, samResponse->size)) {
        if(seader_worker->callback) {
            seader_worker->callback(SeaderWorkerEventSuccess, seader_worker->context);
        }
    } else {
        memset(display, 0, sizeof(display));
        for(uint8_t i = 0; i < samResponse->size; i++) {
            snprintf(display + (i * 2), sizeof(display), "%02x", samResponse->buf[i]);
        }
        FURI_LOG_D(TAG, "unknown samResponse %d: %s", samResponse->size, display);
    }

    return false;
}

bool parseResponse(SeaderWorker* seader_worker, Response_t* response) {
    switch(response->present) {
    case Response_PR_samResponse:
        parseSamResponse(seader_worker, &response->choice.samResponse);
        break;
    default:
        break;
    };
    return false;
}

void sendNFCRx(SeaderUartBridge* seader_uart, uint8_t* buffer, size_t len) {
    OCTET_STRING_t rxData = {.buf = buffer, .size = len};
    uint8_t status[] = {0x00, 0x00};
    RfStatus_t rfStatus = {.buf = status, .size = 2};

    NFCRx_t* nfcRx = 0;
    nfcRx = calloc(1, sizeof *nfcRx);
    assert(nfcRx);

    nfcRx->rfStatus = rfStatus;
    nfcRx->data = &rxData;

    NFCResponse_t* nfcResponse = 0;
    nfcResponse = calloc(1, sizeof *nfcResponse);
    assert(nfcResponse);

    nfcResponse->present = NFCResponse_PR_nfcRx;
    nfcResponse->choice.nfcRx = *nfcRx;

    Response_t* response = 0;
    response = calloc(1, sizeof *response);
    assert(response);

    response->present = Response_PR_nfcResponse;
    response->choice.nfcResponse = *nfcResponse;

    sendResponse(seader_uart, response, 0x14, 0x0a, 0x0);

    ASN_STRUCT_FREE(asn_DEF_NFCRx, nfcRx);
    ASN_STRUCT_FREE(asn_DEF_NFCResponse, nfcResponse);
    ASN_STRUCT_FREE(asn_DEF_Response, response);
}

bool iso14443aTransmit(SeaderWorker* seader_worker, uint8_t* buffer, size_t len) {
    SeaderUartBridge* seader_uart = seader_worker->uart;
    FuriHalNfcTxRxContext tx_rx = {.tx_rx_type = FuriHalNfcTxRxTypeDefault};
    memcpy(&tx_rx.tx_data, buffer, len);
    tx_rx.tx_bits = len * 8;

    if(furi_hal_nfc_tx_rx_full(&tx_rx)) {
        furi_delay_ms(1);
        size_t length = tx_rx.rx_bits / 8;
        memset(display, 0, sizeof(display));
        for(uint8_t i = 0; i < length; i++) {
            snprintf(display + (i * 2), sizeof(display), "%02x", tx_rx.rx_data[i]);
        }
        // FURI_LOG_D(TAG, "NFC Response %d: %s", length, display);
        sendNFCRx(seader_uart, tx_rx.rx_data, length);
    } else {
        FURI_LOG_W(TAG, "Bad exchange");
        if(seader_worker->callback) {
            seader_worker->callback(SeaderWorkerEventFail, seader_worker->context);
        }
    }

    return false;
}

bool iso15693Transmit(SeaderWorker* seader_worker, uint8_t* buffer, size_t len) {
    SeaderUartBridge* seader_uart = seader_worker->uart;
    char display[SEADER_UART_RX_BUF_SIZE * 2 + 1] = {0};
    FuriHalNfcReturn ret;
    uint16_t recvLen = 0;
    uint32_t flags = RFAL_PICOPASS_TXRX_FLAGS;
    uint32_t fwt = furi_hal_nfc_ll_ms2fc(20);

    uint8_t rxBuffer[64] = {0};
    ret = furi_hal_nfc_ll_txrx(buffer, len, rxBuffer, sizeof(rxBuffer), &recvLen, flags, fwt);

    if(ret == FuriHalNfcReturnOk) {
        memset(display, 0, sizeof(display));
        for(uint8_t i = 0; i < recvLen; i++) {
            snprintf(display + (i * 2), sizeof(display), "%02x", rxBuffer[i]);
        }
        // FURI_LOG_D(TAG, "Result %d %s", recvLen, display);

        sendNFCRx(seader_uart, rxBuffer, recvLen);
    } else if(ret == FuriHalNfcReturnCrc) {
        memset(display, 0, sizeof(display));
        for(uint8_t i = 0; i < recvLen; i++) {
            snprintf(display + (i * 2), sizeof(display), "%02x", rxBuffer[i]);
        }
        // FURI_LOG_D(TAG, "[CRC error] Result %d %s", recvLen, display);

        sendNFCRx(seader_uart, rxBuffer, recvLen);

        return true;
    } else {
        FURI_LOG_E(TAG, "furi_hal_nfc_ll_txrx Error %d", ret);

        if(seader_worker->callback) {
            seader_worker->callback(SeaderWorkerEventFail, seader_worker->context);
        }
    }

    return ret == FuriHalNfcReturnOk;
}

bool parseNfcCommandTransmit(SeaderWorker* seader_worker, NFCSend_t* nfcSend) {
    long timeOut = nfcSend->timeOut;
    Protocol_t protocol = nfcSend->protocol;
    FrameProtocol_t frameProtocol = protocol.buf[1];

#ifdef ASN1_DEBUG
    char display[SEADER_UART_RX_BUF_SIZE * 2 + 1] = {0};
    for(uint8_t i = 0; i < nfcSend->data.size; i++) {
        snprintf(display + (i * 2), sizeof(display), "%02x", nfcSend->data.buf[i]);
    }

    char protocolName[8] = {0};
    (&asn_DEF_FrameProtocol)
        ->op->print_struct(&asn_DEF_FrameProtocol, &frameProtocol, 1, toString, protocolName);

    FURI_LOG_D(
        TAG,
        "Transmit (%ld timeout) %d bytes [%s] via %s",
        timeOut,
        nfcSend->data.size,
        display,
        protocolName);
#else
    UNUSED(timeOut);
#endif

    if(frameProtocol == FrameProtocol_iclass) {
        return iso15693Transmit(seader_worker, nfcSend->data.buf, nfcSend->data.size);
    } else if(frameProtocol == FrameProtocol_nfc) {
        return iso14443aTransmit(seader_worker, nfcSend->data.buf, nfcSend->data.size);
    }
    return false;
}

bool parseNfcOff(SeaderUartBridge* seader_uart) {
    FURI_LOG_D(TAG, "Set Field Off");
    seader_worker_disable_field(ERR_NONE);
    nfc_scene_field_on_exit();

    NFCResponse_t* nfcResponse = 0;
    nfcResponse = calloc(1, sizeof *nfcResponse);
    assert(nfcResponse);

    nfcResponse->present = NFCResponse_PR_nfcAck;

    Response_t* response = 0;
    response = calloc(1, sizeof *response);
    assert(response);

    response->present = Response_PR_nfcResponse;
    response->choice.nfcResponse = *nfcResponse;

    sendResponse(seader_uart, response, 0x44, 0x0a, 0);

    ASN_STRUCT_FREE(asn_DEF_Response, response);
    ASN_STRUCT_FREE(asn_DEF_NFCResponse, nfcResponse);

    return false;
}

bool parseNfcCommand(SeaderWorker* seader_worker, NFCCommand_t* nfcCommand) {
    SeaderUartBridge* seader_uart = seader_worker->uart;
    switch(nfcCommand->present) {
    case NFCCommand_PR_nfcSend:
        parseNfcCommandTransmit(seader_worker, &nfcCommand->choice.nfcSend);
        break;
    case NFCCommand_PR_nfcOff:
        parseNfcOff(seader_uart);
        break;
    default:
        FURI_LOG_W(TAG, "unparsed NFCCommand");
        break;
    };

    return false;
}

bool stateMachine(SeaderWorker* seader_worker, Payload_t* payload) {
    switch(payload->present) {
    case Payload_PR_response:
        parseResponse(seader_worker, &payload->choice.response);
        break;
    case Payload_PR_nfcCommand:
        parseNfcCommand(seader_worker, &payload->choice.nfcCommand);
        break;
    case Payload_PR_errorResponse:
        // TODO: screen saying this was a failure, or maybe start over?
        if(seader_worker->callback) {
            seader_worker->callback(SeaderWorkerEventFail, seader_worker->context);
        }
        break;
    default:
        FURI_LOG_W(TAG, "unhandled payload");
        break;
    };

    return false;
}

bool processSuccessResponse(SeaderWorker* seader_worker, uint8_t* apdu, size_t len) {
    Payload_t* payload = 0;
    payload = calloc(1, sizeof *payload);
    assert(payload);

    asn_dec_rval_t rval =
        asn_decode(0, ATS_DER, &asn_DEF_Payload, (void**)&payload, apdu + 6, len - 6);
    if(rval.code == RC_OK) {
#ifdef ASN1_DEBUG
        memset(payloadDebug, 0, sizeof(payloadDebug));
        (&asn_DEF_Payload)->op->print_struct(&asn_DEF_Payload, payload, 1, toString, payloadDebug);
        if(strlen(payloadDebug) > 0) {
            FURI_LOG_D(TAG, "Received payload: %s", payloadDebug);
        }
#endif
        stateMachine(seader_worker, payload);
    }

    ASN_STRUCT_FREE(asn_DEF_Payload, payload);
    return (rval.code == RC_OK);
}

bool processAPDU(SeaderWorker* seader_worker, uint8_t* apdu, size_t len) {
    SeaderUartBridge* seader_uart = seader_worker->uart;
    if(len < 2) {
        return false;
    }
    /*
    memset(display, 0, sizeof(display));
    for(uint8_t i = 0; i < len; i++) {
        snprintf(display + (i * 2), sizeof(display), "%02x", apdu[i]);
    }
    FURI_LOG_I(TAG, "APDU: %s", display);
    */

    uint8_t SW1 = apdu[len - 2];
    uint8_t SW2 = apdu[len - 1];

    switch(SW1) {
    case 0x61:
        // FURI_LOG_I(TAG, "Request %d bytes", SW2);
        GET_RESPONSE[4] = SW2;
        PC_to_RDR_XfrBlock(seader_uart, GET_RESPONSE, sizeof(GET_RESPONSE));
        return true;
        break;

    case 0x90:
        if(SW2 == 0x00) {
            if(len > 2) {
                return processSuccessResponse(seader_worker, apdu, len - 2);
            }
        }
        break;
    }

    return false;
}

ReturnCode picopass_card_init(SeaderUartBridge* seader_uart) {
    rfalPicoPassIdentifyRes idRes;
    rfalPicoPassSelectRes selRes;

    ReturnCode err;

    err = rfalPicoPassPollerIdentify(&idRes);
    if(err != ERR_NONE) {
        FURI_LOG_E(TAG, "rfalPicoPassPollerIdentify error %d", err);
        return err;
    }

    err = rfalPicoPassPollerSelect(idRes.CSN, &selRes);
    if(err != ERR_NONE) {
        FURI_LOG_E(TAG, "rfalPicoPassPollerSelect error %d", err);
        return err;
    }

    memset(display, 0, sizeof(display));
    for(uint8_t i = 0; i < RFAL_PICOPASS_MAX_BLOCK_LEN; i++) {
        snprintf(display + (i * 2), sizeof(display), "%02x", selRes.CSN[i]);
    }

    FURI_LOG_D(TAG, "Sending card detected info: %s", display);

    CardDetails_t* cardDetails = 0;
    cardDetails = calloc(1, sizeof *cardDetails);
    assert(cardDetails);

    uint8_t protocolBytes[] = {0x00, FrameProtocol_iclass};
    OCTET_STRING_fromBuf(
        &cardDetails->protocol, (const char*)protocolBytes, sizeof(protocolBytes));
    OCTET_STRING_fromBuf(&cardDetails->csn, (const char*)selRes.CSN, RFAL_PICOPASS_MAX_BLOCK_LEN);

    sendCardDetected(seader_uart, cardDetails);

    ASN_STRUCT_FREE(asn_DEF_CardDetails, cardDetails);
    return ERR_NONE;
}

ReturnCode picopass_card_detect() {
    ReturnCode err;

    err = rfalPicoPassPollerInitialize();
    if(err != ERR_NONE) {
        FURI_LOG_E(TAG, "rfalPicoPassPollerInitialize error %d", err);
        return err;
    }

    err = rfalFieldOnAndStartGT();
    if(err != ERR_NONE) {
        FURI_LOG_E(TAG, "rfalFieldOnAndStartGT error %d", err);
        return err;
    }

    err = rfalPicoPassPollerCheckPresence();
    if(err != ERR_RF_COLLISION) {
        if(err != ERR_TIMEOUT) {
            FURI_LOG_E(TAG, "rfalPicoPassPollerCheckPresence error %d", err);
        }
        return err;
    }

    return ERR_NONE;
}

ReturnCode picopass_card_read(SeaderWorker* seader_worker) {
    SeaderUartBridge* seader_uart = seader_worker->uart;
    ReturnCode err = ERR_TIMEOUT;

    while(seader_worker->state == SeaderWorkerStateReadPicopass) {
        // Card found
        if(picopass_card_detect() == ERR_NONE) {
            err = picopass_card_init(seader_uart);
            if(err != ERR_NONE) {
                FURI_LOG_E(TAG, "picopass_card_init error %d", err);
            }
            break;
        }

        furi_delay_ms(100);
    }

    return err;
}

void seader_worker_process_message(SeaderWorker* seader_worker, CCID_Message* message) {
    if(processAPDU(seader_worker, message->payload, message->dwLength)) {
        // no-op
    } else {
        memset(display, 0, sizeof(display));
        for(uint8_t i = 0; i < message->dwLength; i++) {
            snprintf(display + (i * 2), sizeof(display), "%02x", message->payload[i]);
        }

        FURI_LOG_W(TAG, "Unknown block: [%ld] %s", message->dwLength, display);
        if(seader_worker->callback) {
            seader_worker->callback(SeaderWorkerEventFail, seader_worker->context);
        }
    }
}

int32_t seader_worker_task(void* context) {
    SeaderWorker* seader_worker = context;
    SeaderUartBridge* seader_uart = seader_worker->uart;

    if(seader_worker->state == SeaderWorkerStateCheckSam) {
        furi_delay_ms(1000);
        FURI_LOG_D(TAG, "PC_to_RDR_GetSlotStatus");
        PC_to_RDR_GetSlotStatus(seader_uart);
    } else if(seader_worker->state == SeaderWorkerStateReadPicopass) {
        FURI_LOG_D(TAG, "Read Picopass");
        requestPacs = true;
        seader_credential_clear(seader_worker->credential);
        seader_worker_enable_field();
        if(picopass_card_read(seader_worker) != ERR_NONE) {
            // Turn off if cancelled / no card found
            seader_worker_disable_field(ERR_NONE);
        }
    } else if(seader_worker->state == SeaderWorkerStateRead14a) {
        FURI_LOG_D(TAG, "Read 14a");
        requestPacs = true;
        seader_credential_clear(seader_worker->credential);
        nfc_scene_field_on_enter();
        if(!detect_nfc(seader_worker)) {
            // Turn off if cancelled / no card found
            nfc_scene_field_on_exit();
        }
    }
    FURI_LOG_D(TAG, "Worker Task Complete");
    seader_worker_change_state(seader_worker, SeaderWorkerStateReady);

    return 0;
}
