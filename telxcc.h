#ifndef TELXCC_H_INCLUDED
#define TELXCC_H_INCLUDED

typedef enum {
    NO = 0x00,
    YES = 0x01,
    UNDEF = 0xff
} bool_t;

typedef struct {
    uint8_t sync;
    uint8_t transport_error;
    uint8_t payload_unit_start;
    uint8_t transport_priority;
    uint16_t pid;
    uint8_t scrambling_control;
    uint8_t adaptation_field_exists;
    uint8_t continuity_counter;
} ts_packet_t;

typedef struct {
    uint16_t program_num;
    uint16_t program_pid;
} pat_section_t;

typedef struct {
    uint8_t pointer_field;
    uint8_t table_id;
    uint16_t section_length;
    uint8_t current_next_indicator;
} pat_t;

typedef struct {
    uint8_t stream_type;
    uint16_t elementary_pid;
    uint16_t es_info_length;
} pmt_program_descriptor_t;

typedef struct {
    uint8_t pointer_field;
    uint8_t table_id;
    uint16_t section_length;
    uint16_t program_num;
    uint8_t current_next_indicator;
    uint16_t pcr_pid;
    uint16_t program_info_length;
} pmt_t;

typedef enum {
    DATA_UNIT_EBU_TELETEXT_NONSUBTITLE = 0x02,
    DATA_UNIT_EBU_TELETEXT_SUBTITLE = 0x03,
    DATA_UNIT_EBU_TELETEXT_INVERTED = 0x0c,
    DATA_UNIT_VPS = 0xc3,
    DATA_UNIT_CLOSED_CAPTIONS = 0xc5
} data_unit_t;

typedef enum {
    TRANSMISSION_MODE_PARALLEL = 0,
    TRANSMISSION_MODE_SERIAL = 1
} transmission_mode_t;

// 1-byte alignment; just to be sure, this struct is being used for explicit type conversion
// FIXME: remove explicit type conversion from buffer to structs
#pragma pack(push)
#pragma pack(1)
typedef struct {
    uint8_t _clock_in; // clock run in
    uint8_t _framing_code; // framing code, not needed, ETSI 300 706: const 0xe4
    uint8_t address[2];
    uint8_t data[40];
} teletext_packet_payload_t;
#pragma pack(pop)

typedef struct {
    uint64_t show_timestamp; // show at timestamp (in ms)
    uint64_t hide_timestamp; // hide at timestamp (in ms)
    uint16_t text[25][40]; // 25 lines x 40 cols (1 screen/page) of wide chars
    uint8_t tainted; // 1 = text variable contains any data
} teletext_page_t;

#define log_warn(...) do { fprintf(stderr, "[WARN] "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while (0)
#define log_info(...) do { fprintf(stderr, "[INFO] "); fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while (0)

#endif
