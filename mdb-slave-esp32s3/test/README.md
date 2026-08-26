# Firmware host tests

`mdb_debug.c` — the MDB bus observability module — is plain C over a byte
stream, so its logic is testable without a board. `run.sh` compiles it with
the system gcc against the stub IDF headers in `stub/` and runs a harness
that feeds it synthetic MDB traffic:

```sh
./run.sh
```

The harness covers the cases the module exists to tell apart: a healthy
exchange, a VMC polling the *other* cashless address, a bus where no
cashless device is polled at all, a bus that goes silent, framing errors and
truncated command blocks, the ACK/NAK/RET handshake counters, error-snapshot
arming and its rate limit, ring-buffer wrap-around, and that neither renderer
can overrun the buffer it is handed.

Everything else in `main/` touches ESP-IDF directly and is built with
`idf.py build` as usual.
