/****************************************************************************
 *  Copyright (C) 2022 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 ***************************************************************************/
#define LOG_TAG "lea_server_stm"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "bt_addr.h"
#include "bt_lea_server.h"
#include "bt_list.h"
#include "hci_parser.h"
#include "lea_audio_sink.h"
#include "lea_audio_source.h"
#include "lea_server_service.h"
#include "lea_server_state_machine.h"
#include "sal_adapter_interface.h"
#include "sal_lea_server_interface.h"
#include "service_loop.h"

#include "bt_utils.h"
#include "utils/log.h"

typedef enum pending_state {
    PENDING_NONE = 0x0,
    PENDING_START = 0X02,
    PENDING_STOP = 0x04,
    PENDING_OFFLOAD_START = 0x08,
    PENDING_OFFLOAD_STOP = 0x10,
} pending_state_t;

typedef struct _lea_server_state_machine {
    state_machine_t sm;
    bool offloading;
    pending_state_t pending;
    bt_address_t addr;
    void* service;
    service_timer_t* offload_timer;
} lea_server_state_machine_t;

#define LEA_SERVER_OFFLOAD_TIMEOUT 500
#define LEA_SERVER_STM_DEBUG 1

#if LEA_SERVER_STM_DEBUG
static void lea_server_trans_debug(state_machine_t* sm, bt_address_t* addr,
    const char* action);
static void lea_server_event_debug(state_machine_t* sm, bt_address_t* addr,
    uint32_t event);
static const char* stack_event_to_string(lea_server_event_t event);

#define LEAS_DBG_ENTER(__sm, __addr) lea_server_trans_debug(__sm, __addr, "Enter")
#define LEAS_DBG_EXIT(__sm, __addr) lea_server_trans_debug(__sm, __addr, "Exit ")
#define LEAS_DBG_EVENT(__sm, __addr, __event) lea_server_event_debug(__sm, __addr, __event);
#else
#define LEAS_DBG_ENTER(__sm, __addr)
#define LEAS_DBG_EXIT(__sm, __addr)
#define LEAS_DBG_EVENT(__sm, __addr, __event)
#endif

extern bt_status_t lea_server_send_message(lea_server_msg_t* msg);

static void closed_enter(state_machine_t* sm);
static void closed_exit(state_machine_t* sm);
static void opening_enter(state_machine_t* sm);
static void opening_exit(state_machine_t* sm);
static void opened_enter(state_machine_t* sm);
static void opened_exit(state_machine_t* sm);
static void started_enter(state_machine_t* sm);
static void started_exit(state_machine_t* sm);
static void closing_enter(state_machine_t* sm);
static void closing_exit(state_machine_t* sm);

static bool closed_process_event(state_machine_t* sm, uint32_t event,
    void* p_data);
static bool opening_process_event(state_machine_t* sm, uint32_t event,
    void* p_data);
static bool opened_process_event(state_machine_t* sm, uint32_t event,
    void* p_data);
static bool started_process_event(state_machine_t* sm, uint32_t event,
    void* p_data);
static bool closing_process_event(state_machine_t* sm, uint32_t event,
    void* p_data);

static bool flag_isset(lea_server_state_machine_t* leas_sm, pending_state_t flag);
static void flag_set(lea_server_state_machine_t* leas_sm, pending_state_t flag);
static void flag_clear(lea_server_state_machine_t* leas_sm, pending_state_t flag);

static const state_t closed_state = {
    .state_name = "Closed",
    .enter = closed_enter,
    .exit = closed_exit,
    .process_event = closed_process_event,
};

static const state_t opening_state = {
    .state_name = "Opening",
    .enter = opening_enter,
    .exit = opening_exit,
    .process_event = opening_process_event,
};

static const state_t opened_state = {
    .state_name = "Opened",
    .enter = opened_enter,
    .exit = opened_exit,
    .process_event = opened_process_event,
};

static const state_t started_state = {
    .state_name = "Started",
    .enter = started_enter,
    .exit = started_exit,
    .process_event = started_process_event,
};

static const state_t closing_state = {
    .state_name = "Closing",
    .enter = closing_enter,
    .exit = closing_exit,
    .process_event = closing_process_event,
};

#if LEA_SERVER_STM_DEBUG
static void lea_server_trans_debug(state_machine_t* sm, bt_address_t* addr, const char* action)
{
    char addr_str[BT_ADDR_STR_LENGTH] = { 0 };
    bt_addr_ba2str(addr, addr_str);
    BT_LOGD("%s State=%s, Peer=[%s]", action, hsm_get_current_state_name(sm), addr_str);
}

static void lea_server_event_debug(state_machine_t* sm, bt_address_t* addr, uint32_t event)
{
    char addr_str[BT_ADDR_STR_LENGTH] = { 0 };
    bt_addr_ba2str(addr, addr_str);
    BT_LOGD("ProcessEvent, State=%s, Peer=[%s], Event=%s", hsm_get_current_state_name(sm),
        addr_str, stack_event_to_string(event));
}

static const char* stack_event_to_string(lea_server_event_t event)
{
    switch (event) {
        CASE_RETURN_STR(DISCONNECT)
        CASE_RETURN_STR(CONFIG_CODEC)
        CASE_RETURN_STR(STARTUP)
        CASE_RETURN_STR(SHUTDOWN)
        CASE_RETURN_STR(TIMEOUT)
        CASE_RETURN_STR(OFFLOAD_START_REQ)
        CASE_RETURN_STR(OFFLOAD_STOP_REQ)
        CASE_RETURN_STR(OFFLOAD_START_EVT)
        CASE_RETURN_STR(OFFLOAD_STOP_EVT)
        CASE_RETURN_STR(OFFLOAD_TIMEOUT)
        CASE_RETURN_STR(STACK_EVENT_STACK_STATE)
        CASE_RETURN_STR(STACK_EVENT_CONNECTION_STATE)
        CASE_RETURN_STR(STACK_EVENT_METADATA_UPDATED)
        CASE_RETURN_STR(STACK_EVENT_STORAGE)
        CASE_RETURN_STR(STACK_EVENT_SERVICE)
        CASE_RETURN_STR(STACK_EVENT_STREAM_ADDED)
        CASE_RETURN_STR(STACK_EVENT_STREAM_REMOVED)
        CASE_RETURN_STR(STACK_EVENT_STREAM_STARTED)
        CASE_RETURN_STR(STACK_EVENT_STREAM_STOPPED)
        CASE_RETURN_STR(STACK_EVENT_STREAM_RESUME)
        CASE_RETURN_STR(STACK_EVENT_STREAM_SUSPEND)
        CASE_RETURN_STR(STACK_EVENT_STREAN_RECV)
        CASE_RETURN_STR(STACK_EVENT_STREAN_SENT)
        CASE_RETURN_STR(STACK_EVENT_ASE_CODEC_CONFIG)
        CASE_RETURN_STR(STACK_EVENT_ASE_QOS_CONFIG)
        CASE_RETURN_STR(STACK_EVENT_ASE_ENABLING)
        CASE_RETURN_STR(STACK_EVENT_ASE_STREAMING)
        CASE_RETURN_STR(STACK_EVENT_ASE_DISABLING)
        CASE_RETURN_STR(STACK_EVENT_ASE_RELEASING)
        CASE_RETURN_STR(STACK_EVENT_ASE_IDLE)
        CASE_RETURN_STR(STACK_EVENT_INIT)
        CASE_RETURN_STR(STACK_EVENT_ANNOUNCE)
        CASE_RETURN_STR(STACK_EVENT_DISCONNECT)
        CASE_RETURN_STR(STACK_EVENT_CLEANUP)
    default:
        return "UNKNOWN_HF_EVENT";
    }
}
#endif

static bool flag_isset(lea_server_state_machine_t* leas_sm, pending_state_t flag)
{
    return (bool)(leas_sm->pending & flag);
}

static void flag_set(lea_server_state_machine_t* leas_sm, pending_state_t flag)
{
    leas_sm->pending |= flag;
}

static void flag_clear(lea_server_state_machine_t* leas_sm, pending_state_t flag)
{
    leas_sm->pending &= ~flag;
}

static void bt_hci_event_callback(bt_hci_event_t* hci_event, void* context)
{
    lea_server_state_machine_t* leas_sm = (lea_server_state_machine_t*)context;
    lea_server_msg_t* msg;
    lea_server_event_t event;

    BT_LOGD("%s, evt_code:0x%x, len:%d", __func__, hci_event->evt_code,
        hci_event->length);
    BT_DUMPBUFFER("vsc", (uint8_t*)hci_event->params, hci_event->length);

    if (flag_isset(leas_sm, PENDING_OFFLOAD_START)) {
        event = OFFLOAD_START_EVT;
        flag_clear(leas_sm, PENDING_OFFLOAD_START);
    } else if (flag_isset(leas_sm, PENDING_OFFLOAD_STOP)) {
        event = OFFLOAD_STOP_EVT;
        flag_clear(leas_sm, PENDING_OFFLOAD_STOP);
    } else {
        return;
    }

    msg = lea_server_msg_new_ext(event, &leas_sm->addr, hci_event, sizeof(bt_hci_event_t) + hci_event->length);
    if (!msg) {
        BT_LOGE("error, hci event lea_server_msg_new_ext");
        return;
    }

    lea_server_send_message(msg);
}

static void lea_offload_config_timeout_callback(service_timer_t* timer, void* data)
{
    lea_server_state_machine_t* leas_sm = (lea_server_state_machine_t*)data;
    lea_server_msg_t* msg;

    msg = lea_server_msg_new(OFFLOAD_TIMEOUT, &leas_sm->addr);
    if (!msg) {
        BT_LOGE("error, offload config lea_server_msg_new");
        return;
    }

    lea_server_state_machine_dispatch(leas_sm, msg);
    lea_server_msg_destory(msg);
}

static void closed_enter(state_machine_t* sm)
{
    lea_server_state_machine_t* leas_sm = (lea_server_state_machine_t*)sm;

    LEAS_DBG_ENTER(sm, &leas_sm->addr);
    if (hsm_get_previous_state(sm)) {
        lea_server_notify_connection_state_changed(&leas_sm->addr,
            PROFILE_STATE_DISCONNECTED);
    }
}

static void closed_exit(state_machine_t* sm)
{
    lea_server_state_machine_t* leas_sm = (lea_server_state_machine_t*)sm;

    LEAS_DBG_EXIT(sm, &leas_sm->addr);
}

static bool closed_process_event(state_machine_t* sm, uint32_t event,
    void* p_data)
{
    lea_server_state_machine_t* leas_sm = (lea_server_state_machine_t*)sm;
    lea_server_data_t* data = (lea_server_data_t*)p_data;

    LEAS_DBG_EVENT(sm, &leas_sm->addr, event);

    switch (event) {
    case STACK_EVENT_CONNECTION_STATE: {
        profile_connection_state_t state = (profile_connection_state_t)data->valueint1;
        switch (state) {
        case PROFILE_STATE_CONNECTED: {
            lea_server_notify_connection_state_changed(&leas_sm->addr, state);
            hsm_transition_to(sm, &opening_state);
            break;
        }
        case PROFILE_STATE_DISCONNECTED:
        case PROFILE_STATE_CONNECTING:
        case PROFILE_STATE_DISCONNECTING:
            BT_LOGW("Ignored connection state:%d", state);
            break;
        }
        break;
    }
    default:
        break;
    }

    return true;
}

static void opening_enter(state_machine_t* sm)
{
    lea_server_state_machine_t* leas_sm = (lea_server_state_machine_t*)sm;

    LEAS_DBG_ENTER(sm, &leas_sm->addr);
}

static void opening_exit(state_machine_t* sm)
{
    lea_server_state_machine_t* leas_sm = (lea_server_state_machine_t*)sm;

    LEAS_DBG_EXIT(sm, &leas_sm->addr);
}

static bool opening_process_event(state_machine_t* sm, uint32_t event, void* p_data)
{
    lea_server_state_machine_t* leas_sm = (lea_server_state_machine_t*)sm;
    lea_server_data_t* data = (lea_server_data_t*)p_data;

    LEAS_DBG_EVENT(sm, &leas_sm->addr, event);

    switch (event) {
    case STACK_EVENT_CONNECTION_STATE: {
        profile_connection_state_t state = data->valueint1;
        switch (state) {
        case PROFILE_STATE_DISCONNECTED:
            hsm_transition_to(sm, &closed_state);
            break;
        case PROFILE_STATE_CONNECTED:
        case PROFILE_STATE_CONNECTING:
        case PROFILE_STATE_DISCONNECTING:
            BT_LOGW("Ignored connection state:%d", state);
            break;
        }
        break;
    }
    case STACK_EVENT_STREAM_ADDED: {
        lea_server_add_stream(data->valueint1, &leas_sm->addr);
        break;
    }
    case STACK_EVENT_STREAM_REMOVED: {
        lea_server_remove_stream(data->valueint1);
        break;
    }
    case STACK_EVENT_ASE_CODEC_CONFIG: {
        break;
    }
    case STACK_EVENT_ASE_QOS_CONFIG: {
        hsm_transition_to(sm, &opened_state);
        break;
    }
    case STACK_EVENT_ASE_RELEASING: {
        hsm_transition_to(sm, &closing_state);
        break;
    }
    default:
        break;
    }

    return true;
}

static void opened_enter(state_machine_t* sm)
{
    lea_server_state_machine_t* leas_sm = (lea_server_state_machine_t*)sm;

    LEAS_DBG_ENTER(sm, &leas_sm->addr);
}

static void opened_exit(state_machine_t* sm)
{
    lea_server_state_machine_t* leas_sm = (lea_server_state_machine_t*)sm;

    LEAS_DBG_EXIT(sm, &leas_sm->addr);
}

static void lea_server_stop_audio(uint32_t stream_id)
{
    lea_audio_stream_t* stream;

    stream = lea_server_find_stream(stream_id);
    if (!stream) {
        BT_LOGE("failed, stream_id:0x%08x not found", stream_id);
        return;
    }

    stream->started = false;
    if (stream->is_source) {
        lea_audio_source_stop(true);
    } else {
        lea_audio_sink_stop(true);
    }
}

static void lea_server_stop_offload_req(lea_server_state_machine_t* leas_sm, lea_server_data_t* data)
{
    uint8_t ogf;
    uint16_t ocf;
    uint8_t len;
    uint8_t* payload;

    BT_DUMPBUFFER("stop req vsc", (uint8_t*)data->data, data->size);
    payload = data->data;
    len = data->size - sizeof(ogf) - sizeof(ocf);
    STREAM_TO_UINT8(ogf, payload)
    STREAM_TO_UINT16(ocf, payload);
    flag_set(leas_sm, PENDING_OFFLOAD_STOP);

    bt_sal_send_hci_command(ogf, ocf, len, payload, bt_hci_event_callback,
        leas_sm);
}

static bool opened_process_event(state_machine_t* sm, uint32_t event, void* p_data)
{
    lea_server_state_machine_t* leas_sm = (lea_server_state_machine_t*)sm;
    lea_server_data_t* data = (lea_server_data_t*)p_data;

    LEAS_DBG_EVENT(sm, &leas_sm->addr, event);

    switch (event) {
    case STACK_EVENT_CONNECTION_STATE: {
        profile_connection_state_t state = data->valueint1;
        switch (state) {
        case PROFILE_STATE_DISCONNECTED:
            hsm_transition_to(sm, &closed_state);
            break;
        case PROFILE_STATE_CONNECTED:
        case PROFILE_STATE_CONNECTING:
        case PROFILE_STATE_DISCONNECTING:
            BT_LOGW("Ignored connection state:%d", state);
            break;
        }
        break;
    }
    case STACK_EVENT_ASE_QOS_CONFIG: {
        break;
    }
    case STACK_EVENT_ASE_ENABLING: {
        if (leas_sm->offloading) {
            break;
        }
        hsm_transition_to(sm, &started_state);
        break;
    }
    case OFFLOAD_START_REQ: {
        uint8_t ogf;
        uint16_t ocf;
        uint8_t len;
        uint8_t* payload;

        BT_DUMPBUFFER("start req vsc", (uint8_t*)data->data, data->size);
        payload = data->data;
        len = data->size - sizeof(ogf) - sizeof(ocf);
        STREAM_TO_UINT8(ogf, payload)
        STREAM_TO_UINT16(ocf, payload);
        flag_set(leas_sm, PENDING_OFFLOAD_START);
        leas_sm->offload_timer = service_loop_timer(LEA_SERVER_OFFLOAD_TIMEOUT, 0, lea_offload_config_timeout_callback, leas_sm);

        bt_sal_send_hci_command(ogf, ocf, len, payload, bt_hci_event_callback,
            leas_sm);
        break;
    }
    case OFFLOAD_START_EVT: {
        bt_hci_event_t* hci_event;
        hci_error_t status;

        hci_event = data->data;
        if (leas_sm->offload_timer) {
            service_loop_cancel_timer(leas_sm->offload_timer);
            leas_sm->offload_timer = NULL;
        }

        status = hci_get_result(hci_event);
        if (status != HCI_SUCCESS) {
            BT_LOGE("LEA_SERVER_OFFLOAD_START fail, status:0x%0x", status);
            break;
        }

        hsm_transition_to(sm, &started_state);
        break;
    }
    case OFFLOAD_TIMEOUT: {
        flag_clear(leas_sm, PENDING_OFFLOAD_START);
        leas_sm->offload_timer = NULL;
        break;
    }
    case OFFLOAD_STOP_REQ: {
        lea_server_stop_offload_req(leas_sm, data);
        break;
    }
    case OFFLOAD_STOP_EVT: {
        break;
    }
    case STACK_EVENT_ASE_CODEC_CONFIG: {
        hsm_transition_to(sm, &opening_state);
        break;
    }
    case STACK_EVENT_ASE_RELEASING: {
        hsm_transition_to(sm, &closing_state);
        break;
    }
    case STACK_EVENT_STREAM_ADDED: {
        lea_server_add_stream(data->valueint1, &leas_sm->addr);
        break;
    }
    case STACK_EVENT_STREAM_REMOVED: {
        lea_server_remove_stream(data->valueint1);
        break;
    }
    case STACK_EVENT_STREAM_STOPPED: {
        lea_server_stop_audio(data->valueint1);
        break;
    }
    default:
        break;
    }

    return true;
}

static void started_enter(state_machine_t* sm)
{
    lea_server_state_machine_t* leas_sm = (lea_server_state_machine_t*)sm;

    LEAS_DBG_ENTER(sm, &leas_sm->addr);
    if (leas_sm->offloading) {
        lea_server_streams_started(&leas_sm->addr);
    }
}

static void started_exit(state_machine_t* sm)
{
    lea_server_state_machine_t* leas_sm = (lea_server_state_machine_t*)sm;

    LEAS_DBG_EXIT(sm, &leas_sm->addr);
}

static bool started_process_event(state_machine_t* sm, uint32_t event, void* p_data)
{
    lea_server_state_machine_t* leas_sm = (lea_server_state_machine_t*)sm;
    lea_server_data_t* data = (lea_server_data_t*)p_data;

    LEAS_DBG_EVENT(sm, &leas_sm->addr, event);

    switch (event) {
    case STACK_EVENT_CONNECTION_STATE: {
        profile_connection_state_t state = data->valueint1;
        switch (state) {
        case PROFILE_STATE_DISCONNECTED:
            hsm_transition_to(sm, &closed_state);
            break;
        case PROFILE_STATE_CONNECTED:
        case PROFILE_STATE_CONNECTING:
        case PROFILE_STATE_DISCONNECTING:
            BT_LOGW("Ignored connection state:%d", state);
            break;
        }
        break;
    }
    case STACK_EVENT_ASE_STREAMING: {
        break;
    }
    case STACK_EVENT_STREAM_RESUME: {
        break; // todo resume stream
    }
    case STACK_EVENT_STREAM_SUSPEND: {
        break; // todo suspend stream
    }
    case STACK_EVENT_ASE_QOS_CONFIG: {
        hsm_transition_to(sm, &opened_state);
        break;
    }
    case STACK_EVENT_STREAM_STARTED: {
        lea_audio_stream_t* audio_stream = (lea_audio_stream_t*)data->data;
        lea_audio_config_t* audio_config;

        audio_config = lea_codec_get_config(audio_stream->is_source);
        if (!audio_config) {
            break;
        }

        if (audio_stream->is_source) {
            lea_audio_source_update_codec(audio_config, audio_stream->sdu_size);
        } else {
            lea_audio_sink_update_codec(audio_config, audio_stream->sdu_size);
            lea_audio_sink_start();
        }
        break;
    }
    case STACK_EVENT_STREAM_STOPPED: {
        lea_server_stop_audio(data->valueint1);
        break;
    }
    case STACK_EVENT_ASE_DISABLING: {
        hsm_transition_to(sm, &closing_state);
        break;
    }
    case STACK_EVENT_ASE_RELEASING: {
        hsm_transition_to(sm, &closing_state);
        break;
    }
    case OFFLOAD_STOP_REQ: {
        lea_server_stop_offload_req(leas_sm, data);
        break;
    }
    case OFFLOAD_TIMEOUT: {
        flag_clear(leas_sm, PENDING_OFFLOAD_START);
        leas_sm->offload_timer = NULL;
        break;
    }
    case OFFLOAD_STOP_EVT: {
        break;
    }
    case STACK_EVENT_STREAM_REMOVED: {
        lea_server_remove_stream(data->valueint1);
        break;
    }
    default:
        break;
    }

    return true;
}

static void closing_enter(state_machine_t* sm)
{
    lea_server_state_machine_t* leas_sm = (lea_server_state_machine_t*)sm;

    LEAS_DBG_ENTER(sm, &leas_sm->addr);
}

static void closing_exit(state_machine_t* sm)
{
    lea_server_state_machine_t* leas_sm = (lea_server_state_machine_t*)sm;

    LEAS_DBG_EXIT(sm, &leas_sm->addr);
}

static bool closing_process_event(state_machine_t* sm, uint32_t event, void* p_data)
{
    lea_server_state_machine_t* leas_sm = (lea_server_state_machine_t*)sm;
    lea_server_data_t* data = (lea_server_data_t*)p_data;

    LEAS_DBG_EVENT(sm, &leas_sm->addr, event);

    switch (event) {
    case STACK_EVENT_CONNECTION_STATE: {
        profile_connection_state_t state = data->valueint1;
        switch (state) {
        case PROFILE_STATE_DISCONNECTED:
            hsm_transition_to(sm, &closed_state);
            break;
        case PROFILE_STATE_CONNECTED:
        case PROFILE_STATE_CONNECTING:
        case PROFILE_STATE_DISCONNECTING:
            BT_LOGW("Ignored connection state:%d", state);
            break;
        }
        break;
    }
    case STACK_EVENT_ASE_CODEC_CONFIG: {
        hsm_transition_to(sm, &opening_state);
        break;
    }
    case STACK_EVENT_ASE_RELEASING: {
        hsm_transition_to(sm, &closing_state);
        break;
    }
    case OFFLOAD_STOP_REQ: {
        lea_server_stop_offload_req(leas_sm, data);
        break;
    }
    case OFFLOAD_TIMEOUT: {
        flag_clear(leas_sm, PENDING_OFFLOAD_START);
        leas_sm->offload_timer = NULL;
        break;
    }
    case STACK_EVENT_STREAM_REMOVED: {
        lea_server_remove_stream(data->valueint1);
        break;
    }
    case STACK_EVENT_STREAM_STOPPED: {
        lea_server_stop_audio(data->valueint1);
        break;
    }
    default:
        break;
    }

    return true;
}

lea_server_state_machine_t* lea_server_state_machine_new(bt_address_t* addr,
    void* context)
{
    lea_server_state_machine_t* leasm;

    leasm = (lea_server_state_machine_t*)malloc(
        sizeof(lea_server_state_machine_t));
    if (!leasm)
        return NULL;

    memset(leasm, 0, sizeof(lea_server_state_machine_t));
    leasm->service = context;
    memcpy(&leasm->addr, addr, sizeof(bt_address_t));

    hsm_ctor(&leasm->sm, (state_t*)&closed_state);

    return leasm;
}

void lea_server_state_machine_destory(lea_server_state_machine_t* leasm)
{
    if (!leasm)
        return;

    hsm_dtor(&leasm->sm);
    free((void*)leasm);
}

void lea_server_state_machine_dispatch(lea_server_state_machine_t* leasm,
    lea_server_msg_t* msg)
{
    if (!leasm || !msg)
        return;

    hsm_dispatch_event(&leasm->sm, msg->event, &msg->data);
}

uint32_t lea_server_state_machine_get_state(lea_server_state_machine_t* leasm)
{
    return hsm_get_current_state_value(&leasm->sm);
}

void lea_server_state_machine_set_offloading(lea_server_state_machine_t* leasm, bool offloading)
{
    leasm->offloading = offloading;
}