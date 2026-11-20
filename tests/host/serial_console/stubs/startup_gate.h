#pragma once
typedef enum { STARTUP_GATE_OFFLINE, STARTUP_GATE_RUNNING } startup_gate_phase_t;
startup_gate_phase_t startup_gate_phase(void);
