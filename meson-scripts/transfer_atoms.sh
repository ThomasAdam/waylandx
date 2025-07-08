#!/bin/sh

out="$1"; shift

> "$out"

for awk_script in mime0.awk mime1.awk mime2.awk mime3.awk mime4.awk; do
    awk -f ../meson-scripts/"$awk_script" short_types.txt >> "$out"
done

