# Future TODO -- Spread/Implied Trading (Parked Items)

Items identified during review of the implied spread trading plan that are
deferred to future phases. Not blocked on current implementation.

| ID | Item | Description | Exchange | Effort |
|---|---|---|---|---|
| M4 | Mass cancel of spread orders | Cancel all spread orders for an instrument/account in one operation | CME, ICE | ~100 lines |
| M5 | Instrument halt propagation | Halt on an outright leg propagates to all spreads referencing that leg | CME, ICE | ~80 lines |
| M6 | Market data implied level display | Show implied price levels in observer TUI alongside outright levels | All | ~120 lines |
| M7 | Reconciler spread-aware fill matching | Extend journal-reconciler to validate spread fill invariants (leg consistency, ratio checks) | All | ~150 lines |
| M8 | Trader binary spread order protocol | Add spread order entry/cancel to exchange-trader's iLink3/FIX protocol paths | CME, ICE | ~100 lines |
| M9 | Stop trigger on implied fills | Configurable: ICE triggers stops on implied fills, CME does not (exchange-specific policy) | CME, ICE | ~60 lines |
| M10 | Auction interaction with spreads | Opening/closing auction implied price participation and priority rules | CME, ICE | ~200 lines |
| W5 | ICE dual-simulator SpreadBook integration | SpreadBook legs using FIFO for one leg and GTBPR for another within same spread | ICE | ~150 lines |
| -- | Implied recalc performance benchmarks | Measure implied price recalculation latency as a function of N spreads per outright | All | ~50 lines |
