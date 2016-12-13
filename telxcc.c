/*!
(c) 2011-2014 Forers, s. r. o.: telxcc

telxcc conforms to ETSI 300 706 Presentation Level 1.5: Presentation Level 1 defines the basic Teletext page,
characterised by the use of spacing attributes only and a limited alphanumeric and mosaics repertoire.
Presentation Level 1.5 decoder responds as Level 1 but the character repertoire is extended via packets X/26.
Selection of national option sub-sets related features from Presentation Level 2.5 feature set have been implemented, too.
(X/28/0 Format 1, X/28/4, M/29/0 and M/29/4 packets)

Further documentation:
ETSI TS 101 154 V1.9.1 (2009-09), Technical Specification
  Digital Video Broadcasting (DVB); Specification for the use of Video and Audio Coding in Broadcasting Applications based on the MPEG-2 Transport Stream
ETSI EN 300 231 V1.3.1 (2003-04), European Standard (Telecommunications series)
  Television systems; Specification of the domestic video Programme Delivery Control system (PDC)
ETSI EN 300 472 V1.3.1 (2003-05), European Standard (Telecommunications series)
  Digital Video Broadcasting (DVB); Specification for conveying ITU-R System B Teletext in DVB bitstreams
ETSI EN 301 775 V1.2.1 (2003-05), European Standard (Telecommunications series)
  Digital Video Broadcasting (DVB); Specification for the carriage of Vertical Blanking Information (VBI) data in DVB bitstreams
ETS 300 706 (May 1997)
  Enhanced Teletext Specification
ETS 300 708 (March 1997)
  Television systems; Data transmission within Teletext
ISO/IEC STANDARD 13818-1 Second edition (2000-12-01)
  Information technology — Generic coding of moving pictures and associated audio information: Systems
ISO/IEC STANDARD 6937 Third edition (2001-12-15)
  Information technology — Coded graphic character set for text communication — Latin alphabet
Werner Brückner -- Teletext in digital television
*/

// Based on telxcc version 2.6.0

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <math.h>
#include <err.h>
#include "rtp.h"
#include "ts.h"
#include "hamming.h"
#include "teletext.h"
#include "telxcc.h"

// size of a TS packet payload in bytes
const uint8_t TS_PACKET_PAYLOAD_SIZE = (TS_SIZE - TS_HEADER_SIZE);

// size of a packet payload buffer
const uint16_t PAYLOAD_BUFFER_SIZE = 4096;

const char* TTXT_COLOURS[8] = {
    //black,     red,       green,     yellow,    blue,      magenta,   cyan,      white
    "#000000", "#ff0000", "#00ff00", "#ffff00", "#0000ff", "#ff00ff", "#00ffff", "#ffffff"
};

// application config global variable
struct {
    uint8_t verbose; // should telxcc be verbose?
    uint16_t page; // teletext page containing cc we want to filter
    uint16_t tid;
    uint64_t utc_refvalue; // UTC referential value
    void (*printer)(frame_t*);
} config = {
    .verbose = NO,
    .page = 0,
    .tid = 0,
    .utc_refvalue = 0,
    .printer = NULL
};

// macro -- output only when increased verbosity was turned on
#define VERBOSE_ONLY if (config.verbose == YES)

// application states -- flags for notices that should be printed only once
struct {
    uint8_t programme_info_processed;
    uint8_t pts_initialized;
} states = {
    .programme_info_processed = NO,
    .pts_initialized = NO
};

// subtitle type pages bitmap, 2048 bits = 2048 possible pages in teletext (excl. subpages)
uint8_t cc_map[256] = { 0 };

// global TS PCR value
uint32_t global_timestamp = 0;

// last timestamp computed
uint64_t last_timestamp = 0;

// working teletext page buffer
teletext_page_t page_buffer = { 0 };

// teletext transmission mode
transmission_mode_t transmission_mode = TRANSMISSION_MODE_SERIAL;

// flag indicating if incoming data should be processed or ignored
uint8_t receiving_data = NO;

// current charset (charset can be -- and always is -- changed during transmission)
struct {
    uint8_t current;
    uint8_t g0_m29;
    uint8_t g0_x28;
} primary_charset = {
    .current = 0x00,
    .g0_m29 = UNDEF,
    .g0_x28 = UNDEF
};

// entities, used in colour mode, to replace unsafe HTML tag chars
struct {
    uint16_t character;
    char *entity;
} const ENTITIES[] = {
    { .character = '<', .entity = "&lt;" },
    { .character = '>', .entity = "&gt;" },
    { .character = '&', .entity = "&amp;" }
};

// 0xff means not set yet
uint8_t continuity_counter = 255;

// PES packet buffer
uint8_t payload_buffer[PAYLOAD_BUFFER_SIZE] = { 0 };
uint16_t payload_counter = 0;

// helper, array length function
#define ARRAY_LENGTH(a) (sizeof(a)/sizeof(a[0]))

// extracts magazine number from teletext page
#define MAGAZINE(p) ((p >> 8) & 0xf)

// extracts page number from teletext page
#define PAGE(p) (p & 0xff)

// ETS 300 706, chapter 8.2
static uint8_t unham_8_4(uint8_t a) {
    uint8_t r = UNHAM_8_4[a];
    if (r == 0xff) {
        r = 0;
        VERBOSE_ONLY fprintf(stderr, "! Unrecoverable data error; UNHAM8/4(%02x)\n", a);
    }
    return (r & 0x0f);
}

// ETS 300 706, chapter 8.3
static uint32_t unham_24_18(uint32_t a) {
    uint8_t test = 0;

    // Tests A-F correspond to bits 0-6 respectively in 'test'.
    for (uint8_t i = 0; i < 23; i++) test ^= ((a >> i) & 0x01) * (i + 33);
    // Only parity bit is tested for bit 24
    test ^= ((a >> 23) & 0x01) * 32;

    if ((test & 0x1f) != 0x1f) {
        // Not all tests A-E correct
        if ((test & 0x20) == 0x20) {
            // F correct: Double error
            return 0xffffffff;
        }
        // Test F incorrect: Single error
        a ^= 1 << (30 - test);
    }

    return (a & 0x000004) >> 2 | (a & 0x000070) >> 3 | (a & 0x007f00) >> 4 | (a & 0x7f0000) >> 5;
}

static void remap_g0_charset(uint8_t c) {
    if (c != primary_charset.current) {
        uint8_t m = G0_LATIN_NATIONAL_SUBSETS_MAP[c];
        if (m == 0xff) {
            fprintf(stderr, "- G0 Latin National Subset ID 0x%1x.%1x is not implemented\n", (c >> 3), (c & 0x7));
        }
        else {
            for (uint8_t j = 0; j < 13; j++) G0[LATIN][G0_LATIN_NATIONAL_SUBSETS_POSITIONS[j]] = G0_LATIN_NATIONAL_SUBSETS[m].characters[j];
            VERBOSE_ONLY fprintf(stderr, "- Using G0 Latin National Subset ID 0x%1x.%1x (%s)\n", (c >> 3), (c & 0x7), G0_LATIN_NATIONAL_SUBSETS[m].language);
            primary_charset.current = c;
        }
    }
}

// UCS-2 (16 bits) to UTF-8 (Unicode Normalization Form C (NFC)) conversion
static void ucs2_to_utf8(char *r, uint16_t ch) {
    if (ch < 0x80) {
        r[0] = ch & 0x7f;
        r[1] = 0;
        r[2] = 0;
        r[3] = 0;
    }
    else if (ch < 0x800) {
        r[0] = (ch >> 6) | 0xc0;
        r[1] = (ch & 0x3f) | 0x80;
        r[2] = 0;
        r[3] = 0;
    }
    else {
        r[0] = (ch >> 12) | 0xe0;
        r[1] = ((ch >> 6) & 0x3f) | 0x80;
        r[2] = (ch & 0x3f) | 0x80;
        r[3] = 0;
    }
}

// check parity and translate any reasonable teletext character into ucs2
static uint16_t telx_to_ucs2(uint8_t c) {
    if (PARITY_8[c] == 0) {
        VERBOSE_ONLY fprintf(stderr, "! Unrecoverable data error; PARITY(%02x)\n", c);
        return 0x20;
    }

    uint16_t r = c & 0x7f;
    if (r >= 0x20) r = G0[LATIN][r - 0x20];
    return r;
}

static void process_page(teletext_page_t *page) {
    // optimization: slicing column by column -- higher probability we could find boxed area start mark sooner
    uint8_t page_is_empty = YES;
    for (uint8_t col = 0; col < 40; col++) {
        for (uint8_t row = 1; row < 25; row++) {
            if (page->text[row][col] == 0x0b) {
                page_is_empty = NO;
                goto page_is_empty;
            }
        }
    }
    page_is_empty:
    if (page_is_empty == YES) return;

    if (page->show_timestamp > page->hide_timestamp) page->hide_timestamp = page->show_timestamp;

    printf("%"PRIu64"\t%"PRIu64"\t", page->show_timestamp, page->hide_timestamp);

    // process data
    for (uint8_t row = 1; row < 25; row++) {
        // anchors for string trimming purpose
        uint8_t col_start = 40;
        uint8_t col_stop = 40;

        for (int8_t col = 39; col >= 0; col--) {
            if (page->text[row][col] == 0xb) {
                col_start = col;
                break;
            }
        }
        // line is empty
        if (col_start > 39) continue;

        for (uint8_t col = col_start + 1; col <= 39; col++) {
            if (page->text[row][col] > 0x20) {
                if (col_stop > 39) col_start = col;
                col_stop = col;
            }
            if (page->text[row][col] == 0xa) break;
        }
        // line is empty
        if (col_stop > 39) continue;

        // ETS 300 706, chapter 12.2: Alpha White ("Set-After") - Start-of-row default condition.
        // used for colour changes _before_ start box mark
        // white is default as stated in ETS 300 706, chapter 12.2
        // black(0), red(1), green(2), yellow(3), blue(4), magenta(5), cyan(6), white(7)
        uint8_t foreground_color = 0x7;
        uint8_t font_tag_opened = NO;

        for (uint8_t col = 0; col <= col_stop; col++) {
            // v is just a shortcut
            uint16_t v = page->text[row][col];

            if (col < col_start) {
                if (v <= 0x7) foreground_color = v;
            }

            if (col == col_start) {
                if (foreground_color != 0x7) {
                    printf("<font color=\"%s\">", TTXT_COLOURS[foreground_color]);

                    font_tag_opened = YES;
                }
            }

            if (col >= col_start) {
                if (v <= 0x7) {
                    // ETS 300 706, chapter 12.2: Unless operating in "Hold Mosaics" mode,
                    // each character space occupied by a spacing attribute is displayed as a SPACE.
                    if (font_tag_opened == YES) {
                        printf("</font> ");
                        font_tag_opened = NO;
                    }

                    // black is considered as white for telxcc purpose
                    // telxcc writes <font/> tags only when needed
                    if ((v > 0x0) && (v < 0x7)) {
                        printf("<font color=\"%s\">", TTXT_COLOURS[v]);
                        font_tag_opened = YES;
                    }
                }

                if (v >= 0x20) {
                    // translate some chars into entities, if in colour mode
                    for (uint8_t i = 0; i < ARRAY_LENGTH(ENTITIES); i++) {
                        if (v == ENTITIES[i].character) {
                            printf("%s", ENTITIES[i].entity);
                            // v < 0x20 won't be printed in next block
                            v = 0;
                            break;
                        }
                    }
                }

                if (v >= 0x20) {
                    char u[4] = { 0, 0, 0, 0 };
                    ucs2_to_utf8(u, v);
                    printf("%s", u);
                }
            }
        }

        // no tag will left opened!
        if (font_tag_opened == YES) {
            printf("</font>");
            font_tag_opened = NO;
        }

        // line delimiter
        printf("\t");
    }

    printf("\n");
    fflush(stdout);
}

static void process_telx_packet(data_unit_t data_unit_id, teletext_packet_payload_t *packet, uint64_t timestamp) {
    // variable names conform to ETS 300 706, chapter 7.1.2
    uint8_t address = (unham_8_4(packet->address[1]) << 4) | unham_8_4(packet->address[0]);
    uint8_t m = address & 0x7;
    if (m == 0) m = 8;
    uint8_t y = (address >> 3) & 0x1f;
    uint8_t designation_code = (y > 25) ? unham_8_4(packet->data[0]) : 0x00;

    if (y == 0) {
        // CC map
        uint8_t i = (unham_8_4(packet->data[1]) << 4) | unham_8_4(packet->data[0]);
        uint8_t flag_subtitle = (unham_8_4(packet->data[5]) & 0x08) >> 3;
        cc_map[i] |= flag_subtitle << (m - 1);

        if ((config.page == 0) && (flag_subtitle == YES) && (i < 0xff)) {
            config.page = (m << 8) | (unham_8_4(packet->data[1]) << 4) | unham_8_4(packet->data[0]);
            fprintf(stderr, "- No teletext page specified, first received suitable page is %03x, not guaranteed\n", config.page);
        }

        // Page number and control bits
        uint16_t page_number = (m << 8) | (unham_8_4(packet->data[1]) << 4) | unham_8_4(packet->data[0]);
        uint8_t charset = ((unham_8_4(packet->data[7]) & 0x08) | (unham_8_4(packet->data[7]) & 0x04) | (unham_8_4(packet->data[7]) & 0x02)) >> 1;
        //uint8_t flag_suppress_header = unham_8_4(packet->data[6]) & 0x01;
        //uint8_t flag_inhibit_display = (unham_8_4(packet->data[6]) & 0x08) >> 3;

        // ETS 300 706, chapter 9.3.1.3:
        // When set to '1' the service is designated to be in Serial mode and the transmission of a page is terminated
        // by the next page header with a different page number.
        // When set to '0' the service is designated to be in Parallel mode and the transmission of a page is terminated
        // by the next page header with a different page number but the same magazine number.
        // The same setting shall be used for all page headers in the service.
        // ETS 300 706, chapter 7.2.1: Page is terminated by and excludes the next page header packet
        // having the same magazine address in parallel transmission mode, or any magazine address in serial transmission mode.
        transmission_mode = unham_8_4(packet->data[7]) & 0x01;

        // FIXME: Well, this is not ETS 300 706 kosher, however we are interested in DATA_UNIT_EBU_TELETEXT_SUBTITLE only
        if ((transmission_mode == TRANSMISSION_MODE_PARALLEL) && (data_unit_id != DATA_UNIT_EBU_TELETEXT_SUBTITLE)) return;

        if ((receiving_data == YES) && (
                ((transmission_mode == TRANSMISSION_MODE_SERIAL) && (PAGE(page_number) != PAGE(config.page))) ||
                ((transmission_mode == TRANSMISSION_MODE_PARALLEL) && (PAGE(page_number) != PAGE(config.page)) && (m == MAGAZINE(config.page)))
            )) {
            receiving_data = NO;
            return;
        }

        // Page transmission is terminated, however now we are waiting for our new page
        if (page_number != config.page) return;

        // Now we have the begining of page transmission; if there is page_buffer pending, process it
        if (page_buffer.tainted == YES) {
            // it would be nice, if subtitle hides on previous video frame, so we contract 40 ms (1 frame @25 fps)
            page_buffer.hide_timestamp = timestamp - 40;
            process_page(&page_buffer);
        }

        page_buffer.show_timestamp = timestamp;
        page_buffer.hide_timestamp = 0;
        memset(page_buffer.text, 0x00, sizeof(page_buffer.text));
        page_buffer.tainted = NO;
        receiving_data = YES;
        primary_charset.g0_x28 = UNDEF;

        uint8_t c = (primary_charset.g0_m29 != UNDEF) ? primary_charset.g0_m29 : charset;
        remap_g0_charset(c);

        /*
        // I know -- not needed; in subtitles we will never need disturbing teletext page status bar
        // displaying tv station name, current time etc.
        if (flag_suppress_header == NO) {
            for (uint8_t i = 14; i < 40; i++) page_buffer.text[y][i] = telx_to_ucs2(packet->data[i]);
            //page_buffer.tainted = YES;
        }
        */
    }
    else if ((m == MAGAZINE(config.page)) && (y >= 1) && (y <= 23) && (receiving_data == YES)) {
        // ETS 300 706, chapter 9.4.1: Packets X/26 at presentation Levels 1.5, 2.5, 3.5 are used for addressing
        // a character location and overwriting the existing character defined on the Level 1 page
        // ETS 300 706, annex B.2.2: Packets with Y = 26 shall be transmitted before any packets with Y = 1 to Y = 25;
        // so page_buffer.text[y][i] may already contain any character received
        // in frame number 26, skip original G0 character
        for (uint8_t i = 0; i < 40; i++) if (page_buffer.text[y][i] == 0x00) page_buffer.text[y][i] = telx_to_ucs2(packet->data[i]);
        page_buffer.tainted = YES;
    }
    else if ((m == MAGAZINE(config.page)) && (y == 26) && (receiving_data == YES)) {
        // ETS 300 706, chapter 12.3.2: X/26 definition
        uint8_t x26_row = 0;
        uint8_t x26_col = 0;

        uint32_t triplets[13] = { 0 };
        for (uint8_t i = 1, j = 0; i < 40; i += 3, j++) triplets[j] = unham_24_18((packet->data[i + 2] << 16) | (packet->data[i + 1] << 8) | packet->data[i]);

        for (uint8_t j = 0; j < 13; j++) {
            if (triplets[j] == 0xffffffff) {
                // invalid data (HAM24/18 uncorrectable error detected), skip group
                VERBOSE_ONLY fprintf(stderr, "! Unrecoverable data error; UNHAM24/18()=%04x\n", triplets[j]);
                continue;
            }

            uint8_t data = (triplets[j] & 0x3f800) >> 11;
            uint8_t mode = (triplets[j] & 0x7c0) >> 6;
            uint8_t address = triplets[j] & 0x3f;
            uint8_t row_address_group = (address >= 40) && (address <= 63);

            // ETS 300 706, chapter 12.3.1, table 27: set active position
            if ((mode == 0x04) && (row_address_group == YES)) {
                x26_row = address - 40;
                if (x26_row == 0) x26_row = 24;
                x26_col = 0;
            }

            // ETS 300 706, chapter 12.3.1, table 27: termination marker
            if ((mode >= 0x11) && (mode <= 0x1f) && (row_address_group == YES)) break;

            // ETS 300 706, chapter 12.3.1, table 27: character from G2 set
            if ((mode == 0x0f) && (row_address_group == NO)) {
                x26_col = address;
                if (data > 31) page_buffer.text[x26_row][x26_col] = G2[0][data - 0x20];
            }

            // ETS 300 706, chapter 12.3.1, table 27: G0 character with diacritical mark
            if ((mode >= 0x11) && (mode <= 0x1f) && (row_address_group == NO)) {
                x26_col = address;

                // A - Z
                if ((data >= 65) && (data <= 90)) page_buffer.text[x26_row][x26_col] = G2_ACCENTS[mode - 0x11][data - 65];
                // a - z
                else if ((data >= 97) && (data <= 122)) page_buffer.text[x26_row][x26_col] = G2_ACCENTS[mode - 0x11][data - 71];
                // other
                else page_buffer.text[x26_row][x26_col] = telx_to_ucs2(data);
            }
        }
    }
    else if ((m == MAGAZINE(config.page)) && (y == 28) && (receiving_data == YES)) {
        // TODO:
        //   ETS 300 706, chapter 9.4.7: Packet X/28/4
        //   Where packets 28/0 and 28/4 are both transmitted as part of a page, packet 28/0 takes precedence over 28/4 for all but the colour map entry coding.
        if ((designation_code == 0) || (designation_code == 4)) {
            // ETS 300 706, chapter 9.4.2: Packet X/28/0 Format 1
            // ETS 300 706, chapter 9.4.7: Packet X/28/4
            uint32_t triplet0 = unham_24_18((packet->data[3] << 16) | (packet->data[2] << 8) | packet->data[1]);

            if (triplet0 == 0xffffffff) {
                // invalid data (HAM24/18 uncorrectable error detected), skip group
                VERBOSE_ONLY fprintf(stderr, "! Unrecoverable data error; UNHAM24/18()=%04x\n", triplet0);
            }
            else {
                // ETS 300 706, chapter 9.4.2: Packet X/28/0 Format 1 only
                if ((triplet0 & 0x0f) == 0x00) {
                    primary_charset.g0_x28 = (triplet0 & 0x3f80) >> 7;
                    remap_g0_charset(primary_charset.g0_x28);
                }
            }
        }
    }
    else if ((m == MAGAZINE(config.page)) && (y == 29)) {
        // TODO:
        //   ETS 300 706, chapter 9.5.1 Packet M/29/0
        //   Where M/29/0 and M/29/4 are transmitted for the same magazine, M/29/0 takes precedence over M/29/4.
        if ((designation_code == 0) || (designation_code == 4)) {
            // ETS 300 706, chapter 9.5.1: Packet M/29/0
            // ETS 300 706, chapter 9.5.3: Packet M/29/4
            uint32_t triplet0 = unham_24_18((packet->data[3] << 16) | (packet->data[2] << 8) | packet->data[1]);

            if (triplet0 == 0xffffffff) {
                // invalid data (HAM24/18 uncorrectable error detected), skip group
                VERBOSE_ONLY fprintf(stderr, "! Unrecoverable data error; UNHAM24/18()=%04x\n", triplet0);
            }
            else {
                // ETS 300 706, table 11: Coding of Packet M/29/0
                // ETS 300 706, table 13: Coding of Packet M/29/4
                if ((triplet0 & 0xff) == 0x00) {
                    primary_charset.g0_m29 = (triplet0 & 0x3f80) >> 7;
                    // X/28 takes precedence over M/29
                    if (primary_charset.g0_x28 == UNDEF) {
                        remap_g0_charset(primary_charset.g0_m29);
                    }
                }
            }
        }
    }
    else if ((m == 8) && (y == 30)) {
        // ETS 300 706, chapter 9.8: Broadcast Service Data Packets
        if (states.programme_info_processed == NO) {
            // ETS 300 706, chapter 9.8.1: Packet 8/30 Format 1
            if (unham_8_4(packet->data[0]) < 2) {
                fprintf(stderr, "- Programme Identification Data = ");
                for (uint8_t i = 20; i < 40; i++) {
                    uint8_t c = telx_to_ucs2(packet->data[i]);
                    // strip any control codes from PID, eg. TVP station
                    if (c < 0x20) continue;

                    char u[4] = { 0, 0, 0, 0 };
                    ucs2_to_utf8(u, c);
                    fprintf(stderr, "%s", u);
                }
                fprintf(stderr, "\n");

                // OMG! ETS 300 706 stores timestamp in 7 bytes in Modified Julian Day in BCD format + HH:MM:SS in BCD format
                // + timezone as 5-bit count of half-hours from GMT with 1-bit sign
                // In addition all decimals are incremented by 1 before transmission.
                uint32_t t = 0;
                // 1st step: BCD to Modified Julian Day
                t += (packet->data[10] & 0x0f) * 10000;
                t += ((packet->data[11] & 0xf0) >> 4) * 1000;
                t += (packet->data[11] & 0x0f) * 100;
                t += ((packet->data[12] & 0xf0) >> 4) * 10;
                t += (packet->data[12] & 0x0f);
                t -= 11111;
                // 2nd step: conversion Modified Julian Day to unix timestamp
                t = (t - 40587) * 86400;
                // 3rd step: add time
                t += 3600 * ( ((packet->data[13] & 0xf0) >> 4) * 10 + (packet->data[13] & 0x0f) );
                t +=   60 * ( ((packet->data[14] & 0xf0) >> 4) * 10 + (packet->data[14] & 0x0f) );
                t +=        ( ((packet->data[15] & 0xf0) >> 4) * 10 + (packet->data[15] & 0x0f) );
                t -= 40271;
                // 4th step: conversion to time_t
                time_t t0 = (time_t)t;

                // Silly SVT timezone offset
                time_t now = time(NULL);
                time_t diff = (time_t) lroundf( (t0 - now) / 3600.0 ) * 3600;
                t0 -= diff;
                fprintf(stderr, "- Programme Timestamp (UTC) = %s", ctime(&t0));

                VERBOSE_ONLY fprintf(stderr, "- Transmission mode = %s\n", (transmission_mode == TRANSMISSION_MODE_SERIAL ? "serial" : "parallel"));

                fprintf(stderr, "- Broadcast Service Data Packet received, resetting UTC referential value to %s", ctime(&t0));
                config.utc_refvalue = (uint32_t) t0;
                states.pts_initialized = NO;

                states.programme_info_processed = YES;
            }
        }
    }
}

static void process_pes_packet(uint8_t *buffer, uint16_t size) {
    if (size < 6) return;

    // Packetized Elementary Stream (PES) 32-bit start code
    uint64_t pes_prefix = (buffer[0] << 16) | (buffer[1] << 8) | buffer[2];
    uint8_t pes_stream_id = buffer[3];

    // check for PES header
    if (pes_prefix != 0x000001) return;

    // stream_id is not "Private Stream 1" (0xbd)
    if (pes_stream_id != 0xbd) return;

    // PES packet length
    // ETSI EN 301 775 V1.2.1 (2003-05) chapter 4.3: (N x 184) - 6 + 6 B header
    uint16_t pes_packet_length = 6 + ((buffer[4] << 8) | buffer[5]);
    // Can be zero. If the "PES packet length" is set to zero, the PES packet can be of any length.
    // A value of zero for the PES packet length can be used only when the PES packet payload is a video elementary stream.
    if (pes_packet_length == 6) return;

    // truncate incomplete PES packets
    if (pes_packet_length > size) pes_packet_length = size;

    uint8_t optional_pes_header_included = NO;
    uint16_t optional_pes_header_length = 0;
    // optional PES header marker bits (10.. ....)
    if ((buffer[6] & 0xc0) == 0x80) {
        optional_pes_header_included = YES;
        optional_pes_header_length = buffer[8];
    }

    // should we use PTS or PCR?
    static uint8_t using_pts = UNDEF;
    if (using_pts == UNDEF) {
        if ((optional_pes_header_included == YES) && ((buffer[7] & 0x80) > 0)) {
            using_pts = YES;
            VERBOSE_ONLY fprintf(stderr, "- PID 0xbd PTS available\n");
        } else {
            using_pts = NO;
            VERBOSE_ONLY fprintf(stderr, "- PID 0xbd PTS unavailable, using TS PCR\n");
        }
    }

    uint32_t t = 0;
    // If there is no PTS available, use global PCR
    if (using_pts == NO) {
        t = global_timestamp;
    }
    else {
        // PTS is 33 bits wide, however, timestamp in ms fits into 32 bits nicely (PTS/90)
        // presentation and decoder timestamps use the 90 KHz clock, hence PTS/90 = [ms]
        uint64_t pts = 0;
        // __MUST__ assign value to uint64_t and __THEN__ rotate left by 29 bits
        // << is defined for signed int (as in "C" spec.) and overflow occures
        pts = (buffer[9] & 0x0e);
        pts <<= 29;
        pts |= (buffer[10] << 22);
        pts |= ((buffer[11] & 0xfe) << 14);
        pts |= (buffer[12] << 7);
        pts |= ((buffer[13] & 0xfe) >> 1);
        t = pts / 90;
    }

    static int64_t delta = 0;
    static uint32_t t0 = 0;
    if (states.pts_initialized == NO) {
        delta = 1000 * config.utc_refvalue - t;
        states.pts_initialized = YES;

        if ((using_pts == NO) && (global_timestamp == 0)) {
            // We are using global PCR, nevertheless we still have not received valid PCR timestamp yet
            states.pts_initialized = NO;
        }
    }
    if (t < t0) delta = last_timestamp;
    last_timestamp = t + delta;
    t0 = t;

    // skip optional PES header and process each 46 bytes long teletext packet
    uint16_t i = 7;
    if (optional_pes_header_included == YES) i += 3 + optional_pes_header_length;
    while (i <= pes_packet_length - 6) {
        uint8_t data_unit_id = buffer[i++];
        uint8_t data_unit_len = buffer[i++];

        if ((data_unit_id == DATA_UNIT_EBU_TELETEXT_NONSUBTITLE) || (data_unit_id == DATA_UNIT_EBU_TELETEXT_SUBTITLE)) {
            // teletext payload has always size 44 bytes
            if (data_unit_len == 44) {
                // reverse endianess (via lookup table), ETS 300 706, chapter 7.1
                for (uint8_t j = 0; j < data_unit_len; j++) buffer[i + j] = REVERSE_8[buffer[i + j]];

                // FIXME: This explicit type conversion could be a problem some day -- do not need to be platform independant
                process_telx_packet(data_unit_id, (teletext_packet_payload_t *)&buffer[i], last_timestamp);
            }
        }

        i += data_unit_len;
    }
}

void process_ts_packet(uint8_t *ts_packet) {
    if (!ts_validate(ts_packet)) {
        VERBOSE_ONLY fprintf(stderr, "Invalid TS packet received. Skipping\n"); // WARN
        return;
    }

    // Transport Stream Header
    // We do not use buffer to struct loading (e.g. ts_packet_t *header = (ts_packet_t *)ts_packet;)
    // -- struct packing is platform dependant and not performing well.
    ts_packet_t header = { 0 };
    header.sync = ts_packet[0];
    header.transport_error = (ts_packet[1] & 0x80) >> 7;
    header.payload_unit_start = (ts_packet[1] & 0x40) >> 6;
    header.transport_priority = (ts_packet[1] & 0x20) >> 5;
    header.pid = ((ts_packet[1] & 0x1f) << 8) | ts_packet[2];
    header.scrambling_control = (ts_packet[3] & 0xc0) >> 6;
    header.adaptation_field_exists = (ts_packet[3] & 0x20) >> 5;
    header.continuity_counter = ts_packet[3] & 0x0f;
    //uint8_t ts_payload_exists = (ts_packet[3] & 0x10) >> 4;

    uint8_t af_discontinuity = 0;
    if (header.adaptation_field_exists > 0) {
        af_discontinuity = (ts_packet[5] & 0x80) >> 7;
    }

    // uncorrectable error?
    if (header.transport_error > 0) {
        VERBOSE_ONLY fprintf(stderr, "! Uncorrectable TS packet error (received CC %1x)\n", header.continuity_counter);
        return;
    }

    // if available, calculate current PCR
    if (header.adaptation_field_exists > 0) {
        // PCR in adaptation field
        uint8_t af_pcr_exists = (ts_packet[5] & 0x10) >> 4;
        if (af_pcr_exists > 0) {
            uint64_t pts = ts_packet[6];
            pts <<= 25;
            pts |= (ts_packet[7] << 17);
            pts |= (ts_packet[8] << 9);
            pts |= (ts_packet[9] << 1);
            pts |= (ts_packet[10] >> 7);
            global_timestamp = pts / 90;
            pts = ((ts_packet[10] & 0x01) << 8);
            pts |= ts_packet[11];
            global_timestamp += pts / 27000;
        }
    }

    // null packet
    if (header.pid == 0x1fff)
        return;

    if (config.tid == header.pid) {
        // TS continuity check
        if (continuity_counter == 255) {
            continuity_counter = header.continuity_counter;
        } else {
            if (af_discontinuity == 0) {
                continuity_counter = (continuity_counter + 1) % 16;
                if (header.continuity_counter != continuity_counter) {
                    VERBOSE_ONLY fprintf(stderr, "- Missing TS packet, flushing pes_buffer (expected CC %1x, received CC %1x, TS discontinuity %s, TS priority %s)\n",
                        continuity_counter, header.continuity_counter, (af_discontinuity ? "YES" : "NO"), (header.transport_priority ? "YES" : "NO"));
                    payload_counter = 0;
                    continuity_counter = 255;
                }
            }
        }

        // waiting for first payload_unit_start indicator
        if ((header.payload_unit_start == 0) && (payload_counter == 0))
            return;

        // proceed with payload buffer
        if ((header.payload_unit_start > 0) && (payload_counter > 0))
            process_pes_packet(payload_buffer, payload_counter);

        // new payload frame start
        if (header.payload_unit_start > 0)
            payload_counter = 0;

        // add payload data to buffer
        if (payload_counter < (PAYLOAD_BUFFER_SIZE - TS_PACKET_PAYLOAD_SIZE)) {
            memcpy(&payload_buffer[payload_counter], &ts_packet[4], TS_PACKET_PAYLOAD_SIZE);
            payload_counter += TS_PACKET_PAYLOAD_SIZE;
        }
        else VERBOSE_ONLY fprintf(stderr, "! Packet payload size exceeds payload_buffer size, probably not teletext stream\n");
    }
}

void telxcc_init(uint16_t pid, uint16_t page) {
}


int main(const int argc, char *argv[]) {
    uint16_t pid, page;
    in_addr_t addr;
    uint32_t port;
    int s, e;

    if (argc != 5)
        errx(1, "usage: teletext-ingest <pid> <page> <addr> <port>");

    pid = strtoul(argv[1], NULL, 10);
    page = strtoul(argv[2], NULL, 10);
    addr = inet_addr(argv[3]);
    port = strtoul(argv[4], NULL, 10);

    // Setup telxcc parser config
    config.utc_refvalue = (uint64_t) time(NULL);
    config.tid = pid;
    // dec to BCD, magazine pages numbers are in BCD (ETSI 300 706)
    config.page = ((page / 100) << 8) | (((page / 10) % 10) << 4) | (page % 10);

    // Multicast receiver
    s = socket(AF_INET, SOCK_DGRAM, PF_UNSPEC);
    if (s == -1)
        err(1, "socket");

    int yes = 1;
    e = setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    if (e == -1)
        err(1, "reuseaddr");

    struct sockaddr_in sin = { 0 };
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = htonl(INADDR_ANY);
    sin.sin_port = htons(port);

    e = bind(s, (struct sockaddr *) &sin, sizeof sin);
    if (e == -1)
        err(1, "bind");

    e = setsockopt(s, IPPROTO_IP, IP_ADD_MEMBERSHIP, (struct ip_mreq[]){{
            .imr_multiaddr.s_addr = addr,
            .imr_interface.s_addr = htonl(INADDR_ANY)
        }}, sizeof(struct ip_mreq));
    if (e == -1)
        err(1, "setsockopt");

    uint8_t buffer[RTP_HEADER_SIZE + 7 * TS_SIZE] = { 0 };
    uint8_t *ts_packet = NULL;

    // reading input
    while (1) {
        if (recv(s, &buffer, sizeof buffer, 0) != sizeof buffer) {
            VERBOSE_ONLY fprintf(stderr, "Read to few packets :-(\n"); // WARN
            continue;
        } else if (!rtp_check_hdr(&buffer[0])) {
            VERBOSE_ONLY fprintf(stderr, "Invalid RTP packet received. Skipping\n"); // WARN
            continue;
        }

        ts_packet = rtp_payload(&buffer[0]);

        for (int i = 0; i < 7; i++) {
            process_ts_packet(ts_packet);
            ts_packet += TS_SIZE;
        }
    }

    return 0;
}
