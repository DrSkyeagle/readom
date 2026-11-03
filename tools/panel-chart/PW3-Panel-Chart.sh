#!/bin/sh
# Name: PW3 Panel Chart
# Shows the panel test chart with the stock system's own drawing tool for one
# minute: a white field, a black field and the panel's 16 grey levels. Draws
# on the screen only; whatever the reader shows next replaces it.
PATH=/sbin:/bin:/usr/sbin:/usr/bin
export PATH
P=/mnt/us/pw3-chart/pw3-chart-stock.png
[ -f "$P" ] || exit 1
/usr/sbin/eips -c
/usr/sbin/eips -f -g "$P"
sleep 60
