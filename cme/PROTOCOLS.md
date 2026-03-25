# CME Globex Protocol Reference Schemas

Protocol schema files for the CME Globex exchange simulator. These schemas
define the wire format for order entry (iLink 3) and market data (MDP 3.0)
using FIX Simple Binary Encoding (SBE).

## Protocol Versions

| Protocol | Package | Schema ID | Schema Version | Semantic Version | Description Date |
|---|---|---|---|---|---|
| iLink 3 | iLinkBinary | 8 | 5 | FIX 5.0 | 2020-02-12 |
| MDP 3.0 | mktdata | 1 | 13 | FIX 5.0 SP2 | 2023-04-11 |

**Note on iLink 3 versioning:** The downloaded schema (from GitHub) is version 5
(schema ID 8). CME production currently runs **schema version 9** (launched
January 25, 2026). Version 9 is only available via CME's authenticated SFTP
site (sftpng.cmegroup.com). The version 5 schema covers the core message
structure and all foundational message types; version 9 adds incremental
extensions via the SBE `sinceVersion` mechanism without breaking backward
compatibility of the base messages.

**Note on MDP 3.0 versioning:** The downloaded schema is version 13
(description 2023-04-11), obtained from CME's public FTP site. This is the
current production schema for market data.

## Directory Structure

```
resources/cme/
  README.md                          -- this file
  ilink3/
    ilinkbinary.xml                  -- iLink 3 SBE schema (order entry)
  mdp3/
    templates_FixBinary.xml          -- MDP 3.0 SBE schema (market data)
```

## Schema File Sources

### iLink 3 -- ilinkbinary.xml

- **Source:** https://github.com/sambacha/CME-iLink3 (public mirror)
- **Official source:** CME SFTP site (sftpng.cmegroup.com), requires CME
  credentials. Path: `/SBEFix/Production/Templates/ilinkbinary.xml`
  (exact path may vary; not publicly documented).
- **Format:** FIX SBE XML schema (namespace `http://www.fixprotocol.org/ns/simple/1.0`)
- **Byte order:** Little endian

### MDP 3.0 -- templates_FixBinary.xml

- **Source:** `ftp://ftp.cmegroup.com/SBEFix/Production/Templates/templates_FixBinary.xml`
- **HTTPS mirror:** `https://www.cmegroup.com/ftp/SBEFix/Production/Templates/templates_FixBinary.xml`
- **Format:** FIX SBE XML schema (namespace `http://www.fixprotocol.org/ns/simple/1.0`)
- **Byte order:** Little endian

## Obtaining Updated Schemas

CME distributes schema files via FTP and SFTP:

| Method | Host | Notes |
|---|---|---|
| FTP (public) | ftp.cmegroup.com | MDP 3.0 schemas available publicly |
| HTTPS (public) | www.cmegroup.com/ftp/ | Web access to FTP content |
| SFTP (authenticated) | sftpng.cmegroup.com | iLink 3 and MDP 3.0; requires CME credentials |

To download the latest schemas with credentials:

```bash
# MDP 3.0 (public)
wget -O resources/cme/mdp3/templates_FixBinary.xml \
  "ftp://ftp.cmegroup.com/SBEFix/Production/Templates/templates_FixBinary.xml"

# iLink 3 (requires SFTP credentials from CME)
sftp user@sftpng.cmegroup.com
# Navigate to the schema directory and download ilinkbinary.xml
```

## iLink 3 -- Order Entry Protocol

iLink 3 is CME's binary order entry protocol using SBE encoding over the
FIX Performance Session Layer (FIXP). It supports low-latency order
submission, modification, cancellation, and execution reporting.

### Session Layer Messages (FIXP)

| Template ID | Message | Description |
|---|---|---|
| 500 | Negotiate | Initiate session negotiation |
| 501 | NegotiationResponse | Successful negotiation response |
| 502 | NegotiationReject | Negotiation rejected |
| 503 | Establish | Establish session after negotiation |
| 504 | EstablishmentAck | Session established |
| 505 | EstablishmentReject | Session establishment rejected |
| 506 | Sequence | Heartbeat / sequence number sync |
| 507 | Terminate | Terminate session |
| 508 | RetransmitRequest | Request message retransmission |
| 509 | Retransmission | Retransmission header |
| 510 | RetransmitReject | Retransmission rejected |
| 513 | NotApplied | Gap notification |

### Order Entry Messages (Client -> Exchange)

| Template ID | Message | FIX MsgType | Description |
|---|---|---|---|
| 514 | NewOrderSingle | D | Submit a new order |
| 515 | OrderCancelReplaceRequest | G | Modify an existing order |
| 516 | OrderCancelRequest | F | Cancel an existing order |
| 517 | MassQuote | i | Submit mass quote (market makers) |
| 528 | QuoteCancel | Z | Cancel quotes |
| 529 | OrderMassActionRequest | CA | Mass cancel orders |
| 530 | OrderMassStatusRequest | AF | Request status of multiple orders |
| 533 | OrderStatusRequest | H | Request status of a single order |
| 543 | RequestForQuote | R | Submit request for quote |
| 544 | NewOrderCross | c | Submit cross order |
| 560 | SecurityDefinitionRequest | c | Request UDS creation |

### Execution Reports (Exchange -> Client)

| Template ID | Message | FIX MsgType | Description |
|---|---|---|---|
| 522 | ExecutionReportNew | 8 | New order acknowledged |
| 523 | ExecutionReportReject | 8 | Order rejected |
| 524 | ExecutionReportElimination | 8 | Order eliminated (IOC/FOK) |
| 525 | ExecutionReportTradeOutright | 8 | Outright trade fill |
| 526 | ExecutionReportTradeSpread | 8 | Spread trade fill |
| 527 | ExecutionReportTradeSpreadLeg | 8 | Spread leg trade fill |
| 531 | ExecutionReportModify | 8 | Order modification acknowledged |
| 532 | ExecutionReportStatus | 8 | Order status response |
| 534 | ExecutionReportCancel | 8 | Order cancellation acknowledged |
| 548 | ExecutionReportTradeAddendumOutright | 8 | Trade addendum (outright) |
| 549 | ExecutionReportTradeAddendumSpread | 8 | Trade addendum (spread) |
| 550 | ExecutionReportTradeAddendumSpreadLeg | 8 | Trade addendum (spread leg) |

### Other Response Messages (Exchange -> Client)

| Template ID | Message | FIX MsgType | Description |
|---|---|---|---|
| 519 | PartyDetailsDefinitionRequestAck | CY | Party details acknowledged |
| 521 | BusinessReject | j | Business-level rejection |
| 535 | OrderCancelReject | 9 | Cancel request rejected |
| 536 | OrderCancelReplaceReject | 9 | Modify request rejected |
| 538 | PartyDetailsListReport | CG | Party details list response |
| 539 | ExecutionAck | BN | Execution acknowledgment |
| 545 | MassQuoteAck | b | Mass quote acknowledged |
| 546 | RequestForQuoteAck | b | RFQ acknowledged |
| 561 | SecurityDefinitionResponse | d | UDS creation response |
| 562 | OrderMassActionReport | BZ | Mass action report |
| 563 | QuoteCancelAck | b | Quote cancel acknowledged |

### Party Management Messages

| Template ID | Message | FIX MsgType | Description |
|---|---|---|---|
| 518 | PartyDetailsDefinitionRequest | CX | Define party details |
| 537 | PartyDetailsListRequest | CF | List party details |

## MDP 3.0 -- Market Data Protocol

MDP 3.0 is CME's market data dissemination protocol using SBE encoding over
UDP multicast (incremental) and TCP (snapshots). It provides real-time book
updates, trade summaries, instrument definitions, and statistics.

### Administrative Messages

| Template ID | Message | FIX MsgType | Description |
|---|---|---|---|
| 4 | ChannelReset | X | Reset all instruments on channel |
| 12 | AdminHeartbeat | 0 | Administrative heartbeat |
| 15 | AdminLogin | A | Login (TCP snapshot) |
| 16 | AdminLogout | 5 | Logout (TCP snapshot) |

### Incremental Refresh Messages (UDP Multicast)

| Template ID | Message | Description |
|---|---|---|
| 46 | MDIncrementalRefreshBook | Book update (bid/ask changes) |
| 47 | MDIncrementalRefreshOrderBook | Order-level book update (MBO) |
| 48 | MDIncrementalRefreshTradeSummary | Trade summary |
| 49 | MDIncrementalRefreshDailyStatistics | Daily statistics (open/high/low/settle) |
| 50 | MDIncrementalRefreshLimitsBanding | Price limits and banding |
| 51 | MDIncrementalRefreshSessionStatistics | Session statistics |
| 37 | MDIncrementalRefreshVolume | Electronic volume |
| 62 | CollateralMarketValue | Collateral market value |
| 64 | MDIncrementalRefreshBookLongQty | Book update (long quantity) |
| 65 | MDIncrementalRefreshTradeSummaryLongQty | Trade summary (long quantity) |
| 66 | MDIncrementalRefreshVolumeLongQty | Volume (long quantity) |
| 67 | MDIncrementalRefreshSessionStatisticsLongQty | Session stats (long quantity) |

### Snapshot Messages

| Template ID | Message | Description |
|---|---|---|
| 52 | SnapshotFullRefresh | Full book snapshot (UDP) |
| 53 | SnapshotFullRefreshOrderBook | Full order book snapshot (MBO, UDP) |
| 59 | SnapshotRefreshTopOrders | Top-of-book snapshot |
| 61 | SnapshotFullRefreshTCP | Full book snapshot (TCP) |
| 68 | SnapshotFullRefreshTCPLongQty | Full snapshot TCP (long quantity) |
| 69 | SnapshotFullRefreshLongQty | Full snapshot (long quantity) |

### Instrument Definition Messages

| Template ID | Message | Description |
|---|---|---|
| 54 | MDInstrumentDefinitionFuture | Futures instrument definition |
| 55 | MDInstrumentDefinitionOption | Options instrument definition |
| 56 | MDInstrumentDefinitionSpread | Spread instrument definition |
| 57 | MDInstrumentDefinitionFixedIncome | Fixed income instrument definition |
| 58 | MDInstrumentDefinitionRepo | Repo instrument definition |
| 63 | MDInstrumentDefinitionFX | FX instrument definition |

### Status Messages

| Template ID | Message | FIX MsgType | Description |
|---|---|---|---|
| 30 | SecurityStatus | f | Security trading status |
| 39 | QuoteRequest | R | Quote request |
| 60 | SecurityStatusWorkup | f | Workup trading status |

## SBE Encoding Reference

Both protocols use the FIX Simple Binary Encoding standard:

- **Byte order:** Little endian
- **Header:** 4 bytes (blockLength: uint16, templateId: uint16) followed by
  (schemaId: uint16, version: uint16) = 8 bytes total message header
- **Repeating groups:** Prefixed with groupSize (blockLength: uint16,
  numInGroup: uint8)
- **Null values:** Specific sentinel values per type (e.g., 2147483647 for
  Int32NULL, 18446744073709551615 for uInt64NULL)
- **Prices:** Fixed-point with mantissa and constant exponent (-9 for PRICE9)

## Official Documentation Links

- CME iLink 3 Specification:
  https://cmegroupclientsite.atlassian.net/wiki/spaces/EPICSANDBOX/pages/714539039/iLink+Functional+Specification
- CME iLink 3 SBE Encoding:
  https://cmegroupclientsite.atlassian.net/wiki/spaces/EPICSANDBOX/pages/714113056/iLink+Simple+Binary+Encoding
- CME MDP 3.0 Message Schema:
  https://cmegroupclientsite.atlassian.net/wiki/spaces/EPICSANDBOX/pages/457672149/MDP+3.0+-+FTP+and+SFTP+Site+Information
- CME MDP 3.0 SBE Encoding:
  https://cmegroupclientsite.atlassian.net/wiki/display/EPICSANDBOX/MDP+3.0+-+Simple+Binary+Encoding
- CME Develop to Globex Portal:
  https://www.cmegroup.com/globex/develop-to-cme-globex.html
- FIX SBE Specification (Real Logic):
  https://github.com/real-logic/simple-binary-encoding
- Open Markets Initiative (protocol references):
  https://github.com/Open-Markets-Initiative/wireshark-lua/tree/main/Cme
