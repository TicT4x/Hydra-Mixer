/*

API definitions

*/





// Set 4-byte alignment to allow 32-bit clients to communicate with 64-bit driver

__pragma (pack (push, 4))





namespace VirtualAudioCable {





/*
################################################################################################

Macros
================================================================================================

################################################################################################
*/





/*
Control API interface to append to a root interface path
================================================================================================

Also used as a private filter property set identifier.

Defined as ANSI string to use in __declspec (uuid ()).

*/

#define ApiInterfaceGuidStr "0d13c528-a6fc-47f1-8fee-c5dd46f1fbb1"





/*
Determine driver config from trial/free flags
================================================================================================
*/

#define GetProductConfig(Trial, Free) ((Trial)? L"trial" : ((Free)? L"lite" : L"full"))





// API version

#define ApiVersion 6





#ifndef NumChannelsMax
__if_not_exists (NumChannelsMax) {

  static UINT const NumChannelsMax = 8;

}
#endif



// Main registry key name

#define AppRegKeyName L"Software\\" SoftwareFolderName L"\\" ProductName L"\\" _US (ProductVersionMajor)



// Global driver parameter registry value names

#define ValName_NumCables L"Number of cables"

#define ValName_KMixerThreadAffinity L"Number of cables to restrict client thread affinity"
#define ValName_ClientThreadAffinity L"Number of cables to restrict KMixer thread affinity"

#if defined (Feature_UseTimerDpcs) && Feature_UseTimerDpcs

  #define ValName_SplitDpcProcessingAmongCpus L"Split DPC processing among CPUs"

#endif // Feature_UseTimerDpcs

#define ValName_MaxWorkerThreads L"Maximum worker threads"
#define ValName_WorkerThreadPriority L"Maximum worker thread priority"

#define ValName_TimerResInPercentsOfEventPeriod L"Timer resolution in percents of event period"
#define ValName_TimerPrecInPercentsOfEventPeriod L"Timer precision in percents of event period"

#define ValName_KdfAttributePolicy L"Format attribute support policy"

#define ValName_EventLogLevel L"Event log level"



// Cable parameter registry value names

#define ValName_MaxInstances L"Maximum instances"
#define ValName_MsPerInt L"Milliseconds per interrupt"

#define ValName_MinSampleRate L"Minimum sampling rate"
#define ValName_MaxSampleRate L"Maximum sampling rate"

#define ValName_MinBitsPerSample L"Minimum bits per sample"
#define ValName_MaxBitsPerSample L"Maximum bits per sample"

#define ValName_MinNumChannels L"Minimum number of channels"
#define ValName_MaxNumChannels L"Maximum number of channels"

#define ValName_StmFmtLmt L"Stream format limiting"

#define ValName_VolCtl L"Volume control"

#define ValName_SrcLineTypesMask L"Source line types mask"
#define ValName_ConnSrcLinesMask L"Connected source lines mask"

#define ValName_ConnDestLinesMask L"Connected destination lines mask"

#define ValName_ClkCorr L"Clock correction"

#define ValName_RestrictClientThreadAffinity L"Restrict client thread affinity"
#define ValName_RestrictKMixerThreadAffinity L"Restrict KMixer thread affinity"

#define ValName_UseStandardPortClsEngine L"Use standard PortCls engine"

#define ValName_EnableChannelMixing L"Enable channel mixing"

#define ValName_EnableSpeakerPin L"Enable speaker pin type"

#define ValName_DisableCableSignalLevel L"Disable cable signal level"
#define ValName_DisableStreamSignalLevel L"Disable stream signal level"

#define ValName_CapturePortType L"PortCls capture port type"
#define ValName_RenderPortType L"PortCls render port type"

#define ValName_CaptureEnablePacketMode L"Enable packet mode on capture"
#define ValName_RenderEnablePacketMode L"Enable packet mode on render"

#define ValName_ReportInstanceCountToAudioDg L"Report instance count to AudioDG"



#if 0

// Not fully supported yet

#define ValName_CaptureDeviceFriendlyName L"Capture device friendly name"
#define ValName_RenderDeviceFriendlyName L"Render device friendly name"
#define ValName_MicInLineName L"Mic in line name"
#define ValName_LineInLineName L"Line in line name"
#define ValName_SpdifInLineName L"S/PDIF in line name"
#define ValName_LineOutLineName L"Line out line name"

#define ValName_HideCaptureDevice L"Hide capture device"
#define ValName_HideRenderDevice L"Hide render device"

#endif





/*
################################################################################################

Constants
================================================================================================

################################################################################################
*/





enum IoCtlFuncs : UINT {

  IOCF_NOP = 0x800,

  IOCF_GetDriverInfo,
  IOCF_GetCableInfo,
  IOCF_SetNumCables,
  IOCF_SetVolumeControl,
  IOCF_SetMsPerInt,
  IOCF_SetDataRange,
  IOCF_SetStreamFmtLimiting,
  IOCF_SetMaxInstances,
  IOCF_SetSourceLineTypesMask,
  IOCF_SetConnectedSourceLinesMask,
  IOCF_SetClockCorrection,
  IOCF_ResetCableCounters,
  IOCF_ResetGlobalCounters,
  IOCF_SetMaxWorkerThreads,
  IOCF_SetWorkerThreadPriority,
  IOCF_SetPortClsEngineMode,
  IOCF_SetChannelMixingMode,
  IOCF_SetSpeakerPinMode,
  IOCF_GetCableStreamsInfo,
  IOCF_GetEventLogBuffer,
  IOCF_SetEventLogLevel,
  IOCF_ClearEventLog,
  IOCF_SetPortType,
  IOCF_SetDriverDataRange,
  IOCF_SetConnectedDestLinesMask,
  IOCF_SetPacketModePolicy,

};




#ifdef CTL_CODE

  #define DefineLocalIoctlCode(Func, Access) \
    static UINT const IoCtl_Local_##Func = CTL_CODE ( \
    FILE_DEVICE_UNKNOWN, \
    static_cast <UINT> (IOCF_##Func), \
    METHOD_BUFFERED, \
    Access \
  )

  DefineLocalIoctlCode (GetDriverInfo, FILE_READ_DATA);
  DefineLocalIoctlCode (GetCableInfo, FILE_READ_DATA);
  DefineLocalIoctlCode (SetNumCables, FILE_WRITE_DATA);
  DefineLocalIoctlCode (SetVolumeControl, FILE_WRITE_DATA);
  DefineLocalIoctlCode (SetMsPerInt, FILE_WRITE_DATA);
  DefineLocalIoctlCode (SetDataRange, FILE_WRITE_DATA);
  DefineLocalIoctlCode (SetStreamFmtLimiting, FILE_WRITE_DATA);
  DefineLocalIoctlCode (SetMaxInstances, FILE_WRITE_DATA);
  DefineLocalIoctlCode (SetSourceLineTypesMask, FILE_WRITE_DATA);
  DefineLocalIoctlCode (SetConnectedSourceLinesMask, FILE_WRITE_DATA);
  DefineLocalIoctlCode (SetClockCorrection, FILE_WRITE_DATA);
  DefineLocalIoctlCode (ResetCableCounters, FILE_WRITE_DATA);
  DefineLocalIoctlCode (ResetGlobalCounters, FILE_WRITE_DATA);
  DefineLocalIoctlCode (SetMaxWorkerThreads, FILE_WRITE_DATA);
  DefineLocalIoctlCode (SetWorkerThreadPriority, FILE_WRITE_DATA);
  DefineLocalIoctlCode (SetPortClsEngineMode, FILE_WRITE_DATA);
  DefineLocalIoctlCode (SetChannelMixingMode, FILE_WRITE_DATA);
  DefineLocalIoctlCode (SetSpeakerPinMode, FILE_WRITE_DATA);
  DefineLocalIoctlCode (GetCableStreamsInfo, FILE_READ_DATA);
  DefineLocalIoctlCode (GetEventLogBuffer, FILE_READ_DATA);
  DefineLocalIoctlCode (SetEventLogLevel, FILE_WRITE_DATA);
  DefineLocalIoctlCode (ClearEventLog, FILE_WRITE_DATA);
  DefineLocalIoctlCode (SetPortType, FILE_WRITE_DATA);
  DefineLocalIoctlCode (SetDriverDataRange, FILE_WRITE_DATA);
  DefineLocalIoctlCode (SetConnectedDestLinesMask, FILE_WRITE_DATA);
  DefineLocalIoctlCode (SetPacketModePolicy, FILE_WRITE_DATA);

  #undef DefineLocalIoctlCode

#endif // CTL_CODE





static UINT const ClockCorrDenom = 1000000000; // Clock correction denominator
static UINT const ClockCorrPrec = 9;   // Clock correction precision

static UINT const ClientClockDenom = 1000; // Client clock denominator
static UINT const ClientClockPrec = 3;   // Client clock precision

UINT const LineNameLengthMax = 48; // Subdevice/line name maximal length


UINT const EventDescriptorSize = 384; // Event descriptor size in event log buffer (bytes)


/*
Private property set (KSPROPSETID_VAC) property IDs
================================================================================================
*/

enum PrivatePropertyIds : UINT {

  KSPROPERTY_VAC_GetKsInterfaceDetails, // Request details of KS interface
  KSPROPERTY_VAC_GetApiInterface, // Request control API interface path
  KSPROPERTY_VAC_SetClientClockFactor, // Set client clock factor

};



// KS subdevice types

enum SubdeviceTypes : UINT {

  Subdevice_Unknown,
  Subdevice_Capture,
  Subdevice_Render,

  Subdevice_Guard

};





/*
################################################################################################

Classes
================================================================================================

################################################################################################
*/





/*
Private property set classes
================================================================================================
*/





struct __declspec (uuid (ApiInterfaceGuidStr)) PrivatePropsetId;

GUID const KSPROPSETID_VAC = __uuidof (PrivatePropsetId);





/*
KS interface details
================================================================================================
*/

struct KsInterfaceDetailsReq {

  UINT CableNumber;
  SubdeviceTypes SubdeviceType;

};





/*
Client clock factor
================================================================================================
*/

struct ClientClockFactorReq {

  UINT Numerator;               // Client clock factor

};





/*
Data range descriptor
================================================================================================
*/

struct DataRangeDesc {

  UINT MinSampleRate;           // Minimum sampling rate
  UINT MaxSampleRate;           // Maximum sampling rate
  UINT MinBitsPerSample;        // Minimum bits per sample
  UINT MaxBitsPerSample;        // Maximum bits per sample
  UINT MinNumChannels;          // Minimum number of channels
  UINT MaxNumChannels;          // Maximum number of channels

};





/*
Extended data range descriptor
================================================================================================
*/

struct DataRangeDescEx : public DataRangeDesc {

  UINT Flags;                   // Additional flags

  enum : UINT {

    F_MinSrValid = 0x1,         // Minimal sampling rate value is specified
    F_MaxSrValid = 0x2,         // Maximal sampling rate value is specified
    F_MinBpsValid = 0x4,        // Minimal bitness value is specified
    F_MaxBpsValid = 0x8,        // Maximal bitness value is specified
    F_MinNcValid = 0x10,        // Minimal number of channels value is specified
    F_MaxNcValid = 0x20,        // Maximal number of channels value is specified

    F_Validity = (
      F_MinSrValid | F_MaxSrValid
      | F_MinBpsValid | F_MaxBpsValid
      | F_MinNcValid | F_MaxNcValid
    ),

    F_All = F_Validity,

  };

};





/*
Main API classes
================================================================================================
*/





/*
Basic driver request descriptor (header)
================================================================================================
*/

struct DriverReq {

  UINT ApiVer;                  // API version

};





/*
Driver information request descriptor
================================================================================================
*/

struct DriverInfoReq : public DriverReq {

};





/*
Driver information descriptor
================================================================================================
*/

struct DriverInfo {

  UINT Flags;                   // Driver flags
  UINT VerMajor;                // Major version number
  UINT VerMinor;                // Minor version number
  UINT VerRev;                  // Revision number
  UINT VerBuild;                // Build number

  UINT NumCables;               // Number of cables

  DataRangeDesc DataRange;      // Global data range

  UINT TotalStreams;            // Total audio (wave) streams created
  UINT TotalClients;            // Total driver clients, including topology

  UINT MaxWorkerThreads;        // Maximum worker threads
  UINT WorkerThreadPriority;    // Worker thread priority

  UINT EventLogLevel;           // Event log importance level (0..9)

  UINT TimerResolution;         // Timer resolution required by the driver (mcs). 0 - not specified

  UINT KdfAttributePolicy;      // KSDATAFORMAT attribute support policy

  UINT SilentSecondsLeft;       // Seconds left before trial reminder becomes audible (for trial builds only)

};





namespace DriverFlags {

  static UINT const Lite = 0x1;  // Free driver version
  static UINT const Trial = 0x2; // Trial driver version
  static UINT const TrialReminder = 0x4; // Trial reminder is on
  static UINT const NativeModeSupported = 0x8; // Native Mode supported
  static UINT const NativeModeUnlocked = 0x10; // Native Mode unlocked (noise/silence is off)
  static UINT const RestartRequired = 0x20; // Driver restart required to apply changes
  static UINT const AudioServiceRestartRequired = 0x40; // Audio Service restart required to apply changes

  static UINT const StreamFormatLimitingControlSupported = 0x10000; // Stream format limiting change supported
  static UINT const MaxInstancesControlSupported = 0x20000; // Maximum number of instances change supported
  static UINT const SourceLineConfigControlSupported = 0x40000; // Source line configuration change supported
  static UINT const ChannelMixingModeControlSupported = 0x80000; // Channel mixing mode change supported
  static UINT const WorkerThreadsControlSupported = 0x100000; // Worker thread parameters control supported
  static UINT const DriverFormatRangeControlSupported = 0x200000; // Driver's format range control supported
  static UINT const SignalProcessingModesSupported = 0x400000; // Signal processing modes
  static UINT const ClockRegisterSupported = 0x800000; // Clock register (RT Audio)
  static UINT const PositionRegisterSupported = 0x1000000; // Position register (RT Audio)
  static UINT const NotificationEventsSupported  = 0x2000000; // Notification events (RT Audio)
  static UINT const PacketModeSupported = 0x4000000; // Packet mode (RT Audio)

  static UINT const All = 0x7FFFFFF; // All flag bits

}





/*
Set number of cables request descriptor
================================================================================================
*/

struct DriverSetNumCablesReq : public DriverReq {

  UINT NumCables;               // Number of cables to be set

};





/*
Set max worker threads request descriptor
================================================================================================
*/

struct DriverSetMaxWorkerThreadsReq : public DriverReq {

  UINT MaxWorkerThreads;        // Maximum number of worker threads

};





/*
Set worker thread priority request descriptor
================================================================================================
*/

struct DriverSetWorkerThreadPriorityReq : public DriverReq {

  UINT WorkerThreadPriority;    // Worker thread priority

};





/*
Event log buffer descriptor
================================================================================================
*/

struct DriverEventLogBuffer {

  UINT64 Address;               // Buffer address, to have equal size in 32/64
  UINT Size;                    // Buffer size

};





/*
Set event log level request descriptor
================================================================================================
*/

struct DriverSetEventLogLevelReq : public DriverReq {

  UINT Level;                   // Importance level (0..9)

};





/*
Event descriptor header in event log buffer
================================================================================================
*/

struct EventDescriptorHdr {

  UINT SeqNum;                  // Sequential number (starting from 1)

  enum DriverEvents : UINT {

    Type_Empty,                 // Empty descriptor
    Type_Text,                  // Uncategorized text message

  } Type;                       // Event type

  char Mark;
  BYTE Month, Day, Hour, Minute, Second;
  WORD Millisecond;

};





struct EventDescriptor : public EventDescriptorHdr {

  wchar_t Text [(EventDescriptorSize - sizeof (EventDescriptorHdr)) / sizeof (wchar_t)];

};





/*
Set driver data range request descriptor
================================================================================================
*/

struct DriverSetDataRangeReq : public DriverReq {

  DataRangeDescEx DataRange;      // Data range descriptor

};





namespace SourceLine {

enum Types : UINT {

  First,

  Mic = First,                  // Microphone
  Line,                         // Analog line
  Spdif,                        // S/PDIF line

  Total,                        // Number of source lines

};

}





/*
Stream format limiting modes (policies)
================================================================================================
*/

namespace StmFmtLmt {

enum Types : UINT {

  First = 0,

  DriverRange = First,
  CableRange,
  CableFormat,

  NumVals

};

}

#define StmFmtLmt_NameList L"Driver range", L"Cable range", L"Cable format"





namespace PortClsPortTypes {

enum Types : UINT {

  First = 0,

  Auto = First,                 // Select most appropriate type automatically
  WaveRT,                       // Use IPortWaveRT
  WavePci,                      // Use IPortWavePci
  WaveCyclic,                   // Use IPortWaveCyclic

  NumVals,                      // Number of type values

};

}

#define PortClsPortTypes_NameList L"Auto", L"WaveRT", L"WavePci", L"WaveCyclic"





// KSDATAFORMAT attributes processing policies

namespace KdfAttrPolicies {

enum Policies : UINT {

  Default,                      // Default mode (process)
  Process,                      // Explicit mode (process)
  Ignore,                       // Explicit mode (ignore)

  Guard

};

}

#define KdfAttrPolicies_NameList L"Default", L"Process", L"Ignore"





__if_exists (WAVEFORMATPCMEX) {

/*
Cable information descriptor
================================================================================================
*/

struct CableInfo {

  UINT Flags;                   // Cable flags

  UINT MsPerInt;                // Milliseconds per interrupt

  UINT MaxInstances;            // Maximum number of instances

  DataRangeDesc DataRange;      // Data range

  WAVEFORMATPCMEX Fmt;          // Current format

  UINT InStreams;               // Input (record, capture) streams counter
  UINT OutStreams;              // Output (playback, render) streams counter

  StmFmtLmt::Types StreamFmtLimiting; // Stream format limiting mode (policy)

  UINT SourceLineTypesMask;     // Source line types mask (topology)
  UINT ConnectedSourceLinesMask; // Connected source lines mask (topology)

  UINT ClockCorrection;         // Clock correction factor numerator (the denominator is 1E9)

  UINT ClientClockFactor;       // Client clock factor numerator (the denominator is 1000)
  UINT ClientClockProcessId;    // ID of the process connected to the client clock, if any
  wchar_t ClientClockProcessName [64]; // Process name, if any

  UINT DataOverflows;           // Number of data overflows
  UINT DataUnderflows;          // Number of data underflows

  BYTE SignalLevels [NumChannelsMax]; // Signal levels in percent

  UINT MuxSource;               // Selected source line (from SourceLine::Types)

  UINT EventPeriodMcsMin;       // Event period: minimum (mcs)
  UINT EventPeriodMcsMax;       // Event period: maximum (mcs)
  UINT EventPeriodMcsAvg;       // Event period: average (mcs)

  UINT64 FramesTransferred;     // Frames transferred

  UINT CpuUsage;                // CPU usage percent

  PortClsPortTypes::Types CapturePortType; // Capture (recording) PortCls port type
  PortClsPortTypes::Types RenderPortType; // Render (playback) PortCls port type

  PortClsPortTypes::Types ActualCapturePortType; // Actual capture port type
  PortClsPortTypes::Types ActualRenderPortType; // Actual render port type

  UINT ActualSR;                // Actual sampling rate

  UINT Problems;                // Cable problem flags
  UINT StreamProblems;          // Stream problem flags

  KdfAttrPolicies::Policies KdfAttributePolicy; // KSDATAFORMAT attribute support policy (set for this cable)
  KdfAttrPolicies::Policies KdfAttributePolicyCapture; // KSDATAFORMAT attribute support policy (actual value for capture miniport)
  KdfAttrPolicies::Policies KdfAttributePolicyRender; // KSDATAFORMAT attribute support policy (actual value for render miniport)

};

} // WAVEFORMATPCMEX





namespace CableFlags {

  static UINT const VolumeControl = 0x1; // Volume control enabled
  static UINT const PortClsEngine = 0x2; // PortCls engine enabled
  static UINT const ChannelMixing = 0x4; // Channel mixing enabled
  static UINT const SpeakerPin = 0x8;  // Speaker pin type enabled (replacement with line disabled)
  static UINT const CableSignalLevel = 0x10; // Cable signal level calculation enabled
  static UINT const StreamSignalLevel = 0x20; // Stream signal level calculation enabled
  static UINT const CapturePacketModeEnabled = 0x40; // Packet mode enabled on capture
  static UINT const RenderPacketModeEnabled = 0x80; // Packet mode enabled on render
  static UINT const ReportInstanceCountToAudioDg = 0x100; // Report act inst to AudioDG enabled

  static UINT const All = 0x1FF;       // All flag bits

}





/*
Cable problem flags
================================================================================================
*/

namespace CableProblemFlags {

  static UINT const Unspecified = 0x1; // Unspecified problems shown in the event log

}





/*
Stream problem flags
================================================================================================
*/

namespace StreamProblemFlags {

  static UINT const Unspecified = 0x1; // Unspecified problems shown in the event log

}





/*
Cable's stream descriptor
================================================================================================
*/

__if_exists (WAVEFORMATPCMEX) {
__if_exists (KSSTATE) {

struct StreamInfo {

  UINT Flags;                   // Stream flags
  BYTE CpuUsage;                // CPU usage percent
  UINT StreamId;                // Unique stream Id
  wchar_t ProcessName [64];     // Process image file name
  UINT ProcessId;               // Owning process Id
  UINT ThreadId;                // Creating thread Id
  WAVEFORMATPCMEX Fmt;          // Audio format
  KSSTATE State;                // Current state
  UINT BufferSize;              // Circular buffer size (0 - not applicable)
  UINT FramesBuffered;          // Amount of data buffered (frames)

  UINT64 LifeTime;              // Life time, in performance timer (QPC) clocks.
                                // Formerly, this field was named CreationTime and
                                // contained stream creation time in absolute (world)
                                // format. It caused time calculation errors if system
                                // time was changed after stream creation.

  UINT64 FramesTransferred;     // Frames transferred
  UINT DataOverflows;           // Number of data overflows
  UINT DataUnderflows;          // Number of data underflows
  BYTE SignalLevels [32];       // Signal levels in percent
  UINT64 RunTime;               // Time spent in RUN state, in performance timer (QPC) clocks
  UINT ActualSR;                // Actual sampling rate

  UINT Problems;                // Problem flags

};

} // KSSTATE
} // WAVEFORMATPCMEX





namespace StreamFlags {

  static UINT const Capture = 1;       // Capture stream
  static UINT const Looped = 2;        // Looped buffer
  static UINT const Notifications = 4; // Notifications are used
  static UINT const PacketMode = 8;    // Packet mode is used (RT Audio)
  static UINT const ClockRegister = 0x10; // Clock register requested (RT Audio)
  static UINT const PositionRegister = 0x20; // Position register requested (RT Audio)
  static UINT const FlowCountersValid = 0x40; // Overflow/underflow counters are valid

  static UINT const All = 0x7F;        // All flag bits

}





/*
Basic cable request descriptor (header)
================================================================================================
*/

struct CableReq : public DriverReq {

  UINT CableNum;                // Cable number (0..N-1)

};





/*
Set volume control status request descriptor
================================================================================================
*/

struct CableSetVolumeControlReq : public CableReq {

  bool Enabled;                 // Volume control status to be set

};





/*
Set number of milliseconds per interrupt request descriptor
================================================================================================
*/

struct CableSetMsPerIntReq : public CableReq {

  UINT MsPerInt;                // Milliseconds per interrupt to be set

};





/*
Set data range request descriptor
================================================================================================
*/

struct CableSetDataRangeReq : public CableReq {

  DataRangeDesc DataRange;      // Data range descriptor

};





/*
Extended set data range request descriptor
================================================================================================
*/

struct CableSetDataRangeExReq : public CableReq {

  DataRangeDescEx DataRange;    // Data range descriptor and flags

};





/*
Set maximum number of instances request descriptor
================================================================================================
*/

struct CableSetMaxInstancesReq : public CableReq {

  UINT MaxInstances;            // Maximum number of instances to be set

};





/*
Set stream format limiting mode request descriptor
================================================================================================
*/

struct CableSetStreamFmtLimitingReq : public CableReq {

  StmFmtLmt::Types Limiting;    // Stream format limiting mode to be set

};





/*
Set source line types mask request descriptor
================================================================================================
*/

struct CableSetSourceLineTypesMaskReq : public CableReq {

  UINT SourceLineTypesMask;     // Source line types mask to be set

};





/*
Set source line types mask request extended descriptor
================================================================================================
*/

struct CableSetSourceLineTypesMaskExReq : public CableSetSourceLineTypesMaskReq {

  UINT Flags;                   // Additional flags
                                // Validity bits are numbered as SourceLine::Types

};





/*
Set connected source lines mask request descriptor
================================================================================================
*/

struct CableSetConnectedSourceLinesMaskReq : public CableReq {

  UINT ConnectedSourceLinesMask; // Connected source lines mask to be set

};





/*
Set connected source lines mask request extended descriptor
================================================================================================
*/

struct CableSetConnectedSourceLinesMaskExReq : public CableSetConnectedSourceLinesMaskReq {

  UINT Flags;                   // Additional flags
                                // Validity bits are numbered as SourceLine::Types

};





/*
Set connected destination lines mask request descriptor
================================================================================================
*/

struct CableSetConnectedDestLinesMaskReq : public CableReq {

  UINT ConnectedDestLinesMask; // Connected destination lines mask to be set

};





/*
Set clock correction request descriptor
================================================================================================
*/

struct CableSetClockCorrectionReq : public CableReq {

  UINT Multiplier;              // Multiplier in 1/65536th

};





/*
Set PortCls engine mode request descriptor
================================================================================================
*/

struct CableSetPortClsEngineModeReq : public CableReq {

  bool Enabled;                 // PortCls engine enabled

};





/*
Set channel mixing mode request descriptor
================================================================================================
*/

struct CableSetChannelMixingModeReq : public CableReq {

  bool Enabled;                 // Channel mixing enabled

};





/*
Set Speaker pin type mode request descriptor
================================================================================================
*/

struct CableSetSpeakerPinModeReq : public CableReq {

  bool Enabled;                 // Speaker pin enabled (replacement with line disabled)

};





/*
Set PortCls port type
================================================================================================
*/

struct CableSetPortTypeReq : public CableReq {

  bool Capture;                 // False for render/playback side
  PortClsPortTypes::Types Type; // Port type

};





/*
Set packet mode policy request descriptor
================================================================================================
*/

struct CableSetPacketModePolicyReq : public CableReq {

  bool Capture;                 // Capture/render side selector
  bool Enabled;                 // Policy to be set

};





} // namespace




__pragma (pack (pop))
