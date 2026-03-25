#pragma once

#include "cme/codec/sbe_header.h"

#include <cstdint>
#include <cstring>

namespace exchange::cme::sbe::ilink3 {

// ---------------------------------------------------------------------------
// Template IDs — used for message dispatch after decoding the SBE header.
// ---------------------------------------------------------------------------

constexpr uint16_t NEW_ORDER_SINGLE_ID              = 514;
constexpr uint16_t ORDER_CANCEL_REPLACE_REQUEST_ID   = 515;
constexpr uint16_t ORDER_CANCEL_REQUEST_ID           = 516;
constexpr uint16_t ORDER_MASS_ACTION_REQUEST_ID      = 529;
constexpr uint16_t EXEC_REPORT_NEW_ID                = 522;
constexpr uint16_t EXEC_REPORT_REJECT_ID             = 523;
constexpr uint16_t EXEC_REPORT_TRADE_OUTRIGHT_ID     = 525;
constexpr uint16_t EXEC_REPORT_CANCEL_ID             = 534;
constexpr uint16_t ORDER_CANCEL_REJECT_ID            = 535;

// ---------------------------------------------------------------------------
// iLink3 field enumerations — values from ilinkbinary.xml <enum> definitions.
// ---------------------------------------------------------------------------

enum class SideReq : uint8_t {
    Buy  = 1,
    Sell = 2,
};

enum class OrdType : uint8_t {
    MarketWithProtection = 1,
    Limit                = 2,
    StopWithProtection   = 3,
    StopLimit            = 4,
};

enum class TimeInForce : uint8_t {
    Day = 0,
    GTC = 1,  // Good Till Cancel
    FAK = 3,  // Fill and Kill (IOC)
    FOK = 4,  // Fill or Kill
    GTD = 6,  // Good Till Date
};

enum class ExecType : uint8_t {
    New          = static_cast<uint8_t>('0'),
    Canceled     = static_cast<uint8_t>('4'),
    Replaced     = static_cast<uint8_t>('5'),
    Rejected     = static_cast<uint8_t>('8'),
    Expired      = static_cast<uint8_t>('C'),
    Trade        = static_cast<uint8_t>('F'),
    TradeCancel  = static_cast<uint8_t>('H'),
    Status       = static_cast<uint8_t>('I'),
};

enum class OrdStatus : uint8_t {
    New             = static_cast<uint8_t>('0'),
    PartiallyFilled = static_cast<uint8_t>('1'),
    Filled          = static_cast<uint8_t>('2'),
    Canceled        = static_cast<uint8_t>('4'),
    Rejected        = static_cast<uint8_t>('8'),
    Expired         = static_cast<uint8_t>('C'),
    Undefined       = static_cast<uint8_t>('U'),  // CancelReject status
};

enum class MassActionScope : uint8_t {
    Instrument    = 1,
    All           = 7,
    MarketSegment = 9,
    InstrumentGroup = 10,  // schema: "Product Group" maps to value 10
};

enum class ManualOrdInd : uint8_t {
    Automated = 0,
    Manual    = 1,
};

enum class ExecMode : uint8_t {
    Aggressive = static_cast<uint8_t>('A'),
    Passive    = static_cast<uint8_t>('P'),
};

// ---------------------------------------------------------------------------
// Client -> Exchange: NewOrderSingle (514)
//
// blockLength = 116 bytes.  Schema: ilinkbinary.xml line 481.
// ---------------------------------------------------------------------------

struct __attribute__((packed)) NewOrderSingle514 {
    static constexpr uint16_t TEMPLATE_ID   = NEW_ORDER_SINGLE_ID;
    static constexpr uint16_t BLOCK_LENGTH  = 116;

    PRICE9   price;                       // offset 0   — PRICENULL9
    uint32_t order_qty;                   // offset 8
    int32_t  security_id;                 // offset 12
    uint8_t  side;                        // offset 16
    uint32_t seq_num;                     // offset 17
    char     sender_id[20];              // offset 21
    char     cl_ord_id[20];              // offset 41
    uint64_t party_details_list_req_id;  // offset 61
    uint64_t order_request_id;           // offset 69
    uint64_t sending_time_epoch;         // offset 77
    PRICE9   stop_px;                    // offset 85  — PRICENULL9
    char     location[5];               // offset 93
    uint32_t min_qty;                    // offset 98  — uInt32NULL
    uint32_t display_qty;               // offset 102 — uInt32NULL
    uint16_t expire_date;               // offset 106 — LocalMktDate
    uint8_t  ord_type;                   // offset 108
    uint8_t  time_in_force;              // offset 109
    uint8_t  manual_order_indicator;     // offset 110
    uint8_t  exec_inst;                  // offset 111
    uint8_t  execution_mode;             // offset 112
    uint8_t  liquidity_flag;             // offset 113 — BooleanNULL
    uint8_t  managed_order;              // offset 114 — BooleanNULL
    uint8_t  short_sale_type;            // offset 115
};

static_assert(sizeof(NewOrderSingle514) == 116,
    "NewOrderSingle514 must be 116 bytes (schema blockLength)");

// ---------------------------------------------------------------------------
// Client -> Exchange: OrderCancelReplaceRequest (515)
//
// blockLength = 125 bytes.  Schema: ilinkbinary.xml line 506.
// ---------------------------------------------------------------------------

struct __attribute__((packed)) OrderCancelReplaceRequest515 {
    static constexpr uint16_t TEMPLATE_ID   = ORDER_CANCEL_REPLACE_REQUEST_ID;
    static constexpr uint16_t BLOCK_LENGTH  = 125;

    PRICE9   price;                       // offset 0   — PRICENULL9
    uint32_t order_qty;                   // offset 8
    int32_t  security_id;                 // offset 12
    uint8_t  side;                        // offset 16
    uint32_t seq_num;                     // offset 17
    char     sender_id[20];              // offset 21
    char     cl_ord_id[20];              // offset 41
    uint64_t party_details_list_req_id;  // offset 61
    uint64_t order_id;                   // offset 69
    PRICE9   stop_px;                    // offset 77  — PRICENULL9
    uint64_t order_request_id;           // offset 85
    uint64_t sending_time_epoch;         // offset 93
    char     location[5];               // offset 101
    uint32_t min_qty;                    // offset 106 — uInt32NULL
    uint32_t display_qty;               // offset 110 — uInt32NULL
    uint16_t expire_date;               // offset 114 — LocalMktDate
    uint8_t  ord_type;                   // offset 116
    uint8_t  time_in_force;              // offset 117
    uint8_t  manual_order_indicator;     // offset 118
    uint8_t  ofm_override;              // offset 119
    uint8_t  exec_inst;                  // offset 120
    uint8_t  execution_mode;             // offset 121
    uint8_t  liquidity_flag;             // offset 122 — BooleanNULL
    uint8_t  managed_order;              // offset 123 — BooleanNULL
    uint8_t  short_sale_type;            // offset 124
};

static_assert(sizeof(OrderCancelReplaceRequest515) == 125,
    "OrderCancelReplaceRequest515 must be 125 bytes (schema blockLength)");

// ---------------------------------------------------------------------------
// Client -> Exchange: OrderCancelRequest (516)
//
// blockLength = 88 bytes.  Schema: ilinkbinary.xml line 533.
// ---------------------------------------------------------------------------

struct __attribute__((packed)) OrderCancelRequest516 {
    static constexpr uint16_t TEMPLATE_ID   = ORDER_CANCEL_REQUEST_ID;
    static constexpr uint16_t BLOCK_LENGTH  = 88;

    uint64_t order_id;                   // offset 0
    uint64_t party_details_list_req_id;  // offset 8
    uint8_t  manual_order_indicator;     // offset 16
    uint32_t seq_num;                    // offset 17
    char     sender_id[20];             // offset 21
    char     cl_ord_id[20];             // offset 41
    uint64_t order_request_id;          // offset 61
    uint64_t sending_time_epoch;        // offset 69
    char     location[5];              // offset 77
    int32_t  security_id;              // offset 82
    uint8_t  side;                      // offset 86
    uint8_t  liquidity_flag;            // offset 87  — BooleanNULL
};

static_assert(sizeof(OrderCancelRequest516) == 88,
    "OrderCancelRequest516 must be 88 bytes (schema blockLength)");

// ---------------------------------------------------------------------------
// Client -> Exchange: OrderMassActionRequest (529)
//
// blockLength = 71 bytes.  Schema: ilinkbinary.xml line 949.
// ---------------------------------------------------------------------------

struct __attribute__((packed)) OrderMassActionRequest529 {
    static constexpr uint16_t TEMPLATE_ID   = ORDER_MASS_ACTION_REQUEST_ID;
    static constexpr uint16_t BLOCK_LENGTH  = 71;

    uint64_t party_details_list_req_id;  // offset 0
    uint64_t order_request_id;           // offset 8
    uint8_t  manual_order_indicator;     // offset 16
    uint32_t seq_num;                    // offset 17
    char     sender_id[20];             // offset 21
    // MassActionType is constant '3', no wire field (offset 41 is after sender_id)
    uint64_t sending_time_epoch;         // offset 41
    char     security_group[6];         // offset 49
    char     location[5];              // offset 55
    int32_t  security_id;              // offset 60 — Int32NULL
    uint8_t  mass_action_scope;         // offset 64
    uint8_t  market_segment_id;         // offset 65 — uInt8NULL
    uint8_t  mass_cancel_request_type;  // offset 66
    uint8_t  side;                      // offset 67 — SideNULL
    uint8_t  ord_type;                  // offset 68
    uint8_t  time_in_force;             // offset 69
    uint8_t  liquidity_flag;            // offset 70 — BooleanNULL
};

static_assert(sizeof(OrderMassActionRequest529) == 71,
    "OrderMassActionRequest529 must be 71 bytes (schema blockLength)");

// ---------------------------------------------------------------------------
// Exchange -> Client: ExecutionReportNew (522)
//
// blockLength = 209 bytes.  Schema: ilinkbinary.xml line 650.
// ---------------------------------------------------------------------------

struct __attribute__((packed)) ExecutionReportNew522 {
    static constexpr uint16_t TEMPLATE_ID   = EXEC_REPORT_NEW_ID;
    static constexpr uint16_t BLOCK_LENGTH  = 209;

    uint32_t seq_num;                     // offset 0
    uint64_t uuid;                        // offset 4
    char     exec_id[40];               // offset 12
    char     sender_id[20];             // offset 52
    char     cl_ord_id[20];             // offset 72
    uint64_t party_details_list_req_id;  // offset 92
    uint64_t order_id;                   // offset 100
    PRICE9   price;                       // offset 108
    PRICE9   stop_px;                    // offset 116 — PRICENULL9
    uint64_t transact_time;              // offset 124
    uint64_t sending_time_epoch;         // offset 132
    uint64_t order_request_id;           // offset 140
    uint64_t cross_id;                   // offset 148 — uInt64NULL
    uint64_t host_cross_id;             // offset 156 — uInt64NULL
    char     location[5];               // offset 164
    int32_t  security_id;               // offset 169
    uint32_t order_qty;                  // offset 173
    uint32_t min_qty;                    // offset 177 — uInt32NULL
    uint32_t display_qty;               // offset 181 — uInt32NULL
    uint16_t expire_date;               // offset 185 — LocalMktDate
    uint16_t delay_duration;            // offset 187 — uInt16NULL
    uint8_t  ord_type;                   // offset 189
    uint8_t  side;                       // offset 190
    uint8_t  time_in_force;              // offset 191
    uint8_t  manual_order_indicator;     // offset 192
    uint8_t  poss_retrans_flag;          // offset 193
    uint8_t  split_msg;                  // offset 194 — SplitMsg (uInt8NULL)
    uint8_t  cross_type;                 // offset 195 — uInt8NULL
    uint8_t  exec_inst;                  // offset 196
    uint8_t  execution_mode;             // offset 197
    uint8_t  liquidity_flag;             // offset 198 — BooleanNULL
    uint8_t  managed_order;              // offset 199 — BooleanNULL
    uint8_t  short_sale_type;            // offset 200
    uint64_t delay_to_time;             // offset 201 — uInt64NULL
};

static_assert(sizeof(ExecutionReportNew522) == 209,
    "ExecutionReportNew522 must be 209 bytes (schema blockLength)");

// ---------------------------------------------------------------------------
// Exchange -> Client: ExecutionReportReject (523)
//
// blockLength = 467 bytes.  Schema: ilinkbinary.xml line 688.
// ---------------------------------------------------------------------------

struct __attribute__((packed)) ExecutionReportReject523 {
    static constexpr uint16_t TEMPLATE_ID   = EXEC_REPORT_REJECT_ID;
    static constexpr uint16_t BLOCK_LENGTH  = 467;

    uint32_t seq_num;                     // offset 0
    uint64_t uuid;                        // offset 4
    char     text[256];                  // offset 12
    char     exec_id[40];               // offset 268
    char     sender_id[20];             // offset 308
    char     cl_ord_id[20];             // offset 328
    uint64_t party_details_list_req_id;  // offset 348
    uint64_t order_id;                   // offset 356
    PRICE9   price;                       // offset 364 — PRICENULL9
    PRICE9   stop_px;                    // offset 372 — PRICENULL9
    uint64_t transact_time;              // offset 380
    uint64_t sending_time_epoch;         // offset 388
    uint64_t order_request_id;           // offset 396
    uint64_t cross_id;                   // offset 404 — uInt64NULL
    uint64_t host_cross_id;             // offset 412 — uInt64NULL
    char     location[5];               // offset 420
    int32_t  security_id;               // offset 425
    uint32_t order_qty;                  // offset 429
    uint32_t min_qty;                    // offset 433 — uInt32NULL
    uint32_t display_qty;               // offset 437 — uInt32NULL
    uint16_t ord_rej_reason;            // offset 441
    uint16_t expire_date;               // offset 443 — LocalMktDate
    uint16_t delay_duration;            // offset 445 — uInt16NULL
    uint8_t  ord_type;                   // offset 447
    uint8_t  side;                       // offset 448
    uint8_t  time_in_force;              // offset 449
    uint8_t  manual_order_indicator;     // offset 450
    uint8_t  poss_retrans_flag;          // offset 451
    uint8_t  split_msg;                  // offset 452
    uint8_t  cross_type;                 // offset 453 — uInt8NULL
    uint8_t  exec_inst;                  // offset 454
    uint8_t  execution_mode;             // offset 455
    uint8_t  liquidity_flag;             // offset 456 — BooleanNULL
    uint8_t  managed_order;              // offset 457 — BooleanNULL
    uint8_t  short_sale_type;            // offset 458
    uint64_t delay_to_time;             // offset 459 — uInt64NULL
};

static_assert(sizeof(ExecutionReportReject523) == 467,
    "ExecutionReportReject523 must be 467 bytes (schema blockLength)");

// ---------------------------------------------------------------------------
// Exchange -> Client: ExecutionReportTradeOutright (525)
//
// blockLength = 235 bytes.  Schema: ilinkbinary.xml line 764.
// Root block only — NoFills and NoOrderEvents repeating groups follow.
// ---------------------------------------------------------------------------

struct __attribute__((packed)) ExecutionReportTradeOutright525 {
    static constexpr uint16_t TEMPLATE_ID   = EXEC_REPORT_TRADE_OUTRIGHT_ID;
    static constexpr uint16_t BLOCK_LENGTH  = 235;

    uint32_t seq_num;                     // offset 0
    uint64_t uuid;                        // offset 4
    char     exec_id[40];               // offset 12
    char     sender_id[20];             // offset 52
    char     cl_ord_id[20];             // offset 72
    uint64_t party_details_list_req_id;  // offset 92
    PRICE9   last_px;                    // offset 100
    uint64_t order_id;                   // offset 108
    PRICE9   price;                       // offset 116
    PRICE9   stop_px;                    // offset 124 — PRICENULL9
    uint64_t transact_time;              // offset 132
    uint64_t sending_time_epoch;         // offset 140
    uint64_t order_request_id;           // offset 148
    uint64_t sec_exec_id;               // offset 156
    uint64_t cross_id;                   // offset 164 — uInt64NULL
    uint64_t host_cross_id;             // offset 172 — uInt64NULL
    char     location[5];               // offset 180
    int32_t  security_id;               // offset 185
    uint32_t order_qty;                  // offset 189
    uint32_t last_qty;                   // offset 193
    uint32_t cum_qty;                    // offset 197
    uint32_t md_trade_entry_id;         // offset 201
    uint32_t side_trade_id;             // offset 205
    uint32_t trade_link_id;             // offset 209 — uInt32NULL
    uint32_t leaves_qty;                // offset 213
    uint16_t trade_date;                // offset 217 — LocalMktDate
    uint16_t expire_date;               // offset 219 — LocalMktDate
    uint8_t  ord_status;                // offset 221
    uint8_t  ord_type;                   // offset 222
    uint8_t  side;                       // offset 223
    uint8_t  time_in_force;              // offset 224
    uint8_t  manual_order_indicator;     // offset 225
    uint8_t  poss_retrans_flag;          // offset 226
    uint8_t  aggressor_indicator;        // offset 227
    uint8_t  cross_type;                 // offset 228 — uInt8NULL
    uint8_t  exec_inst;                  // offset 229
    uint8_t  execution_mode;             // offset 230
    uint8_t  liquidity_flag;             // offset 231 — BooleanNULL
    uint8_t  managed_order;              // offset 232 — BooleanNULL
    uint8_t  short_sale_type;            // offset 233
    uint8_t  ownership;                  // offset 234
};

static_assert(sizeof(ExecutionReportTradeOutright525) == 235,
    "ExecutionReportTradeOutright525 must be 235 bytes (schema blockLength)");

// Fill entry within NoFills repeating group (blockLength = 15)
struct __attribute__((packed)) FillEntry {
    PRICE9   fill_px;         // offset 0
    uint32_t fill_qty;        // offset 8
    char     fill_exec_id[2]; // offset 12
    uint8_t  fill_yield_type; // offset 14
};

static_assert(sizeof(FillEntry) == 15, "FillEntry must be 15 bytes");

// OrderEvent entry within NoOrderEvents repeating group (blockLength = 23)
struct __attribute__((packed)) OrderEventEntry {
    PRICE9   order_event_px;       // offset 0
    char     order_event_text[5];  // offset 8
    uint32_t order_event_exec_id;  // offset 13
    uint32_t order_event_qty;      // offset 17
    uint8_t  order_event_type;     // offset 21
    uint8_t  order_event_reason;   // offset 22
};

static_assert(sizeof(OrderEventEntry) == 23, "OrderEventEntry must be 23 bytes");

// ---------------------------------------------------------------------------
// Exchange -> Client: ExecutionReportCancel (534)
//
// blockLength = 214 bytes.  Schema: ilinkbinary.xml line 1074.
// ---------------------------------------------------------------------------

struct __attribute__((packed)) ExecutionReportCancel534 {
    static constexpr uint16_t TEMPLATE_ID   = EXEC_REPORT_CANCEL_ID;
    static constexpr uint16_t BLOCK_LENGTH  = 214;

    uint32_t seq_num;                     // offset 0
    uint64_t uuid;                        // offset 4
    char     exec_id[40];               // offset 12
    char     sender_id[20];             // offset 52
    char     cl_ord_id[20];             // offset 72
    uint64_t party_details_list_req_id;  // offset 92
    uint64_t order_id;                   // offset 100
    PRICE9   price;                       // offset 108
    PRICE9   stop_px;                    // offset 116 — PRICENULL9
    uint64_t transact_time;              // offset 124
    uint64_t sending_time_epoch;         // offset 132
    uint64_t order_request_id;           // offset 140
    uint64_t cross_id;                   // offset 148 — uInt64NULL
    uint64_t host_cross_id;             // offset 156 — uInt64NULL
    char     location[5];               // offset 164
    int32_t  security_id;               // offset 169
    uint32_t order_qty;                  // offset 173
    uint32_t cum_qty;                    // offset 177
    uint32_t min_qty;                    // offset 181 — uInt32NULL
    uint32_t display_qty;               // offset 185 — uInt32NULL
    uint16_t expire_date;               // offset 189 — LocalMktDate
    uint16_t delay_duration;            // offset 191 — uInt16NULL
    uint8_t  ord_type;                   // offset 193
    uint8_t  side;                       // offset 194
    uint8_t  time_in_force;              // offset 195
    uint8_t  manual_order_indicator;     // offset 196
    uint8_t  poss_retrans_flag;          // offset 197
    uint8_t  split_msg;                  // offset 198
    uint8_t  exec_restatement_reason;   // offset 199
    uint8_t  cross_type;                 // offset 200 — uInt8NULL
    uint8_t  exec_inst;                  // offset 201
    uint8_t  execution_mode;             // offset 202
    uint8_t  liquidity_flag;             // offset 203 — BooleanNULL
    uint8_t  managed_order;              // offset 204 — BooleanNULL
    uint8_t  short_sale_type;            // offset 205
    uint64_t delay_to_time;             // offset 206 — uInt64NULL
};

static_assert(sizeof(ExecutionReportCancel534) == 214,
    "ExecutionReportCancel534 must be 214 bytes (schema blockLength)");

// ---------------------------------------------------------------------------
// Exchange -> Client: OrderCancelReject (535)
//
// blockLength = 409 bytes.  Schema: ilinkbinary.xml line 1114.
// ---------------------------------------------------------------------------

struct __attribute__((packed)) OrderCancelReject535 {
    static constexpr uint16_t TEMPLATE_ID   = ORDER_CANCEL_REJECT_ID;
    static constexpr uint16_t BLOCK_LENGTH  = 409;

    uint32_t seq_num;                     // offset 0
    uint64_t uuid;                        // offset 4
    char     text[256];                  // offset 12
    char     exec_id[40];               // offset 268
    char     sender_id[20];             // offset 308
    char     cl_ord_id[20];             // offset 328
    uint64_t party_details_list_req_id;  // offset 348
    uint64_t order_id;                   // offset 356
    uint64_t transact_time;              // offset 364
    uint64_t sending_time_epoch;         // offset 372
    uint64_t order_request_id;           // offset 380
    char     location[5];               // offset 388
    uint16_t cxl_rej_reason;            // offset 393
    uint16_t delay_duration;            // offset 395 — uInt16NULL
    uint8_t  manual_order_indicator;     // offset 397
    uint8_t  poss_retrans_flag;          // offset 398
    uint8_t  split_msg;                  // offset 399
    uint8_t  liquidity_flag;             // offset 400 — BooleanNULL
    uint64_t delay_to_time;             // offset 401 — uInt64NULL
};

static_assert(sizeof(OrderCancelReject535) == 409,
    "OrderCancelReject535 must be 409 bytes (schema blockLength)");

// ---------------------------------------------------------------------------
// Helper: build a MessageHeader for a given message type.
// ---------------------------------------------------------------------------

template <typename MsgT>
constexpr MessageHeader make_header() {
    return MessageHeader{
        MsgT::BLOCK_LENGTH,
        MsgT::TEMPLATE_ID,
        ILINK3_SCHEMA_ID,
        ILINK3_VERSION
    };
}

}  // namespace exchange::cme::sbe::ilink3
