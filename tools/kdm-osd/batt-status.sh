#!/usr/bin/env bash
# batt-status.sh - MAX17055ETB+T Battery Telemetry & EZ-Config Utility
#
# Hardware Configuration:
#   - Battery Pack   : Nintendo Switch Lite HDH-003 (3.8V nom / 4.35V max, 3570mAh, 13.6Wh)
#   - Sense Resistor : YAGEO PS0306DRF7T0R01L (0.01 Ohm / 10 mOhm ±0.5% 0.5W 0306)
#   - Fuel Gauge IC  : Maxim MAX17055ETB+T on /dev/i2c-1 @ 0x36 (GPIO2=SDA1, GPIO3=SCL1)

set -euo pipefail

I2C_BUS="${I2C_BUS:-1}"
I2C_ADDR="0x36"
I2CGET="/usr/sbin/i2cget"
I2CSET="/usr/sbin/i2cset"

# Check if sudo is needed to access /dev/i2c-${I2C_BUS}
SUDO=""
if [ ! -r "/dev/i2c-${I2C_BUS}" ] || [ ! -w "/dev/i2c-${I2C_BUS}" ]; then
    SUDO="sudo"
fi

read_reg16() {
    local reg="$1"
    $SUDO "$I2CGET" -y "$I2C_BUS" "$I2C_ADDR" "$reg" w 2>/dev/null
}

write_reg16() {
    local reg="$1"
    local val="$2"
    $SUDO "$I2CSET" -y "$I2C_BUS" "$I2C_ADDR" "$reg" "$val" w >/dev/null
}

init_ez_config_if_needed() {
    local status_hex dcap_hex status dcap
    status_hex=$(read_reg16 0x00) || {
        echo "Error: MAX17055ETB+T not responding on /dev/i2c-${I2C_BUS} @ ${I2C_ADDR}" >&2
        exit 1
    }
    dcap_hex=$(read_reg16 0x18)
    status=$((status_hex))
    dcap=$((dcap_hex))

    # 3570 mAh / 0.5 mAh (Rsense = 0.01 Ohm) = 7140 LSB (0x1BE4)
    if (( (status & 0x0002) != 0 || dcap != 7140 )); then
        local hibcfg
        hibcfg=$(read_reg16 0xba)
        # Wakeup from Hibernate per Maxim ModelGauge m5 EZ Guide
        write_reg16 0x60 0x0090
        write_reg16 0xba 0x0000
        write_reg16 0x60 0x0000

        # Program HDH-003 (3570mAh, 3.8V/4.35V) with 10mOhm sense resistor
        write_reg16 0x18 0x1be4  # DesignCap = 7140 (3570.0 mAh)
        write_reg16 0x45 0x00df  # dQAcc     = 223
        write_reg16 0x1e 0x02da  # IChgTerm  = 730 (~114 mA)
        write_reg16 0x3a 0xa561  # VEmpty    = 3.30V empty / 3.88V recovery
        write_reg16 0x46 0x063f  # dPAcc     = 1599 (VChg > 4.275V scale)
        write_reg16 0xdb 0x8400  # ModelCfg  = Refresh(1) | VChg(1 for 4.35V cell)

        for _ in {1..50}; do
            local mcfg
            mcfg=$(( $(read_reg16 0xdb) ))
            if (( (mcfg & 0x8000) == 0 )); then
                break
            fi
            sleep 0.01
        done

        write_reg16 0xba "$hibcfg"
        status=$(( $(read_reg16 0x00) ))
        write_reg16 0x00 "$(printf '0x%04x' $(( status & ~0x0002 )))"
        sleep 0.15
    fi
}

print_status() {
    init_ez_config_if_needed

    local dev_h status_h vcell_h avgv_h curr_h avgc_h soc_h repcap_h fullcap_h dcap_h tte_h ttf_h temp_h cyc_h cpu_mc
    dev_h=$(read_reg16 0x21)
    status_h=$(read_reg16 0x00)
    vcell_h=$(read_reg16 0x09)
    avgv_h=$(read_reg16 0x19)
    curr_h=$(read_reg16 0x0a)
    avgc_h=$(read_reg16 0x0b)
    soc_h=$(read_reg16 0x06)
    repcap_h=$(read_reg16 0x05)
    fullcap_h=$(read_reg16 0x10)
    dcap_h=$(read_reg16 0x18)
    tte_h=$(read_reg16 0x11)
    ttf_h=$(read_reg16 0x20)
    temp_h=$(read_reg16 0x08)
    cyc_h=$(read_reg16 0x17)
    cpu_mc=$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null || echo 0)

    awk -v dev="$((dev_h))" \
        -v status="$((status_h))" \
        -v vcell="$((vcell_h))" \
        -v avgv="$((avgv_h))" \
        -v curr="$((curr_h))" \
        -v avgc="$((avgc_h))" \
        -v soc="$((soc_h))" \
        -v repcap="$((repcap_h))" \
        -v fullcap="$((fullcap_h))" \
        -v dcap="$((dcap_h))" \
        -v tte="$((tte_h))" \
        -v ttf="$((ttf_h))" \
        -v temp="$((temp_h))" \
        -v cyc="$((cyc_h))" \
        -v cpu_mc="$((cpu_mc))" \
    'BEGIN {
        if (curr >= 32768) curr -= 65536;
        if (avgc >= 32768) avgc -= 65536;
        if (temp >= 32768) temp -= 65536;

        # MAX17055 scaling with Rsense = 0.01 Ohm (10 mOhm):
        #   Voltage  LSB = 0.078125 mV (1.25mV / 16)
        #   Current  LSB = 0.15625 mA  (1.5625uV / 0.01 Ohm)
        #   Capacity LSB = 0.5 mAh     (5.0uVh / 0.01 Ohm)
        #   SOC      LSB = 1 / 256 %
        #   Time     LSB = 5.625 s
        #   Temp     LSB = 1 / 256 C
        v_mv     = vcell * 0.078125;
        avgv_mv  = avgv  * 0.078125;
        i_ma     = curr  * 0.15625;
        avgi_ma  = avgc  * 0.15625;
        pwr_w    = (v_mv * i_ma) / 1000000.0;
        soc_pct  = soc / 256.0;
        soc_int  = int(soc_pct + 0.5);
        if (soc_int > 100) soc_int = 100;
        if (soc_int < 0)   soc_int = 0;

        rcap_mah = repcap  * 0.5;
        fcap_mah = fullcap * 0.5;
        dcap_mah = dcap    * 0.5;
        rcap_wh  = (rcap_mah * 3.8) / 1000.0;
        dcap_wh  = (dcap_mah * 3.8) / 1000.0;
        temp_c   = temp / 256.0;
        cpu_c    = cpu_mc / 1000.0;
        cycles   = cyc * 0.01;

        if (i_ma > 20.0 || avgi_ma > 20.0) {
            state = (soc_int >= 99) ? "Full (Charging)" : "Charging";
        } else if (i_ma < -20.0 || avgi_ma < -20.0) {
            state = "Discharging";
        } else {
            state = (soc_int >= 99) ? "Full" : "Idle";
        }

        if (tte == 65535) {
            tte_str = "N/A";
        } else {
            tte_min = int((tte * 5.625) / 60.0 + 0.5);
            tte_str = sprintf("%dh %02dm", int(tte_min / 60), tte_min % 60);
        }

        if (ttf == 65535) {
            ttf_str = "N/A";
        } else {
            ttf_min = int((ttf * 5.625) / 60.0 + 0.5);
            ttf_str = sprintf("%dh %02dm", int(ttf_min / 60), ttf_min % 60);
        }

        printf "============================================================\033[K\n";
        printf "  KDM Battery Status (MAX17055ETB+T @ /dev/i2c-1:0x36)\033[K\n";
        printf "============================================================\033[K\n";
        printf "  Battery Model  : Nintendo Switch Lite HDH-003 (3.8V, 3570mAh, 13.6Wh)\033[K\n";
        printf "  Sense Resistor : YAGEO PS0306DRF7T0R01L (0.01 Ohm / 10 mOhm)\033[K\n";
        printf "  Chip DevName   : 0x%04X (Status: 0x%04X)\033[K\n", dev, status;
        printf "------------------------------------------------------------\033[K\n";
        printf "  State          : %s\033[K\n", state;
        printf "  SOC (RepSOC)   : %.2f%% (OSD: %d%%)\033[K\n", soc_pct, soc_int;
        printf "  Voltage        : %.3f V (%d mV)  [Avg: %.3f V (%d mV)]\033[K\n",
               v_mv / 1000.0, int(v_mv + 0.5), avgv_mv / 1000.0, int(avgv_mv + 0.5);
        printf "  Current        : %+.1f mA         [Avg: %+.1f mA]\033[K\n", i_ma, avgi_ma;
        printf "  Power          : %+.2f W\033[K\n", pwr_w;
        printf "  Capacity       : %.1f / %.1f mAh (Design: %.1f mAh | %.2f / %.2f Wh)\033[K\n",
               rcap_mah, fcap_mah, dcap_mah, rcap_wh, dcap_wh;
        printf "  Time to Empty  : %s\033[K\n", tte_str;
        printf "  Time to Full   : %s\033[K\n", ttf_str;
        printf "  MAX17055 Temp  : %.1f C (IC Die)\033[K\n", temp_c;
        printf "  CPU Temp       : %.1f C (BCM2711)\033[K\n", cpu_c;
        printf "  Charge Cycles  : %.2f\033[K\n", cycles;
        printf "============================================================\033[K\n";
    }'
}

if [[ "${1:-}" == "-w" || "${1:-}" == "--watch" ]]; then
    trap 'printf "\033[?25h\n"; exit 0' INT TERM EXIT
    printf '\033[?25l\033[2J\033[H'
    while true; do
        out="$(print_status)"
        printf '\033[H%s\033[J\n' "$out"
        sleep 1
    done
else
    print_status
fi
