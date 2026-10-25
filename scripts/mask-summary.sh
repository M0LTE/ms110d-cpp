#!/bin/sh
# Turns the CSV rows the mask tests append to MS110D_MASK_LOG (one file per precision, or one file with both) into a
# markdown table: the C# result and the C++ result in double and single precision for each point.
# Usage: scripts/mask-summary.sh log.csv [log.csv ...]
# Row: precision,id,channel,wn,snr_db,bursts,bits,cs_errors,cpp_errors,cs_acq_failures,cpp_acq_failures,cs_met,cpp_met
cat "$@" | sort -t, -k2,2 -k1,1 | awk -F, '
function ber(e, b) { return b > 0 ? sprintf("%.2e", e / b) : "-" }
function verdict(m) { return m ? "met" : "not shown" }
{
  id = $2
  if (!(id in seen)) { seen[id] = 1; order[++n] = id }
  chan[id] = $3; wn[id] = $4; snr[id] = $5; bursts[id] = $6; bits[id] = $7
  cs[id] = $8; csmet[id] = $12
  e[id, $1] = $9; met[id, $1] = $13; fail[id, $1] = $11
}
END {
  print "| Point | Channel | WN | SNR (dB) | Bits | C# errors (BER) | C++ double | C++ single | Mask (C# / C++ double / C++ single) |"
  print "| --- | --- | --- | --- | --- | --- | --- | --- | --- |"
  for (i = 1; i <= n; i++) {
    id = order[i]
    d = (((id, "double") in e) ? e[id, "double"] " (" ber(e[id, "double"], bits[id]) ")" : "-")
    s = (((id, "single") in e) ? e[id, "single"] " (" ber(e[id, "single"], bits[id]) ")" : "-")
    printf "| %s | %s | %s | %s | %d | %d (%s) | %s | %s | %s / %s / %s |\n", id, chan[id], wn[id], snr[id], bits[id], cs[id], ber(cs[id], bits[id]), d, s, verdict(csmet[id]), ((id, "double") in met ? verdict(met[id, "double"]) : "-"), ((id, "single") in met ? verdict(met[id, "single"]) : "-")
  }
}'
