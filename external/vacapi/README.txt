VACAPI.h - Virtual Audio Cable driver API, version 7 (VAC 4.71), by Eugene V. Muzychenko.
Source: VAC 4.71.0.14964 generic custom binary package (external/vac_4_71_0_14964_gnr/VACAPI.h, unchanged copy).
Previous: API 6 from https://software.muzychenko.net/special/vac470api6.7z.
Used by src/state/VacControl.cpp (number of cables, clock, timing, cable info, minimum WaveRT buffer).
Redistribution: clarify with the manufacturer (see external/vac471full/license.txt).
Note: API 7 adds CableInfo::MinRtBufFacts at the end. Older 4.71 builds answer API 7 with the shorter
CableInfo; VacControl asks with the new size first, then with the old one. It tries API 7, then 6.
