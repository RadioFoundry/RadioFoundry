// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Integer fixed-point Kepler propagation from TLE mean elements and topocentric observation.
#include <limits.h>
#include <string.h>
#include "satellite/orbit.h"

#define SAT_TLE_MAX_AGE_SECONDS (30 * 86400)
#define SAT_TLE_FUTURE_SECONDS 86400
#define SAT_PHASE_QUARTER UINT32_C(0x40000000)
#define SAT_PHASE_HALF UINT32_C(0x80000000)
#define SAT_Q30_ONE INT32_C(0x40000000)
#define SAT_TLE_DOT_MAX_E8 1000000

#define SAT_MEAN_MOTION_PHASE_Q40 UINT32_C(546570195)
#define SAT_MEAN_MOTION_RAD_Q32 UINT32_C(3353713)
#define SAT_SIDEREAL_RATE_PHASE_S_Q16 UINT32_C(3266731825)
#define SAT_DEGREES_E7_PHASE_Q24 UINT32_C(20015998)
#define SAT_MICRODEGREES_PHASE_Q24 UINT32_C(200159983)
#define SAT_PHASE_PER_RAD_Q30 INT32_C(683565276)
#define SAT_PI_OVER_2_Q30 INT32_C(1686629713)
#define SAT_WGS72_RADIUS_M UINT32_C(6378135)
#define SAT_WGS84_RADIUS_M UINT32_C(6378137)
#define SAT_WGS72_SEMI_MAJOR_KM_MILLI UINT32_C(42241108)
#define SAT_EARTH_J2_1P5_Q30 INT32_C(1743698)
#define SAT_WGS84_E2_Q30 INT32_C(7188036)
#define SAT_EARTH_RATE_RAD_S_Q30 INT32_C(78298)
#define SAT_GST_T2_PHASE UINT32_C(4628)

static const uint32_t cordic_atan_phase[] = {
	536870912u, 316933406u, 167458907u, 85004756u, 42667331u, 21354465u, 10679838u,
	5340245u, 2670163u, 1335087u, 667544u, 333772u, 166886u, 83443u, 41722u, 20861u,
	10430u, 5215u, 2608u, 1304u, 652u, 326u, 163u, 81u, 41u, 20u, 10u, 5u, 3u, 1u, 1u,
};

static bool digits(const char *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		if (p[i] < '0' || p[i] > '9')
			return false;
	}
	return true;
}

static uint32_t number(const char *p, size_t n)
{
	uint32_t value = 0u;
	for (size_t i = 0; i < n; i++)
		value = value * 10u + (uint32_t)(p[i] - '0');
	return value;
}

static bool checksum(const char *line)
{
	if (line[68] < '0' || line[68] > '9')
		return false;
	unsigned sum = 0u;
	for (size_t i = 0; i < 68u; i++) {
		if (line[i] >= '0' && line[i] <= '9')
			sum += (unsigned)(line[i] - '0');
		else if (line[i] == '-')
			sum++;
	}
	return sum % 10u == (unsigned)(line[68] - '0');
}

static bool decimal_fixed(const char *line, size_t offset, size_t length, uint32_t scale, int32_t *value)
{
	if (length == 0u || value == NULL)
		return false;
	size_t i = 0u;
	while (i < length && line[offset + i] == ' ')
		i++;
	bool negative = false;
	if (i < length && (line[offset + i] == '-' || line[offset + i] == '+')) {
		negative = line[offset + i] == '-';
		i++;
	}
	uint32_t integer = 0u, fraction = 0u, fraction_scale = 1u;
	bool has_digit = false;
	while (i < length && line[offset + i] >= '0' && line[offset + i] <= '9') {
		if (integer > (uint32_t)INT32_MAX / scale)
			return false;
		integer = integer * 10u + (uint32_t)(line[offset + i] - '0');
		has_digit = true;
		i++;
	}
	if (i < length && line[offset + i] == '.') {
		i++;
		while (i < length && line[offset + i] >= '0' && line[offset + i] <= '9') {
			if (fraction_scale < scale) {
				fraction = fraction * 10u + (uint32_t)(line[offset + i] - '0');
				fraction_scale *= 10u;
			}
			has_digit = true;
			i++;
		}
	}
	while (i < length && line[offset + i] == ' ')
		i++;
	if (!has_digit || i != length)
		return false;
	while (fraction_scale < scale) {
		fraction *= 10u;
		fraction_scale *= 10u;
	}
	if (fraction_scale != scale || integer > ((uint32_t)INT32_MAX - fraction) / scale)
		return false;
	uint32_t magnitude = integer * scale + fraction;
	if (negative) {
		if (magnitude > (uint32_t)INT32_MAX + 1u)
			return false;
		*value = magnitude == (uint32_t)INT32_MAX + 1u ? INT32_MIN : -(int32_t)magnitude;
	} else {
		if (magnitude > (uint32_t)INT32_MAX)
			return false;
		*value = (int32_t)magnitude;
	}
	return true;
}

static uint32_t divide_u32_q30(uint32_t numerator, uint32_t denominator)
{
	uint32_t remainder = numerator;
	uint32_t quotient = 0u;
	if (denominator == 0u)
		return 0u;
	uint32_t whole = remainder / denominator;
	remainder %= denominator;
	if (whole > 3u)
		return UINT32_MAX;
	for (uint8_t bit = 0u; bit < 30u; bit++) {
		remainder <<= 1;
		quotient <<= 1;
		if (remainder >= denominator) {
			remainder -= denominator;
			quotient |= 1u;
		}
	}
	return (whole << 30) | quotient;
}

static bool divide_i32_q30(int32_t numerator, uint32_t denominator, int32_t *result)
{
	if (denominator == 0u || result == NULL)
		return false;
	bool negative = numerator < 0;
	uint32_t magnitude = negative ? (uint32_t)(-(int64_t)numerator) : (uint32_t)numerator;
	uint32_t whole = magnitude / denominator;
	uint32_t remainder = magnitude % denominator;
	if (whole > 1u)
		return false;
	uint64_t fraction = 0u;
	for (uint8_t bit = 0u; bit < 30u; bit++) {
		remainder <<= 1;
		fraction <<= 1;
		if (remainder >= denominator) {
			remainder -= denominator;
			fraction |= 1u;
		}
	}
	uint64_t quotient = ((uint64_t)whole << 30) | fraction;
	if (quotient > (negative ? (uint64_t)INT32_MAX + 1u : (uint64_t)INT32_MAX))
		return false;
	*result = negative ? (quotient == (uint64_t)INT32_MAX + 1u ? INT32_MIN : -(int32_t)quotient) :
		(int32_t)quotient;
	return true;
}

static int32_t q30_mul(int32_t left, int32_t right)
{
	return (int32_t)(((int64_t)left * right) >> 30);
}

static int32_t value_mul_q30(int32_t value, int32_t factor_q30)
{
	return (int32_t)(((int64_t)value * factor_q30) >> 30);
}

static uint64_t integer_sqrt(uint64_t value)
{
	uint64_t remainder = value;
	uint64_t result = 0u;
	uint64_t bit = UINT64_C(1) << 62;
	while (bit > remainder)
		bit >>= 2;
	while (bit != 0u) {
		if (remainder >= result + bit) {
			remainder -= result + bit;
			result = (result >> 1) + bit;
		} else {
			result >>= 1;
		}
		bit >>= 2;
	}
	return result;
}

static int32_t sqrt_q30(int32_t value_q30)
{
	if (value_q30 <= 0)
		return 0;
	return (int32_t)integer_sqrt((uint64_t)(uint32_t)value_q30 << 30);
}

static void phase_sincos(uint32_t phase, int32_t *sine, int32_t *cosine)
{
	uint8_t quadrant = (uint8_t)(phase >> 30);
	int32_t z = (int32_t)(phase & (SAT_PHASE_QUARTER - 1u));
	int32_t x = 652032874;
	int32_t y = 0;
	for (uint8_t i = 0u; i < sizeof(cordic_atan_phase) / sizeof(cordic_atan_phase[0]); i++) {
		int32_t old_x = x;
		if (z >= 0) {
			x -= y >> i;
			y += old_x >> i;
			z -= (int32_t)cordic_atan_phase[i];
		} else {
			x += y >> i;
			y -= old_x >> i;
			z += (int32_t)cordic_atan_phase[i];
		}
	}
	switch (quadrant) {
	case 0u: *sine = y; *cosine = x; break;
	case 1u: *sine = x; *cosine = -y; break;
	case 2u: *sine = -y; *cosine = -x; break;
	default: *sine = -x; *cosine = y; break;
	}
}

static uint32_t phase_atan2(int32_t y, int32_t x)
{
	uint32_t base = 0u;
	if (x == 0 && y == 0)
		return 0u;
	if (x < 0) {
		base = SAT_PHASE_HALF;
		x = -x;
		y = -y;
	}
	int32_t z = 0;
	for (uint8_t i = 0u; i < sizeof(cordic_atan_phase) / sizeof(cordic_atan_phase[0]); i++) {
		int32_t old_x = x;
		if (y > 0) {
			x += y >> i;
			y -= old_x >> i;
			z += (int32_t)cordic_atan_phase[i];
		} else if (y < 0) {
			x -= y >> i;
			y += old_x >> i;
			z -= (int32_t)cordic_atan_phase[i];
		}
	}
	return base + (uint32_t)z;
}

static uint32_t microdegrees_to_phase(int32_t microdegrees)
{
	return (uint32_t)(((int64_t)microdegrees * SAT_MICRODEGREES_PHASE_Q24) >> 24);
}

static uint32_t degrees_e7_to_phase(int32_t degrees_e7)
{
	return (uint32_t)(((int64_t)degrees_e7 * SAT_DEGREES_E7_PHASE_Q24) >> 24);
}

static int32_t phase_rate_q16_to_rad_q30(int32_t rate_phase_s_q16)
{
	return (int32_t)(((int64_t)rate_phase_s_q16 * SAT_PI_OVER_2_Q30) >> 46);
}

static int32_t mean_motion_rad_s_q30(uint32_t mean_motion_rev_day_e8)
{
	return (int32_t)(((uint64_t)mean_motion_rev_day_e8 * SAT_MEAN_MOTION_RAD_Q32) >> 32);
}

static uint32_t mean_motion_phase_s_q8(uint32_t mean_motion_rev_day_e8)
{
	return (uint32_t)(((uint64_t)mean_motion_rev_day_e8 * SAT_MEAN_MOTION_PHASE_Q40) >> 32);
}

static uint16_t fraction_u32_q16(uint32_t numerator, uint32_t denominator)
{
	uint32_t remainder = numerator;
	uint16_t fraction = 0u;
	for (uint8_t bit = 0u; bit < 16u; bit++) {
		remainder <<= 1;
		fraction <<= 1;
		if (remainder >= denominator) {
			remainder -= denominator;
			fraction |= 1u;
		}
	}
	return fraction;
}

static uint64_t divide_u64_u32(uint64_t numerator, uint32_t denominator)
{
	uint32_t remainder = 0u;
	uint64_t quotient = 0u;
	for (int8_t bit = 63; bit >= 0; bit--) {
		remainder = (remainder << 1) | (uint32_t)((numerator >> bit) & 1u);
		quotient <<= 1;
		if (remainder >= denominator) {
			remainder -= denominator;
			quotient |= 1u;
		}
	}
	return quotient;
}

static int64_t phase_advance(int32_t rate_phase_s_q, int32_t elapsed_s,
			     uint16_t epoch_fraction_q16)
{
	return (int64_t)rate_phase_s_q * elapsed_s -
	       (int64_t)rate_phase_s_q * epoch_fraction_q16 / 65536;
}

static int32_t days_q16(int32_t seconds)
{
	int32_t days = seconds / 86400;
	int32_t remainder = seconds % 86400;
	return days * 65536 + remainder * 512 / 675;
}

static bool cube_root_u64(uint64_t value, uint32_t *root)
{
	if (root == NULL || value == 0u)
		return false;
	uint32_t low = 0u, high = 2000001u;
	while (high - low > 1u) {
		uint32_t middle = low + (high - low) / 2u;
		uint64_t cube = (uint64_t)middle * middle * middle;
		if (cube <= value)
			low = middle;
		else
			high = middle;
	}
	*root = low;
	return low != 0u;
}

static bool semi_major_axis_m(uint32_t mean_motion_rev_day_e8, uint32_t *axis_m)
{
	if (axis_m == NULL || mean_motion_rev_day_e8 == 0u)
		return false;
	uint32_t mean_motion_e6 = (mean_motion_rev_day_e8 + 50u) / 100u;
	if (mean_motion_e6 > UINT32_MAX / 100u)
		return false;
	uint32_t scaled_motion = mean_motion_e6 * 100u;
	uint64_t square = (uint64_t)scaled_motion * scaled_motion;
	uint32_t root;
	if (!cube_root_u64(square, &root))
		return false;
	uint64_t base = (uint64_t)SAT_WGS72_SEMI_MAJOR_KM_MILLI * 10000u;
	uint64_t numerator = base * 21u +
		divide_u64_u32(base * 5443469u, 10000000u);
	uint64_t axis_wide = divide_u64_u32(numerator, root);
	if (axis_wide > UINT32_MAX)
		return false;
	uint32_t axis = (uint32_t)axis_wide;
	if (axis == 0u || axis > INT32_MAX / 2)
		return false;
	*axis_m = axis;
	return true;
}

static int64_t days_before_year(uint32_t year)
{
	int64_t days = 0;
	for (uint32_t y = 1970u; y < year; y++)
		days += y % 4u == 0u && (y % 100u != 0u || y % 400u == 0u) ? 366 : 365;
	return days;
}

bool satellite_tle_epoch(const char line1[SATELLITE_TLE_LINE_LENGTH + 1u], int64_t *epoch_s,
			 uint16_t *fraction_q16)
{
	if (line1 == NULL || epoch_s == NULL || fraction_q16 == NULL ||
	    line1[SATELLITE_TLE_LINE_LENGTH] != 0 || !digits(line1 + 18, 5u) || line1[23] != '.' ||
	    !digits(line1 + 24, 8u))
		return false;
	uint32_t short_year = number(line1 + 18, 2u);
	uint32_t year = short_year >= 57u ? 1900u + short_year : 2000u + short_year;
	uint32_t day = number(line1 + 20, 3u);
	if (year < 1970u)
		return false;
	bool leap = year % 4u == 0u && (year % 100u != 0u || year % 400u == 0u);
	if (day == 0u || day > (leap ? 366u : 365u))
		return false;
	uint32_t fraction_digits = number(line1 + 24, 8u);
	uint32_t fractional_numerator = (fraction_digits % 1000000u) * 864u;
	uint32_t fractional_seconds = fraction_digits / 1000000u * 864u +
		fractional_numerator / 1000000u;
	int64_t days = days_before_year(year) + (int64_t)day - 1;
	if (days < 0)
		return false;
	*epoch_s = days * 86400 + fractional_seconds;
	*fraction_q16 = fraction_u32_q16(fractional_numerator % 1000000u, 1000000u);
	return true;
}

static bool parse_elements(satellite_orbit_t *orbit, const satellite_record_t *record)
{
	const char *line1 = record->line1;
	const char *line2 = record->line2;
	if (line1[SATELLITE_TLE_LINE_LENGTH] != 0 || line2[SATELLITE_TLE_LINE_LENGTH] != 0 ||
	    line1[0] != '1' || line1[1] != ' ' ||
	    line2[0] != '2' || line2[1] != ' ' || !digits(line1 + 2, 5u) || !digits(line2 + 2, 5u) ||
	    number(line1 + 2, 5u) != record->catalog_id || number(line2 + 2, 5u) != record->catalog_id ||
	    !checksum(line1) || !checksum(line2) || !digits(line1 + 18, 5u) || line1[23] != '.' ||
	    !digits(line1 + 24, 8u) || line1[32] != ' ' || line1[43] != ' ' || line1[52] != ' ' ||
	    line1[61] != ' ' || line1[63] != ' ' || line2[7] != ' ' || line2[16] != ' ' ||
	    line2[25] != ' ' || line2[33] != ' ' || line2[42] != ' ' || line2[51] != ' ' ||
	    !digits(line2 + 26, 7u))
		return false;

	int64_t epoch_s;
	uint16_t epoch_fraction_q16;
	if (!satellite_tle_epoch(line1, &epoch_s, &epoch_fraction_q16))
		return false;
	if ((uint64_t)epoch_s != record->epoch)
		return false;

	int32_t inclination, raan, argument, anomaly, mean_motion, mean_motion_dot;
	if (!decimal_fixed(line2, 8u, 8u, 1000000u, &inclination) ||
	    !decimal_fixed(line2, 17u, 8u, 1000000u, &raan) ||
	    !decimal_fixed(line2, 34u, 8u, 1000000u, &argument) ||
	    !decimal_fixed(line2, 43u, 8u, 1000000u, &anomaly) ||
	    !decimal_fixed(line2, 52u, 11u, 100000000u, &mean_motion) ||
	    !decimal_fixed(line1, 33u, 10u, 100000000u, &mean_motion_dot))
		return false;
	uint32_t eccentricity_digits = number(line2 + 26, 7u);
	if (inclination < 0 || inclination > 180000000 || raan < 0 || raan >= 360000000 ||
	    argument < 0 || argument >= 360000000 || anomaly < 0 || anomaly >= 360000000 ||
	    mean_motion <= 0 || mean_motion >= 2000000000 ||
	    mean_motion_dot < -SAT_TLE_DOT_MAX_E8 || mean_motion_dot > SAT_TLE_DOT_MAX_E8 ||
	    eccentricity_digits >= 10000000u)
		return false;

	orbit->mean_motion_rev_day_e8 = (uint32_t)mean_motion;
	orbit->mean_motion_dot_half_rev_day2_e8 = mean_motion_dot;
	orbit->eccentricity_q30 = divide_u32_q30(eccentricity_digits, 10000000u);
	orbit->inclination_phase = microdegrees_to_phase(inclination);
	orbit->raan_phase = microdegrees_to_phase(raan);
	orbit->argument_perigee_phase = microdegrees_to_phase(argument);
	orbit->mean_anomaly_phase = microdegrees_to_phase(anomaly);
	orbit->epoch_s = epoch_s;
	orbit->epoch_fraction_q16 = epoch_fraction_q16;
	if (!semi_major_axis_m(orbit->mean_motion_rev_day_e8, &orbit->semi_major_axis_m))
		return false;
	if (((uint64_t)orbit->semi_major_axis_m *
	     (SAT_Q30_ONE + orbit->eccentricity_q30)) >> 30 > INT32_MAX / 2)
		return false;

	int32_t sine_inc, cosine_inc;
	phase_sincos(orbit->inclination_phase, &sine_inc, &cosine_inc);
	int32_t eccentricity_sq = q30_mul((int32_t)orbit->eccentricity_q30,
					   (int32_t)orbit->eccentricity_q30);
	int32_t semilatus_rectum_m = value_mul_q30((int32_t)orbit->semi_major_axis_m,
						      SAT_Q30_ONE - eccentricity_sq);
	if (semilatus_rectum_m <= 0)
		return false;
	int32_t radius_ratio_q30;
	if (!divide_i32_q30((int32_t)SAT_WGS72_RADIUS_M, (uint32_t)semilatus_rectum_m,
			    &radius_ratio_q30))
		return false;
	int32_t ratio_sq_q30 = q30_mul(radius_ratio_q30, radius_ratio_q30);
	int32_t j2_factor_q30 = q30_mul(SAT_EARTH_J2_1P5_Q30, ratio_sq_q30);
	int32_t j2_factor_phase_s_q16 = (int32_t)(((int64_t)mean_motion_phase_s_q8(
		orbit->mean_motion_rev_day_e8) * j2_factor_q30) >> 22);
	int64_t perigee_factor_q30 = ((int64_t)5 *
		q30_mul(cosine_inc, cosine_inc) - SAT_Q30_ONE) / 2;
	orbit->raan_rate_phase_s_q16 = -q30_mul(j2_factor_phase_s_q16, cosine_inc);
	orbit->argument_perigee_rate_phase_s_q16 = (int32_t)(
		(int64_t)j2_factor_phase_s_q16 * perigee_factor_q30 / SAT_Q30_ONE);
	orbit->catalog_id = record->catalog_id;
	return true;
}

bool satellite_orbit_init(satellite_orbit_t *orbit, const satellite_record_t *record)
{
	if (orbit == NULL || record == NULL || record->catalog_id == 0u || record->catalog_id > 99999u)
		return false;
	memset(orbit, 0, sizeof(*orbit));
	if (!parse_elements(orbit, record))
		return false;
	orbit->valid = true;
	return true;
}

bool satellite_orbit_is_fresh(const satellite_orbit_t *orbit, int64_t utc_s)
{
	if (orbit == NULL || !orbit->valid || utc_s < 0)
		return false;
	int64_t age_s = utc_s - orbit->epoch_s;
	return age_s >= -SAT_TLE_FUTURE_SECONDS && age_s <= SAT_TLE_MAX_AGE_SECONDS;
}

bool satellite_orbit_at(satellite_orbit_t *orbit, int64_t utc_s, satellite_orbit_state_t *state)
{
	if (orbit == NULL || state == NULL || !satellite_orbit_is_fresh(orbit, utc_s))
		return false;
	int32_t elapsed_s = (int32_t)(utc_s - orbit->epoch_s);
	int32_t elapsed_days_q16 = days_q16(elapsed_s);
	int64_t motion_delta_e8 = (int64_t)orbit->mean_motion_dot_half_rev_day2_e8 *
		elapsed_days_q16 * 2 / 65536;
	int64_t current_motion_e8 = (int64_t)orbit->mean_motion_rev_day_e8 + motion_delta_e8;
	if (current_motion_e8 <= 0 || current_motion_e8 >= 2000000000)
		return false;
	uint32_t current_motion = (uint32_t)current_motion_e8;
	uint32_t semi_major_axis;
	if (!semi_major_axis_m(current_motion, &semi_major_axis))
		return false;
	if (((uint64_t)semi_major_axis *
	     (SAT_Q30_ONE + orbit->eccentricity_q30)) >> 30 > INT32_MAX / 2)
		return false;

	uint32_t mean_rate_q8 = mean_motion_phase_s_q8(orbit->mean_motion_rev_day_e8);
	int64_t mean_advance_q8 = phase_advance(
		(int32_t)mean_rate_q8, elapsed_s, orbit->epoch_fraction_q16);
	uint32_t mean_phase = orbit->mean_anomaly_phase + (uint32_t)(mean_advance_q8 >> 8);
	int64_t drift_numerator = (int64_t)orbit->mean_motion_dot_half_rev_day2_e8 *
		elapsed_days_q16 * elapsed_days_q16;
	bool drift_negative = drift_numerator < 0;
	uint64_t drift_magnitude = drift_negative ? (uint64_t)(-drift_numerator) :
		(uint64_t)drift_numerator;
	uint32_t drift_phase = (uint32_t)divide_u64_u32(drift_magnitude, 100000000u);
	if (drift_negative)
		drift_phase = 0u - drift_phase;
	mean_phase += drift_phase;

	uint32_t eccentric_anomaly = mean_phase;
	if (orbit->eccentricity_q30 > (uint32_t)SAT_Q30_ONE * 8u / 10u) {
		int32_t sine_sign = (int32_t)orbit->eccentricity_q30;
		int32_t eccentricity_phase = value_mul_q30(sine_sign, SAT_PHASE_PER_RAD_Q30);
		eccentric_anomaly += mean_phase < SAT_PHASE_HALF ?
			(uint32_t)eccentricity_phase : (uint32_t)-eccentricity_phase;
	}
	bool converged = false;
	for (uint8_t iteration = 0u; iteration < 12u; iteration++) {
		int32_t sine_e, cosine_e;
		phase_sincos(eccentric_anomaly, &sine_e, &cosine_e);
		int32_t eccentric_sine_phase = value_mul_q30(
			value_mul_q30(sine_e, SAT_PHASE_PER_RAD_Q30),
			(int32_t)orbit->eccentricity_q30);
		int32_t residual_phase = (int32_t)(eccentric_anomaly - mean_phase) - eccentric_sine_phase;
		int32_t derivative_q30 = SAT_Q30_ONE -
			q30_mul((int32_t)orbit->eccentricity_q30, cosine_e);
		int32_t correction_phase;
		if (derivative_q30 <= 0 ||
		    !divide_i32_q30(residual_phase, (uint32_t)derivative_q30, &correction_phase))
			return false;
		eccentric_anomaly -= (uint32_t)correction_phase;
		if (correction_phase >= -1 && correction_phase <= 1) {
			converged = true;
			break;
		}
	}
	if (!converged)
		return false;

	int32_t sine_e, cosine_e, sine_inc, cosine_inc, sine_raan, cosine_raan, sine_arg, cosine_arg;
	phase_sincos(eccentric_anomaly, &sine_e, &cosine_e);
	phase_sincos(orbit->inclination_phase, &sine_inc, &cosine_inc);
	uint32_t raan_phase = orbit->raan_phase +
		(uint32_t)(phase_advance(orbit->raan_rate_phase_s_q16, elapsed_s,
					orbit->epoch_fraction_q16) >> 16);
	uint32_t argument_phase = orbit->argument_perigee_phase +
		(uint32_t)(phase_advance(orbit->argument_perigee_rate_phase_s_q16, elapsed_s,
					orbit->epoch_fraction_q16) >> 16);
	phase_sincos(raan_phase, &sine_raan, &cosine_raan);
	phase_sincos(argument_phase, &sine_arg, &cosine_arg);

	int32_t eccentricity_q30 = (int32_t)orbit->eccentricity_q30;
	int32_t beta_q30 = sqrt_q30(SAT_Q30_ONE - q30_mul(eccentricity_q30, eccentricity_q30));
	int32_t x_orbit_m = value_mul_q30((int32_t)semi_major_axis, cosine_e - eccentricity_q30);
	int32_t y_orbit_m = value_mul_q30(value_mul_q30((int32_t)semi_major_axis, beta_q30), sine_e);
	int32_t denominator_q30 = SAT_Q30_ONE - q30_mul(eccentricity_q30, cosine_e);
	int32_t eccentric_rate_q30;
	if (denominator_q30 <= 0 ||
	    !divide_i32_q30(mean_motion_rad_s_q30(current_motion), (uint32_t)denominator_q30,
			    &eccentric_rate_q30))
		return false;
	int32_t vx_orbit_cm_s = -value_mul_q30(
		value_mul_q30((int32_t)semi_major_axis, sine_e), eccentric_rate_q30) * 100;
	int32_t vy_orbit_cm_s = value_mul_q30(
		value_mul_q30(value_mul_q30((int32_t)semi_major_axis, beta_q30), cosine_e),
		eccentric_rate_q30) * 100;

	int32_t p_x = q30_mul(cosine_raan, cosine_arg) -
		q30_mul(q30_mul(sine_raan, sine_arg), cosine_inc);
	int32_t p_y = q30_mul(sine_raan, cosine_arg) +
		q30_mul(q30_mul(cosine_raan, sine_arg), cosine_inc);
	int32_t p_z = q30_mul(sine_arg, sine_inc);
	int32_t q_x = -q30_mul(cosine_raan, sine_arg) -
		q30_mul(q30_mul(sine_raan, cosine_arg), cosine_inc);
	int32_t q_y = -q30_mul(sine_raan, sine_arg) +
		q30_mul(q30_mul(cosine_raan, cosine_arg), cosine_inc);
	int32_t q_z = q30_mul(cosine_arg, sine_inc);
	state->position_m[0] = value_mul_q30(x_orbit_m, p_x) + value_mul_q30(y_orbit_m, q_x);
	state->position_m[1] = value_mul_q30(x_orbit_m, p_y) + value_mul_q30(y_orbit_m, q_y);
	state->position_m[2] = value_mul_q30(x_orbit_m, p_z) + value_mul_q30(y_orbit_m, q_z);

	int32_t normal_x = q30_mul(sine_raan, sine_inc);
	int32_t normal_y = -q30_mul(cosine_raan, sine_inc);
	int32_t normal_z = cosine_inc;
	int32_t raan_rate_rad_s_q30 = phase_rate_q16_to_rad_q30(orbit->raan_rate_phase_s_q16);
	int32_t arg_rate_rad_s_q30 =
		phase_rate_q16_to_rad_q30(orbit->argument_perigee_rate_phase_s_q16);
	int32_t node_vx_cm_s = -value_mul_q30(state->position_m[1], raan_rate_rad_s_q30) * 100;
	int32_t node_vy_cm_s = value_mul_q30(state->position_m[0], raan_rate_rad_s_q30) * 100;
	int32_t perigee_vx_cm_s = value_mul_q30(
		value_mul_q30(state->position_m[2], normal_y) -
		value_mul_q30(state->position_m[1], normal_z), arg_rate_rad_s_q30) * 100;
	int32_t perigee_vy_cm_s = value_mul_q30(
		value_mul_q30(state->position_m[0], normal_z) -
		value_mul_q30(state->position_m[2], normal_x), arg_rate_rad_s_q30) * 100;
	int32_t perigee_vz_cm_s = value_mul_q30(
		value_mul_q30(state->position_m[1], normal_x) -
		value_mul_q30(state->position_m[0], normal_y), arg_rate_rad_s_q30) * 100;
	state->velocity_cm_s[0] = value_mul_q30(vx_orbit_cm_s, p_x) +
		value_mul_q30(vy_orbit_cm_s, q_x) + node_vx_cm_s + perigee_vx_cm_s;
	state->velocity_cm_s[1] = value_mul_q30(vx_orbit_cm_s, p_y) +
		value_mul_q30(vy_orbit_cm_s, q_y) + node_vy_cm_s + perigee_vy_cm_s;
	state->velocity_cm_s[2] = value_mul_q30(vx_orbit_cm_s, p_z) +
		value_mul_q30(vy_orbit_cm_s, q_z) + perigee_vz_cm_s;
	return true;
}

static int16_t signed_phase_to_tenths(uint32_t phase)
{
	int32_t signed_phase = (int32_t)phase;
	bool negative = signed_phase < 0;
	uint32_t magnitude = negative ? (uint32_t)(-(int64_t)signed_phase) : (uint32_t)signed_phase;
	uint16_t tenths = (uint16_t)(((uint64_t)magnitude * 3600u + (UINT64_C(1) << 31)) >> 32);
	return negative ? -(int16_t)tenths : (int16_t)tenths;
}

static uint16_t phase_to_azimuth_tenths(uint32_t phase)
{
	uint16_t tenths = (uint16_t)(((uint64_t)phase * 3600u + (UINT64_C(1) << 31)) >> 32);
	return tenths >= 3600u ? 0u : tenths;
}

static uint32_t greenwich_sidereal_phase(int64_t utc_s)
{
	int32_t delta_s = (int32_t)(utc_s - INT64_C(946728000));
	int32_t days = days_q16(delta_s);
	int32_t centuries_q16 = days / 36525;
	int32_t centuries_sq_q16 =
		(int32_t)(((int64_t)centuries_q16 * centuries_q16) >> 16);
	uint32_t phase = microdegrees_to_phase(280460618);
	phase += (uint32_t)(((int64_t)delta_s * SAT_SIDEREAL_RATE_PHASE_S_Q16) >> 16);
	phase += (uint32_t)(((int64_t)centuries_sq_q16 * SAT_GST_T2_PHASE) >> 16);
	return phase;
}

bool satellite_orbit_observe(satellite_orbit_t *orbit, int64_t utc_s, int32_t latitude_e7,
			    int32_t longitude_e7, int32_t altitude_m,
			    satellite_observation_t *observation)
{
	if (observation == NULL || latitude_e7 < -900000000 || latitude_e7 > 900000000 ||
	    longitude_e7 < -1800000000 || longitude_e7 > 1800000000 ||
	    altitude_m < -1000 || altitude_m > 100000)
		return false;
	satellite_orbit_state_t state;
	if (!satellite_orbit_at(orbit, utc_s, &state))
		return false;

	int32_t sine_lat, cosine_lat, sine_lon, cosine_lon;
	phase_sincos(degrees_e7_to_phase(latitude_e7), &sine_lat, &cosine_lat);
	phase_sincos(degrees_e7_to_phase(longitude_e7), &sine_lon, &cosine_lon);
	int32_t sine_lat_sq = q30_mul(sine_lat, sine_lat);
	int32_t prime_vertical_root = sqrt_q30(SAT_Q30_ONE -
		q30_mul(SAT_WGS84_E2_Q30, sine_lat_sq));
	int32_t inverse_root_q30;
	if (prime_vertical_root <= 0 ||
	    !divide_i32_q30(SAT_Q30_ONE, (uint32_t)prime_vertical_root, &inverse_root_q30))
		return false;
	int32_t prime_vertical_m = value_mul_q30((int32_t)SAT_WGS84_RADIUS_M, inverse_root_q30);
	int32_t observer_radius_m = prime_vertical_m + altitude_m;
	int32_t observer_x_m = value_mul_q30(value_mul_q30(observer_radius_m, cosine_lat), cosine_lon);
	int32_t observer_y_m = value_mul_q30(value_mul_q30(observer_radius_m, cosine_lat), sine_lon);
	int32_t observer_z_m = value_mul_q30(
		prime_vertical_m - value_mul_q30(prime_vertical_m, SAT_WGS84_E2_Q30) + altitude_m,
		sine_lat);

	int32_t sine_theta, cosine_theta;
	phase_sincos(greenwich_sidereal_phase(utc_s), &sine_theta, &cosine_theta);
	int32_t satellite_x_m = value_mul_q30(state.position_m[0], cosine_theta) +
		value_mul_q30(state.position_m[1], sine_theta);
	int32_t satellite_y_m = -value_mul_q30(state.position_m[0], sine_theta) +
		value_mul_q30(state.position_m[1], cosine_theta);
	int32_t satellite_z_m = state.position_m[2];
	int32_t inertial_vx_cm_s = state.velocity_cm_s[0] +
		value_mul_q30(value_mul_q30(state.position_m[1], SAT_EARTH_RATE_RAD_S_Q30), 100);
	int32_t inertial_vy_cm_s = state.velocity_cm_s[1] -
		value_mul_q30(value_mul_q30(state.position_m[0], SAT_EARTH_RATE_RAD_S_Q30), 100);
	int32_t satellite_vx_cm_s = value_mul_q30(inertial_vx_cm_s, cosine_theta) +
		value_mul_q30(inertial_vy_cm_s, sine_theta);
	int32_t satellite_vy_cm_s = -value_mul_q30(inertial_vx_cm_s, sine_theta) +
		value_mul_q30(inertial_vy_cm_s, cosine_theta);

	int32_t dx_m = satellite_x_m - observer_x_m;
	int32_t dy_m = satellite_y_m - observer_y_m;
	int32_t dz_m = satellite_z_m - observer_z_m;
	int32_t east_m = -value_mul_q30(dx_m, sine_lon) + value_mul_q30(dy_m, cosine_lon);
	int32_t north_m = -value_mul_q30(value_mul_q30(dx_m, sine_lat), cosine_lon) -
		value_mul_q30(value_mul_q30(dy_m, sine_lat), sine_lon) + value_mul_q30(dz_m, cosine_lat);
	int32_t up_m = value_mul_q30(value_mul_q30(dx_m, cosine_lat), cosine_lon) +
		value_mul_q30(value_mul_q30(dy_m, cosine_lat), sine_lon) + value_mul_q30(dz_m, sine_lat);
	uint64_t range_squared = (uint64_t)((int64_t)dx_m * dx_m) +
		(uint64_t)((int64_t)dy_m * dy_m) + (uint64_t)((int64_t)dz_m * dz_m);
	uint64_t range_root = integer_sqrt(range_squared);
	if (range_root > UINT32_MAX)
		return false;
	uint32_t range_m = (uint32_t)range_root;
	if (range_m == 0u)
		return false;
	uint32_t horizontal_m = (uint32_t)integer_sqrt(
		(uint64_t)((int64_t)east_m * east_m) + (uint64_t)((int64_t)north_m * north_m));
	int32_t direction_x_q30, direction_y_q30, direction_z_q30;
	if (!divide_i32_q30(dx_m, range_m, &direction_x_q30) ||
	    !divide_i32_q30(dy_m, range_m, &direction_y_q30) ||
	    !divide_i32_q30(dz_m, range_m, &direction_z_q30))
		return false;
	observation->azimuth_tenths_deg = phase_to_azimuth_tenths(phase_atan2(east_m, north_m));
	observation->elevation_tenths_deg = signed_phase_to_tenths(phase_atan2(up_m, (int32_t)horizontal_m));
	observation->range_m = range_m;
	observation->range_rate_cm_s = value_mul_q30(direction_x_q30, satellite_vx_cm_s) +
		value_mul_q30(direction_y_q30, satellite_vy_cm_s) +
		value_mul_q30(direction_z_q30, state.velocity_cm_s[2]);
	return true;
}
