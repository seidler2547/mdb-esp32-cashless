# Firmware host tests

Two pieces of `main/` are plain C with no hardware dependency, so their logic
is testable without a board. `run.sh` compiles both with the system gcc —
against the stub IDF headers in `stub/` where they are needed — and runs
them:

```sh
./run.sh
```

## `mdb_debug.c` — bus observability

A harness feeds the module synthetic MDB traffic and checks the counters, the
verdict and the renderers. It covers the cases the module exists to tell
apart: a healthy exchange, a VMC polling the *other* cashless address, a bus
where no cashless device is polled at all, a reader that is polled but never
enabled, a bus that goes silent, framing errors and truncated command blocks,
the ACK/NAK/RET handshake counters, error-snapshot arming and its rate limit,
ring-buffer wrap-around, the last-sale record, and that neither renderer can
overrun the buffer it is handed.

## `mdb_price.h` — price conversion

The conversion between MDB scale-factor units and the whole cents the sale
payload carries is compile-time specialised on the scale factor and decimal
places, so `run.sh` builds the test once for each of the twelve pairs
menuconfig can produce and checks all 65 536 raw prices in each: the result
matches the reference formula exactly, the regression list of prices the old
`pow()`-based macros dropped a cent from round-trips cleanly, and cents
convert back to raw units without drift. Three decimal places is the one
lossy configuration — an MDB unit is finer than a cent there and the payload
carries cents — so it is checked to the half-cent instead.

Everything else in `main/` touches ESP-IDF directly and is built with
`idf.py build` as usual.
