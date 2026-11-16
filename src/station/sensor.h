// The only part of the station that depends on the weather sensor hardware.
#pragma once
#include "wx_frame.h"

// Called once per wake, before sensorRead().
bool sensorBegin();

// Fill in the fields the sensor measures, scaled as in wx::Reading; leave the rest at their
// "not measured" sentinels. Return false if the sensor could not be read (the frame is still
// sent, with kFlagSensorFail set, so the gateway can tell "sensor broken" from "link down").
bool sensorRead(wx::Reading& r);

// Called first thing after a deep-sleep wake. Returns true if the wake was only a sensor event
// (e.g. a rain-gauge tip that has now been counted) and the station should go straight back to
// sleep until its next scheduled report.
bool sensorHandleWake();

// Called right before deep sleep, e.g. to arm the rain gauge as a wake source.
void sensorBeforeSleep();
