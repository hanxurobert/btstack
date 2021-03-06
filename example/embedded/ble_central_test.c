/*
 * Copyright (C) 2014 BlueKitchen GmbH
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
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MATTHIAS
 * RINGWALD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
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

// *****************************************************************************
//
// BLE Central PTS Test
//
// *****************************************************************************

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/uio.h>
#include <unistd.h>
#include <termios.h>

#include "btstack-config.h"

#include <btstack/run_loop.h>
#include "debug.h"
#include "btstack_memory.h"
#include "hci.h"
#include "hci_dump.h"

#include "l2cap.h"

#include "sm.h"
#include "att.h"
#include "gap_le.h"
#include "le_device_db.h"
#include "stdin_support.h"

typedef struct advertising_report {
    uint8_t   type;
    uint8_t   event_type;
    uint8_t   address_type;
    bd_addr_t address;
    uint8_t   rssi;
    uint8_t   length;
    uint8_t * data;
} advertising_report_t;


static uint8_t test_irk[] =  { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

static int gap_privacy = 0;
static int gap_bondable = 0;
static char gap_device_name[20];

static char * sm_io_capabilities = NULL;
static int sm_mitm_protection = 0;
static int sm_have_oob_data = 0;
static uint8_t * sm_oob_data = (uint8_t *) "0123456789012345"; // = { 0x30...0x39, 0x30..0x35}
static int sm_min_key_size = 7;

static int peer_addr_type;
static bd_addr_t peer_address;
static int ui_passkey = 0;
static int ui_digits_for_passkey = 0;

static uint16_t handle = 0;
static bd_addr_t tester_address = {0x00, 0x1B, 0xDC, 0x07, 0x32, 0xef};
static int tester_address_type = 0;
uint16_t gc_id;


static void show_usage();
static void fill_advertising_report_from_packet(advertising_report_t * report, uint8_t *packet);
static void dump_advertising_report(advertising_report_t * e);

///

static void printUUID(uint8_t * uuid128, uint16_t uuid16){
    if (uuid16){
        printf("%04x",uuid16);
    } else {
        printUUID128(uuid128);
    }
}

static void dump_advertising_report(advertising_report_t * e){
    printf("    * adv. event: evt-type %u, addr-type %u, addr %s, rssi %u, length adv %u, data: ", e->event_type,
           e->address_type, bd_addr_to_str(e->address), e->rssi, e->length);
    printf_hexdump(e->data, e->length);
}

static void dump_characteristic(le_characteristic_t * characteristic){
    printf("    * characteristic: [0x%04x-0x%04x-0x%04x], properties 0x%02x, uuid ",
                            characteristic->start_handle, characteristic->value_handle, characteristic->end_handle, characteristic->properties);
    printUUID(characteristic->uuid128, characteristic->uuid16);
    printf("\n");
}

static void dump_service(le_service_t * service){
    printf("    * service: [0x%04x-0x%04x], uuid ", service->start_group_handle, service->end_group_handle);
    printUUID(service->uuid128, service->uuid16);
    printf("\n");
}

static void fill_advertising_report_from_packet(advertising_report_t * report, uint8_t *packet){
    int pos = 2;
    report->event_type = packet[pos++];
    report->address_type = packet[pos++];
    memcpy(report->address, &packet[pos], 6);
    pos += 6;
    report->rssi = packet[pos++];
    report->length = packet[pos++];
    report->data = &packet[pos];
    pos += report->length;
    dump_advertising_report(report);
    
    bd_addr_t found_device_addr;
    memcpy(found_device_addr, report->address, 6);
    swapX(found_device_addr, report->address, 6);
}

static void gap_run(){
    if (!hci_can_send_command_packet_now()) return;
}

void app_packet_handler (uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    advertising_report_t report;
    
    switch (packet_type) {
            
        case HCI_EVENT_PACKET:
            switch (packet[0]) {
                
                case BTSTACK_EVENT_STATE:
                    // bt stack activated, get started
                    if (packet[2] == HCI_STATE_WORKING) {
                        printf("SM Init completed\n");
                        show_usage();
                        gap_run();
                    }
                    break;
                
                case HCI_EVENT_LE_META:
                    switch (packet[2]) {
                        case HCI_SUBEVENT_LE_CONNECTION_COMPLETE:
                            handle = READ_BT_16(packet, 4);
                            printf("Connection complete, handle 0x%04x\n", handle);
                            break;

                        default:
                            break;
                    }
                    break;

                case HCI_EVENT_DISCONNECTION_COMPLETE:
                    break;
                    
                case SM_PASSKEY_INPUT_NUMBER: {
                    // display number
                    sm_event_t * event = (sm_event_t *) packet;
                    memcpy(peer_address, event->address, 6);
                    peer_addr_type = event->addr_type;
                    printf("\nGAP Bonding %s (%u): Enter 6 digit passkey: '", bd_addr_to_str(peer_address), peer_addr_type);
                    fflush(stdout);
                    ui_passkey = 0;
                    ui_digits_for_passkey = 6;
                    break;
                }

                case SM_PASSKEY_DISPLAY_NUMBER: {
                    // display number
                    sm_event_t * event = (sm_event_t *) packet;
                    printf("\nGAP Bonding %s (%u): Display Passkey '%06u\n", bd_addr_to_str(peer_address), peer_addr_type, event->passkey);
                    break;
                }

                case SM_PASSKEY_DISPLAY_CANCEL: 
                    printf("\nGAP Bonding %s (%u): Display cancel\n", bd_addr_to_str(peer_address), peer_addr_type);
                    break;

                case SM_AUTHORIZATION_REQUEST: {
                    // auto-authorize connection if requested
                    sm_event_t * event = (sm_event_t *) packet;
                    sm_authorization_grant(event->addr_type, event->address);
                    break;
                }

                case GAP_LE_ADVERTISING_REPORT:
                    fill_advertising_report_from_packet(&report, packet);
                    dump_advertising_report(&report);
                    break;
                default:
                    break;
            }
    }
    gap_run();
}



void handle_gatt_client_event(le_event_t * event){
    le_service_t service;
    le_characteristic_t characteristic;
    switch(event->type){
        case GATT_SERVICE_QUERY_RESULT:
            service = ((le_service_event_t *) event)->service;
            dump_service(&service);
            break;
        case GATT_QUERY_COMPLETE:
            printf("\ntest client - CHARACTERISTIC for SERVICE ");
            printUUID128(service.uuid128); printf("\n");
            break;
        case GATT_CHARACTERISTIC_QUERY_RESULT:
            characteristic = ((le_characteristic_event_t *) event)->characteristic;
            dump_characteristic(&characteristic);
            break;
        default:
            break;
    }
}

uint16_t value_handle = 1;
uint16_t attribute_size = 1;

void show_usage(){
    printf("\e[1;1H\e[2J");
    printf("--- CLI for LE Central ---\n");
    printf("SM: %s, MITM protection %u, OOB data %u, key range [%u..16]\n",
        sm_io_capabilities, sm_mitm_protection, sm_have_oob_data, sm_min_key_size);
    printf("Privacy %u\n", gap_privacy);
    printf("Device name %s\n", gap_device_name);
    printf("Value Handle: %x\n", value_handle);
    printf("Attribute Size: %u\n", attribute_size);
    printf("---\n");
    printf("p/P - privacy flag off\n");
    printf("z   - send Connection Parameter Update Request\n");
    printf("t   - terminate connection\n");
    printf("j   - create L2CAP LE connection to %s\n", bd_addr_to_str(tester_address));
    printf("---\n");
    printf("d   - discover all services\n");
    printf("v   - set value handle\n");
    printf("s   - set attribute size\n");
    printf("---\n");
    printf("e   - IO_CAPABILITY_DISPLAY_ONLY\n");
    printf("f   - IO_CAPABILITY_DISPLAY_YES_NO\n");
    printf("g   - IO_CAPABILITY_NO_INPUT_NO_OUTPUT\n");
    printf("h   - IO_CAPABILITY_KEYBOARD_ONLY\n");
    printf("i   - IO_CAPABILITY_KEYBOARD_DISPLAY\n");
    printf("o/O - OOB data off/on ('%s')\n", sm_oob_data);
    printf("m/M - MITM protection off\n");
    printf("k/k - encryption key range [7..16]/[16..16]\n");
    printf("---\n");
    printf("Ctrl-c - exit\n");
    printf("---\n");
}

void update_auth_req(){
    uint8_t auth_req = 0;
    if (sm_mitm_protection){
        auth_req |= SM_AUTHREQ_MITM_PROTECTION;
    }
    if (gap_bondable){
        auth_req |= SM_AUTHREQ_BONDING;
    }
    sm_set_authentication_requirements(auth_req);
}

int  stdin_process(struct data_source *ds){
    char buffer;
    read(ds->fd, &buffer, 1);

    // passkey input
    if (ui_digits_for_passkey){
        if (buffer < '0' || buffer > '9') return 0;
        printf("%c", buffer);
        fflush(stdout);
        ui_passkey = ui_passkey * 10 + buffer - '0';
        ui_digits_for_passkey--;
        if (ui_digits_for_passkey == 0){
            printf("\nSending Passkey '%06x'\n", ui_passkey);
            sm_passkey_input(peer_addr_type, peer_address, ui_passkey);
        }
        return 0;
    }

    switch (buffer){
        case 'e':
            sm_io_capabilities = "IO_CAPABILITY_DISPLAY_ONLY";
            sm_set_io_capabilities(IO_CAPABILITY_DISPLAY_ONLY);
            show_usage();
            break;
        case 'f':
            sm_io_capabilities = "IO_CAPABILITY_DISPLAY_YES_NO";
            sm_set_io_capabilities(IO_CAPABILITY_DISPLAY_YES_NO);
            show_usage();
            break;
        case 'g':
            sm_io_capabilities = "IO_CAPABILITY_NO_INPUT_NO_OUTPUT";
            sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
            show_usage();
            break;
        case 'h':
            sm_io_capabilities = "IO_CAPABILITY_KEYBOARD_ONLY";
            sm_set_io_capabilities(IO_CAPABILITY_KEYBOARD_ONLY);
            show_usage();
            break;
        case 'i':
            sm_io_capabilities = "IO_CAPABILITY_KEYBOARD_DISPLAY";
            sm_set_io_capabilities(IO_CAPABILITY_KEYBOARD_DISPLAY);
            show_usage();
            break;
        case 'o':
            sm_have_oob_data = 0;
            show_usage();
            break;
        case 'O':
            sm_have_oob_data = 1;
            show_usage();
            break;
        case 'k':
            sm_min_key_size = 7;
            sm_set_encryption_key_size_range(7, 16);
            show_usage();
            break;
        case 'K':
            sm_min_key_size = 16;
            sm_set_encryption_key_size_range(16, 16);
            show_usage();
            break;
        case 'm':
            sm_mitm_protection = 0;
            update_auth_req();
            show_usage();
            break;
        case 'M':
            sm_mitm_protection = 1;
            update_auth_req();
            show_usage();
            break;
        case 'z':
            printf("Sending l2cap connection update parameter request\n");
            l2cap_le_request_connection_parameter_update(handle, 50, 120, 0, 550);
            break;
        case 'j':
            printf("Create L2CAP Connection to %s\n", bd_addr_to_str(tester_address));
            hci_send_cmd(&hci_le_create_connection, 
                1000,      // scan interval: 625 ms
                1000,      // scan interval: 625 ms
                0,         // don't use whitelist
                0,         // peer address type: public
                tester_address,      // remote bd addr
                tester_address_type, // random or public
                80,        // conn interval min
                80,        // conn interval max (3200 * 0.625)
                0,         // conn latency
                2000,      // supervision timeout
                0,         // min ce length
                1000       // max ce length
                );
            break;
        case 't':
            printf("Terminating connection\n");
            hci_send_cmd(&hci_disconnect, handle, 0x13);
            break;
        case 'd':
            printf("Discover all primary services\n");
            gatt_client_discover_primary_services(gc_id, handle);
            break;
        case 's':
            attribute_size = btstack_stdin_query_int("Attrbute Size");
            show_usage();
            break;
        case 'v':
            value_handle = btstack_stdin_query_hex("Value Handle");
            show_usage();
            break;
        default:
            show_usage();
            break;

    }
    return 0;
}

static int get_oob_data_callback(uint8_t addres_type, bd_addr_t addr, uint8_t * oob_data){
    if(!sm_have_oob_data) return 0;
    memcpy(oob_data, sm_oob_data, 16);
    return 1;
}

void setup(void){

}

int btstack_main(int argc, const char * argv[]);
int btstack_main(int argc, const char * argv[]){
    
    printf("BTstack LE Peripheral starting up...\n");

    // set up l2cap_le
    l2cap_init();
    
    gatt_client_init();
    gc_id = gatt_client_register_packet_handler(handle_gatt_client_event);;

    // setup le device db
    le_device_db_init();

    // setup SM: Display only
    sm_init();
    sm_register_packet_handler(app_packet_handler);
    sm_register_oob_data_callback(get_oob_data_callback);

    sm_set_io_capabilities(IO_CAPABILITY_DISPLAY_ONLY);
    sm_set_authentication_requirements( SM_AUTHREQ_BONDING | SM_AUTHREQ_MITM_PROTECTION); 

    btstack_stdin_setup(stdin_process);

    gap_random_address_set_update_period(300000);
    gap_random_address_set_mode(GAP_RANDOM_ADDRESS_RESOLVABLE);
    strcpy(gap_device_name, "BTstack");
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_io_capabilities =  "IO_CAPABILITY_NO_INPUT_NO_OUTPUT";
    sm_set_authentication_requirements(0);
    sm_set_encryption_key_size_range(sm_min_key_size, 16);
    sm_test_set_irk(test_irk);

    // turn on!
    hci_power_control(HCI_POWER_ON);
    
    return 0;
}
