# j1939 host suite

Covers the PURE half of the component (no FreeRTOS, no CAN driver, no clock:
time is an argument). The frames and payloads are the ones the bench truck
sends (`tools/testbench/lib/j1939_ref.py` in the firmware repo, the bench's
independent reference, itself cross-checked against a J1939 DBC), so the C
side and the bench agree byte for byte before a device is involved.

- `test_main.c` (`j1939_core.c`): the 29-bit identifier taken apart and
  built (PDU1 destination, PDU2 broadcast, the extended data page bit never
  set), frame kinds (parameter group, transport protocol, ISO 15765
  diagnostics on J1939 identifiers, foreign frames such as 11-bit and the
  29-bit identifiers of passenger cars), the "is this a J1939 bus" evidence
  (two distinct well-known groups, 8 bytes long; one group or a short frame
  is not enough; 50 frames of something else = other), the VIN message (with
  and without the `*` delimiter, padded, and everything that is not a VIN).
- `test_tp.c` (`j1939_tp_core.c`): the VIN by BAM; a dropped, a reordered
  and a missing packet; the timeout; a new announce replacing the open
  session; RTS/CTS between two OTHER nodes read as it goes by, including a
  CTS that asks for a packet again, a hold, both aborts, the acknowledge of
  a message this listener missed part of; every announce that cannot be;
  the largest message (1785 bytes, 255 packets); eight sessions side by
  side and the ninth refused; a BAM and a connection of one sender at once.
  Every test ends on the books: started = completed + seq_errors + timeouts
  + aborted + replaced + open.
- `test_values.c` (`j1939_dm_core.c`, `j1939_spn_core.c`): DM1 with none,
  one (in a frame) and three codes (reassembled), the lamps, the conversion
  method bit, short payloads; every entry of the value table decoded from
  the truck's default payloads to the value the truck was told to send;
  not available / error / parameter specific / reserved by width, the
  largest valid raw value = the table's `max`, a message too short; names
  unique and none equal to a name of autopid's OBD table.
- `test_cache.c` (`j1939_cache_core.c`): newest payload, count and period
  per (group, source, destination); the pick among several sources (lowest
  address heard in the last 5 s, else the lowest of all); PDU1 destinations
  as part of the key; long messages and their buffers (reuse, release,
  running out); a full table (not kept and counted while everything is
  recent, the longest-silent entry making room after 30 s), with every
  other key still found after the removal; the table of sources.
- `test_active.c` (`j1939_claim_core.c`, the codec in `j1939_core.c`, the
  destination role in `j1939_tp_core.c`; phase 6): the NAME built as the
  reference builds it and compared as J1939-81 does (the lower number wins,
  most significant byte first); the claim of the preferred address after
  the 250 ms wait; a contest won (defended, no second wait) and the defence
  held to one per round against a node that contests every claim; a
  contest lost (250, then the dynamic range, each with a new wait); every
  address lost (122 contests: Cannot Claim from 254, silence, the request
  answered with it); a Request for Address Claimed while claiming and once
  claimed; the Request and Acknowledgment bytes against the reference; as
  the destination of RTS/CTS: the clear-to-send for the whole window, the
  end-of-message with the message handed out, a window of one packet (one
  CTS per packet), the three aborts (bad sequence, timeout, no session
  free), no reply without an address or for a connection to somebody else
  or a BAM, the reply queue's bound counted.

Run: `.\test.ps1 host j1939` from the firmware repo (or by hand on a Linux
host: `idf.py --preview set-target linux && idf.py build && build/*.elf`).
Expected: Unity `57 Tests 0 Failures 0 Ignored`.
