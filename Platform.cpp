/****************************************************************************************************

 RepRapFirmware - Platform: RepRapPro Ormerod with Arduino Due controller

 Platform contains all the code and definitions to deal with machine-dependent things such as control
 pins, bed area, number of extruders, tolerable accelerations and speeds and so on.

 -----------------------------------------------------------------------------------------------------

 Version 0.1

 18 November 2012

 Adrian Bowyer
 RepRap Professional Ltd
 http://reprappro.com

 Licence: GPL

 ****************************************************************************************************/

#include "RepRapFirmware.h"
#include "DueFlashStorage.h"

extern char _end;
extern "C" char *sbrk(int i);

const uint8_t memPattern = 0xA5;

static uint32_t fanInterruptCount = 0;				// accessed only in ISR, so no need to declare it volatile
const uint32_t fanMaxInterruptCount = 32;			// number of fan interrupts that we average over
static volatile uint32_t fanLastResetTime = 0;		// time (microseconds) at which we last reset the interrupt count, accessed inside and outside ISR
static volatile uint32_t fanInterval = 0;			// written by ISR, read outside the ISR

// Arduino initialise and loop functions
// Put nothing in these other than calls to the RepRap equivalents

void setup()
{
	// Fill the free memory with a pattern so that we can check for stack usage and memory corruption
	char* heapend = sbrk(0);
	register const char * stack_ptr asm ("sp");
	while (heapend + 16 < stack_ptr)
	{
		*heapend++ = memPattern;
	}

	reprap.Init();
}

void loop()
{
	reprap.Spin();
}

extern "C"
{
	// This intercepts the 1ms system tick. It must return 'false', otherwise the Arduino core tick handler will be bypassed.
	int sysTickHook()
	{
		reprap.Tick();
		return 0;
	}
}

//*************************************************************************************************
// PidParameters class

bool PidParameters::UsePID() const
{
	return kP >= 0;
}

float PidParameters::GetThermistorR25() const
{
	return thermistorInfR * exp(thermistorBeta / (25.0 - ABS_ZERO));
}

void PidParameters::SetThermistorR25AndBeta(float r25, float beta)
{
	thermistorInfR = r25 * exp(-beta / (25.0 - ABS_ZERO));
	thermistorBeta = beta;
}

bool PidParameters::operator==(const PidParameters& other) const
{
	return kI == other.kI && kD == other.kD && kP == other.kP && kT == other.kT && kS == other.kS
				&& fullBand == other.fullBand && pidMin == other.pidMin
				&& pidMax == other.pidMax && thermistorBeta == other.thermistorBeta && thermistorInfR == other.thermistorInfR
				&& thermistorSeriesR == other.thermistorSeriesR && adcLowOffset == other.adcLowOffset
				&& adcHighOffset == other.adcHighOffset;
}

//*******************************************************************************************************************
// Platform class

Platform::Platform() :
		tickState(0), fileStructureInitialised(false), active(false), errorCodeBits(0), debugCode(0),
		autoSaveEnabled(false), auxOutputBuffer(nullptr), usbOutputBuffer(nullptr)
{
	// Files

	massStorage = new MassStorage(this);

	for(size_t i = 0; i < MAX_FILES; i++)
	{
		files[i] = new FileStore(this);
	}
}

//*******************************************************************************************************************

void Platform::Init()
{
	digitalWrite(atxPowerPin, LOW);		// ensure ATX power is off by default
	pinMode(atxPowerPin, OUTPUT);

	idleCurrentFactor = DEFAULT_IDLE_CURRENT_FACTOR;

	baudRates[0] = USB_BAUD_RATE;
	baudRates[1] = AUX_BAUD_RATE;
	commsParams[0] = 0;
	commsParams[1] = 1;							// by default we require a checksum on data from the aux port, to guard against overrun errors

	SerialUSB.begin(baudRates[0]);
	Serial.begin(baudRates[1]);					// this can't be done in the constructor because the Arduino port initialisation isn't complete at that point

	static_assert(sizeof(FlashData) + sizeof(SoftwareResetData) <= 1024, "NVData too large");

	ResetNvData();

	addToTime = 0.0;
	lastTimeCall = 0;
	lastTime = Time();
	longWait = lastTime;

	massStorage->Init();

	for(size_t file = 0; file < MAX_FILES; file++)
	{
		files[file]->Init();
	}

	fileStructureInitialised = true;

	mcpDuet.begin(); //only call begin once in the entire execution, this begins the I2C comms on that channel for all objects
	mcpExpansion.setMCP4461Address(0x2E); //not required for mcpDuet, as this uses the default address
	sysDir = SYS_DIR;
	macroDir = MACRO_DIR;
	configFile = CONFIG_FILE;
	defaultFile = DEFAULT_FILE;

	// DRIVES

	ARRAY_INIT(stepPins, STEP_PINS);
	ARRAY_INIT(directionPins, DIRECTION_PINS);
	ARRAY_INIT(directions, DIRECTIONS);
	ARRAY_INIT(enablePins, ENABLE_PINS);
	ARRAY_INIT(lowStopPins, LOW_STOP_PINS);
	ARRAY_INIT(highStopPins, HIGH_STOP_PINS);
	ARRAY_INIT(maxFeedrates, MAX_FEEDRATES);
	ARRAY_INIT(accelerations, ACCELERATIONS);
	ARRAY_INIT(driveStepsPerUnit, DRIVE_STEPS_PER_UNIT);
	ARRAY_INIT(instantDvs, INSTANT_DVS);
	ARRAY_INIT(potWipes, POT_WIPES);

	senseResistor = SENSE_RESISTOR;
	maxStepperDigipotVoltage = MAX_STEPPER_DIGIPOT_VOLTAGE;

	// Z PROBE

	zProbePin = Z_PROBE_PIN;
	zProbeModulationPin = Z_PROBE_MOD_PIN;
	zProbeAdcChannel = PinToAdcChannel(zProbePin);
	InitZProbe();

	// AXES

	ARRAY_INIT(axisMaxima, AXIS_MAXIMA);
	ARRAY_INIT(axisMinima, AXIS_MINIMA);
	ARRAY_INIT(homeFeedrates, HOME_FEEDRATES);

	SetSlowestDrive();

	// HEATERS - Bed is assumed to be the first

	ARRAY_INIT(tempSensePins, TEMP_SENSE_PINS);
	ARRAY_INIT(heatOnPins, HEAT_ON_PINS);
	ARRAY_INIT(standbyTemperatures, STANDBY_TEMPERATURES);
	ARRAY_INIT(activeTemperatures, ACTIVE_TEMPERATURES);

	heatSampleTime = HEAT_SAMPLE_TIME;
	coolingFanValue = 0.0;
	coolingFanPin = COOLING_FAN_PIN;
	coolingFanRpmPin = COOLING_FAN_RPM_PIN;
	timeToHot = TIME_TO_HOT;
	lastRpmResetTime = 0.0;

	webDir = WEB_DIR;
	gcodeDir = GCODE_DIR;

	for(size_t drive = 0; drive < DRIVES; drive++)
	{
		if (stepPins[drive] >= 0)
		{
			pinMode(stepPins[drive], OUTPUT);
		}
		if (directionPins[drive] >= 0)
		{
			pinMode(directionPins[drive], OUTPUT);
		}
		if (enablePins[drive] >= 0)
		{
			pinMode(enablePins[drive], OUTPUT);
		}
		if (lowStopPins[drive] >= 0)
		{
			pinMode(lowStopPins[drive], INPUT_PULLUP);
		}
		if (highStopPins[drive] >= 0)
		{
			pinMode(highStopPins[drive], INPUT_PULLUP);
		}
		motorCurrents[drive] = 0.0;
		DisableDrive(drive);
		driveState[drive] = DriveStatus::disabled;
	}

	extrusionAncilliaryPWM = 0.0;

	for(size_t heater = 0; heater < HEATERS; heater++)
	{
		if (heatOnPins[heater] >= 0)
		{
			digitalWrite(heatOnPins[heater], HIGH);	// turn the heater off
			pinMode(heatOnPins[heater], OUTPUT);
		}
		analogReadResolution(12);
		thermistorFilters[heater].Init(analogRead(tempSensePins[heater]));
		heaterAdcChannels[heater] = PinToAdcChannel(tempSensePins[heater]);

		// Calculate and store the ADC average sum that corresponds to an overheat condition, so that we can check is quickly in the tick ISR
		float thermistorOverheatResistance = nvData.pidParams[heater].GetRInf()
				* exp(-nvData.pidParams[heater].GetBeta() / (BAD_HIGH_TEMPERATURE - ABS_ZERO));
		float thermistorOverheatAdcValue = (AD_RANGE_REAL + 1) * thermistorOverheatResistance
				/ (thermistorOverheatResistance + nvData.pidParams[heater].thermistorSeriesR);
		thermistorOverheatSums[heater] = (uint32_t) (thermistorOverheatAdcValue + 0.9) * THERMISTOR_AVERAGE_READINGS;
	}

	if (coolingFanPin >= 0)
	{
		// Inverse logic for Duet v0.6 and later; this turns it off
		analogWriteDuet(coolingFanPin, (HEAT_ON == 0) ? 255 : 0, true);
	}
	if (coolingFanRpmPin >= 0)
	{
		pinModeDuet(coolingFanRpmPin, INPUT_PULLUP, 1500);
	}

	// Hotend configuration
	nozzleDiameter = NOZZLE_DIAMETER;
	filamentWidth = FILAMENT_WIDTH;

	// Inkjet

	inkjetBits = INKJET_BITS;
	if(inkjetBits >= 0)
	{
		inkjetFireMicroseconds = INKJET_FIRE_MICROSECONDS;
		inkjetDelayMicroseconds = INKJET_DELAY_MICROSECONDS;

		inkjetSerialOut = INKJET_SERIAL_OUT;
		pinMode(inkjetSerialOut, OUTPUT);
		digitalWrite(inkjetSerialOut, 0);

		inkjetShiftClock = INKJET_SHIFT_CLOCK;
		pinMode(inkjetShiftClock, OUTPUT);
		digitalWrite(inkjetShiftClock, LOW);

		inkjetStorageClock = INKJET_STORAGE_CLOCK;
		pinMode(inkjetStorageClock, OUTPUT);
		digitalWrite(inkjetStorageClock, LOW);

		inkjetOutputEnable = INKJET_OUTPUT_ENABLE;
		pinMode(inkjetOutputEnable, OUTPUT);
		digitalWrite(inkjetOutputEnable, HIGH);

		inkjetClear = INKJET_CLEAR;
		pinMode(inkjetClear, OUTPUT);
		digitalWrite(inkjetClear, HIGH);
	}

	// Get the show on the road...

	InitialiseInterrupts();

	lastTime = Time();
	longWait = lastTime;
}

// Specify which thermistor channel a particular heater uses
void Platform::SetThermistorNumber(size_t heater, size_t thermistor)
{
	if (heater < HEATERS && thermistor < ARRAY_SIZE(tempSensePins))
	{
		heaterAdcChannels[heater] = PinToAdcChannel(tempSensePins[thermistor]);
	}
}

int Platform::GetThermistorNumber(size_t heater) const
{
	if (heater < HEATERS)
	{
		for(size_t thermistor = 0; thermistor < HEATERS; ++thermistor)
		{
			if (heaterAdcChannels[heater] == PinToAdcChannel(tempSensePins[thermistor]))
			{
				return thermistor;
			}
		}
	}
	return -1;
}

void Platform::SetSlowestDrive()
{
	slowestDrive = 0;
	for(size_t drive = 1; drive < DRIVES; drive++)
	{
		if(InstantDv(drive) < InstantDv(slowestDrive))
		{
			slowestDrive = drive;
		}
	}
}

void Platform::InitZProbe()
{
	zProbeOnFilter.Init(0);
	zProbeOffFilter.Init(0);

	if (nvData.zProbeType >= 1)
	{
		zProbeModulationPin = (nvData.zProbeChannel == 1) ? Z_PROBE_MOD_PIN07 : Z_PROBE_MOD_PIN;
		pinMode(zProbeModulationPin, OUTPUT);
		digitalWrite(zProbeModulationPin, (nvData.zProbeType <= 2) ? HIGH : LOW);	// enable the IR LED or alternate sensor
	}
}

int Platform::GetRawZHeight() const
{
	return (nvData.zProbeType != 0) ? analogRead(zProbePin) : 0;
}

// Return the Z probe data.
// The ADC readings are 12 bits, so we convert them to 10-bit readings for compatibility with the old firmware.
int Platform::ZProbe() const
{
	if (zProbeOnFilter.IsValid() && zProbeOffFilter.IsValid())
	{
		switch (nvData.zProbeType)
		{
			case 1:
			case 3:
				// Simple IR sensor, or direct-mode ultrasonic sensor
				return (int) ((zProbeOnFilter.GetSum() + zProbeOffFilter.GetSum()) / (8 * Z_PROBE_AVERAGE_READINGS));

			case 2:
				// Modulated IR sensor. We assume that zProbeOnFilter and zprobeOffFilter average the same number of readings.
				// Because of noise, it is possible to get a negative reading, so allow for this.
				return (int) (((int32_t) zProbeOnFilter.GetSum() - (int32_t) zProbeOffFilter.GetSum())
						/ (int)(4 * Z_PROBE_AVERAGE_READINGS));

			default:
				break;
		}
	}
	return 0;	// Z probe not turned on or not initialised yet
}

// Provide the Z probe secondary values and return the number of secondary values
int Platform::GetZProbeSecondaryValues(int& v1, int& v2)
{
	if (zProbeOnFilter.IsValid() && zProbeOffFilter.IsValid())
	{
		switch (nvData.zProbeType)
		{
			case 2:		// modulated IR sensor
				v1 = (int) (zProbeOnFilter.GetSum() / (4 * Z_PROBE_AVERAGE_READINGS));	// pass back the reading with IR turned on
				return 1;

			default:
				break;
		}
	}
	return 0;
}

int Platform::GetZProbeType() const
{
	return nvData.zProbeType;
}

int Platform::GetZProbeChannel() const
{
	return nvData.zProbeChannel;
}

void Platform::SetZProbeAxes(const bool axes[AXES])
{
	for(int axis=0; axis<AXES; axis++)
	{
		nvData.zProbeAxes[axis] = axes[axis];
	}

	if (autoSaveEnabled)
	{
		WriteNvData();
	}
}

void Platform::GetZProbeAxes(bool (&axes)[AXES])
{
	for(int axis=0; axis<AXES; axis++)
	{
		axes[axis] = nvData.zProbeAxes[axis];
	}
}

float Platform::ZProbeStopHeight() const
{
	switch (nvData.zProbeType)
	{
		case 0:
			return nvData.switchZProbeParameters.GetStopHeight(GetTemperature(0));
		case 1:
		case 2:
			return nvData.irZProbeParameters.GetStopHeight(GetTemperature(0));
		case 3:
			return nvData.alternateZProbeParameters.GetStopHeight(GetTemperature(0));
		default:
			return 0;
	}
}

float Platform::GetZProbeDiveHeight() const
{
	switch (nvData.zProbeType)
	{
		case 0:
			return nvData.switchZProbeParameters.diveHeight;
		case 1:
		case 2:
			return nvData.irZProbeParameters.diveHeight;
		case 3:
			return nvData.alternateZProbeParameters.diveHeight;
	}
	return DEFAULT_Z_DIVE;
}

void Platform::SetZProbeDiveHeight(float height)
{
	switch (nvData.zProbeType)
	{
		case 0:
			nvData.switchZProbeParameters.diveHeight = height;
			break;
		case 1:
		case 2:
			nvData.irZProbeParameters.diveHeight = height;
			break;
		case 3:
			nvData.alternateZProbeParameters.diveHeight = height;
			break;
	}
}

void Platform::SetZProbeType(int pt)
{
	int newZProbeType = (pt >= 0 && pt <= 3) ? pt : 0;
	if (newZProbeType != nvData.zProbeType)
	{
		nvData.zProbeType = newZProbeType;
		if (autoSaveEnabled)
		{
			WriteNvData();
		}
	}
	InitZProbe();
}

void Platform::SetZProbeChannel(int channel)
{
	switch (channel)
	{
		case 1:
			zProbeModulationPin = Z_PROBE_MOD_PIN07;
			break;

		default:
			zProbeModulationPin = Z_PROBE_MOD_PIN;
			channel = 0;
			break;
	}

	if (channel != nvData.zProbeChannel)
	{
		nvData.zProbeChannel = channel;
		if (autoSaveEnabled)
		{
			WriteNvData();
		}
	}
}

const ZProbeParameters& Platform::GetZProbeParameters() const
{
	switch (nvData.zProbeType)
	{
		case 0:
		default:
			return nvData.switchZProbeParameters;

		case 1:
		case 2:
			return nvData.irZProbeParameters;

		case 3:
			return nvData.alternateZProbeParameters;
	}
}

bool Platform::SetZProbeParameters(const struct ZProbeParameters& params)
{
	switch (nvData.zProbeType)
	{
		case 0:
			if (nvData.switchZProbeParameters != params)
			{
				nvData.switchZProbeParameters = params;
				if (autoSaveEnabled)
				{
					WriteNvData();
				}
			}
			return true;

		case 1:
		case 2:
			if (nvData.irZProbeParameters != params)
			{
				nvData.irZProbeParameters = params;
				if (autoSaveEnabled)
				{
					WriteNvData();
				}
			}
			return true;

		case 3:
			if (nvData.alternateZProbeParameters != params)
			{
				nvData.alternateZProbeParameters = params;
				if (autoSaveEnabled)
				{
					WriteNvData();
				}
			}
			return true;
	}
	return false;
}

// Return true if we must home X and Y before we home Z (i.e. we are using a bed probe)
bool Platform::MustHomeXYBeforeZ() const
{
	return nvData.zProbeType != 0;
}

void Platform::ResetNvData()
{
	nvData.compatibility = me;

	ARRAY_INIT(nvData.ipAddress, IP_ADDRESS);
	ARRAY_INIT(nvData.netMask, NET_MASK);
	ARRAY_INIT(nvData.gateWay, GATE_WAY);
	ARRAY_INIT(nvData.macAddress, MAC_ADDRESS);

	nvData.zProbeType = 0;			// Default is to use the switch
	nvData.zProbeChannel = 0;		// Ormerods are usually shipped with a Duet v0.6
	ARRAY_INIT(nvData.zProbeAxes, Z_PROBE_AXES);
	nvData.switchZProbeParameters.Init(0.0);
	nvData.irZProbeParameters.Init(Z_PROBE_STOP_HEIGHT);
	nvData.alternateZProbeParameters.Init(Z_PROBE_STOP_HEIGHT);

	for (size_t i = 0; i < HEATERS; ++i)
	{
		PidParameters& pp = nvData.pidParams[i];
		pp.thermistorSeriesR = DEFAULT_THERMISTOR_SERIES_RS[i];
		pp.SetThermistorR25AndBeta(DEFAULT_THERMISTOR_25_RS[i], DEFAULT_THERMISTOR_BETAS[i]);
		pp.kI = DEFAULT_PID_KIS[i];
		pp.kD = DEFAULT_PID_KDS[i];
		pp.kP = DEFAULT_PID_KPS[i];
		pp.kT = DEFAULT_PID_KTS[i];
		pp.kS = DEFAULT_PID_KSS[i];
		pp.fullBand = DEFAULT_PID_FULLBANDS[i];
		pp.pidMin = DEFAULT_PID_MINS[i];
		pp.pidMax = DEFAULT_PID_MAXES[i];
		pp.adcLowOffset = pp.adcHighOffset = 0.0;
	}

#ifdef FLASH_SAVE_ENABLED
	nvData.magic = FlashData::magicValue;
#endif
}

void Platform::ReadNvData()
{
#ifdef FLASH_SAVE_ENABLED
	DueFlashStorage::read(FlashData::nvAddress, &nvData, sizeof(nvData));
	if (nvData.magic != FlashData::magicValue)
	{
		// Nonvolatile data has not been initialized since the firmware was last written, so set up default values
		ResetNvData();
		// No point in writing it back here
	}
#else
	Message(GENERIC_MESSAGE, "Error: Cannot load non-volatile data, because Flash support has been disabled!\n");
#endif
}

void Platform::WriteNvData()
{
#ifdef FLASH_SAVE_ENABLED
	DueFlashStorage::write(FlashData::nvAddress, &nvData, sizeof(nvData));
#else
	Message(GENERIC_MESSAGE, "Error: Cannot write non-volatile data, because Flash support has been disabled!\n");
#endif
}

void Platform::SetAutoSave(bool enabled)
{
#ifdef FLASH_SAVE_ENABLED
	autoSaveEnabled = enabled;
#else
	Message(GENERIC_MESSAGE, "Error: Cannot enable auto-save, because Flash support has been disabled!\n");
#endif
}

// AUX device
void Platform::Beep(int freq, int ms)
{
	// Send the beep command to the aux channel. There is no flow control on this port, so it can't block for long.
	scratchString.printf("{\"beep_freq\":%d,\"beep_length\":%d}\n", freq, ms);
	Serial.print(scratchString.Pointer());
}

// Note: the use of floating point time will cause the resolution to degrade over time.
// For example, 1ms time resolution will only be available for about half an hour from startup.
// Personally, I (dc42) would rather just maintain and provide the time in milliseconds in a uint32_t.
// This would wrap round after about 49 days, but that isn't difficult to handle.
float Platform::Time()
{
	unsigned long now = micros();
	if (now < lastTimeCall) // Has timer overflowed?
	{
		addToTime += ((float) ULONG_MAX) * TIME_FROM_REPRAP;
	}
	lastTimeCall = now;
	return addToTime + TIME_FROM_REPRAP * (float) now;
}

void Platform::Exit()
{
	Message(GENERIC_MESSAGE, "Platform class exited.\n");
	active = false;
}

Compatibility Platform::Emulating() const
{
	if (nvData.compatibility == reprapFirmware)
		return me;
	return nvData.compatibility;
}

void Platform::SetEmulating(Compatibility c)
{
	if (c != me && c != reprapFirmware && c != marlin)
	{
		Message(GENERIC_MESSAGE, "Error: Attempt to emulate unsupported firmware.\n");
		return;
	}
	if (c == reprapFirmware)
	{
		c = me;
	}
	if (c != nvData.compatibility)
	{
		nvData.compatibility = c;
		if (autoSaveEnabled)
		{
			WriteNvData();
		}
	}
}

void Platform::UpdateNetworkAddress(byte dst[4], const byte src[4])
{
	bool changed = false;
	for (uint8_t i = 0; i < 4; i++)
	{
		if (dst[i] != src[i])
		{
			dst[i] = src[i];
			changed = true;
		}
	}
	if (changed && autoSaveEnabled)
	{
		WriteNvData();
	}
}

void Platform::SetIPAddress(byte ip[])
{
	UpdateNetworkAddress(nvData.ipAddress, ip);
}

void Platform::SetGateWay(byte gw[])
{
	UpdateNetworkAddress(nvData.gateWay, gw);
}

void Platform::SetNetMask(byte nm[])
{
	UpdateNetworkAddress(nvData.netMask, nm);
}

void Platform::Spin()
{
	if (!active)
		return;

	// Write non-blocking data to the AUX line
	if (auxOutputBuffer != nullptr)
	{
		size_t bytesToWrite = min<size_t>(Serial.canWrite(), auxOutputBuffer->BytesLeft());
		if (bytesToWrite > 0)
		{
			Serial.write(auxOutputBuffer->Read(bytesToWrite), bytesToWrite);
		}

		if (auxOutputBuffer->BytesLeft() == 0)
		{
			auxOutputBuffer = reprap.ReleaseOutput(auxOutputBuffer);
		}
	}

	// Write non-blocking data to the USB line
	if (usbOutputBuffer != nullptr)
	{
		size_t bytesToWrite = min<size_t>(SerialUSB.canWrite(), usbOutputBuffer->BytesLeft());
		if (bytesToWrite > 0)
		{
			SerialUSB.write(usbOutputBuffer->Read(bytesToWrite), bytesToWrite);
		}

		if (usbOutputBuffer->BytesLeft() == 0)
		{
			usbOutputBuffer = reprap.ReleaseOutput(usbOutputBuffer);
		}
	}

	// Diagnostics test
	if (debugCode == DiagnosticTest::TestSpinLockup)
	{
		for (;;) {}
	}

	ClassReport(longWait);
}

void Platform::SoftwareReset(uint16_t reason)
{
	if (reason != SoftwareResetReason::user)
	{
		if (!SerialUSB.canWrite())
		{
			reason |= SoftwareResetReason::inUsbOutput;	// if we are resetting because we are stuck in a Spin function, record whether we are trying to send to USB
		}
		if (reprap.GetNetwork()->InLwip())
		{
			reason |= SoftwareResetReason::inLwipSpin;
		}
		if (!Serial.canWrite())
		{
			reason |= SoftwareResetReason::inAuxOutput;	// if we are resetting because we are stuck in a Spin function, record whether we are trying to send to aux
		}
	}
	reason |= reprap.GetSpinningModule();

	// Record the reason for the software reset
	SoftwareResetData temp;
	temp.magic = SoftwareResetData::magicValue;
	temp.resetReason = reason;
	GetStackUsage(nullptr, nullptr, &temp.neverUsedRam);

	// Save diagnostics data to Flash and reset the software
	DueFlashStorage::write(SoftwareResetData::nvAddress, &temp, sizeof(SoftwareResetData));

	rstc_start_software_reset(RSTC);
	for(;;) {}
}

//*****************************************************************************************************************

// Interrupts

void TC3_Handler()
{
	TC_GetStatus(TC1, 0);
	reprap.Interrupt();
}

void TC4_Handler()
{
	TC_GetStatus(TC1, 1);
	reprap.GetNetwork()->Interrupt();
}

void FanInterrupt()
{
	++fanInterruptCount;
	if (fanInterruptCount == fanMaxInterruptCount)
	{
		uint32_t now = micros();
		fanInterval = now - fanLastResetTime;
		fanLastResetTime = now;
		fanInterruptCount = 0;
	}
}

void Platform::InitialiseInterrupts()
{
	// Timer interrupt for stepper motors
	pmc_set_writeprotect(false);
	pmc_enable_periph_clk((uint32_t) TC3_IRQn);
	TC_Configure(TC1, 0, TC_CMR_WAVE | TC_CMR_WAVSEL_UP_RC | TC_CMR_TCCLKS_TIMER_CLOCK4);
	TC1 ->TC_CHANNEL[0].TC_IER = TC_IER_CPCS;
	TC1 ->TC_CHANNEL[0].TC_IDR = ~TC_IER_CPCS;
	SetInterrupt(STANDBY_INTERRUPT_RATE);

	// Timer interrupt to keep the networking timers running (called at 16Hz)
	pmc_enable_periph_clk((uint32_t) TC4_IRQn);
	TC_Configure(TC1, 1, TC_CMR_WAVE | TC_CMR_WAVSEL_UP_RC | TC_CMR_TCCLKS_TIMER_CLOCK2);
	uint32_t rc = VARIANT_MCK/8/16; // 8 because we selected TIMER_CLOCK2 above
	TC_SetRA(TC1, 1, rc/2); // 50% high, 50% low
	TC_SetRC(TC1, 1, rc);
	TC_Start(TC1, 1);
	TC1 ->TC_CHANNEL[1].TC_IER = TC_IER_CPCS;
	TC1 ->TC_CHANNEL[1].TC_IDR = ~TC_IER_CPCS;
	NVIC_EnableIRQ(TC4_IRQn);

	// Interrupt for 4-pin PWM fan sense line
	attachInterrupt(coolingFanRpmPin, FanInterrupt, FALLING);

	// Tick interrupt for ADC conversions
	tickState = 0;
	currentHeater = 0;

	active = true;							// this enables the tick interrupt, which keeps the watchdog happy
}

void Platform::SetInterrupt(float s) // Seconds
{
	if (s <= 0.0)
	{
		//NVIC_DisableIRQ(TC3_IRQn);
		Message(GENERIC_MESSAGE, "Error: Negative interrupt!\n");
		s = STANDBY_INTERRUPT_RATE;
	}
	uint32_t rc = (uint32_t)( (((long)(TIME_TO_REPRAP*s))*84l)/128l );
	TC_SetRA(TC1, 0, rc/2); //50% high, 50% low
	TC_SetRC(TC1, 0, rc);
	TC_Start(TC1, 0);
	NVIC_EnableIRQ(TC3_IRQn);
}

//void Platform::DisableInterrupts()
//{
//	NVIC_DisableIRQ(TC3_IRQn);
///	NVIC_DisableIRQ(TC4_IRQn);
//}

// Process a 1ms tick interrupt
// This function must be kept fast so as not to disturb the stepper timing, so don't do any floating point maths in here.
// This is what we need to do:
// 0.  Kick the watchdog.
// 1.  Kick off a new ADC conversion.
// 2.  Fetch and process the result of the last ADC conversion.
// 3a. If the last ADC conversion was for the Z probe, toggle the modulation output if using a modulated IR sensor.
// 3b. If the last ADC reading was a thermistor reading, check for an over-temperature situation and turn off the heater if necessary.
//     We do this here because the usual polling loop sometimes gets stuck trying to send data to the USB port.

//#define TIME_TICK_ISR	1		// define this to store the tick ISR time in errorCodeBits

void Platform::Tick()
{
#ifdef TIME_TICK_ISR
	uint32_t now = micros();
#endif
	switch (tickState)
	{
		case 1:			// last conversion started was a thermistor
		case 3:
		{
			ThermistorAveragingFilter& currentFilter = const_cast<ThermistorAveragingFilter&>(thermistorFilters[currentHeater]);
			currentFilter.ProcessReading(GetAdcReading(heaterAdcChannels[currentHeater]));
			StartAdcConversion(zProbeAdcChannel);
			if (currentFilter.IsValid())
			{
				uint32_t sum = currentFilter.GetSum();
				if (sum < thermistorOverheatSums[currentHeater] || sum >= AD_DISCONNECTED_REAL * THERMISTOR_AVERAGE_READINGS)
				{
					// We have an over-temperature or bad reading from this thermistor, so turn off the heater
					// NB - the SetHeater function we call does floating point maths, but this is an exceptional situation so we allow it
					SetHeater(currentHeater, 0.0);
					errorCodeBits |= ErrorBadTemp;
				}
			}
			++currentHeater;
			if (currentHeater == HEATERS)
			{
				currentHeater = 0;
			}
			++tickState;
			break;
		}

		case 2:			// last conversion started was the Z probe, with IR LED on
			const_cast<ZProbeAveragingFilter&>(zProbeOnFilter).ProcessReading(GetAdcReading(zProbeAdcChannel));
			StartAdcConversion(heaterAdcChannels[currentHeater]);	// read a thermistor
			if (currentZProbeType == 2)								// if using a modulated IR sensor
			{
				digitalWrite(zProbeModulationPin, LOW);				// turn off the IR emitter
			}
			++tickState;
			break;

		case 4:			// last conversion started was the Z probe, with IR LED off if modulation is enabled
			const_cast<ZProbeAveragingFilter&>(zProbeOffFilter).ProcessReading(GetAdcReading(zProbeAdcChannel));
			// no break
		case 0:			// this is the state after initialisation, no conversion has been started
		default:
		{
			StartAdcConversion(heaterAdcChannels[currentHeater]);	// read a thermistor
			currentZProbeType = nvData.zProbeType;
			if (currentZProbeType <= 2)								// if using an IR sensor
			{
				digitalWrite(zProbeModulationPin, HIGH);			// turn on the IR emitter
			}
			tickState = 1;
			break;
		}
	}
#ifdef TIME_TICK_ISR
	uint32_t now2 = micros();
	if (now2 - now > errorCodeBits)
	{
		errorCodeBits = now2 - now;
	}
#endif
}

/*static*/uint16_t Platform::GetAdcReading(adc_channel_num_t chan)
{
	uint16_t rslt = (uint16_t) adc_get_channel_value(ADC, chan);
	adc_disable_channel(ADC, chan);
	return rslt;
}

/*static*/void Platform::StartAdcConversion(adc_channel_num_t chan)
{
	adc_enable_channel(ADC, chan);
	adc_start(ADC);
}

// Convert an Arduino Due pin number to the corresponding ADC channel number
/*static*/adc_channel_num_t Platform::PinToAdcChannel(int pin)
{
	if (pin < A0)
	{
		pin += A0;
	}
	return (adc_channel_num_t) (int) g_APinDescription[pin].ulADCChannelNumber;
}

//*************************************************************************************************

void Platform::Diagnostics()
{
	Message(GENERIC_MESSAGE, "Platform Diagnostics:\n");

	// Print memory stats and error codes to USB and copy them to the current webserver reply
	const char *ramstart = (char *) 0x20070000;
	const struct mallinfo mi = mallinfo();
	Message(GENERIC_MESSAGE, "Memory usage:\n");
	MessageF(GENERIC_MESSAGE, "Program static ram used: %d\n", &_end - ramstart);
	MessageF(GENERIC_MESSAGE, "Dynamic ram used: %d\n", mi.uordblks);
	MessageF(GENERIC_MESSAGE, "Recycled dynamic ram: %d\n", mi.fordblks);
	size_t currentStack, maxStack, neverUsed;
	GetStackUsage(&currentStack, &maxStack, &neverUsed);
	MessageF(GENERIC_MESSAGE, "Current stack ram used: %d\n", currentStack);
	MessageF(GENERIC_MESSAGE, "Maximum stack ram used: %d\n", maxStack);
	MessageF(GENERIC_MESSAGE, "Never used ram: %d\n", neverUsed);

	// Show the up time and reason for the last reset
	const uint32_t now = (uint32_t)Time();		// get up time in seconds
	const char* resetReasons[8] = { "power up", "backup", "watchdog", "software", "external", "?", "?", "?" };
	MessageF(GENERIC_MESSAGE, "Last reset %02d:%02d:%02d ago, cause: %s\n",
			(unsigned int)(now/3600), (unsigned int)((now % 3600)/60), (unsigned int)(now % 60),
			resetReasons[(REG_RSTC_SR & RSTC_SR_RSTTYP_Msk) >> RSTC_SR_RSTTYP_Pos]);

	// Show the error code stored at the last software reset
	{
		SoftwareResetData temp;
		temp.magic = 0;
		DueFlashStorage::read(SoftwareResetData::nvAddress, &temp, sizeof(SoftwareResetData));
		if (temp.magic == SoftwareResetData::magicValue)
		{
			MessageF(GENERIC_MESSAGE, "Last software reset code & available RAM: 0x%04x, %u\n", temp.resetReason, temp.neverUsedRam);
			MessageF(GENERIC_MESSAGE, "Spinning module during software reset: %s\n", moduleName[temp.resetReason & 0x0F]);
		}
	}

	// Show the current error codes
	MessageF(GENERIC_MESSAGE, "Error status: %u\n", errorCodeBits);

	// Show the current probe position heights
	MessageF(GENERIC_MESSAGE, "Bed probe heights:");
	for(size_t i = 0; i < MAX_PROBE_POINTS; ++i)
	{
		MessageF(GENERIC_MESSAGE, " %.3f", reprap.GetMove()->ZBedProbePoint(i));
	}
	MessageF(GENERIC_MESSAGE, "\n");

	// Show the number of free entries in the file table
	size_t numFreeFiles = 0;
	for (size_t i = 0; i < MAX_FILES; i++)
	{
		if (!files[i]->inUse)
		{
			++numFreeFiles;
		}
	}
	MessageF(GENERIC_MESSAGE, "Free file entries: %u\n", numFreeFiles);

	// Show the longest write time
	MessageF(GENERIC_MESSAGE, "Longest block write time: %.1fms\n", FileStore::GetAndClearLongestWriteTime());

	reprap.Timing();
}

void Platform::DiagnosticTest(int d)
{
	switch (d)
	{
		case DiagnosticTest::TestWatchdog:
			SysTick ->CTRL &= ~(SysTick_CTRL_TICKINT_Msk);	// disable the system tick interrupt so that we get a watchdog timeout reset
			break;

		case DiagnosticTest::TestSpinLockup:
			debugCode = d;									// tell the Spin function to loop
			break;

		case DiagnosticTest::TestSerialBlock:				// write an arbitrary message via debugPrintf()
			debugPrintf("Diagnostic Test\n");
			break;

		default:
			break;
	}
}

// Return the stack usage and amount of memory that has never been used, in bytes
void Platform::GetStackUsage(size_t* currentStack, size_t* maxStack, size_t* neverUsed) const
{
	const char *ramend = (const char *) 0x20088000;
	register const char * stack_ptr asm ("sp");
	const char *heapend = sbrk(0);
	const char* stack_lwm = heapend;
	while (stack_lwm < stack_ptr && *stack_lwm == memPattern)
	{
		++stack_lwm;
	}
	if (currentStack) { *currentStack = ramend - stack_ptr; }
	if (maxStack) { *maxStack = ramend - stack_lwm; }
	if (neverUsed) { *neverUsed = stack_lwm - heapend; }
}

void Platform::ClassReport(float &lastTime)
{
	const Module spinningModule = reprap.GetSpinningModule();
	if (reprap.Debug(spinningModule))
	{
		if (Time() - lastTime >= LONG_TIME)
		{
			lastTime = Time();
			MessageF(HOST_MESSAGE, "Class %s spinning.\n", moduleName[spinningModule]);
		}
	}
}

//===========================================================================
//=============================Thermal Settings  ============================
//===========================================================================

// See http://en.wikipedia.org/wiki/Thermistor#B_or_.CE.B2_parameter_equation

// BETA is the B value
// RS is the value of the series resistor in ohms
// R_INF is R0.exp(-BETA/T0), where R0 is the thermistor resistance at T0 (T0 is in kelvin)
// Normally T0 is 298.15K (25 C).  If you write that expression in brackets in the #define the compiler 
// should compute it for you (i.e. it won't need to be calculated at run time).

// If the A->D converter has a range of 0..1023 and the measured voltage is V (between 0 and 1023)
// then the thermistor resistance, R = V.RS/(1024 - V)
// and the temperature, T = BETA/ln(R/R_INF)
// To get degrees celsius (instead of kelvin) add -273.15 to T

// Result is in degrees celsius

float Platform::GetTemperature(size_t heater) const
{
	int rawTemp = GetRawTemperature(heater);

	// If the ADC reading is N then for an ideal ADC, the input voltage is at least N/(AD_RANGE + 1) and less than (N + 1)/(AD_RANGE + 1), times the analog reference.
	// So we add 0.5 to to the reading to get a better estimate of the input.

	float reading = (float) rawTemp + 0.5;

	// Recognise the special case of thermistor disconnected.
	// For some ADCs, the high-end offset is negative, meaning that the ADC never returns a high enough value. We need to allow for this here.

	const PidParameters& p = nvData.pidParams[heater];
	if (p.adcHighOffset < 0.0)
	{
		rawTemp -= (int) p.adcHighOffset;
	}
	if (rawTemp >= AD_DISCONNECTED_VIRTUAL)
	{
		return ABS_ZERO;		// thermistor is disconnected
	}

	// Correct for the low and high ADC offsets
	reading -= p.adcLowOffset;
	reading *= (AD_RANGE_VIRTUAL + 1) / (AD_RANGE_VIRTUAL + 1 + p.adcHighOffset - p.adcLowOffset);

	float resistance = reading * p.thermistorSeriesR / ((AD_RANGE_VIRTUAL + 1) - reading);
	return (resistance <= p.GetRInf()) ? 2000.0			// thermistor short circuit, return a high temperature
			: ABS_ZERO + p.GetBeta() / log(resistance / p.GetRInf());
}

void Platform::SetPidParameters(size_t heater, const PidParameters& params)
{
	if (heater < HEATERS && params != nvData.pidParams[heater])
	{
		nvData.pidParams[heater] = params;
		if (autoSaveEnabled)
		{
			WriteNvData();
		}
	}
}
const PidParameters& Platform::GetPidParameters(size_t heater) const
{
	// Default to hot bed if an invalid heater index is passed
	if (heater >= HEATERS)
		heater = 0;

	return nvData.pidParams[heater];
}

// power is a fraction in [0,1]

void Platform::SetHeater(size_t heater, float power)
{
	if (heatOnPins[heater] < 0)
		return;

	byte p = (byte) (255.0 * min<float>(1.0, max<float>(0.0, power)));
	analogWrite(heatOnPins[heater], (HEAT_ON == 0) ? 255 - p : p);
}

EndStopHit Platform::Stopped(size_t drive)
{
	if (nvData.zProbeType > 0 && drive < AXES && nvData.zProbeAxes[drive])
	{
		int zProbeVal = ZProbe();
		int zProbeADValue = (nvData.zProbeType == 3) ?
								nvData.alternateZProbeParameters.adcValue :
								nvData.irZProbeParameters.adcValue;

		if (zProbeVal >= zProbeADValue)
			return EndStopHit::lowHit;
		else if (zProbeVal * 10 >= zProbeADValue * 9)	// if we are at/above 90% of the target value
			return EndStopHit::lowNear;
		else
			return EndStopHit::noStop;
	}

	if (lowStopPins[drive] >= 0)
	{
		if (digitalRead(lowStopPins[drive]) == ENDSTOP_HIT)
			return EndStopHit::lowHit;
	}
	if (highStopPins[drive] >= 0)
	{
		if (digitalRead(highStopPins[drive]) == ENDSTOP_HIT)
			return EndStopHit::highHit;
	}
	return EndStopHit::noStop;
}

// This is called from the step ISR as well as other places, so keep it fast, especially in the case where the motor is already enabled
void Platform::SetDirection(size_t drive, bool direction)
{
	const int8_t pin = directionPins[drive];
	if (pin >= 0)
	{
		bool d = (direction == FORWARDS) ? directions[drive] : !directions[drive];
		digitalWrite(pin, d);
	}
}

// Enable a drive. Must not be called from an ISR, or with interrupts disabled.
void Platform::EnableDrive(size_t drive)
{
	if (drive < DRIVES && driveState[drive] != DriveStatus::enabled)
	{
		driveState[drive] = DriveStatus::enabled;
		UpdateMotorCurrent(drive);

		const int pin = enablePins[drive];
		if (pin >= 0)
		{
			digitalWrite(pin, ENABLE_DRIVE);
		}
	}
}

// Disable a drive, if it has a disable pin
void Platform::DisableDrive(size_t drive)
{
	if (drive < DRIVES)
	{
		const int pin = enablePins[drive];
		if (pin >= 0)
		{
			digitalWrite(pin, DISABLE_DRIVE);
			driveState[drive] = DriveStatus::disabled;
		}
	}
}

// Set a drive to idle hold if it is enabled. If it is disabled, leave it alone.
// Must not be called from an ISR, or with interrupts disabled.
void Platform::SetDriveIdle(size_t drive)
{
	if (drive < DRIVES && driveState[drive] == DriveStatus::enabled)
	{
		driveState[drive] = DriveStatus::idle;
		UpdateMotorCurrent(drive);
	}
}

// Set the current for a motor. Current is in mA.
void Platform::SetMotorCurrent(size_t drive, float current)
{
	if (drive < DRIVES)
	{
		motorCurrents[drive] = current;
		UpdateMotorCurrent(drive);
	}
}

// This must not be called from an ISR, or with interrupts disabled.
void Platform::UpdateMotorCurrent(size_t drive)
{
	if (drive < DRIVES)
	{
		float current = motorCurrents[drive];
		if (driveState[drive] == DriveStatus::idle)
		{
			current *= idleCurrentFactor;
		}
		unsigned short pot = (unsigned short)((0.256*current*8.0*senseResistor + maxStepperDigipotVoltage/2)/maxStepperDigipotVoltage);
		if (drive < 4)
		{
			mcpDuet.setNonVolatileWiper(potWipes[drive], pot);
			mcpDuet.setVolatileWiper(potWipes[drive], pot);
		}
		else
		{
			mcpExpansion.setNonVolatileWiper(potWipes[drive], pot);
			mcpExpansion.setVolatileWiper(potWipes[drive], pot);
		}
	}
}

float Platform::MotorCurrent(size_t drive) const
{
	return (drive < DRIVES) ? motorCurrents[drive] : 0.0;
}

// Set the motor idle current factor
void Platform::SetIdleCurrentFactor(float f)
{
	idleCurrentFactor = f;
	for (size_t drive = 0; drive < DRIVES; ++drive)
	{
		if (driveState[drive] == DriveStatus::idle)
		{
			UpdateMotorCurrent(drive);
		}
	}
}

void Platform::Step(size_t drive)
{
	if (stepPins[drive] >= 0)
	{
		digitalWrite(stepPins[drive], 0);
		digitalWrite(stepPins[drive], 1);
	}
}

// Get current cooling fan speed on a scale between 0 and 1
float Platform::GetFanValue() const
{
	return coolingFanValue;
}

// This is a bit of a compromise - old RepRaps used fan speeds in the range
// [0, 255], which is very hardware dependent.  It makes much more sense
// to specify speeds in [0.0, 1.0].  This looks at the value supplied (which
// the G Code reader will get right for a float or an int) and attempts to
// do the right thing whichever the user has done.  This will only not work
// for an old-style fan speed of 1/255...
void Platform::SetFanValue(float speed)
{
	if (coolingFanPin >= 0)
	{
		byte p;
		if (speed <= 1.0)
		{
			p = (byte)(255.0 * max<float>(0.0, speed));
			coolingFanValue = speed;
		}
		else
		{
			p = (byte)speed;
			coolingFanValue = speed / 255.0;
		}

		// The cooling fan output pin gets inverted if HEAT_ON == 0
		analogWriteDuet(coolingFanPin, (HEAT_ON == 0) ? (255 - p) : p, true);
	}
}

// Get current fan RPM
float Platform::GetFanRPM()
{
	// The ISR sets fanInterval to the number of microseconds it took to get fanMaxInterruptCount interrupts.
	// We get 2 tacho pulses per revolution, hence 2 interrupts per revolution.
	// However, if the fan stops then we get no interrupts and fanInterval stops getting updated.
	// We must recognise this and return zero.
	return (fanInterval != 0 && micros() - fanLastResetTime < 3000000U)		// if we have a reading and it is less than 3 second old
			? (float)((30000000U * fanMaxInterruptCount)/fanInterval)		// then calculate RPM assuming 2 interrupts per rev
			: 0.0;															// else assume fan is off or tacho not connected
}

//-----------------------------------------------------------------------------------------------------

FileStore* Platform::GetFileStore(const char* directory, const char* fileName, bool write)
{
	if (!fileStructureInitialised)
		return nullptr;

	for(size_t i = 0; i < MAX_FILES; i++)
	{
		if (!files[i]->inUse)
		{
			files[i]->inUse = true;
			if (files[i]->Open(directory, fileName, write))
			{
				return files[i];
			}
			else
			{
				files[i]->inUse = false;
				return nullptr;
			}
		}
	}
	Message(HOST_MESSAGE, "Max open file count exceeded.\n");
	return nullptr;
}

FileStore* Platform::GetFileStore(const char* filePath, bool write)
{
	return GetFileStore(nullptr, filePath, write);
}

MassStorage* Platform::GetMassStorage()
{
	return massStorage;
}

void Platform::Message(MessageType type, const char *message)
{
	switch (type)
	{
		case FLASH_LED:
			// Message that is to flash an LED; the next two bytes define
			// the frequency and M/S ratio.
			// (not implemented yet)
			break;

		case AUX_MESSAGE:
			// Message that is to be sent to an auxiliary device (blocking)
			Serial.write(message);
			Serial.flush();
			break;

		case DISPLAY_MESSAGE:
			// Message that is to appear on a local display;  \f and \n should be supported.
			reprap.SetMessage(message);
			break;

		case DEBUG_MESSAGE:
			// Debug messages in blocking mode - potentially DANGEROUS, use with care!
			SerialUSB.write(message);
			SerialUSB.flush();
			break;

		case HOST_MESSAGE:
			// Message that is to be sent via the USB line (non-blocking)
			//
			// Allow this type of message only if the USB port is opened
			if (SerialUSB)
			{
				// Ensure we have a valid buffer to write to
				if (usbOutputBuffer == nullptr && !reprap.AllocateOutput(usbOutputBuffer))
				{
					// Should never happen
					return;
				}

				// Check if we need to write the indentation chars first
				const size_t stackPointer = reprap.GetGCodes()->GetStackPointer();
				if (stackPointer > 0)
				{
					// First, make sure we get the indentation right
					char indentation[STACK + 1];
					for(size_t i = 0; i < stackPointer; i++)
					{
						indentation[i] = ' ';
					}
					indentation[stackPointer] = 0;

					// Append the indentation string to our chain, or allocate a new buffer if there is none
					usbOutputBuffer->cat(indentation);
				}

				// Append the message string to the output buffer chain
				usbOutputBuffer->cat(message);
			}
			break;

		case HTTP_MESSAGE:
		case TELNET_MESSAGE:
			// Message that is to be sent to the web
			{
				const WebSource source = (type == HTTP_MESSAGE) ? WebSource::HTTP : WebSource::Telnet;
				reprap.GetWebserver()->HandleGCodeReply(source, message);
			}
			break;

		case GENERIC_MESSAGE:
			// Message that is to be sent to the web & host. Make this the default one, too.
		default:
			Message(HOST_MESSAGE, message);
			Message(HTTP_MESSAGE, message);
			Message(TELNET_MESSAGE, message);
			break;
	}
}

void Platform::Message(const MessageType type, const StringRef &message)
{
	Message(type, message.Pointer());
}

void Platform::Message(const MessageType type, OutputBuffer *buffer)
{
	switch (type)
	{
		case AUX_MESSAGE:
			// If no AUX device is attached, don't queue this buffer
			if (!reprap.GetGCodes()->HaveAux())
			{
				while (buffer != nullptr)
				{
					buffer = reprap.ReleaseOutput(buffer);
				}
				break;
			}

			// For big responses it makes sense to write big chunks of data in portions. Store this data here
			if (auxOutputBuffer == nullptr)
			{
				auxOutputBuffer = buffer;
			}
			else
			{
				auxOutputBuffer->Append(buffer);
			}
			break;

		case DEBUG_MESSAGE:
			// Probably rarely used, but supported.
			while (buffer != nullptr)
			{
				SerialUSB.write(buffer->Data(), buffer->DataLength());
				SerialUSB.flush();

				buffer = reprap.ReleaseOutput(buffer);
			}
			break;

		case HOST_MESSAGE:
			// If the serial USB line is not open, discard its content right away
			if (!SerialUSB)
			{
				while (buffer != nullptr)
				{
					buffer = reprap.ReleaseOutput(buffer);
				}
			}
			else
			{
				// Append incoming data to the list of our output buffers
				if (usbOutputBuffer == nullptr)
				{
					usbOutputBuffer = buffer;
				}
				else
				{
					usbOutputBuffer->Append(buffer);
				}
			}
			break;

		case HTTP_MESSAGE:
		case TELNET_MESSAGE:
			// Message that is to be sent to the web
			{
				const WebSource source = (type == HTTP_MESSAGE) ? WebSource::HTTP : WebSource::Telnet;
				reprap.GetWebserver()->HandleGCodeReply(source, buffer);
			}
			break;

		case GENERIC_MESSAGE:
			// Message that is to be sent to the web & host.
			buffer->SetReferences(3);		// This one is referenced by three destinations
			Message(HOST_MESSAGE, buffer);
			Message(HTTP_MESSAGE, buffer);
			Message(TELNET_MESSAGE, buffer);
			break;

		default:
			// Everything else is unsupported (and probably not used)
			MessageF(HOST_MESSAGE, "Warning: Unsupported Message call for type %u!\n", type);
			break;
	}
}

void Platform::MessageF(MessageType type, const char *fmt, va_list vargs)
{
	char formatBuffer[FORMAT_STRING_LENGTH];
	StringRef formatString(formatBuffer, ARRAY_SIZE(formatBuffer));
	formatString.vprintf(fmt, vargs);

	Message(type, formatBuffer);
}

void Platform::MessageF(MessageType type, const char *fmt, ...)
{
	char formatBuffer[FORMAT_STRING_LENGTH];
	StringRef formatString(formatBuffer, ARRAY_SIZE(formatBuffer));

	va_list vargs;
	va_start(vargs, fmt);
	formatString.vprintf(fmt, vargs);
	va_end(vargs);

	Message(type, formatBuffer);
}

bool Platform::AtxPower() const
{
	return (digitalRead(atxPowerPin) == HIGH);
}

void Platform::SetAtxPower(bool on)
{
	digitalWrite(atxPowerPin, (on) ? HIGH : LOW);
}

void Platform::SetBaudRate(size_t chan, uint32_t br)
{
	if (chan < NUM_SERIAL_CHANNELS)
	{
		baudRates[chan] = br;
		ResetChannel(chan);
	}
}

uint32_t Platform::GetBaudRate(size_t chan) const
{
	return (chan < NUM_SERIAL_CHANNELS) ? baudRates[chan] : 0;
}

void Platform::SetCommsProperties(size_t chan, uint32_t cp)
{
	if (chan < NUM_SERIAL_CHANNELS)
	{
		commsParams[chan] = cp;
		ResetChannel(chan);
	}
}

uint32_t Platform::GetCommsProperties(size_t chan) const
{
	return (chan < NUM_SERIAL_CHANNELS) ? commsParams[chan] : 0;
}


// Re-initialise a serial channel.
// Ideally, this would be part of the Line class. However, the Arduino core inexplicably fails to make the serial I/O begin() and end() members
// virtual functions of a base class, which makes that difficult to do.
void Platform::ResetChannel(size_t chan)
{
	switch(chan)
	{
		case 0:
			SerialUSB.end();
			SerialUSB.begin(baudRates[0]);
			break;
		case 1:
			Serial.end();
			Serial.begin(baudRates[1]);
			break;
		default:
			break;
	}
}

// Fire the inkjet (if any) in the given pattern
// If there is no inkjet, false is returned; if there is one this returns true
// So you can test for inkjet presence with if(platform->Inkjet(0))

bool Platform::Inkjet(int bitPattern)
{
	if(inkjetBits < 0)
		return false;
	if(!bitPattern)
		return true;

    for(int8_t i = 0; i < inkjetBits; i++)
	{
			if(bitPattern & 1)
			{
        		digitalWrite(inkjetSerialOut, 1); //Write data to shift register
            
        		for(int8_t j = 0; j <= i; j++)
				{
					digitalWrite(inkjetShiftClock, HIGH);
					digitalWrite(inkjetShiftClock,LOW);
            		digitalWrite(inkjetSerialOut,0);
        		}

				digitalWrite(inkjetStorageClock, HIGH); //Transfers data from shift register to output register
				digitalWrite(inkjetStorageClock,LOW);
            
				digitalWrite(inkjetOutputEnable, LOW);  // Fire the droplet out
				delayMicroseconds(inkjetFireMicroseconds);
				digitalWrite(inkjetOutputEnable,HIGH);

				digitalWrite(inkjetClear, LOW);         // Clear to 0
				digitalWrite(inkjetClear,HIGH);
            
        		delayMicroseconds(inkjetDelayMicroseconds); // Wait for the next bit
			}

			bitPattern >>= 1; // Put the next bit in the units column
	}

	return true;
}

/*********************************************************************************

 Files & Communication

 */

MassStorage::MassStorage(Platform* p) : platform(p), combinedName(combinedNameBuffer, ARRAY_SIZE(combinedNameBuffer))
{
	memset(&fileSystem, 0, sizeof(FATFS));
	findDir = new DIR();
}

void MassStorage::Init()
{
	// Initialize SD MMC stack

	sd_mmc_init();
	delay(20);

	bool abort = false;
	sd_mmc_err_t err;
	do {
		err = sd_mmc_check(0);
		if (err > SD_MMC_ERR_NO_CARD)
		{
			abort = true;
			delay(3000);	// Wait a few seconds, so users have a chance to see the following error message
		}
		else
		{
			abort = (err == SD_MMC_ERR_NO_CARD && platform->Time() > 5.0);
		}

		if (abort)
		{
			platform->Message(HOST_MESSAGE, "Cannot initialize the SD card: ");
			switch (err)
			{
				case SD_MMC_ERR_NO_CARD:
					platform->Message(HOST_MESSAGE, "Card not found\n");
					break;
				case SD_MMC_ERR_UNUSABLE:
					platform->Message(HOST_MESSAGE, "Card is unusable, try another one\n");
					break;
				case SD_MMC_ERR_SLOT:
					platform->Message(HOST_MESSAGE, "Slot unknown\n");
					break;
				case SD_MMC_ERR_COMM:
					platform->Message(HOST_MESSAGE, "General communication error\n");
					break;
				case SD_MMC_ERR_PARAM:
					platform->Message(HOST_MESSAGE, "Illegal input parameter\n");
					break;
				case SD_MMC_ERR_WP:
					platform->Message(HOST_MESSAGE, "Card write protected\n");
					break;
				default:
					platform->MessageF(HOST_MESSAGE, "Unknown (code %d)\n", err);
					break;
			}
			return;
		}
	} while (err != SD_MMC_OK);

	// Print some card details (optional)

	/*platform->Message(HOST_MESSAGE, "SD card detected!\nCapacity: %d\n", sd_mmc_get_capacity(0));
	platform->Message(HOST_MESSAGE, "Bus clock: %d\n", sd_mmc_get_bus_clock(0));
	platform->Message(HOST_MESSAGE, "Bus width: %d\nCard type: ", sd_mmc_get_bus_width(0));
	switch (sd_mmc_get_type(0))
	{
		case CARD_TYPE_SD | CARD_TYPE_HC:
			platform->Message(HOST_MESSAGE, "SDHC\n");
			break;
		case CARD_TYPE_SD:
			platform->Message(HOST_MESSAGE, "SD\n");
			break;
		case CARD_TYPE_MMC | CARD_TYPE_HC:
			platform->Message(HOST_MESSAGE, "MMC High Density\n");
			break;
		case CARD_TYPE_MMC:
			platform->Message(HOST_MESSAGE, "MMC\n");
			break;
		case CARD_TYPE_SDIO:
			platform->Message(HOST_MESSAGE, "SDIO\n");
			return;
		case CARD_TYPE_SD_COMBO:
			platform->Message(HOST_MESSAGE, "SD COMBO\n");
			break;
		case CARD_TYPE_UNKNOWN:
		default:
			platform->Message(HOST_MESSAGE, "Unknown\n");
			return;
	}*/

	// Mount the file system

	int mounted = f_mount(0, &fileSystem);
	if (mounted != FR_OK)
	{
		platform->MessageF(HOST_MESSAGE, "Can't mount filesystem 0: code %d\n", mounted);
	}
}

const char* MassStorage::CombineName(const char* directory, const char* fileName)
{
	int out = 0;
	int in = 0;

	if (directory != nullptr)
	{
		while (directory[in] != 0 && directory[in] != '\n')
		{
			combinedName[out] = directory[in];
			in++;
			out++;
			if (out >= combinedName.Length())
			{
				platform->Message(GENERIC_MESSAGE, "Error: CombineName() buffer overflow.\n");
				out = 0;
			}
		}
	}

	if (in > 0 && directory[in - 1] != '/' && out < combinedName.Length() - 1)
	{
		combinedName[out] = '/';
		out++;
	}

	in = 0;
	while (fileName[in] != 0 && fileName[in] != '\n')
	{
		combinedName[out] = fileName[in];
		in++;
		out++;
		if (out >= combinedName.Length())
		{
			platform->Message(GENERIC_MESSAGE, "Error: CombineName() buffer overflow.\n");
			out = 0;
		}
	}
	combinedName[out] = 0;

	return combinedName.Pointer();
}

// Open a directory to read a file list. Returns true if it contains any files, false otherwise.
bool MassStorage::FindFirst(const char *directory, FileInfo &file_info)
{
	TCHAR loc[FILENAME_LENGTH];

	// Remove the trailing '/' from the directory name
	size_t len = strnlen(directory, ARRAY_UPB(loc));
	if (len == 0)
	{
		loc[0] = 0;
	}
	else if (directory[len - 1] == '/')
	{
		strncpy(loc, directory, len - 1);
		loc[len - 1] = 0;
	}
	else
	{
		strncpy(loc, directory, len);
		loc[len] = 0;
	}

	findDir->lfn = nullptr;
	FRESULT res = f_opendir(findDir, loc);
	if (res == FR_OK)
	{
		FILINFO entry;
		entry.lfname = file_info.fileName;
		entry.lfsize = ARRAY_SIZE(file_info.fileName);

		for(;;)
		{
			res = f_readdir(findDir, &entry);
			if (res != FR_OK || entry.fname[0] == 0) break;
			if (StringEquals(entry.fname, ".") || StringEquals(entry.fname, "..")) continue;

			file_info.isDirectory = (entry.fattrib & AM_DIR);
			file_info.size = entry.fsize;
			uint16_t day = entry.fdate & 0x1F;
			if (day == 0)
			{
				// This can happen if a transfer hasn't been processed completely.
				day = 1;
			}
			file_info.day = day;
			file_info.month = (entry.fdate & 0x01E0) >> 5;
			file_info.year = (entry.fdate >> 9) + 1980;
			if (file_info.fileName[0] == 0)
			{
				strncpy(file_info.fileName, entry.fname, ARRAY_SIZE(file_info.fileName));
			}

			return true;
		}
	}

	return false;
}

// Find the next file in a directory. Returns true if another file has been read.
bool MassStorage::FindNext(FileInfo &file_info)
{
	FILINFO entry;
	entry.lfname = file_info.fileName;
	entry.lfsize = ARRAY_SIZE(file_info.fileName);

	findDir->lfn = nullptr;
	if (f_readdir(findDir, &entry) != FR_OK || entry.fname[0] == 0)
	{
		//f_closedir(findDir);
		return false;
	}

	file_info.isDirectory = (entry.fattrib & AM_DIR);
	file_info.size = entry.fsize;
	uint16_t day = entry.fdate & 0x1F;
	if (day == 0)
	{
		// This can happen if a transfer hasn't been processed completely.
		day = 1;
	}
	file_info.day = day;
	file_info.month = (entry.fdate & 0x01E0) >> 5;
	file_info.year = (entry.fdate >> 9) + 1980;
	if (file_info.fileName[0] == 0)
	{
		strncpy(file_info.fileName, entry.fname, ARRAY_SIZE(file_info.fileName));
	}

	return true;
}

// Month names. The first entry is used for invalid month numbers.
static const char *monthNames[13] = { "???", "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

// Returns the name of the specified month or '???' if the specified value is invalid.
const char* MassStorage::GetMonthName(const uint8_t month)
{
	return (month <= 12) ? monthNames[month] : monthNames[0];
}

// Delete a file or directory
bool MassStorage::Delete(const char* directory, const char* fileName)
{
	const char* location = (directory != nullptr)
							? platform->GetMassStorage()->CombineName(directory, fileName)
								: fileName;
	if (f_unlink(location) != FR_OK)
	{
		platform->MessageF(GENERIC_MESSAGE, "Error: Can't delete file %s\n", location);
		return false;
	}
	return true;
}

// Create a new directory
bool MassStorage::MakeDirectory(const char *parentDir, const char *dirName)
{
	const char* location = platform->GetMassStorage()->CombineName(parentDir, dirName);
	if (f_mkdir(location) != FR_OK)
	{
		platform->MessageF(GENERIC_MESSAGE, "Error: Can't create directory %s\n", location);
		return false;
	}
	return true;
}

bool MassStorage::MakeDirectory(const char *directory)
{
	if (f_mkdir(directory) != FR_OK)
	{
		platform->MessageF(GENERIC_MESSAGE, "Error: Can't create directory %s\n", directory);
		return false;
	}
	return true;
}

// Rename a file or directory
bool MassStorage::Rename(const char *oldFilename, const char *newFilename)
{
	if (f_rename(oldFilename, newFilename) != FR_OK)
	{
		platform->MessageF(GENERIC_MESSAGE, "Error: Can't rename file or directory %s to %s\n", oldFilename, newFilename);
		return false;
	}
	return true;
}

// Check if the specified file exists
bool MassStorage::FileExists(const char *file) const
{
 	FILINFO fil;
 	fil.lfname = nullptr;
	return (f_stat(file, &fil) == FR_OK);
}

// Check if the specified directory exists
bool MassStorage::DirectoryExists(const char *path) const
{
 	DIR dir;
 	dir.lfn = nullptr;
	return (f_opendir(&dir, path) == FR_OK);
}

bool MassStorage::DirectoryExists(const char* directory, const char* subDirectory)
{
	const char* location = (directory != nullptr)
							? platform->GetMassStorage()->CombineName(directory, subDirectory)
								: subDirectory;
	return DirectoryExists(location);
}

//------------------------------------------------------------------------------------------------

FileStore::FileStore(Platform* p) : platform(p)
{
}

void FileStore::Init()
{
	bufferPointer = 0;
	inUse = false;
	writing = false;
	lastBufferEntry = 0;
	openCount = 0;
}

// Open a local file (for example on an SD card).
// This is protected - only Platform can access it.

bool FileStore::Open(const char* directory, const char* fileName, bool write)
{
	const char *location = (directory != nullptr)
							? platform->GetMassStorage()->CombineName(directory, fileName)
							: fileName;
	writing = write;
	lastBufferEntry = FILE_BUFFER_LENGTH - 1;
	bytesRead = 0;

	FRESULT openReturn = f_open(&file, location, (writing) ? FA_CREATE_ALWAYS | FA_WRITE : FA_OPEN_EXISTING | FA_READ);
	if (openReturn != FR_OK)
	{
		platform->MessageF(GENERIC_MESSAGE, "Error: Can't open %s to %s, error code %d\n", location, (writing) ? "write" : "read", openReturn);
		return false;
	}

	bufferPointer = (writing) ? 0 : FILE_BUFFER_LENGTH;
	inUse = true;
	openCount = 1;
	return true;
}

void FileStore::Duplicate()
{
	if (!inUse)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Attempt to dup a non-open file.\n");
		return;
	}
	++openCount;
}

bool FileStore::Close()
{
	if (!inUse)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Attempt to close a non-open file.\n");
		return false;
	}
	--openCount;
	if (openCount != 0)
	{
		return true;
	}
	bool ok = true;
	if (writing)
	{
		ok = Flush();
	}
	FRESULT fr = f_close(&file);
	inUse = false;
	writing = false;
	lastBufferEntry = 0;
	return ok && fr == FR_OK;
}

FilePosition FileStore::Position() const
{
	return bytesRead;
}

bool FileStore::Seek(FilePosition pos)
{
	if (!inUse)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Attempt to seek on a non-open file.\n");
		return false;
	}
	if (writing)
	{
		WriteBuffer();
	}
	FRESULT fr = f_lseek(&file, pos);
	if (fr == FR_OK)
	{
		bufferPointer = (writing) ? 0 : FILE_BUFFER_LENGTH;
		bytesRead = pos;
		return true;
	}
	return false;
}

bool FileStore::GoToEnd()
{
	return Seek(Length());
}

FilePosition FileStore::Length() const
{
	if (!inUse)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Attempt to size non-open file.\n");
		return 0;
	}
	return file.fsize;
}

float FileStore::FractionRead() const
{
	FilePosition len = Length();
	if (len <= 0)
	{
		return 0.0;
	}

	return (float)bytesRead / (float)len;
}

IOStatus FileStore::Status() const
{
	if (!inUse)
		return IOStatus::nothing;

	if (lastBufferEntry == FILE_BUFFER_LENGTH)
		return IOStatus::byteAvailable;

	if (bufferPointer < lastBufferEntry)
		return IOStatus::byteAvailable;

	return IOStatus::nothing;
}

bool FileStore::ReadBuffer()
{
	FRESULT readStatus = f_read(&file, buf, FILE_BUFFER_LENGTH, &lastBufferEntry);	// Read a chunk of file
	if (readStatus)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Cannot read file.\n");
		return false;
	}
	bufferPointer = 0;
	return true;
}

// Single character read via the buffer
bool FileStore::Read(char& b)
{
	if (!inUse)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Attempt to read from a non-open file.\n");
		return false;
	}

	if (bufferPointer >= FILE_BUFFER_LENGTH)
	{
		bool ok = ReadBuffer();
		if (!ok)
		{
			return false;
		}
	}

	if (bufferPointer >= lastBufferEntry)
	{
		b = 0;  // Good idea?
		return false;
	}

	b = (char) buf[bufferPointer];
	bufferPointer++;
	bytesRead++;

	return true;
}

// Block read, doesn't use the buffer
// Returns -1 if the read process failed
int FileStore::Read(char* extBuf, unsigned int nBytes)
{
	if (!inUse)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Attempt to read from a non-open file.\n");
		return -1;
	}

	bufferPointer = FILE_BUFFER_LENGTH;	// invalidate the buffer
	UINT bytes_read;
	FRESULT readStatus = f_read(&file, extBuf, nBytes, &bytes_read);

	if (readStatus)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Cannot read file.\n");
		return -1;
	}

	bytesRead += bytes_read;
	return (int)bytes_read;
}

bool FileStore::WriteBuffer()
{
	if (bufferPointer != 0)
	{
		bool ok = InternalWriteBlock((const char*)buf, bufferPointer);
		if (!ok)
		{
			platform->Message(GENERIC_MESSAGE, "Error: Cannot write to file. Disc may be full.\n");
			return false;
		}
		bufferPointer = 0;
	}
	return true;
}

bool FileStore::Write(char b)
{
	if (!inUse)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Attempt to write byte to a non-open file.\n");
		return false;
	}
	buf[bufferPointer] = b;
	bufferPointer++;
	if (bufferPointer >= FILE_BUFFER_LENGTH)
	{
		return WriteBuffer();
	}
	return true;
}

bool FileStore::Write(const char* b)
{
	if (!inUse)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Attempt to write string to a non-open file.\n");
		return false;
	}
	int i = 0;
	while (b[i])
	{
		if (!Write(b[i++]))
		{
			return false;
		}
	}
	return true;
}

// Direct block write that bypasses the buffer. Used when uploading files.
bool FileStore::Write(const char *s, unsigned int len)
{
	if (!inUse)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Attempt to write block to a non-open file.\n");
		return false;
	}
	if (!WriteBuffer())
	{
		return false;
	}
	return InternalWriteBlock(s, len);
}

bool FileStore::InternalWriteBlock(const char *s, unsigned int len)
{
 	unsigned int bytesWritten;
	uint32_t time = micros();
 	FRESULT writeStatus = f_write(&file, s, len, &bytesWritten);
	time = micros() - time;
	if (time > longestWriteTime)
	{
		longestWriteTime = time;
	}
 	if ((writeStatus != FR_OK) || (bytesWritten != len))
 	{
 		platform->Message(GENERIC_MESSAGE, "Error: Cannot write to file. Disc may be full.\n");
 		return false;
 	}
 	return true;
 }

bool FileStore::Flush()
{
	if (!inUse)
	{
		platform->Message(GENERIC_MESSAGE, "Error: Attempt to flush a non-open file.\n");
		return false;
	}
	if (!WriteBuffer())
	{
		return false;
	}
	return f_sync(&file) == FR_OK;
}

float FileStore::GetAndClearLongestWriteTime()
{
	float ret = (float)longestWriteTime/1000.0;
	longestWriteTime = 0;
	return ret;
}

uint32_t FileStore::longestWriteTime = 0;

// vim: ts=4:sw=4
