/*
 * Copyright (C) 2022 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL BLUEKITCHEN
 * GMBH OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at 
 * contact@bluekitchen-gmbh.com
 *
 */

#define BTSTACK_FILE__ "published_audio_capabilities_service_server.c"

#include "ble/att_db.h"
#include "ble/att_server.h"
#include "bluetooth_gatt.h"
#include "btstack_debug.h"
#include "btstack_defines.h"
#include "btstack_event.h"
#include "btstack_util.h"

#include "ble/gatt-service/published_audio_capabilities_service_server.h"

typedef enum {
    PAC_RECORD_FIELD_RECORDS_NUM = 0,
    PAC_RECORD_FIELD_CODEC_ID,
    PAC_RECORD_FIELD_CODEC_SPECIFIC_CAPABILITIES_LENGTH,
    PAC_RECORD_FIELD_CODEC_SPECIFIC_CAPABILITIES,
    PAC_RECORD_FIELD_METADATA_LENGTH,
    PAC_RECORD_FIELD_METADATA,
    PAC_RECORD_FIELD_NUM_FIELDS
} pac_record_field_t;

typedef enum {
    PAC_RECORD_CAPABILITY_FIELD_LENGTH = 0,
    PAC_RECORD_CAPABILITY_FIELD_TYPE,
    PAC_RECORD_CAPABILITY_FIELD_VALUE,
    PAC_RECORD_CAPABILITY_FIELD_NUM_FIELDS
} pac_record_capability_field_t;

static const uint8_t pacs_codec_specific_capability_lengths[] = {0, 2, 2, 5, 3, 2};

static att_service_handler_t    published_audio_capabilities_service;
static hci_con_handle_t         pacs_con_handle;
static btstack_packet_handler_t pacs_event_callback;

// characteristic: SINK_PAC                      READ  | NOTIFY  |  
static uint16_t  pacs_sinc_pac_handle;
static uint16_t  pacs_sinc_pac_client_configuration_handle;
static uint16_t  pacs_sinc_pac_client_configuration;

// characteristic: SINK_AUDIO_LOCATIONS          READ  | NOTIFY  | WRITE 
static uint16_t  pacs_sink_audio_locations_handle;
static uint16_t  pacs_sink_audio_locations_client_configuration_handle;
static uint16_t  pacs_sink_audio_locations_client_configuration;

// characteristic: SOURCE_PAC                    READ  | NOTIFY  |       
static uint16_t  pacs_source_pac_handle;
static uint16_t  pacs_source_pac_client_configuration_handle;
static uint16_t  pacs_source_pac_client_configuration;

// characteristic: SOURCE_AUDIO_LOCATIONS        READ  | NOTIFY  | WRITE 
static uint16_t  pacs_source_audio_locations_handle;
static uint16_t  pacs_source_audio_locations_client_configuration_handle;
static uint16_t  pacs_source_audio_locations_client_configuration;

// characteristic: AVAILABLE_AUDIO_CONTEXTS      READ  |     
static uint16_t  pacs_available_audio_contexts_handle;

// characteristic: SUPPORTED_AUDIO_CONTEXTS      READ  | NOTIFY  |  
static uint16_t  pacs_supported_audio_contexts_handle;
static uint16_t  pacs_supported_audio_contexts_client_configuration_handle;
static uint16_t  pacs_supported_audio_contexts_client_configuration;

static const pacs_record_t * pacs_sink_pac_records;
static uint8_t pacs_sink_pac_records_num;

static const  pacs_record_t * pacs_source_pac_records;
static uint8_t pacs_source_pac_records_num;

static void published_audio_capabilities_service_server_reset_values(void){
    pacs_con_handle = HCI_CON_HANDLE_INVALID;
    pacs_sinc_pac_client_configuration = 0;
    pacs_sink_audio_locations_client_configuration = 0;
    pacs_source_pac_client_configuration = 0;
    pacs_source_audio_locations_client_configuration = 0;
    pacs_supported_audio_contexts_client_configuration = 0; 
}

static void pacs_set_con_handle(hci_con_handle_t con_handle, uint16_t configuration){
    pacs_con_handle = (configuration == 0) ? HCI_CON_HANDLE_INVALID : con_handle;
}

static uint8_t pacs_codec_specific_capability_value_length(pacs_codec_specific_capability_t capability){
    return pacs_codec_specific_capability_lengths[(uint8_t)capability.type];
}

static uint8_t pacs_total_codec_specific_capabilities_length(pacs_record_t record){
    uint8_t i;
    uint8_t length = 0;
    for (i = 0; i < record.codec_specific_capabilities_num; i++){
        length += pacs_codec_specific_capability_value_length(record.capabilities[i]) + 2; // type(1) + length(1)
    }
    return length;
}
static uint8_t pack_codec_capability(pacs_codec_specific_capability_t capability, uint8_t * value){
    uint8_t pos = 0;
    uint8_t value_length = pacs_codec_specific_capability_value_length(capability);
    value[pos++] = value_length + 1; // 1 == sizeof(type)
    value[pos++] = (uint8_t)capability.type;
    memcpy(&value[pos], capability.value, value_length);
    pos += value_length;
    return pos;
}

static uint16_t pacs_store_field(
    const uint8_t * field_data, uint16_t field_len, 
    // position of field in complete data block
    uint16_t pac_records_offset, 
    uint16_t read_offset,
    uint8_t * buffer, uint16_t buffer_size){

    // only calc total size
    if (buffer == NULL) {
        return field_len;
    }

    uint16_t after_buffer = read_offset + buffer_size ;
    // bail before buffer
    if ((pac_records_offset + field_len) < read_offset){
        return 0;
    }
    // bail after buffer
    if (pac_records_offset >= after_buffer){
        return 0;
    }
    // calc overlap
    uint16_t bytes_to_copy = field_len;
    
    uint16_t skip_at_start = 0;
    if (pac_records_offset < read_offset){
        skip_at_start = read_offset - pac_records_offset;
        bytes_to_copy -= skip_at_start;
    }

    uint16_t skip_at_end = 0;
    if ((pac_records_offset + field_len) > after_buffer){
        skip_at_end = (pac_records_offset + field_len) - after_buffer;
        bytes_to_copy -= skip_at_end;
    }
    
    btstack_assert((skip_at_end + skip_at_start) <= field_len);
    btstack_assert(bytes_to_copy <= field_len);

    memcpy(&buffer[(pac_records_offset + skip_at_start) - read_offset], &field_data[skip_at_start], bytes_to_copy);
    return bytes_to_copy;
}

// offset gives position into fully serialized pacs record
static uint16_t pacs_store_records(const pacs_record_t * pacs, uint8_t pac_records_num, uint16_t read_offset, uint8_t * buffer, uint16_t buffer_size){
    uint8_t  field_data[7];
    uint16_t pac_records_offset = 0;
    uint8_t  i;
    uint16_t stored_bytes = 0;
    memset(buffer, 0, buffer_size);

    field_data[0] = pac_records_num;
    stored_bytes += pacs_store_field(field_data, 1, pac_records_offset, read_offset, buffer, buffer_size);
    pac_records_offset++;
    
    for (i = 0; i < pac_records_num; i++){
        stored_bytes += pacs_store_field(pacs[i].codec_id, 5, pac_records_offset, read_offset, buffer, buffer_size);
        pac_records_offset += 5;
    }

    for (i = 0; i < pac_records_num; i++){
        field_data[0] = pacs_total_codec_specific_capabilities_length(pacs[i]);
        stored_bytes += pacs_store_field(field_data, 1, pac_records_offset, read_offset, buffer, buffer_size);
        pac_records_offset++;
    }

    for (i = 0; i < pac_records_num; i++){
        if (pacs_total_codec_specific_capabilities_length(pacs[i]) == 0){
            continue;
        }
        uint8_t j;
        for (j = 0; j < pacs[i].codec_specific_capabilities_num; j++){
            uint16_t field_len = pack_codec_capability(pacs[i].capabilities[j], field_data);
            stored_bytes += pacs_store_field(field_data, field_len, pac_records_offset, read_offset, buffer, buffer_size);
            pac_records_offset += field_len;
        }
    }
 
    for (i = 0; i < pac_records_num; i++){
        field_data[0] = pacs[i].metadata_length;
        stored_bytes += pacs_store_field(field_data, 1, pac_records_offset, read_offset, buffer, buffer_size);
        pac_records_offset++;
    }

    for (i = 0; i < pac_records_num; i++){
        stored_bytes += pacs_store_field(pacs[i].metadata, pacs[i].metadata_length, pac_records_offset, read_offset, buffer, buffer_size);
        pac_records_offset += pacs[i].metadata_length;
    }
    return stored_bytes;
}


static uint16_t published_audio_capabilities_service_read_callback(hci_con_handle_t con_handle, uint16_t attribute_handle, uint16_t offset, uint8_t * buffer, uint16_t buffer_size){
    UNUSED(con_handle);
    if (attribute_handle == pacs_sinc_pac_handle){
        return pacs_store_records(pacs_sink_pac_records, pacs_sink_pac_records_num, offset, buffer, buffer_size);
    }

    if (attribute_handle == pacs_source_pac_handle){
        return pacs_store_records(pacs_source_pac_records, pacs_source_pac_records_num, offset, buffer, buffer_size);
    }

    if (attribute_handle == pacs_sink_audio_locations_handle){
        // TODO
    }
    
    if (attribute_handle == pacs_source_audio_locations_handle){
        // TODO
    }
    
    if (attribute_handle == pacs_supported_audio_contexts_handle){
        // TODO
    }

    if (attribute_handle == pacs_available_audio_contexts_handle){
        // TODO
    }
    
    if (attribute_handle == pacs_sinc_pac_client_configuration_handle){
        return att_read_callback_handle_little_endian_16(pacs_sinc_pac_client_configuration, offset, buffer, buffer_size);
    }

    if (attribute_handle == pacs_sink_audio_locations_client_configuration_handle){
        return att_read_callback_handle_little_endian_16(pacs_sink_audio_locations_client_configuration, offset, buffer, buffer_size);
    }
    
    if (attribute_handle == pacs_source_pac_client_configuration_handle){
        return att_read_callback_handle_little_endian_16(pacs_source_pac_client_configuration, offset, buffer, buffer_size);
    }

    if (attribute_handle == pacs_source_audio_locations_client_configuration_handle){
        return att_read_callback_handle_little_endian_16(pacs_source_audio_locations_client_configuration, offset, buffer, buffer_size);
    }
    
    if (attribute_handle == pacs_supported_audio_contexts_client_configuration_handle){
        return att_read_callback_handle_little_endian_16(pacs_supported_audio_contexts_client_configuration, offset, buffer, buffer_size);
    }
    return 0;
}

static int published_audio_capabilities_service_write_callback(hci_con_handle_t con_handle, uint16_t attribute_handle, uint16_t transaction_mode, uint16_t offset, uint8_t *buffer, uint16_t buffer_size){
    UNUSED(transaction_mode);
    UNUSED(offset);
    UNUSED(buffer_size);

    if (attribute_handle == pacs_sink_audio_locations_handle){
        // TODO
    }

    else if (attribute_handle == pacs_source_audio_locations_handle){
        // TODO
    }

    else if (attribute_handle == pacs_sinc_pac_client_configuration_handle){
        pacs_sinc_pac_client_configuration = little_endian_read_16(buffer, 0);
        pacs_set_con_handle(con_handle, pacs_sinc_pac_client_configuration);
    }

    else if (attribute_handle == pacs_sink_audio_locations_client_configuration_handle){
        pacs_sink_audio_locations_client_configuration = little_endian_read_16(buffer, 0);
        pacs_set_con_handle(con_handle, pacs_sink_audio_locations_client_configuration);
    }
    
    else if (attribute_handle == pacs_source_pac_client_configuration_handle){
        pacs_source_pac_client_configuration = little_endian_read_16(buffer, 0);
        pacs_set_con_handle(con_handle, pacs_source_pac_client_configuration);
    }

    else if (attribute_handle == pacs_source_audio_locations_client_configuration_handle){
        pacs_source_audio_locations_client_configuration = little_endian_read_16(buffer, 0);
        pacs_set_con_handle(con_handle, pacs_source_audio_locations_client_configuration);
    }
    
    else if (attribute_handle == pacs_supported_audio_contexts_client_configuration_handle){
        pacs_supported_audio_contexts_client_configuration = little_endian_read_16(buffer, 0);
        pacs_set_con_handle(con_handle, pacs_supported_audio_contexts_client_configuration);
    }

    return 0;
}

static void published_audio_capabilities_service_packet_handler(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    UNUSED(channel);
    UNUSED(packet);
    UNUSED(size);

    if (packet_type != HCI_EVENT_PACKET){
        return;
    }

    hci_con_handle_t con_handle;
    switch (hci_event_packet_get_type(packet)) {
        case HCI_EVENT_DISCONNECTION_COMPLETE:
            con_handle = hci_event_disconnection_complete_get_connection_handle(packet);
            if (pacs_con_handle == con_handle){
                published_audio_capabilities_service_server_reset_values();
            }
            break;
        default:
            break;
    }
}

void published_audio_capabilities_service_server_init(const pacs_record_t * sink_pac_records, uint8_t sink_pac_records_num, const pacs_record_t * source_pac_records, uint8_t source_pac_records_num){
    // get service handle range
    uint16_t start_handle = 0;
    uint16_t end_handle   = 0xfff;
    int service_found = gatt_server_get_handle_range_for_service_with_uuid16(ORG_BLUETOOTH_SERVICE_PUBLISHED_AUDIO_CAPABILITIES_SERVICE, &start_handle, &end_handle);
    btstack_assert(service_found != 0);
    UNUSED(service_found);

    published_audio_capabilities_service_server_reset_values();

    pacs_sink_pac_records = sink_pac_records;
    pacs_sink_pac_records_num = sink_pac_records_num;

    pacs_source_pac_records = source_pac_records;
    pacs_source_pac_records_num = source_pac_records_num;

    pacs_sinc_pac_handle = gatt_server_get_value_handle_for_characteristic_with_uuid16(start_handle, end_handle, ORG_BLUETOOTH_CHARACTERISTIC_SINK_PAC);
    pacs_sinc_pac_client_configuration_handle = gatt_server_get_client_configuration_handle_for_characteristic_with_uuid16(start_handle, end_handle, ORG_BLUETOOTH_CHARACTERISTIC_SINK_PAC);

    pacs_sink_audio_locations_handle = gatt_server_get_value_handle_for_characteristic_with_uuid16(start_handle, end_handle, ORG_BLUETOOTH_CHARACTERISTIC_SINK_AUDIO_LOCATIONS);
    pacs_sink_audio_locations_client_configuration_handle = gatt_server_get_client_configuration_handle_for_characteristic_with_uuid16(start_handle, end_handle, ORG_BLUETOOTH_CHARACTERISTIC_SINK_AUDIO_LOCATIONS);

    pacs_source_pac_handle = gatt_server_get_value_handle_for_characteristic_with_uuid16(start_handle, end_handle, ORG_BLUETOOTH_CHARACTERISTIC_SOURCE_PAC);
    pacs_source_pac_client_configuration_handle = gatt_server_get_client_configuration_handle_for_characteristic_with_uuid16(start_handle, end_handle, ORG_BLUETOOTH_CHARACTERISTIC_SOURCE_PAC);

    pacs_source_audio_locations_handle = gatt_server_get_value_handle_for_characteristic_with_uuid16(start_handle, end_handle, ORG_BLUETOOTH_CHARACTERISTIC_SOURCE_AUDIO_LOCATIONS);
    pacs_source_audio_locations_client_configuration_handle = gatt_server_get_client_configuration_handle_for_characteristic_with_uuid16(start_handle, end_handle, ORG_BLUETOOTH_CHARACTERISTIC_SOURCE_AUDIO_LOCATIONS);

    pacs_available_audio_contexts_handle = gatt_server_get_value_handle_for_characteristic_with_uuid16(start_handle, end_handle, ORG_BLUETOOTH_CHARACTERISTIC_AVAILABLE_AUDIO_CONTEXTS);

    pacs_supported_audio_contexts_handle = gatt_server_get_value_handle_for_characteristic_with_uuid16(start_handle, end_handle, ORG_BLUETOOTH_CHARACTERISTIC_SUPPORTED_AUDIO_CONTEXTS);
    pacs_supported_audio_contexts_client_configuration_handle = gatt_server_get_client_configuration_handle_for_characteristic_with_uuid16(start_handle, end_handle, ORG_BLUETOOTH_CHARACTERISTIC_SUPPORTED_AUDIO_CONTEXTS);

    log_info("Found PACS service 0x%02x-0x%02x", start_handle, end_handle);

    // register service with ATT Server
    published_audio_capabilities_service.start_handle   = start_handle;
    published_audio_capabilities_service.end_handle     = end_handle;
    published_audio_capabilities_service.read_callback  = &published_audio_capabilities_service_read_callback;
    published_audio_capabilities_service.write_callback = &published_audio_capabilities_service_write_callback;
    published_audio_capabilities_service.packet_handler = published_audio_capabilities_service_packet_handler;
    att_server_register_service_handler(&published_audio_capabilities_service);
}

void published_audio_capabilities_service_server_register_packet_handler(btstack_packet_handler_t callback){
    btstack_assert(callback != NULL);
    pacs_event_callback = callback;
}

