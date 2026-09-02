// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The OpenDigitalRadio Authors
/*
 * The DM-32's GPS (ATGM336H, interfaces/gps.c) as the position source of the shared DMR data service: ETSI UDT NMEA
 * location reports and Motorola LRRP (shared/source/functions/dmrDataService.c, dmrLrrp.c).
 */

#include <string.h>
// Not built yet: gpsData and nonVolatileSettings come from headers this tree doesn't have; the code waits for a native
// GPS driver (CAP_GPS) and the settings store (core/settings) to stand in for them
#include "functions/settings.h"
#include "dmr/dmrDataService.h"
#include "interfaces/gps.h"

#if defined(HAS_GPS)

// Degrees and ten thousandths of minutes of one coordinate, from the magnitude in degrees
static void gpsDmrDataSplitDegrees(double value, uint8_t *degrees, uint32_t *minutes10000)
{
	uint32_t whole = (uint32_t)value;
	uint32_t m = (uint32_t)(((value - whole) * 60.0 * 10000.0) + 0.5);

	if (m >= (60U * 10000U))// rounding up to the next degree
	{
		m -= (60U * 10000U);
		whole++;
	}

	*degrees = (uint8_t)whole;
	*minutes10000 = m;
}

// Days since 1970-01-01 to the civil date (Howard Hinnant's days_from_civil, reversed)
static void gpsDmrDataCivilFromDays(uint32_t days, uint16_t *year, uint8_t *month, uint8_t *day)
{
	int32_t z = (int32_t)days + 719468;
	int32_t era = z / 146097;
	uint32_t doe = (uint32_t)(z - (era * 146097));
	uint32_t yoe = (doe - (doe / 1460) + (doe / 36524) - (doe / 146096)) / 365;
	int32_t y = (int32_t)yoe + (era * 400);
	uint32_t doy = doe - ((365 * yoe) + (yoe / 4) - (yoe / 100));
	uint32_t mp = ((5 * doy) + 2) / 153;
	uint32_t d = doy - (((153 * mp) + 2) / 5) + 1;
	uint32_t m = (mp < 10) ? (mp + 3) : (mp - 9);

	*year = (uint16_t)(y + ((m <= 2) ? 1 : 0));
	*month = (uint8_t)m;
	*day = (uint8_t)d;
}

static bool gpsDmrDataIsOn(void)
{
	return (nonVolatileSettings.gps > GPS_MODE_OFF) && (nonVolatileSettings.gps < NUM_GPS_MODES);
}

bool dmrDataServiceGetLocation(dmrDataLocation_t *location)
{
	if (!gpsDmrDataIsOn() || ((gpsData.Status & GPS_STATUS_HAS_POSITION) == 0))
	{
		return false;// no GPS, or never had a position
	}

	double lat = gpsData.LatitudeHiRes;
	double lon = gpsData.LongitudeHiRes;

	memset(location, 0, sizeof(*location));
	location->fixValid = ((gpsData.Status & GPS_STATUS_HAS_FIX) != 0);
	location->north = (lat >= 0.0);
	location->east = (lon >= 0.0);
	gpsDmrDataSplitDegrees((lat < 0.0) ? -lat : lat, &location->latDegrees, &location->latMinutes10000);
	gpsDmrDataSplitDegrees((lon < 0.0) ? -lon : lon, &location->lonDegrees, &location->lonMinutes10000);
	location->speedKnots = (gpsData.Status & GPS_STATUS_HAS_SPEED) ? ((gpsData.SpeedInHundredthKn + 50) / 100) : DMR_LOCATION_SPEED_UNKNOWN;

	uint32_t secondsOfDay = gpsData.Time % 86400U;
	location->utcHours = secondsOfDay / 3600U;
	location->utcMinutes = (secondsOfDay / 60U) % 60U;
	location->utcSeconds = secondsOfDay % 60U;

	return true;
}

bool dmrDataServiceGetPosition(lrrpPosition_t *position)
{
	memset(position, 0, sizeof(*position));
	if (!dmrDataServiceGetLocation(&position->location))
	{
		return false;
	}

	if ((gpsData.Status & GPS_STATUS_HAS_TIME) && (gpsData.Time > 86400U))
	{
		gpsDmrDataCivilFromDays(gpsData.Time / 86400U, &position->utcYear, &position->utcMonth, &position->utcDay);
	}

	position->hasCourse = ((gpsData.Status & GPS_STATUS_HAS_COURSE) != 0);
	position->courseDegrees = gpsData.CourseInHundredthDeg / 100;

	return true;
}

#endif
