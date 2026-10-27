#ifndef CHIP_TEMPO_H
#define CHIP_TEMPO_H
#include <math.h>
// Shared tick-linear BPM integral/inverse. Host validates positive BPM/PPQ and
// segment bounds; units are seconds and ticks, rounding belongs to absolute time.
static inline double cs_tempo_seconds(double ticks,double bpm,double slope,double ppq){
    double factor=60.0/ppq;
    return fabs(slope)<1e-12?factor*ticks/bpm:factor*log1p(slope*ticks/bpm)/slope;
}
static inline double cs_tempo_ticks(double seconds,double bpm,double slope,double ppq){
    double factor=60.0/ppq;
    return fabs(slope)<1e-12?seconds*bpm/factor:bpm*expm1(seconds*slope/factor)/slope;
}
// Full ramp duration retains the original endpoint expression (log ratio).
static inline double cs_tempo_duration(double ticks,double from,double to,double ppq){
    double slope=(to-from)/ticks,factor=60.0/ppq;
    return fabs(slope)<1e-12?factor*ticks/from:factor*log(to/from)/slope;
}
#endif
