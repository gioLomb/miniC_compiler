#!/bin/bash

PERF_DATA="$PWD/perf.data"
MINICC="./bin/minicc"
EXAMPLES_DIR="examples"
FREQ=9999

# Pulizia preventiva
rm -f "$PERF_DATA" "$PERF_DATA.old" /tmp/out_*.s

echo "=== Inizio Profiling Multi-Sample ==="

# Sblocco limiti del kernel
sysctl -w kernel.perf_event_paranoid=-1 > /dev/null 2>&1
sysctl -w kernel.perf_event_max_sample_rate=50000 > /dev/null 2>&1

# Eseguiamo perf record UNA SOLA VOLTA su un ciclo che compila tutti i file 50 volte ciascuno
perf record -F $FREQ -g -o "$PERF_DATA" -- bash -c "
    for file in $EXAMPLES_DIR/*.c; do
        echo \"[-] Profiling su: \$file...\"
        for i in {1..50}; do
            $MINICC \"\$file\" -S -o /tmp/out.s > /dev/null 2>&1
        done
    done
"

echo ""
if [ -f "$PERF_DATA" ]; then
    echo "✅ PROFILING COMPLETATO CON SUCCESSO!"
    echo "File salvato in: $PERF_DATA ($(du -h "$PERF_DATA" | cut -f1))"
else
    echo "❌ ERRORE: Nessun file generato."
fi
