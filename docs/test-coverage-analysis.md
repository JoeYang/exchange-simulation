# Exchange-Core Test Coverage Analysis

**Date:** 2026-03-25
**Scope:** All engine features vs. journal test scenarios and C++ unit tests

---

## 1. Feature Inventory

### 1.1 Order Types

| Feature | Description |
|---------|-------------|
| Limit | Rests on book at specified price; matches at that price or better |
| Market | Fills immediately at best available price; remainder cancelled (IOC-like) |
| Stop | Rests in stop book; triggers as Market when last_trade crosses stop_price |
| StopLimit | Rests in stop book; triggers as Limit at order price when last_trade crosses stop_price |

### 1.2 Time-in-Force (TIF)

| Feature | Description |
|---------|-------------|
| DAY | Rests until end-of-day expiry via trigger_expiry(ts, DAY) |
| GTC | Rests indefinitely (good-til-cancel) |
| IOC | Immediate-or-cancel: fills what it can, cancels remainder |
| FOK | Fill-or-kill: pre-checks full liquidity; cancels entire order if insufficient |
| GTD | Good-til-date: expires when trigger_expiry(ts, GTD) and ts >= gtd_expiry |

### 1.3 Matching Algorithms

| Feature | Description |
|---------|-------------|
| FIFO (time priority) | Fills orders oldest-first at each price level |
| ProRata (proportional) | Allocates proportionally by size; remainder distributed FIFO |

### 1.4 Order Lifecycle

| Feature | Description |
|---------|-------------|
| New order (accept) | Validates, assigns OrderId, fires OrderAccepted |
| New order (reject) | Validation failure fires OrderRejected with reason |
| Cancel (user) | Removes from book/stop-book, fires OrderCancelled(UserRequested) |
| Cancel (unknown) | Fires OrderCancelRejected(UnknownOrder) |
| Cancel (already filled) | Fires OrderCancelRejected(UnknownOrder) |
| Modify (cancel-replace) | Removes old, re-inserts with new price/qty, may trigger fill |
| Modify (amend-down) | In-place qty reduction at same price, preserves priority |
| Modify (reject unknown) | Fires OrderModifyRejected(UnknownOrder) |
| Modify (reject invalid) | Rejects for invalid price/qty/tick/lot |
| Expiry (DAY) | trigger_expiry cancels all DAY orders |
| Expiry (GTD) | trigger_expiry cancels GTD orders where ts >= gtd_expiry |

### 1.5 Session States (8 states)

| State | Accept Orders | Match | Stop Triggers |
|-------|--------------|-------|---------------|
| Closed | No | No | No |
| PreOpen | Yes (limit only) | No (collect) | No |
| OpeningAuction | No (uncrossing) | Auction algo | No |
| Continuous | Yes | Yes | Yes |
| PreClose | Yes (limit only) | No (collect) | No |
| ClosingAuction | No (uncrossing) | Auction algo | No |
| Halt | Configurable | No | No |
| VolatilityAuction | Yes (collect) | No | No |

### 1.6 Auctions

| Feature | Description |
|---------|-------------|
| calculate_auction_price | Equilibrium price: max volume, min imbalance, closest to ref, higher price tiebreak |
| execute_auction | Fill all matchable orders at single auction price |
| Indicative price | Publish what-if auction price during collection (publish_indicative_price) |
| Auction with iceberg | Iceberg display qty visible during auction; tranche reveal after fill |
| MarketStatus callback | Fires on session state transition |

### 1.7 Iceberg Orders

| Feature | Description |
|---------|-------------|
| Validation: display_qty > quantity | Rejected (InvalidQuantity) |
| Validation: display_qty not lot-aligned | Rejected (InvalidQuantity) |
| Validation: display_qty == quantity | Accepted (degenerate iceberg) |
| Field initialization | remaining_quantity = display_qty; total_qty = quantity |
| Tranche reveal after fill | Next tranche = min(display_qty, total_qty - filled_qty) |
| Priority loss on reveal | Order moved to back of queue at same price level |
| MD: L3 Add on reveal | OrderBookAction(Add) fires with display qty |
| MD: L2 Update on reveal | DepthUpdate(Update) fires with new level total |
| Full consumption | All tranches filled; order removed from book |
| Last tranche smaller | min(display_qty, remainder) correctly sized |
| Multiple icebergs at same level | FIFO priority with interleaved reveals |

### 1.8 Self-Match Prevention (SMP)

| Feature | Description |
|---------|-------------|
| CancelNewest | Default SMP action: cancel aggressor on self-match |
| CancelOldest | Cancel resting order on self-match |
| CancelBoth | Cancel both aggressor and resting |
| Decrement | Reduce both orders by match quantity |
| None / disabled | Default: no SMP (is_self_match returns false) |

### 1.9 Risk Controls

| Feature | Description |
|---------|-------------|
| Tick size validation | Price must be multiple of tick_size (0 = disabled) |
| Lot size validation | Quantity must be multiple of lot_size (0 = disabled) |
| Price band (static) | Reject if price outside [band_low, band_high] (0 = disabled) |
| Price band (dynamic) | CRTP hook calculate_dynamic_bands(last_trade_price) |
| Max order size | Reject if qty > max_order_size (0 = no limit) |
| Order pool exhaustion | Reject with PoolExhausted when pool full |
| Level pool exhaustion | Cancel with LevelPoolExhausted when no level slots |
| OrderId exhaustion | Reject with PoolExhausted when next_order_id >= MaxOrderIds |
| Zero quantity | Rejected (InvalidQuantity) |
| Zero/negative price (limit) | Rejected (InvalidPrice) |
| Stop price <= 0 | Rejected (InvalidPrice) |

### 1.10 Market Data Callbacks

| Feature | Description |
|---------|-------------|
| L1 (TopOfBook) | Best bid/ask price and qty; fires when best changes |
| L2 (DepthUpdate) | Per-level: Add, Update, Remove with total_qty and order_count |
| L3 (OrderBookAction) | Per-order: Add, Modify, Cancel, Fill with qty |
| Trade | Price, qty, aggressor_id, resting_id, aggressor_side |
| MarketStatus | Session state changes |
| IndicativePrice | Auction indicative during collection phases |

### 1.11 Mass Cancel

| Feature | Description |
|---------|-------------|
| mass_cancel(account_id) | Cancel all orders for a specific account |
| mass_cancel_all() | Cancel all active orders |

### 1.12 Stop Triggers and Cascading

| Feature | Description |
|---------|-------------|
| Buy stop trigger | last_trade >= stop_price |
| Sell stop trigger | last_trade <= stop_price |
| Stop cascade | After triggered stop fills, re-check stops against new last_trade |
| Stop-limit trigger | Converts to limit order at specified price |

### 1.13 Modify Policies

| Feature | Description |
|---------|-------------|
| CancelReplace | Default: remove old, re-insert new (loses priority) |
| AmendDown | In-place qty reduction at same price (preserves priority) |
| RejectModify | Reject all modify requests |

---

## 2. Coverage Matrix

### 2.1 Order Types

| Feature | Journal Coverage | Unit Test Coverage | Gap? |
|---------|-----------------|-------------------|------|
| Limit buy rests on empty book | basic_limit_buy.journal | LimitOrderRestsOnEmptyBook | No |
| Limit sell rests on empty book | basic_limit_sell.journal | (in LimitOrderRestsOnEmptyBook variant) | No |
| Limit full fill | limit_full_fill.journal | TwoLimitsFill | No |
| Limit partial fill | limit_partial_fill.journal | PartialFill | No |
| Market order fills resting | market_order_fill.journal | MarketOrderFills | No |
| Market order no liquidity | market_order_no_liquidity.journal | MarketOrderNoLiquidity | No |
| Market order sweep multi-level | market_order_sweep.journal | -- | No |
| Market order TIF behavior | market_order_tif.journal | -- | No |
| Stop order trigger | stop_trigger.journal | StopOrderTriggers | No |
| Stop-limit trigger and rest | stop_limit_trigger.journal | -- | No |
| Stop order cancel | cancel_stop_order.journal | -- | No |

### 2.2 Time-in-Force

| Feature | Journal Coverage | Unit Test Coverage | Gap? |
|---------|-----------------|-------------------|------|
| IOC full fill | ioc_full_fill.journal | IocFullFill | No |
| IOC partial cancel | ioc_partial_cancel.journal | IocPartialCancel | No |
| FOK full fill | fok_full_fill.journal | FokFullFill | No |
| FOK no fill (insufficient) | fok_no_fill.journal | FokNoFill | No |
| FOK multi-level check | fok_multi_level.journal | -- | No |
| DAY expiry | day_expiry.journal | DayOrderExpiry | No |
| GTD expiry (expired) | gtd_expiry.journal | -- | No |
| GTD not expired | gtd_not_expired.journal | -- | No |
| GTC rests indefinitely | basic_limit_buy.journal (implicit) | LimitOrderRestsOnEmptyBook | No |
| IOC on empty book (no fill) | -- | -- | **GAP** |
| FOK on empty book (no fill) | -- | -- | **GAP** |

### 2.3 Matching Algorithms

| Feature | Journal Coverage | Unit Test Coverage | Gap? |
|---------|-----------------|-------------------|------|
| FIFO priority | fifo_priority.journal | -- | No |
| ProRata basic (proportional) | pro_rata_basic.journal | -- | No |
| ProRata equal sizes (remainder FIFO) | pro_rata_equal_sizes.journal | -- | No |
| ProRata remainder distribution | pro_rata_remainder.journal | -- | No |
| ProRata single order (degenerate) | pro_rata_single_order.journal | -- | No |
| ProRata multi-level sweep | -- | -- | **GAP** |
| ProRata with FOK | -- | -- | **GAP** |
| ProRata with IOC | -- | -- | **GAP** |

### 2.4 Order Lifecycle

| Feature | Journal Coverage | Unit Test Coverage | Gap? |
|---------|-----------------|-------------------|------|
| Cancel resting order | cancel_resting.journal | CancelRestingOrder | No |
| Cancel unknown order | cancel_unknown.journal | CancelUnknownOrder | No |
| Cancel already-filled order | cancel_already_filled.journal | -- | No |
| Cancel on empty book | empty_book_cancel.journal | -- | No |
| Cancel stop order | cancel_stop_order.journal | -- | No |
| Cancel partially-filled order | -- | -- | **GAP** |
| Modify price change (cancel-replace) | modify_price_change.journal | ModifyOrderCancelReplace | No |
| Modify qty down (cancel-replace) | modify_qty_down.journal | -- | No |
| Modify qty up (cancel-replace) | modify_qty_up.journal | -- | No |
| Modify same price (priority loss) | modify_same_price.journal | -- | No |
| Modify triggers fill | modify_triggers_fill.journal | -- | No |
| Modify unknown order | modify_unknown.journal | ModifyUnknownOrder | No |
| Modify stop order (rejected) | modify_stop_order.journal | -- | No |
| Modify amend-down policy | -- | -- | **GAP** |
| Modify reject-modify policy | -- | -- | **GAP** |
| Multiple fills at same level | multiple_fills.journal | -- | No |

### 2.5 Session States

| Feature | Journal Coverage | Unit Test Coverage | Gap? |
|---------|-----------------|-------------------|------|
| Closed -> reject new order | -- | -- | **GAP** |
| Closed -> reject cancel | -- | -- | **GAP** |
| Closed -> reject modify | -- | -- | **GAP** |
| PreOpen -> accept limit, no match | auction_simple_cross.journal (implicit) | -- | No |
| PreOpen -> reject market order | -- | -- | **GAP** |
| PreOpen -> cancel resting order | -- | -- | **GAP** |
| PreOpen -> modify resting order | -- | -- | **GAP** |
| Continuous -> normal matching | auction_full_lifecycle.journal (partial) | all matching tests | No |
| PreClose -> accept limit, no match | -- | -- | **GAP** |
| VolatilityAuction -> collect, no match | -- | -- | **GAP** |
| Halt -> configurable acceptance | -- | -- | **GAP** |
| OpeningAuction -> no new orders | -- | -- | **GAP** |
| ClosingAuction -> no new orders | -- | -- | **GAP** |
| Session transition: set_session_state | auction_full_lifecycle.journal | -- | No |
| MarketStatus callback | auction_full_lifecycle.journal | -- | No |
| Transition from Closed to PreOpen | auction_full_lifecycle.journal (starts at PreOpen) | -- | Partial |
| Transition from PreOpen to Continuous | auction_full_lifecycle.journal | -- | No |
| All 8 state transitions | -- | -- | **GAP** |

### 2.6 Auctions

| Feature | Journal Coverage | Unit Test Coverage | Gap? |
|---------|-----------------|-------------------|------|
| Empty book auction | auction_empty_book.journal | EmptyBook_NoPrice | No |
| Simple cross (1 bid + 1 ask) | auction_simple_cross.journal | OneBidOneAsk_Crossing | No |
| No crossing | auction_no_cross.journal | NoCrossing_BestBidBelowBestAsk | No |
| Multiple levels | auction_multiple_levels.journal | MultipleBids_OneAsk_MaximizesVolume | No |
| Partial fill (surplus remains) | auction_partial_fill.journal | ExecuteAuction_PartialFill_SurplusRemains | No |
| Reference tiebreak | auction_reference_tiebreak.journal | EqualImbalance_RefPriceTiebreak | No |
| Indicative price | auction_indicative_price.journal | PublishIndicativePrice_DuringPreOpen | No |
| Full lifecycle (PreOpen->auction->Continuous) | auction_full_lifecycle.journal | -- | No |
| Idempotency (calc is read-only) | -- | Idempotent_NoSideEffects | Journal gap |
| Fill price = auction price | -- | ExecuteAuction_FillPriceIsAuctionPrice | Journal gap |
| All callbacks fire in auction | -- | ExecuteAuction_AllCallbacksFire | Journal gap |
| Auction with iceberg orders | -- | ExecuteAuction_IcebergOrder | **GAP** (journal) |
| Indicative price updates with new orders | -- | PublishIndicativePrice_UpdatesWithNewOrders | Journal gap |
| Indicative price no crossing | -- | PublishIndicativePrice_NoCrossing_NoEvent | Journal gap |
| Indicative price no side effects | -- | PublishIndicativePrice_NoSideEffects | Journal gap |
| Symmetric book max volume | -- | SymmetricBook_MaxVolumeAtMiddle | Journal gap |
| Complex scenario clear winner | -- | ComplexScenario_ClearWinner | Journal gap |
| Volume tie imbalance tiebreak | -- | VolumeTie_ImbalanceTiebreak | Journal gap |
| Higher price convention | -- | AllTied_HigherPriceWins | Journal gap |
| Bids only no price | -- | BidsOnly_NoPrice | Journal gap |
| Asks only no price | -- | AsksOnly_NoPrice | Journal gap |
| Exact price match single level | -- | ExactPriceMatch_SingleLevel | Journal gap |

### 2.7 Iceberg Orders

| Feature | Journal Coverage | Unit Test Coverage | Gap? |
|---------|-----------------|-------------------|------|
| Non-iceberg unchanged | -- | NonIcebergOrderBehavesNormally | **GAP** (journal) |
| Reject display_qty > quantity | -- | RejectIcebergDisplayQtyExceedsTotal | **GAP** (journal) |
| Reject display_qty not lot-aligned | -- | RejectIcebergDisplayQtyNotAlignedToLotSize | **GAP** (journal) |
| display_qty == quantity valid | -- | IcebergDisplayQtyEqualToTotalIsValid | **GAP** (journal) |
| Fields correctly initialized | -- | IcebergOrderFieldsCorrectlyInitialized | **GAP** (journal) |
| No lot_size constraint | -- | IcebergAcceptedWithNoLotSizeConstraint | **GAP** (journal) |
| Non-iceberg total_qty == quantity | -- | NonIcebergTotalQtyEqualsQuantity | **GAP** (journal) |
| Tranche reveal after fill | -- | IcebergTrancheRevealAfterFill | **GAP** (journal) |
| Fully consumed removed from book | -- | IcebergFullyConsumedRemovedFromBook | **GAP** (journal) |
| Tranche reveal fires callbacks | -- | IcebergTrancheRevealFiresCallbacks | **GAP** (journal) |
| Priority loss on reveal | -- | IcebergLosesPriorityOnReveal | **GAP** (journal) |
| Multiple icebergs at same level | -- | MultipleIcebergsAtSameLevel | **GAP** (journal) |
| Non-iceberg regression full fill | -- | NonIcebergRegressionFullFill | Journal gap |
| Non-iceberg regression partial fill | -- | NonIcebergRegressionPartialFill | Journal gap |
| Last tranche smaller than display | -- | IcebergLastTrancheSmaller | **GAP** (journal) |
| Large aggressor fills multiple tranches | -- | AggressorFillsMultipleIcebergTranches | **GAP** (journal) |
| Iceberg in auction | -- | ExecuteAuction_IcebergOrder | **GAP** (journal) |
| Both sides iceberg matching | -- | -- | **GAP** (both) |
| Iceberg cancel | -- | -- | **GAP** (both) |
| Iceberg modify | -- | -- | **GAP** (both) |

### 2.8 Self-Match Prevention (SMP)

| Feature | Journal Coverage | Unit Test Coverage | Gap? |
|---------|-----------------|-------------------|------|
| Default: no SMP (self-trade happens) | self_match_prevention.journal | -- | No |
| SMP CancelNewest | -- | SelfMatchPrevention | **GAP** (journal) |
| SMP CancelOldest | -- | -- | **GAP** (both) |
| SMP CancelBoth | -- | -- | **GAP** (both) |
| SMP Decrement | -- | -- | **GAP** (both) |
| SMP with partial fill before self-match | -- | -- | **GAP** (both) |

### 2.9 Risk Controls

| Feature | Journal Coverage | Unit Test Coverage | Gap? |
|---------|-----------------|-------------------|------|
| Tick size reject | tick_size_reject.journal | RejectInvalidTickSize | No |
| Lot size reject | lot_size_reject.journal | RejectInvalidLotSize | No |
| Zero quantity reject | zero_quantity.journal | RejectZeroQuantity | No |
| Zero price limit reject | zero_price_limit.journal | -- | No |
| Negative price reject | negative_price.journal | -- | No |
| Price band reject (static) | price_band_reject.journal | -- | No |
| Price band (dynamic) | -- | DynamicBandsRejectOutsideRange | **GAP** (journal) |
| Max order size exceeded | -- | MaxOrderSizeExceeded | **GAP** (journal) |
| Max order size at limit | -- | MaxOrderSizeAtLimit | **GAP** (journal) |
| Max order size = 0 (no limit) | -- | MaxOrderSizeZeroMeansNoLimit | **GAP** (journal) |
| Order pool exhaustion | pool_exhaustion_orders.journal | RejectPoolExhaustion | No |
| Level pool exhaustion | pool_exhaustion_levels.journal | -- | Partial |
| OrderId space exhaustion | -- | -- | **GAP** (both) |
| Stop price <= 0 reject | -- | -- | **GAP** (both) |
| Negative quantity reject | -- | -- | **GAP** (both) |

### 2.10 Market Data Callbacks

| Feature | Journal Coverage | Unit Test Coverage | Gap? |
|---------|-----------------|-------------------|------|
| L1 TopOfBook updates | l1_updates.journal | LimitOrderRestsOnEmptyBook | No |
| L2 DepthUpdate (Add/Update/Remove) | l2_depth_updates.journal | all matching tests | No |
| L3 OrderBookAction (Add/Cancel/Fill) | l3_order_actions.journal | ModifyOrderCancelReplace | No |
| Trade events | trade_events.journal | TwoLimitsFill | No |
| MarketStatus callback | auction_full_lifecycle.journal | -- | No |
| IndicativePrice callback | auction_indicative_price.journal | PublishIndicativePrice_* | No |
| L3 Modify action | l3_order_actions.journal | ModifyOrderCancelReplace | No |

### 2.11 Mass Cancel

| Feature | Journal Coverage | Unit Test Coverage | Gap? |
|---------|-----------------|-------------------|------|
| mass_cancel(account_id) | -- | -- | **GAP** (both) |
| mass_cancel_all() | -- | -- | **GAP** (both) |
| Mass cancel with mixed account_ids | -- | -- | **GAP** (both) |
| Mass cancel empty book | -- | -- | **GAP** (both) |
| Mass cancel during PreOpen | -- | -- | **GAP** (both) |

### 2.12 Stop Triggers and Cascading

| Feature | Journal Coverage | Unit Test Coverage | Gap? |
|---------|-----------------|-------------------|------|
| Buy stop trigger | stop_trigger.journal | StopOrderTriggers | No |
| Sell stop trigger | stop_cascade.journal | -- | No |
| Stop cascade (2 levels) | stop_cascade.journal | -- | No |
| Stop-limit trigger and rest | stop_limit_trigger.journal | -- | No |
| Multiple stop cascade (3+ levels) | -- | -- | **GAP** (both) |
| Stop triggered during auction | -- | -- | **GAP** (both) |
| Buy stop and sell stop in same cascade | -- | -- | **GAP** (both) |
| Stop trigger from modify-induced fill | -- | -- | **GAP** (both) |

### 2.13 Stress / Performance

| Feature | Journal Coverage | Unit Test Coverage | Gap? |
|---------|-----------------|-------------------|------|
| 500 resting orders + sweep | stress_500_resting_orders.journal | -- | No |
| Buy/sell alternating fills | stress_buy_sell_alternating.journal | -- | No |
| 500 cancel storm | stress_cancel_storm.journal | -- | No |
| Deep book (100 levels) | stress_deep_book.journal | -- | No |
| Modify storm (200 modifies) | stress_modify_storm.journal | -- | No |

---

## 3. Gap Analysis

### 3.1 Critical Gaps

| # | Missing Feature | Severity | What Should Be Tested | Recommended Journal File |
|---|----------------|----------|----------------------|-------------------------|
| 1 | **Mass cancel (account)** | Critical | Cancel all orders for one account; verify only that account's orders are cancelled; others survive | `mass_cancel_account.journal` |
| 2 | **Mass cancel (all)** | Critical | Cancel every active order; verify book is empty after | `mass_cancel_all.journal` |
| 3 | **Closed state rejects all** | Critical | Orders, cancels, modifies all rejected in Closed state | `session_closed_rejects.journal` |
| 4 | **Iceberg basic (rest + fill + reveal)** | Critical | Place iceberg, aggressor fills one tranche, verify reveal and priority loss | `iceberg_basic.journal` |
| 5 | **Iceberg full consumption** | Critical | Aggressor fills all tranches; order removed from book | `iceberg_full_fill.journal` |
| 6 | **SMP CancelNewest in journal** | Critical | Same-account crossing with SMP enabled; aggressor cancelled, no trade | `smp_cancel_newest.journal` |
| 7 | **Iceberg validation** | Critical | display_qty > qty rejected; not lot-aligned rejected; display_qty == qty accepted | `iceberg_validation.journal` |

### 3.2 Important Gaps

| # | Missing Feature | Severity | What Should Be Tested | Recommended Journal File |
|---|----------------|----------|----------------------|-------------------------|
| 8 | **Cancel partially-filled order** | Important | Fill an order partially, then cancel the remainder; verify correct remaining qty in cancel | `cancel_partial_fill.journal` |
| 9 | **Modify amend-down policy** | Important | AmendDown exchange: reduce qty at same price; verify priority preserved, level qty updated | `modify_amend_down.journal` |
| 10 | **PreOpen rejects market orders** | Important | Market order in PreOpen is cancelled immediately (collection phase) | `session_preopen_market_reject.journal` |
| 11 | **PreClose collection** | Important | Orders rest without matching in PreClose; then transition to ClosingAuction | `session_preclose_collect.journal` |
| 12 | **VolatilityAuction collection** | Important | Orders rest without matching in VolatilityAuction | `session_volatility_auction.journal` |
| 13 | **Iceberg priority loss (journal)** | Important | Two orders at same level; iceberg reveals and goes behind plain order | `iceberg_priority_loss.journal` |
| 14 | **Multiple icebergs at same level** | Important | Two icebergs interleave tranches; verify FIFO ordering of reveals | `iceberg_multiple_same_level.journal` |
| 15 | **Iceberg last tranche smaller** | Important | total_qty not evenly divisible by display_qty; last tranche is smaller | `iceberg_last_tranche.journal` |
| 16 | **Max order size reject (journal)** | Important | Order exceeding max_order_size rejected with MaxOrderSizeExceeded | `max_order_size_reject.journal` |
| 17 | **Dynamic price bands (journal)** | Important | Orders outside dynamic band rejected | `dynamic_price_bands.journal` |
| 18 | **SMP CancelOldest** | Important | SMP cancels resting order instead of aggressor | `smp_cancel_oldest.journal` |
| 19 | **SMP CancelBoth** | Important | SMP cancels both aggressor and resting | `smp_cancel_both.journal` |
| 20 | **ProRata multi-level sweep** | Important | ProRata aggressor sweeps across 2+ price levels | `pro_rata_multi_level.journal` |
| 21 | **Iceberg in auction (journal)** | Important | Auction with iceberg bid; verify only display qty visible | `auction_iceberg.journal` |
| 22 | **Modify reject-modify policy** | Important | RejectModify exchange rejects all modify requests | `modify_reject_policy.journal` |
| 23 | **Stop trigger from modify fill** | Important | Modify crosses spread, fill triggers a stop | `modify_triggers_stop.journal` |

### 3.3 Nice-to-Have Gaps

| # | Missing Feature | Severity | What Should Be Tested | Recommended Journal File |
|---|----------------|----------|----------------------|-------------------------|
| 24 | **IOC on empty book** | Nice-to-have | IOC order with no liquidity; accepted then cancelled | `ioc_empty_book.journal` |
| 25 | **FOK on empty book** | Nice-to-have | FOK order with no liquidity; accepted then FOKFailed | `fok_empty_book.journal` |
| 26 | **SMP Decrement** | Nice-to-have | SMP reduces both orders by matched qty; both survive | `smp_decrement.journal` |
| 27 | **Multiple stop cascade (3+ levels)** | Nice-to-have | Chain of 3+ stop triggers in one cascade | `stop_triple_cascade.journal` |
| 28 | **Both sides iceberg matching** | Nice-to-have | Buy iceberg vs sell iceberg at same price | `iceberg_vs_iceberg.journal` |
| 29 | **Iceberg cancel** | Nice-to-have | Cancel a resting iceberg; full hidden qty removed | `iceberg_cancel.journal` |
| 30 | **Iceberg modify** | Nice-to-have | Modify an iceberg order (cancel-replace) | `iceberg_modify.journal` |
| 31 | **OrderId space exhaustion** | Nice-to-have | next_order_id reaches MaxOrderIds; new order rejected | `orderid_exhaustion.journal` |
| 32 | **Stop price <= 0 reject** | Nice-to-have | Stop order with stop_price=0 or negative; rejected | `stop_invalid_price.journal` |
| 33 | **Halt state behavior** | Nice-to-have | Orders in Halt state (configurable) | `session_halt.journal` |
| 34 | **All 8 state transitions** | Nice-to-have | Walk through all session states in sequence | `session_full_cycle.journal` |
| 35 | **ProRata with IOC** | Nice-to-have | IOC order partially fills via ProRata, remainder cancelled | `pro_rata_ioc.journal` |
| 36 | **ProRata with FOK** | Nice-to-have | FOK order checks ProRata liquidity before matching | `pro_rata_fok.journal` |
| 37 | **Mass cancel during PreOpen** | Nice-to-have | Mass cancel orders that were collected in PreOpen | `mass_cancel_preopen.journal` |
| 38 | **Negative quantity reject** | Nice-to-have | Order with qty < 0; rejected | `negative_quantity.journal` |
| 39 | **Auction idempotency (journal)** | Nice-to-have | calculate_auction_price called twice; same result, book unchanged | `auction_idempotent.journal` |
| 40 | **PreOpen cancel/modify** | Nice-to-have | Cancel and modify orders during PreOpen collection | `session_preopen_cancel_modify.journal` |
| 41 | **SMP with partial fill before self-match** | Nice-to-have | Aggressor fills non-self orders before hitting self-match | `smp_partial_before_self.journal` |
| 42 | **Buy stop + sell stop same cascade** | Nice-to-have | Trade triggers both a buy stop and sell stop | `stop_mixed_cascade.journal` |

---

## 4. Edge Cases Not Tested

### 4.1 Both sides iceberg matching each other

**Status:** Not tested in any journal or unit test.

When a buy iceberg and sell iceberg match at the same price, the engine must handle tranche reveals on both sides within the same match cycle. The resting iceberg reveals tranches as each is consumed; if the aggressor is also an iceberg, its display quantity limits how much it can match per level pass. This is a complex interaction that could expose bugs in the tranche reveal loop.

### 4.2 Auction with iceberg orders

**Status:** Tested in C++ unit test (ExecuteAuction_IcebergOrder) but NOT in any journal.

The auction algorithm uses level total_quantity which for icebergs is only the display qty. The unit test verifies this, but a journal test would provide deterministic regression coverage with exact event sequences.

### 4.3 Modify an order during PreOpen phase

**Status:** Not tested anywhere.

In PreOpen, orders are collected without matching. A modify (cancel-replace) should remove the old order and re-insert the new one -- but should NOT trigger matching. The engine code does call `match_order()` in the modify path, which might incorrectly match during collection phases.

### 4.4 Stop order triggered during auction

**Status:** Not tested anywhere.

The design spec says stop triggers are disabled during auctions. However, `execute_auction()` does update `last_trade_price_` but does NOT call `check_and_trigger_stops()`. This is likely correct, but no test verifies it. If a stop's trigger price is crossed by the auction fill, it should NOT trigger until Continuous trading begins.

### 4.5 Mass cancel during PreOpen

**Status:** Not tested anywhere.

mass_cancel and mass_cancel_all do a linear scan and call cancel_active_order for each. During PreOpen, this should work but the interaction with collected-but-unmatched orders needs verification.

### 4.6 FOK in ProRata matching

**Status:** Not tested in any journal or unit test.

FOK pre-checks `compute_matchable_qty()` before matching. With ProRata, the matchable quantity at a single level equals the level total, but the actual ProRata allocation may differ from FIFO. The FOK check should still work correctly since it only checks total available qty, not allocation details. However, no test verifies this.

### 4.7 Cancel an order that was partially filled

**Status:** Not tested in any journal. Partially covered in unit tests (IOC partial cancel cancels remainder, but no explicit "cancel a resting order that has a partial fill").

A limit order is partially filled and rests with remaining quantity. User cancels it. The cancel should fire with the remaining (not original) quantity, and MD events should reflect the correct level update.

### 4.8 Multiple stop cascades (3+ levels deep)

**Status:** Only 2-level cascade tested (stop_cascade.journal). No 3+ level cascade tested.

The iterative cascade loop in `check_and_trigger_stops()` should handle arbitrary depth, but only 2 levels are verified. A 3-level cascade would confirm the loop does not have off-by-one or state corruption issues.

### 4.9 Modify to a price that crosses the spread and triggers stops

**Status:** Not tested anywhere.

`modify_order()` calls `match_order()` and then `check_and_trigger_stops()`. If the modified order crosses the spread, fills, and the resulting trade triggers a stop, this three-step interaction (modify -> fill -> stop cascade) is untested.

### 4.10 Market order in collection phase (PreOpen/PreClose)

**Status:** Not tested in journals.

The engine code handles this: market orders in collection phases are accepted and then immediately cancelled with IOCRemainder. This edge case is in the code (matching_engine.h lines 210-222) but no journal verifies it.

### 4.11 Iceberg order with display_qty not aligned to lot_size

**Status:** Tested in C++ unit test (RejectIcebergDisplayQtyNotAlignedToLotSize) but not in journal.

### 4.12 Modify an order to new_quantity < filled_quantity

**Status:** Not tested anywhere.

If an order has been partially filled (e.g., filled 30000 of 50000), and a modify sets new_quantity to 20000 (less than already filled), the remaining_quantity would go negative. The engine should reject this, but no test verifies the behavior.

---

## 5. Recommendations

Prioritized list of journal scenarios to add, ordered by importance.

### Priority 1 -- Critical (blocking production readiness)

1. **`mass_cancel_account.journal`** -- Place 3 orders (2 from account A, 1 from account B). Mass cancel account A. Verify account A's orders are cancelled with MassCancelled reason and account B's order survives. Verify MD events (L3 Cancel, L2 Update/Remove, L1 TopOfBook).

2. **`mass_cancel_all.journal`** -- Place 5 orders across multiple levels and accounts. Mass cancel all. Verify all 5 cancelled with MassCancelled reason. Verify book is empty via TopOfBook(0,0,0,0).

3. **`session_closed_rejects.journal`** -- Start in Continuous, transition to Closed. Submit new order, cancel, and modify -- all three should be rejected with ExchangeSpecific reason.

4. **`iceberg_basic.journal`** -- Place iceberg sell (total=30000, display=10000). Aggressor buy 10000 fills first tranche. Verify: tranche reveal (remaining=10000), L3 Add for revealed tranche, L2 Update with display qty, priority loss (iceberg at back of queue).

5. **`iceberg_full_fill.journal`** -- Place iceberg sell (total=20000, display=10000). Aggressor buy 20000 consumes both tranches. Verify order removed from book, all fills and MD events correct.

6. **`iceberg_validation.journal`** -- Three orders: (a) display_qty > qty rejected, (b) display_qty not lot-aligned rejected, (c) display_qty == qty accepted. Verify all three outcomes.

7. **`smp_cancel_newest.journal`** -- Requires a custom SMP engine in the test harness. Place resting buy from account A. Submit crossing sell from account A. Verify: OrderAccepted for sell, then OrderCancelled with SelfMatchPrevention. No Trade events. Resting buy survives.

### Priority 2 -- Important (needed for comprehensive regression)

8. **`cancel_partial_fill.journal`** -- Place buy 30000. Sell 10000 partially fills it. Cancel the remainder. Verify OrderCancelled with remaining=20000, correct L3 and L2 events.

9. **`session_preopen_market_reject.journal`** -- Transition to PreOpen. Submit market order. Verify accepted then immediately cancelled with IOCRemainder (market orders cannot rest in collection phase).

10. **`modify_amend_down.journal`** -- Requires AmendDown engine. Place buy 30000 at price P. Modify to qty 20000 at same price. Verify OrderModified fires, L2 Update shows reduced qty, NO L3 Cancel+Add (priority preserved).

11. **`iceberg_priority_loss.journal`** -- Place iceberg sell (total=20000, display=10000) at price P. Place plain sell 10000 at same price P. Buy 10000 fills iceberg first tranche. Buy another 10000 fills plain order (not iceberg's second tranche). Verifies priority loss.

12. **`max_order_size_reject.journal`** -- Config with max_order_size=50000. Submit order with qty=60000. Verify rejected with MaxOrderSizeExceeded. Submit order with qty=50000. Verify accepted.

13. **`pro_rata_multi_level.journal`** -- ProRata engine with resting orders at two price levels. Aggressor sweeps through both. Verify ProRata allocation at first level, then second.

14. **`auction_iceberg.journal`** -- PreOpen: place iceberg bid and normal ask. Execute auction. Verify auction only sees display qty from iceberg side, fills correct amount.

15. **`smp_cancel_oldest.journal`** -- SMP CancelOldest engine. Same-account crossing. Verify resting order cancelled, aggressor survives and rests.

16. **`smp_cancel_both.journal`** -- SMP CancelBoth engine. Same-account crossing. Verify both orders cancelled, no trades.

17. **`modify_triggers_stop.journal`** -- Place resting sell and a buy stop. Place resting buy below ask. Modify buy price to cross spread. Fill triggers the stop. Verify full cascade.

18. **`session_preclose_collect.journal`** -- Transition to PreClose. Place crossing orders. Verify they rest without matching.

### Priority 3 -- Nice-to-Have (completeness)

19. **`ioc_empty_book.journal`** -- IOC buy on empty book. Verify accepted then cancelled with IOCRemainder.

20. **`fok_empty_book.journal`** -- FOK buy on empty book. Verify accepted then cancelled with FOKFailed.

21. **`stop_triple_cascade.journal`** -- Three sell stops at descending prices with matching buy liquidity at each. One triggering fill causes a 3-level cascade.

22. **`iceberg_vs_iceberg.journal`** -- Buy iceberg and sell iceberg at same price. Verify tranche interactions.

23. **`iceberg_cancel.journal`** -- Place iceberg, cancel it. Verify full hidden qty is reflected in cancel event.

24. **`iceberg_modify.journal`** -- Place iceberg, modify its price. Verify cancel-replace behavior.

25. **`session_full_cycle.journal`** -- Walk through: Closed -> PreOpen -> execute auction -> Continuous -> PreClose -> execute auction -> Closed. Verify MarketStatus at each transition.

26. **`smp_decrement.journal`** -- SMP Decrement. Both orders reduced by match quantity; both survive.

27. **`pro_rata_ioc.journal`** -- IOC order with ProRata. Partial fill via ProRata, remainder cancelled.

28. **`pro_rata_fok.journal`** -- FOK order with ProRata. Pre-check and fill.

29. **`session_preopen_cancel_modify.journal`** -- Cancel and modify orders during PreOpen. Verify they work correctly against collected (unmatched) orders.

30. **`orderid_exhaustion.journal`** -- Fill OrderId space. Verify PoolExhausted rejection.

### Summary Statistics

| Category | Total Features | Covered by Journal | Covered by Unit Test Only | Not Covered at All |
|----------|---------------|-------------------|--------------------------|-------------------|
| Order Types | 11 | 11 | 0 | 0 |
| TIF | 9 | 7 | 0 | 2 |
| Matching Algos | 7 | 5 | 0 | 2 |
| Order Lifecycle | 15 | 12 | 0 | 3 |
| Session States | 15 | 4 | 0 | 11 |
| Auctions | 20 | 8 | 10 | 2 |
| Iceberg | 18 | 0 | 15 | 3 |
| SMP | 6 | 1 | 1 | 4 |
| Risk Controls | 14 | 9 | 3 | 2 |
| Market Data | 7 | 7 | 0 | 0 |
| Mass Cancel | 5 | 0 | 0 | 5 |
| Stop Triggers | 8 | 4 | 0 | 4 |
| **Total** | **135** | **68** | **29** | **38** |

**Overall journal coverage: 50% of features.**
**Combined coverage (journal + unit test): 72% of features.**
**Uncovered: 28% of features (38 features with no test of any kind).**

The most critical gaps are:
1. **Mass cancel** -- entirely untested in both journals and unit tests
2. **Session state enforcement** -- only PreOpen and Continuous are tested; 6 other states are untested
3. **Iceberg orders in journals** -- all 15+ iceberg unit tests have zero journal equivalents
4. **SMP actions** -- only CancelNewest has a unit test; CancelOldest, CancelBoth, and Decrement are completely untested
