VACAPI.h - Virtual Audio Cable driver API, version 6 (VAC 4.70+), by Eugene V. Muzychenko.
Source: https://software.muzychenko.net/special/vac470api6.7z (unchanged copy).
Used by src/state/VacControl.cpp to set the number of cables and restart the driver.
Redistribution: clarify with the manufacturer (see external/vac471full/license.txt).
Note: VAC 4.71 only answers API version 7 (same structures for DriverInfo / CableInfo); VacControl tries 6, then 7.
