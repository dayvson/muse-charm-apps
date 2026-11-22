#!/bin/bash
# Real Yahoo chart responses for the parser tests (not committed: see .gitignore).
cd "$(dirname "$0")" && mkdir -p fixtures
for s in META SHOP SPY; do
  for r in 1d:5m 5d:15m 1mo:60m 1y:1d 5y:1wk; do
    range=${r%%:*}; iv=${r##*:}
    curl -s -m 20 -A "Mozilla/5.0" -o "fixtures/${s}_${range}.json" -w "$s $range/$iv http=%{http_code} %{size_download}B\n" \
      "https://query1.finance.yahoo.com/v8/finance/chart/$s?range=$range&interval=$iv&includePrePost=false"
    sleep 0.3
  done
done
for spot in "crandon 25.7120 -80.1535" "stiltsville 25.6529 -80.1754" "tokyo 35.68 139.69" "capetown -34.0 18.4"; do
  set -- $spot
  curl -s -m 20 -o "fixtures/wind_$1.json" -w "wind $1 http=%{http_code} %{size_download}B\n" \
    "https://api.open-meteo.com/v1/forecast?latitude=$2&longitude=$3&current=wind_speed_10m,wind_direction_10m,wind_gusts_10m&hourly=wind_speed_10m,wind_direction_10m,wind_gusts_10m&daily=wind_speed_10m_max,wind_gusts_10m_max,wind_direction_10m_dominant&wind_speed_unit=kn&timezone=auto&timeformat=unixtime&forecast_days=7&cell_selection=sea"
done
