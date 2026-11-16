#!/bin/sh
# Print everything the gateway publishes. Usage: tools/mqtt_watch.sh [broker-host]
exec mosquitto_sub -h "${1:-localhost}" -v -t 'weather/#'
