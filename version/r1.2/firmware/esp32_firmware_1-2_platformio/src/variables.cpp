/**
 * variables.cpp - One-definition-rule home for the shared globals
 * declared in variables.h. Member-level defaults inside each struct
 * cover initial state, so this file just instantiates them.
 */

#include "variables.h"

Preferences preferences;

RadarState  radar;
SleepGate   sleep_gate;
UploadJob   upload;
NetConfig   net;
EspuiHandles ui_ids;

std::atomic<bool> service_mode{false};
