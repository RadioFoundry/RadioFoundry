// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// Lightweight two-body Kepler propagation from TLE mean elements and topocentric observation.
#include <math.h>
#include <string.h>
#include "satellite/orbit.h"

#define SAT_TLE_MAX_AGE_DAYS 30
#define SAT_TLE_FUTURE_DAYS 1
#define SAT_MICROSECONDS_PER_SECOND INT64_C(1000000)
#define SAT_MICROSECONDS_PER_DAY (INT64_C(86400) * SAT_MICROSECONDS_PER_SECOND)
#define SAT_SECONDS_PER_DAY 86400.0
#define SAT_PI 3.14159265358979323846
#define SAT_TWO_PI 6.28318530717958647693
#define SAT_WGS72_MU_KM3_S2 398600.8
#define SAT_WGS72_RADIUS_KM 6378.135
#define SAT_EARTH_J2 1.08263e-3
#define SAT_WGS84_A_KM 6378.137
#define SAT_WGS84_E2 0.0066943799901413165
#define SAT_EARTH_RATE_RAD_S 7.29211514670698e-5

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
	uint32_t value = 0;
	for (size_t i = 0; i < n; i++)
		value = value * 10u + (uint32_t)(p[i] - '0');
	return value;
}

static bool checksum(const char *line)
{
	if (line[68] < '0' || line[68] > '9')
		return false;
	unsigned sum = 0;
	for (size_t i = 0; i < 68u; i++) {
		if (line[i] >= '0' && line[i] <= '9')
			sum += (unsigned)(line[i] - '0');
		else if (line[i] == '-')
			sum++;
	}
	return sum % 10u == (unsigned)(line[68] - '0');
}

static bool decimal(const char *line, size_t offset, size_t length, double *value)
{
	if (length == 0u)
		return false;
	size_t i = 0u;
	while (i < length && line[offset + i] == ' ')
		i++;
	bool negative = false;
	if (i < length && (line[offset + i] == '-' || line[offset + i] == '+')) {
		negative = line[offset + i] == '-';
		i++;
	}
	uint64_t integer = 0u, fraction = 0u;
	double scale = 1.0;
	bool has_digit = false;
	while (i < length && line[offset + i] >= '0' && line[offset + i] <= '9') {
		integer = integer * 10u + (uint64_t)(line[offset + i] - '0');
		has_digit = true;
		i++;
	}
	if (i < length && line[offset + i] == '.') {
		i++;
		while (i < length && line[offset + i] >= '0' && line[offset + i] <= '9') {
			fraction = fraction * 10u + (uint64_t)(line[offset + i] - '0');
			scale *= 10.0;
			has_digit = true;
			i++;
		}
	}
	while (i < length && line[offset + i] == ' ')
		i++;
	if (!has_digit || i != length)
		return false;
	*value = (double)integer + (double)fraction / scale;
	if (negative)
		*value = -*value;
	return true;
}

static bool leap_year(uint32_t year)
{
	return year % 4u == 0u && (year % 100u != 0u || year % 400u == 0u);
}

static int64_t days_before_year(uint32_t year)
{
	int64_t days = 0;
	if (year >= 1970u) {
		for (uint32_t y = 1970u; y < year; y++)
			days += leap_year(y) ? 366 : 365;
	} else {
		for (uint32_t y = year; y < 1970u; y++)
			days -= leap_year(y) ? 366 : 365;
	}
	return days;
}

static double greenwich_sidereal_time(double julian_date)
{
	double days = julian_date - 2451545.0;
	double centuries = days / 36525.0;
	double degrees = 280.46061837 + 360.98564736629 * days +
		0.000387933 * centuries * centuries -
		centuries * centuries * centuries / 38710000.0;
	degrees = fmod(degrees, 360.0);
	if (degrees < 0.0)
		degrees += 360.0;
	return degrees * (SAT_TWO_PI / 360.0);
}

__attribute__((noinline)) static bool parse_elements(satellite_orbit_t *orbit, const satellite_record_t *record)
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

	uint32_t short_year = number(line1 + 18, 2u);
	uint32_t year = short_year >= 57u ? 1900u + short_year : 2000u + short_year;
	uint32_t day = number(line1 + 20, 3u);
	uint32_t day_max = leap_year(year) ? 366u : 365u;
	if (day == 0u || day > day_max)
		return false;
	uint32_t fraction_digits = number(line1 + 24, 8u);
	double fraction = (double)fraction_digits / 100000000.0;
	int64_t unix_days = days_before_year(year) + (int64_t)day - 1;
	uint64_t fraction_us = (uint64_t)fraction_digits * UINT64_C(86400000000) / UINT64_C(100000000);
	int64_t epoch_us = (unix_days * 86400 + (int64_t)(fraction * 86400.0)) * SAT_MICROSECONDS_PER_SECOND +
			(int64_t)fraction_us % SAT_MICROSECONDS_PER_SECOND;
	if (epoch_us < 0 || (uint64_t)(epoch_us / SAT_MICROSECONDS_PER_SECOND) != record->epoch)
		return false;

	double inclination, raan, argument, anomaly, mean_motion, mean_motion_dot;
	if (!decimal(line2, 8u, 8u, &inclination) || !decimal(line2, 17u, 8u, &raan) ||
	    !decimal(line2, 34u, 8u, &argument) || !decimal(line2, 43u, 8u, &anomaly) ||
	    !decimal(line2, 52u, 11u, &mean_motion) || !decimal(line1, 33u, 10u, &mean_motion_dot))
		return false;
	double eccentricity = (double)number(line2 + 26, 7u) / 10000000.0;
	if (inclination < 0.0 || inclination > 180.0 || raan < 0.0 || raan >= 360.0 ||
	    eccentricity < 0.0 || eccentricity >= 1.0 || argument < 0.0 || argument >= 360.0 ||
	    anomaly < 0.0 || anomaly >= 360.0 || mean_motion <= 0.0 || mean_motion >= 20.0)
		return false;

	double radians_per_degree = SAT_TWO_PI / 360.0;
	orbit->eccentricity = eccentricity;
	orbit->inclination_rad = inclination * radians_per_degree;
	orbit->raan_rad = raan * radians_per_degree;
	orbit->argument_perigee_rad = argument * radians_per_degree;
	orbit->mean_anomaly_rad = anomaly * radians_per_degree;
	orbit->mean_motion_rad_s = mean_motion * SAT_TWO_PI / SAT_SECONDS_PER_DAY;
	/* TLE line 1 stores one half of the first mean-motion derivative. */
	orbit->mean_motion_dot_rad_s2 = 2.0 * mean_motion_dot * SAT_TWO_PI /
		(SAT_SECONDS_PER_DAY * SAT_SECONDS_PER_DAY);
	orbit->semi_major_axis_km = cbrt(SAT_WGS72_MU_KM3_S2 /
		(orbit->mean_motion_rad_s * orbit->mean_motion_rad_s));
	double semilatus_rectum_km = orbit->semi_major_axis_km * (1.0 - eccentricity * eccentricity);
	double cos_inclination = cos(orbit->inclination_rad);
	double j2_factor = 1.5 * SAT_EARTH_J2 * orbit->mean_motion_rad_s *
		(SAT_WGS72_RADIUS_KM / semilatus_rectum_km) *
		(SAT_WGS72_RADIUS_KM / semilatus_rectum_km);
	orbit->raan_rate_rad_s = -j2_factor * cos_inclination;
	orbit->argument_perigee_rate_rad_s = 0.5 * j2_factor * (5.0 * cos_inclination * cos_inclination - 1.0);
	if (!isfinite(orbit->semi_major_axis_km) || !isfinite(orbit->mean_motion_dot_rad_s2) ||
	    !isfinite(orbit->raan_rate_rad_s) || !isfinite(orbit->argument_perigee_rate_rad_s) ||
	    orbit->semi_major_axis_km <= 0.0)
		return false;
	orbit->epoch_us = epoch_us;
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

bool satellite_orbit_at(satellite_orbit_t *orbit, int64_t utc_us, satellite_orbit_state_t *state)
{
	if (orbit == NULL || !orbit->valid || state == NULL || utc_us < 0 ||
	    utc_us > INT64_C(253402300799999999))
		return false;
	double elapsed_s = (double)(utc_us - orbit->epoch_us) / 1000000.0;
	double mean_motion = orbit->mean_motion_rad_s + orbit->mean_motion_dot_rad_s2 * elapsed_s;
	if (!isfinite(mean_motion) || mean_motion <= 0.0)
		return false;
	double semi_major_axis_km = cbrt(SAT_WGS72_MU_KM3_S2 / (mean_motion * mean_motion));
	double mean_anomaly = orbit->mean_anomaly_rad + orbit->mean_motion_rad_s * elapsed_s +
		0.5 * orbit->mean_motion_dot_rad_s2 * elapsed_s * elapsed_s;
	mean_anomaly = fmod(mean_anomaly, SAT_TWO_PI);
	if (mean_anomaly < 0.0)
		mean_anomaly += SAT_TWO_PI;
	double eccentric_anomaly = mean_anomaly;
	if (orbit->eccentricity > 0.8)
		eccentric_anomaly += orbit->eccentricity * (mean_anomaly < 3.14159265358979323846 ? 1.0 : -1.0);
	bool converged = false;
	for (uint8_t iteration = 0u; iteration < 12u; iteration++) {
		double correction = (eccentric_anomaly - orbit->eccentricity * sin(eccentric_anomaly) - mean_anomaly) /
			(1.0 - orbit->eccentricity * cos(eccentric_anomaly));
		eccentric_anomaly -= correction;
		if (fabs(correction) < 1e-12) {
			converged = true;
			break;
		}
	}
	if (!converged)
		return false;
	double cos_e = cos(eccentric_anomaly), sin_e = sin(eccentric_anomaly);
	double beta = sqrt(1.0 - orbit->eccentricity * orbit->eccentricity);
	double denominator = 1.0 - orbit->eccentricity * cos_e;
	double x_orbit = semi_major_axis_km * (cos_e - orbit->eccentricity);
	double y_orbit = semi_major_axis_km * beta * sin_e;
	double semi_major_axis_rate = -(2.0 / 3.0) * semi_major_axis_km *
		orbit->mean_motion_dot_rad_s2 / mean_motion;
	double eccentric_anomaly_rate = mean_motion / denominator;
	double vx_orbit = semi_major_axis_rate * (cos_e - orbit->eccentricity) -
		semi_major_axis_km * sin_e * eccentric_anomaly_rate;
	double vy_orbit = semi_major_axis_rate * beta * sin_e +
		semi_major_axis_km * beta * cos_e * eccentric_anomaly_rate;
	double raan = orbit->raan_rad + orbit->raan_rate_rad_s * elapsed_s;
	double argument = orbit->argument_perigee_rad + orbit->argument_perigee_rate_rad_s * elapsed_s;
	double cos_raan = cos(raan), sin_raan = sin(raan);
	double cos_arg = cos(argument), sin_arg = sin(argument);
	double cos_inc = cos(orbit->inclination_rad), sin_inc = sin(orbit->inclination_rad);
	double p_x = cos_raan * cos_arg - sin_raan * sin_arg * cos_inc;
	double p_y = sin_raan * cos_arg + cos_raan * sin_arg * cos_inc;
	double p_z = sin_arg * sin_inc;
	double q_x = -cos_raan * sin_arg - sin_raan * cos_arg * cos_inc;
	double q_y = -sin_raan * sin_arg + cos_raan * cos_arg * cos_inc;
	double q_z = cos_arg * sin_inc;
	state->position_km[0] = p_x * x_orbit + q_x * y_orbit;
	state->position_km[1] = p_y * x_orbit + q_y * y_orbit;
	state->position_km[2] = p_z * x_orbit + q_z * y_orbit;
	double node_vx = -orbit->raan_rate_rad_s * state->position_km[1];
	double node_vy = orbit->raan_rate_rad_s * state->position_km[0];
	double normal_x = sin_raan * sin_inc, normal_y = -cos_raan * sin_inc, normal_z = cos_inc;
	double perigee_vx = orbit->argument_perigee_rate_rad_s *
		(normal_y * state->position_km[2] - normal_z * state->position_km[1]);
	double perigee_vy = orbit->argument_perigee_rate_rad_s *
		(normal_z * state->position_km[0] - normal_x * state->position_km[2]);
	double perigee_vz = orbit->argument_perigee_rate_rad_s *
		(normal_x * state->position_km[1] - normal_y * state->position_km[0]);
	state->velocity_km_s[0] = p_x * vx_orbit + q_x * vy_orbit + node_vx + perigee_vx;
	state->velocity_km_s[1] = p_y * vx_orbit + q_y * vy_orbit + node_vy + perigee_vy;
	state->velocity_km_s[2] = p_z * vx_orbit + q_z * vy_orbit + perigee_vz;
	for (uint8_t i = 0; i < 3u; i++) {
		if (!isfinite(state->position_km[i]) || !isfinite(state->velocity_km_s[i]))
			return false;
	}
	return true;
}

bool satellite_orbit_is_fresh(const satellite_orbit_t *orbit, int64_t utc_us)
{
	if (orbit == NULL || !orbit->valid || utc_us < 0)
		return false;
	int64_t age_us = utc_us - orbit->epoch_us;
	return age_us >= -(int64_t)SAT_TLE_FUTURE_DAYS * SAT_MICROSECONDS_PER_DAY &&
	       age_us <= (int64_t)SAT_TLE_MAX_AGE_DAYS * SAT_MICROSECONDS_PER_DAY;
}

bool satellite_orbit_observe(satellite_orbit_t *orbit, int64_t utc_us, int32_t latitude_e7, int32_t longitude_e7,
			    int32_t altitude_m, satellite_observation_t *observation)
{
	if (observation == NULL || latitude_e7 < -900000000 || latitude_e7 > 900000000 ||
	    longitude_e7 < -1800000000 || longitude_e7 > 1800000000 || altitude_m < -1000 || altitude_m > 100000)
		return false;
	satellite_orbit_state_t state;
	if (!satellite_orbit_at(orbit, utc_us, &state))
		return false;
	const double radians = SAT_PI / 180.0;
	double latitude = (double)latitude_e7 / 10000000.0 * radians;
	double longitude = (double)longitude_e7 / 10000000.0 * radians;
	double sin_lat = sin(latitude), cos_lat = cos(latitude);
	double sin_lon = sin(longitude), cos_lon = cos(longitude);
	double altitude_km = (double)altitude_m / 1000.0;
	double prime_vertical = SAT_WGS84_A_KM / sqrt(1.0 - SAT_WGS84_E2 * sin_lat * sin_lat);
	double observer_x = (prime_vertical + altitude_km) * cos_lat * cos_lon;
	double observer_y = (prime_vertical + altitude_km) * cos_lat * sin_lon;
	double observer_z = (prime_vertical * (1.0 - SAT_WGS84_E2) + altitude_km) * sin_lat;
	double jd = 2440587.5 + (double)utc_us / (double)SAT_MICROSECONDS_PER_DAY;
	double theta = greenwich_sidereal_time(jd);
	double cos_theta = cos(theta), sin_theta = sin(theta);
	double sat_x = cos_theta * state.position_km[0] + sin_theta * state.position_km[1];
	double sat_y = -sin_theta * state.position_km[0] + cos_theta * state.position_km[1];
	double sat_z = state.position_km[2];
	double inertial_vx = state.velocity_km_s[0] + SAT_EARTH_RATE_RAD_S * state.position_km[1];
	double inertial_vy = state.velocity_km_s[1] - SAT_EARTH_RATE_RAD_S * state.position_km[0];
	double sat_vx = cos_theta * inertial_vx + sin_theta * inertial_vy;
	double sat_vy = -sin_theta * inertial_vx + cos_theta * inertial_vy;
	double dx = sat_x - observer_x, dy = sat_y - observer_y, dz = sat_z - observer_z;
	double east = -sin_lon * dx + cos_lon * dy;
	double north = -sin_lat * cos_lon * dx - sin_lat * sin_lon * dy + cos_lat * dz;
	double up = cos_lat * cos_lon * dx + cos_lat * sin_lon * dy + sin_lat * dz;
	double range = sqrt(dx * dx + dy * dy + dz * dz);
	if (!isfinite(range) || range <= 0.0)
		return false;
	double azimuth = atan2(east, north) / radians;
	if (azimuth < 0.0)
		azimuth += 360.0;
	observation->azimuth_deg = azimuth;
	observation->elevation_deg = atan2(up, sqrt(east * east + north * north)) / radians;
	observation->range_km = range;
	observation->range_rate_m_s = (dx * sat_vx + dy * sat_vy + dz * state.velocity_km_s[2]) / range * 1000.0;
	return isfinite(observation->azimuth_deg) && isfinite(observation->elevation_deg) &&
	       isfinite(observation->range_rate_m_s);
}