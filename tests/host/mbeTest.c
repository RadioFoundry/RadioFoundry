// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
// The MBE synthesis core and the full-rate vocoder front end (core/mbe): the dequantizer against golden vectors
// from a floating-point reference decoder (numbers only), the pitch code table, the synthesis of a tone, of noise and of
// silence, concealment (repeat, then mute), the level against the reference's, and extreme inputs. Built with
// -fsanitize=address,undefined by the host test run.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbe/imbe.h"

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t rng_state = 0xC0FFEE;
static uint32_t rng(void)
{
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

// Golden vectors: 88-bit frames (random bits with valid pitch codes) in five sequences, each from a reset state, with
// the harmonic count, the fundamental as a phase step (2^32 = one turn), the voicing bits (bit l - 1 for harmonic l) and
// the log2 amplitudes (Q11) that the reference decoder gave. The last two tables are steady voiced sequences with the
// reference's frame rms, for the level.
static const uint8_t GOLD_BITS[][IMBE_BYTES] = {
	{ 0x2E, 0xBD, 0xFD, 0xC4, 0x47, 0x41, 0xD7, 0xA6, 0x1F, 0x30, 0x3A },
	{ 0x78, 0xD7, 0x75, 0x5F, 0x61, 0x6D, 0x7E, 0xB7, 0xD8, 0x43, 0xDE },
	{ 0x42, 0xBA, 0x33, 0xE6, 0xB5, 0x43, 0x10, 0xF6, 0x2E, 0xE1, 0xFD },
	{ 0xC5, 0xF1, 0x5E, 0x96, 0x24, 0xAE, 0x48, 0x97, 0x32, 0x5A, 0x37 },
	{ 0x1B, 0x9B, 0xE6, 0x14, 0x01, 0xC1, 0x1E, 0xA2, 0xA7, 0xE9, 0x3E },
	{ 0x2A, 0xAC, 0x91, 0x5B, 0x4C, 0x80, 0xF2, 0x01, 0xF9, 0x7D, 0x2F },
	{ 0xBF, 0x8C, 0xD2, 0x7C, 0x9B, 0x2C, 0x85, 0xE2, 0x05, 0x4B, 0xF1 },
	{ 0x34, 0x9D, 0x5F, 0x9E, 0x6C, 0x5D, 0xED, 0xB2, 0x16, 0xF1, 0x11 },
	{ 0x94, 0x66, 0xD7, 0xA9, 0xFD, 0x89, 0x1E, 0x48, 0x9D, 0xD3, 0xF9 },
	{ 0x91, 0x7E, 0x82, 0xC4, 0x71, 0x44, 0xD6, 0xC6, 0xA6, 0x19, 0xF6 },
	{ 0x41, 0x9C, 0x3C, 0x23, 0x18, 0xB1, 0x46, 0x8E, 0xFF, 0xC4, 0xA3 },
	{ 0x4F, 0xB0, 0xAE, 0x2A, 0xCD, 0xA2, 0x9A, 0x5F, 0x08, 0xF7, 0x67 },
	{ 0x89, 0x09, 0x87, 0x15, 0x7F, 0xD5, 0xF1, 0x0F, 0x5F, 0xD7, 0x03 },
	{ 0x97, 0x9E, 0x6F, 0x3D, 0x3E, 0xE5, 0x8E, 0x0A, 0x9B, 0x8B, 0x32 },
	{ 0x34, 0xFC, 0x96, 0x4C, 0x61, 0xD2, 0xAD, 0xE6, 0x93, 0xC2, 0x79 },
	{ 0x35, 0x0A, 0x92, 0xA3, 0xBD, 0x61, 0xFD, 0x09, 0xA7, 0xD4, 0x80 },
	{ 0x96, 0xEF, 0x48, 0xD1, 0xC1, 0x15, 0x24, 0xA8, 0xCB, 0x9D, 0xAE },
	{ 0x9D, 0x90, 0x6A, 0x19, 0xFA, 0xCF, 0xBE, 0xFB, 0x82, 0x0C, 0x98 },
	{ 0x9A, 0x90, 0x09, 0x85, 0x8E, 0x06, 0xFD, 0xB6, 0x22, 0x6B, 0x67 },
	{ 0x15, 0x7E, 0x69, 0xA9, 0x6F, 0x83, 0x11, 0x4C, 0x89, 0x52, 0xE1 },
	{ 0x79, 0x8D, 0xE7, 0x21, 0x66, 0xF4, 0xC4, 0xE2, 0xCA, 0xAB, 0xA9 },
	{ 0xBF, 0x7E, 0xD4, 0x4E, 0xEC, 0xF0, 0x26, 0x45, 0x02, 0xF8, 0x27 },
	{ 0x66, 0xDE, 0x40, 0x3B, 0xEC, 0x4C, 0x05, 0x09, 0xC2, 0x5C, 0xD4 },
	{ 0x22, 0x3C, 0xF4, 0x68, 0xE7, 0xC6, 0x8C, 0xB5, 0xE9, 0x55, 0x74 },
	{ 0xA4, 0xA8, 0x16, 0xF2, 0x0F, 0x00, 0x5F, 0x3F, 0x08, 0xD6, 0xF8 },
	{ 0x7F, 0x2F, 0x20, 0xF5, 0x1A, 0x69, 0xA1, 0xB0, 0xE4, 0x05, 0x9F },
	{ 0x46, 0xD2, 0xE1, 0x46, 0x14, 0x76, 0xF6, 0xD3, 0x41, 0xF0, 0x76 },
	{ 0x56, 0x92, 0xB2, 0x30, 0xEB, 0x8F, 0x69, 0xA6, 0xEB, 0xBE, 0x36 },
	{ 0x87, 0xD0, 0x24, 0xC7, 0x0F, 0x58, 0x8A, 0x0A, 0xFF, 0x2D, 0xFE },
	{ 0x41, 0xA9, 0x2F, 0xA9, 0xCD, 0x74, 0x32, 0xCB, 0xA6, 0x56, 0x89 },
	{ 0x45, 0x05, 0x3B, 0x16, 0xA0, 0xB2, 0x71, 0x00, 0xE7, 0xAD, 0x44 },
	{ 0x45, 0x24, 0x82, 0xA1, 0x34, 0xC3, 0x5E, 0x2B, 0x73, 0xB8, 0x54 },
	{ 0x44, 0xFD, 0x54, 0x8C, 0x34, 0x1D, 0xD1, 0x83, 0xF8, 0x79, 0x6D },
	{ 0x47, 0xEC, 0x90, 0xAD, 0xFC, 0x3B, 0xF0, 0x1A, 0x32, 0x5B, 0x6D },
	{ 0x47, 0x3A, 0x18, 0x02, 0xF6, 0x02, 0xA1, 0xC7, 0x8E, 0xBE, 0x65 },
	{ 0x44, 0x9E, 0xD0, 0x07, 0x00, 0x8C, 0x6D, 0x71, 0x6B, 0x69, 0x5C },
	{ 0x47, 0x8F, 0x82, 0x25, 0x91, 0x1B, 0x7E, 0x30, 0x78, 0x94, 0x24 },
	{ 0x46, 0x2F, 0x98, 0x78, 0x36, 0x81, 0x74, 0xE1, 0xC2, 0x99, 0xD4 },
	{ 0x45, 0x3E, 0x0F, 0x91, 0xCA, 0x5D, 0xB8, 0xA5, 0xC8, 0xA3, 0x0C },
	{ 0x44, 0x2D, 0xE3, 0xE2, 0x9E, 0x82, 0xAD, 0x92, 0xCD, 0x8B, 0xBC },
	{ 0xAD, 0x30, 0x81, 0xAC, 0x61, 0xC7, 0x5C, 0xE2, 0xCA, 0x89, 0x80 },
	{ 0x9F, 0x5F, 0x98, 0x7D, 0xD0, 0x29, 0xBA, 0x47, 0x7D, 0xDF, 0xC8 },
	{ 0x20, 0xBA, 0xCB, 0x03, 0x97, 0x46, 0xBA, 0x4C, 0xF4, 0x40, 0x8C },
	{ 0x79, 0x43, 0xA4, 0x03, 0xF6, 0x9E, 0x56, 0xE7, 0x64, 0x9F, 0x52 },
	{ 0x90, 0xA6, 0x82, 0xF3, 0x4D, 0xAB, 0x7A, 0x9A, 0xF6, 0x5E, 0x02 },
	{ 0x6B, 0xFF, 0xC7, 0x42, 0x58, 0x39, 0x21, 0x14, 0x82, 0x4F, 0x46 },
	{ 0x2A, 0xDE, 0x16, 0x58, 0x89, 0x83, 0xE1, 0x03, 0x1F, 0xE0, 0x8E },
	{ 0x58, 0x9E, 0xA8, 0xBE, 0xD7, 0x4E, 0x6C, 0x64, 0xCE, 0x89, 0xFC },
};
static const struct { uint8_t L; uint32_t w0; uint64_t v; uint16_t at; } GOLD[] = {
	{ 19, 101656036u, 0x78E3FULL, 0 },
	{ 37, 52861137u, 0x1FC71FFFF8ULL, 19 },
	{ 24, 81421183u, 0xE00ULL, 56 },
	{ 54, 36016499u, 0x3FFFFE07007038ULL, 80 },
	{ 14, 129171955u, 0x3E00ULL, 134 },
	{ 18, 104120427u, 0xFFFULL, 148 },
	{ 52, 37757954u, 0x1FFE38007ULL, 166 },
	{ 21, 93879069u, 0x3F1FFULL, 218 },
	{ 43, 45812987u, 0x381FFE00ULL, 239 },
	{ 42, 46058631u, 0x3F1F8E3FULL, 282 },
	{ 24, 82200335u, 0x1F8038ULL, 324 },
	{ 26, 72488904u, 0x1C7E07ULL, 348 },
	{ 40, 48668186u, 0xE00FFFULL, 374 },
	{ 43, 45569946u, 0x1FF007ULL, 414 },
	{ 21, 93879069u, 0x3F1C7ULL, 457 },
	{ 21, 93879069u, 0x3FFFFULL, 478 },
	{ 43, 45091524u, 0x1C70381C0ULL, 499 },
	{ 45, 43938285u, 0x1FFFFF1FFFC7ULL, 542 },
	{ 44, 44164188u, 0xFFFC7E3FFFFULL, 587 },
	{ 13, 144368649u, 0xE00ULL, 631 },
	{ 37, 53855390u, 0x1FF03803FULL, 644 },
	{ 52, 37266530u, 0x381F81C0ULL, 681 },
	{ 32, 60706254u, 0xE38000ULL, 733 },
	{ 16, 116869863u, 0xF007ULL, 765 },
	{ 47, 42210982u, 0x7FFFC0FFFE38ULL, 781 },
	{ 37, 51591198u, 0x1FC7E001C7ULL, 828 },
	{ 24, 77736968u, 0x1F8FFFULL, 865 },
	{ 28, 67904625u, 0x7E071F8ULL, 889 },
	{ 39, 49225987u, 0x1C7007ULL, 917 },
	{ 24, 82994541u, 0x1C0FC0ULL, 956 },
	{ 24, 78446896u, 0xE00FF8ULL, 980 },
	{ 24, 78446896u, 0x1FFE38ULL, 1004 },
	{ 24, 78446896u, 0xE00E3FULL, 1028 },
	{ 24, 78446896u, 0xFFFULL, 1052 },
	{ 24, 78446896u, 0xE001C7ULL, 1076 },
	{ 24, 78446896u, 0xE3F1F8ULL, 1100 },
	{ 24, 78446896u, 0x1FFFF8ULL, 1124 },
	{ 24, 78446896u, 0x38FF8ULL, 1148 },
	{ 24, 78446896u, 0x7FC7ULL, 1172 },
	{ 24, 78446896u, 0xE3F1C7ULL, 1196 },
	{ 49, 40614350u, 0x1FF03FE38ULL, 1220 },
	{ 45, 43938285u, 0x381C7FC7ULL, 1269 },
	{ 16, 116869863u, 0x7FC7ULL, 1314 },
	{ 37, 53519845u, 0x1FF1F8E38ULL, 1330 },
	{ 42, 46557914u, 0x3FE071C7FF8ULL, 1367 },
	{ 33, 58634367u, 0xE001C0ULL, 1409 },
	{ 18, 104120427u, 0x1FFULL, 1442 },
	{ 29, 66331543u, 0x1803F1F8ULL, 1460 },
};
static const int16_t GOLD_A[1489] = {
	16842, 13649, 11049, 4421, 5280, 12654, 8381, 10225, 5332, 6939, 7011, 11894, 13805, 13924,
	10266, 12206, 11913, 12541, 11061, 884, 4767, 4204, 66, -1081, 1120, -956, -7888, -5800,
	-5364, -1731, -362, -3563, -4088, -2187, -763, -4865, -6096, -7097, -5991, -5728, -4847, -5280,
	-3006, -2015, -1375, -359, -2313, -5080, -4038, -169, -973, -1212, -1431, -28, 981, -995,
	14525, 12441, 11909, 14733, 6581, 6554, 7365, 10652, 4182, 6024, 4845, 6508, 6598, 6962,
	5855, 8668, 16963, 13787, 12340, 15821, 15335, 14161, 15112, 12028, 3300, 7757, 6953, 5489,
	4930, 6855, 7672, 5982, 7260, 5143, 1676, 1294, 1603, 2306, 1288, 1508, 5041, 5408,
	2038, 702, 196, 2796, 2534, -834, -1483, -1408, -1414, -1123, -703, -770, -808, -461,
	-917, -1651, 41, 2860, 5070, 7967, 7596, 6354, 6057, 5535, 4877, 6893, 9995, 6144,
	5159, 4481, 4825, 5573, 5602, 4553, 2840, 1707, 15031, 19676, 13956, 9018, 14702, 22579,
	20725, 24171, 15632, 23804, 20960, 16499, 17084, 15273, 1647, 9466, 6761, 7872, 7446, 1784,
	10014, 11967, 6458, 15475, 14560, 17956, 11464, 10812, 10891, 11853, 9798, 8348, 3910, 3266,
	3999, 6258, 8307, 13795, 13661, 13115, 11813, 11402, 10765, 13333, 11944, 11418, 10634, 8337,
	8237, 12549, 14538, 16995, 18267, 18108, 18593, 16623, 15163, 15061, 16358, 17897, 19148, 17901,
	17622, 18502, 18835, 17956, 18393, 16504, 15206, 14515, 13935, 13849, 14381, 13467, 11466, 17648,
	15735, 14796, 14731, 13939, 12652, 12217, 11902, 11108, -8694, -9382, -825, 1642, -4062, -7604,
	-148, -196, 3517, 2904, 1880, 2497, 2135, 2401, 3386, 3925, 5193, -2402, -3472, -5412,
	-9726, -1938, -6682, -10036, -8312, -5206, -2292, -926, 707, 636, -3085, -4140, -6774, -7455,
	-6995, -1479, -2024, 1275, 1246, -102, -1851, -2227, -4098, -4984, -5448, -3497, -2433, -2781,
	-1265, -2303, -1967, -1139, -1352, -3094, -3761, -3286, -3040, -7736, -8880, -9016, -10471, -11595,
	-12941, -14872, 5980, 4421, 230, 2493, 5500, 5906, 3258, 9058, 9702, 6365, 1997, -81,
	-1574, 1458, -614, -173, 2121, -293, 323, 1866, 1883, -4797, -3396, -408, 379, 482,
	1930, 567, -667, 232, 1566, 1128, -587, -775, 464, -103, -3320, -4519, -5325, -6695,
	-8301, -9490, -997, 4297, 3483, 4240, -3230, -2836, -2880, 3866, -85, 2002, 317, 1497,
	4876, 5770, 6398, 3732, 6412, 7238, 7769, 5036, 8860, 8612, 8899, 12211, 10911, 13588,
	14806, 15544, 8048, 10657, 9323, 14686, 15444, 12638, 11350, 10374, 11526, 13860, 15824, 15221,
	13847, 14174, 17768, 18498, 17814, 21526, 23353, 22866, 23939, 24111, -8345, -3644, -4565, -4427,
	-2923, 66, -1480, -4772, -3062, -5249, -5303, -2032, 3541, 2454, 2834, 177, -2045, -3472,
	-3004, 2307, 2633, 2683, 4450, 3681, 994, 487, 7206, 5954, 8347, 9230, 8076, 6887,
	6723, 6606, 9066, 9523, 8280, 8388, 8561, 8041, 11583, 10083, 8895, 10162, 8480, 9586,
	8859, 8416, 5570, 6674, 7607, 5601, 7937, 11189, 14901, 14153, 12474, 10656, 6827, 7877,
	10321, 17496, 18046, 16790, 17764, 18880, 17263, 16963, 22612, 21328, 22554, 22921, 21425, 20932,
	22187, 21300, 21566, 24196, 25399, 23699, 23051, 23741, 23668, 364, -2568, 3097, -2931, -4844,
	-4777, -1716, -3091, -6743, -1273, 3512, -303, 905, -2702, -669, -290, 2170, -1082, 1247,
	1222, 526, -6238, -6866, -732, -501, -1806, -4925, 3240, 1490, -4468, 2879, 7241, 3424,
	2153, 838, 2026, 1622, -501, 7675, 8876, 7609, 8241, 8938, 9782, 8907, 5232, 7376,
	8244, 8713, 14121, 13307, 10922, 9955, 5744, 7574, 13912, 9875, 10735, 9870, 7621, 7180,
	11536, 12259, 14655, 15230, 12245, 11591, 13022, 12537, 11882, 10017, 9805, 8405, 7273, 7590,
	6848, 5555, 7192, 9624, 9021, 8617, 7724, 5908, 6018, 8174, 398, 326, 876, 755,
	467, 2227, 1862, 552, 1140, 3268, 2646, 1565, 1177, 4751, 4857, 4706, 2644, 1716,
	724, 2520, 6388, 6015, 9624, 10093, 7140, 6479, 6954, 6311, 6133, 4749, 4275, 3631,
	4290, 5951, 5367, 3789, 4386, 8263, 9430, 9374, 7933, 5744, 5501, 6861, 7730, -725,
	2612, 2318, 2084, 1113, 2734, 3024, 4450, 6246, 7249, 5878, 7131, 7453, 10674, 5393,
	6848, 6472, 8186, 7009, 8281, 10680, 11088, 12728, 11801, 8733, 8452, 9992, 9324, 10441,
	9747, 10042, 10594, 11634, 12637, 11477, 9494, 16793, 17504, 16675, 16093, 15245, 13356, 12465,
	13349, 4612, 3126, 3166, -4723, -3292, 2917, -2803, -1421, -190, -3063, -3660, -1001, -1564,
	916, 1504, -155, -1303, 2195, 1987, -40, 2820, 984, 19, -1020, -3317, 4268, 5061,
	4833, 3483, -326, -311, 5174, 4828, 4657, 3697, 3902, 1129, 6685, 6053, 6001, 5005,
	4578, 6372, 4663, 4891, 4395, 4823, 4555, 4428, 6883, 16077, 15770, 14052, 12974, 13961,
	12036, 14192, 14136, 12742, 13829, 12370, 11098, 9393, 9558, 9657, 9433, 4830, 11806, 12783,
	13301, 13817, 13120, 12649, 10925, 11303, 8013, 9188, 9645, 10742, 9893, 7921, 7678, 7252,
	7112, 10460, 11179, 10251, 8906, 8942, 10211, 10715, 11251, 10639, 11826, 12640, 12678, 12843,
	13315, 12467, 10775, 11131, 13725, 13995, 11730, 9929, 13389, 14557, 12357, 9666, 10005, 10611,
	8171, 7748, 9999, 13259, 11933, 9749, 7744, 9718, 10652, 9144, 5746, 3182, 5640, 3054,
	4337, 6162, 6219, 5626, 5957, 6554, 5955, 4768, 8810, 8417, 3128, 1029, -3584, 5646,
	5931, 3300, 5741, 8127, 6189, 7879, 8862, 10252, 9675, 6725, 8779, -3743, -3051, -3579,
	-3146, -5787, -9329, -11008, -6076, -4607, -5428, -6370, -4239, -3036, -914, -1093, -617, 592,
	-259, -2668, -4550, -3544, -4550, -2582, -918, -255, 212, 1286, 1913, -147, -2096, -1640,
	1245, 1425, 1343, 2687, 4499, 3979, 3087, 3611, 8941, 7061, 5134, 4409, 3645, 3359,
	3459, 4168, 8923, 7549, 8191, 6924, 1095, 3202, 9454, 8598, 6659, 6113, 6965, 9848,
	9571, 10980, 9676, 7300, 8614, 8856, 10395, 13702, 14199, 15819, 14276, 13708, 10620, 12145,
	12900, 15830, 16980, 14818, 17353, 18818, 16788, 13741, 12474, 13725, 13619, 6132, 8363, 4467,
	11942, 12556, 13309, 11245, 9193, 10849, 6946, 6508, 11090, 9656, 9132, 5373, 3371, 8184,
	13369, 14495, 14637, 16266, 12605, 11683, 9941, 2099, 6272, 8119, 5285, 8531, 8375, 8562,
	13137, 8054, 7089, 10348, 7303, 5729, 11032, 10422, 12455, 9856, 6608, 9056, 12384, 15738,
	17766, 16370, 11387, 9940, 9715, 7996, 5833, 9520, 10235, 14840, 19205, 15802, 15511, 13511,
	14851, 18488, 19255, 21986, 20357, 15159, 11996, 14164, 13733, 13723, 13684, 10917, 13195, 14303,
	14303, 13467, 13252, 10725, 14073, 14921, 16833, 18573, 21400, 23678, 21738, 20119, 17001, 16110,
	17073, 16750, 15893, 15404, 4081, 3406, 1227, -56, 2114, 5575, 7329, 6831, 2641, 3655,
	66, -1175, 4795, 5712, 6045, 5007, 6252, 9776, 13882, 10075, 5422, 6322, 6339, 7778,
	-4162, -3593, -9274, -7797, -7236, -6093, -2063, -1827, -467, -2044, -2397, 564, 4582, 1916,
	2544, 3438, -1834, 291, 578, 1517, 4951, 7685, 7154, 6328, -9134, -7978, -10313, -9457,
	-3256, -5442, -3444, 114, -2056, 1640, 2301, 2686, 4110, 2314, 3359, 7508, -1897, -691,
	-1848, -250, 5064, 5927, 6921, 6585, -553, -813, -1428, -2796, -8075, -1802, -1140, 2630,
	-434, -2767, -639, 1161, 499, 775, 847, 3920, -5647, -3870, -5994, -5142, -3482, -1357,
	-1222, -1087, 16064, 17208, 19038, 18532, 11456, 16682, 13369, 19640, 18525, 12210, 12737, 15542,
	17240, 17415, 18088, 23697, 9466, 11047, 7457, 7232, 9364, 8805, 6728, 8396, 10282, 14811,
	18416, 19117, 2910, 12970, 9688, 15911, 9690, 10211, 11462, 11743, 17731, 14740, 15192, 19402,
	11844, 15637, 12700, 11724, 10345, 11517, 9595, 11073, -11076, -1296, 1075, 1939, -13302, -4445,
	-9382, -4002, -7695, -8571, -3545, -1861, 3901, 2049, -152, 1634, -205, 2089, 409, 3067,
	-897, 137, -1439, -3760, 2780, 11905, 8830, 7443, 2752, 10072, 2238, 8136, 10408, 8187,
	13784, 17816, 18612, 17220, 18249, 20480, 19525, 20844, 18369, 21119, 17395, 19056, 16651, 14878,
	-3239, 5180, 1400, 54, -1424, -1280, -7753, -2210, 6325, 8762, 9576, 9851, 12371, 13802,
	15115, 17788, 14590, 12740, 11613, 14281, 8747, 11264, 11002, 7659, -4501, -1427, 182, -2378,
	-12870, -9356, -8259, -7481, -1332, 1355, 5462, 3978, 2548, 6615, 7494, 8832, 9890, 6667,
	3745, 7109, 1808, 6807, 6918, 4430, -4747, -9245, -7105, -10842, -11409, -11020, -14948, -11855,
	-11124, -3629, -1311, -2963, -843, 545, -119, 3002, 660, -1282, -1891, -605, -2308, 680,
	1286, -1928, -6267, -5636, -2294, 78, -1533, -733, -889, 812, -3572, -6069, -5710, -3327,
	-2536, 92, -478, -89, -3213, -2752, -3436, -3966, -5054, -4012, -1199, -1807, 1573, 2240,
	1149, 391, 1896, 2912, 2464, 2264, 2931, 3713, 3512, 2123, 1349, 1921, 2626, 2729,
	615, 2113, 2249, 955, 606, 1306, 1754, 2277, 3114, 10641, 14204, 17068, 15383, 14213,
	12172, 13195, 11803, 9018, 10014, 10283, 12047, 13451, 11407, 12832, 8606, 9098, 7702, 6881,
	6541, 8218, 11371, 11414, 11113, 10685, 9808, 10303, 10573, 9782, 13117, 11746, 11868, 12352,
	12214, 13394, 14861, 14513, 14288, 15239, 12974, 10792, 11735, 12463, 11946, 12139, 3066, 320,
	-4152, -1559, -8209, -6666, -4501, 2722, 1783, -1593, -3745, -1769, -1819, 1597, 632, -2910,
	235, 2728, 3593, 1191, 123, -2838, 314, 2576, 3927, 867, -1768, -4391, 960, 615,
	-1066, -3461, -318, 1812, 1395, 1892, 306, 577, -1234, -3577, 2769, 473, 540, 874,
	-47, 16, 2065, 2665, 2994, 1811, 1079, -238, -3971, -4819, -3096, -2907, -4462, -4348,
	-7558, -9155, 2810, 390, -544, -1401, -2537, -6484, -4603, -1906, -2706, -2373, -4675, -1685,
	-189, -1538, 957, -33, -1185, -1861, -2329, -2755, -1914, -844, -1756, -2564, -2591, -1589,
	-821, -449, 656, 2542, 2515, 822, -106, -1215, -4116, 20840, 22425, 18065, 15853, 14017,
	14345, 18515, 15916, 13249, 10578, 10650, 15543, 15525, 14524, 15718, 16531, 16808, 15520, 12718,
	14469, 14619, 12175, 11833, 13728, 14155, 15143, 15185, 14534, 17072, 16274, 16855, 15627, 12015,
	11989, 16920, 15702, 4088, 8464, 4623, 9645, 12694, 9769, 6577, 3772, 4688, 6846, 9775,
	8263, 8795, 11995, 9224, -1074, 4168, 2124, 691, 3591, -1172, -4784, -6949, -8115, -4185,
	-4702, -1181, 2356, 1941, -1351, -1160, -2815, -1732, 138, -5613, -3540, -1183, -2192, -2012,
	-2040, -3324, -700, -533, -1141,
};
#define GOLD_FRAMES 48
static const uint8_t GOLD_SEQ[] = { 0, 10, 20, 30, 40 };
static const uint8_t LVL_A_BITS[][IMBE_BYTES] = {
	{ 0x53, 0x46, 0xC0, 0xDE, 0x90, 0x34, 0xFF, 0xDE, 0xDB, 0x46, 0x60 },
	{ 0x53, 0x39, 0x3F, 0x21, 0x6F, 0xCB, 0xFF, 0xC1, 0x24, 0xB9, 0x90 },
	{ 0x53, 0x46, 0xC0, 0xDE, 0x90, 0x34, 0xFF, 0xDE, 0xDB, 0x46, 0x60 },
	{ 0x53, 0x39, 0x3F, 0x21, 0x6F, 0xCB, 0xFF, 0xC1, 0x24, 0xB9, 0x90 },
	{ 0x53, 0x46, 0xC0, 0xDE, 0x90, 0x34, 0xFF, 0xDE, 0xDB, 0x46, 0x60 },
	{ 0x53, 0x39, 0x3F, 0x21, 0x6F, 0xCB, 0xFF, 0xC1, 0x24, 0xB9, 0x90 },
	{ 0x53, 0x46, 0xC0, 0xDE, 0x90, 0x34, 0xFF, 0xDE, 0xDB, 0x46, 0x60 },
	{ 0x53, 0x39, 0x3F, 0x21, 0x6F, 0xCB, 0xFF, 0xC1, 0x24, 0xB9, 0x90 },
	{ 0x53, 0x46, 0xC0, 0xDE, 0x90, 0x34, 0xFF, 0xDE, 0xDB, 0x46, 0x60 },
	{ 0x53, 0x39, 0x3F, 0x21, 0x6F, 0xCB, 0xFF, 0xC1, 0x24, 0xB9, 0x90 },
	{ 0x53, 0x46, 0xC0, 0xDE, 0x90, 0x34, 0xFF, 0xDE, 0xDB, 0x46, 0x60 },
	{ 0x53, 0x39, 0x3F, 0x21, 0x6F, 0xCB, 0xFF, 0xC1, 0x24, 0xB9, 0x90 },
	{ 0x53, 0x46, 0xC0, 0xDE, 0x90, 0x34, 0xFF, 0xDE, 0xDB, 0x46, 0x60 },
	{ 0x53, 0x39, 0x3F, 0x21, 0x6F, 0xCB, 0xFF, 0xC1, 0x24, 0xB9, 0x90 },
	{ 0x53, 0x46, 0xC0, 0xDE, 0x90, 0x34, 0xFF, 0xDE, 0xDB, 0x46, 0x60 },
	{ 0x53, 0x39, 0x3F, 0x21, 0x6F, 0xCB, 0xFF, 0xC1, 0x24, 0xB9, 0x90 },
	{ 0x53, 0x46, 0xC0, 0xDE, 0x90, 0x34, 0xFF, 0xDE, 0xDB, 0x46, 0x60 },
	{ 0x53, 0x39, 0x3F, 0x21, 0x6F, 0xCB, 0xFF, 0xC1, 0x24, 0xB9, 0x90 },
	{ 0x53, 0x46, 0xC0, 0xDE, 0x90, 0x34, 0xFF, 0xDE, 0xDB, 0x46, 0x60 },
	{ 0x53, 0x39, 0x3F, 0x21, 0x6F, 0xCB, 0xFF, 0xC1, 0x24, 0xB9, 0x90 },
	{ 0x53, 0x46, 0xC0, 0xDE, 0x90, 0x34, 0xFF, 0xDE, 0xDB, 0x46, 0x60 },
	{ 0x53, 0x39, 0x3F, 0x21, 0x6F, 0xCB, 0xFF, 0xC1, 0x24, 0xB9, 0x90 },
	{ 0x53, 0x46, 0xC0, 0xDE, 0x90, 0x34, 0xFF, 0xDE, 0xDB, 0x46, 0x60 },
	{ 0x53, 0x39, 0x3F, 0x21, 0x6F, 0xCB, 0xFF, 0xC1, 0x24, 0xB9, 0x90 },
	{ 0x53, 0x46, 0xC0, 0xDE, 0x90, 0x34, 0xFF, 0xDE, 0xDB, 0x46, 0x60 },
	{ 0x53, 0x39, 0x3F, 0x21, 0x6F, 0xCB, 0xFF, 0xC1, 0x24, 0xB9, 0x90 },
	{ 0x53, 0x46, 0xC0, 0xDE, 0x90, 0x34, 0xFF, 0xDE, 0xDB, 0x46, 0x60 },
	{ 0x53, 0x39, 0x3F, 0x21, 0x6F, 0xCB, 0xFF, 0xC1, 0x24, 0xB9, 0x90 },
	{ 0x53, 0x46, 0xC0, 0xDE, 0x90, 0x34, 0xFF, 0xDE, 0xDB, 0x46, 0x60 },
	{ 0x53, 0x39, 0x3F, 0x21, 0x6F, 0xCB, 0xFF, 0xC1, 0x24, 0xB9, 0x90 },
	{ 0x53, 0x46, 0xC0, 0xDE, 0x90, 0x34, 0xFF, 0xDE, 0xDB, 0x46, 0x60 },
	{ 0x53, 0x39, 0x3F, 0x21, 0x6F, 0xCB, 0xFF, 0xC1, 0x24, 0xB9, 0x90 },
	{ 0x53, 0x46, 0xC0, 0xDE, 0x90, 0x34, 0xFF, 0xDE, 0xDB, 0x46, 0x60 },
	{ 0x53, 0x39, 0x3F, 0x21, 0x6F, 0xCB, 0xFF, 0xC1, 0x24, 0xB9, 0x90 },
	{ 0x53, 0x46, 0xC0, 0xDE, 0x90, 0x34, 0xFF, 0xDE, 0xDB, 0x46, 0x60 },
	{ 0x53, 0x39, 0x3F, 0x21, 0x6F, 0xCB, 0xFF, 0xC1, 0x24, 0xB9, 0x90 },
	{ 0x53, 0x46, 0xC0, 0xDE, 0x90, 0x34, 0xFF, 0xDE, 0xDB, 0x46, 0x60 },
	{ 0x53, 0x39, 0x3F, 0x21, 0x6F, 0xCB, 0xFF, 0xC1, 0x24, 0xB9, 0x90 },
	{ 0x53, 0x46, 0xC0, 0xDE, 0x90, 0x34, 0xFF, 0xDE, 0xDB, 0x46, 0x60 },
	{ 0x53, 0x39, 0x3F, 0x21, 0x6F, 0xCB, 0xFF, 0xC1, 0x24, 0xB9, 0x90 },
};
static const uint16_t LVL_A_RMS[] = {
	1025, 1674, 1374, 1664, 1665, 1379, 1653, 1663, 1384, 1656,
	1663, 1393, 1642, 1662, 1595, 1454, 1663, 1642, 1393, 1665,
	1659, 1378, 1663, 1658, 1375, 1664, 1663, 1376, 1660, 1661,
	1376, 1662, 1665, 1375, 1658, 1662, 1378, 1661, 1663, 1389,
};
static const uint8_t LVL_B_BITS[][IMBE_BYTES] = {
	{ 0x2B, 0x41, 0xDE, 0x97, 0x83, 0xDE, 0xFE, 0x00, 0xEF, 0x00, 0x00 },
	{ 0x2B, 0x3E, 0x21, 0x68, 0x7C, 0x21, 0xFE, 0xFF, 0x10, 0xFF, 0xF0 },
	{ 0x2B, 0x41, 0xDE, 0x97, 0x83, 0xDE, 0xFE, 0x00, 0xEF, 0x00, 0x00 },
	{ 0x2B, 0x3E, 0x21, 0x68, 0x7C, 0x21, 0xFE, 0xFF, 0x10, 0xFF, 0xF0 },
	{ 0x2B, 0x41, 0xDE, 0x97, 0x83, 0xDE, 0xFE, 0x00, 0xEF, 0x00, 0x00 },
	{ 0x2B, 0x3E, 0x21, 0x68, 0x7C, 0x21, 0xFE, 0xFF, 0x10, 0xFF, 0xF0 },
	{ 0x2B, 0x41, 0xDE, 0x97, 0x83, 0xDE, 0xFE, 0x00, 0xEF, 0x00, 0x00 },
	{ 0x2B, 0x3E, 0x21, 0x68, 0x7C, 0x21, 0xFE, 0xFF, 0x10, 0xFF, 0xF0 },
	{ 0x2B, 0x41, 0xDE, 0x97, 0x83, 0xDE, 0xFE, 0x00, 0xEF, 0x00, 0x00 },
	{ 0x2B, 0x3E, 0x21, 0x68, 0x7C, 0x21, 0xFE, 0xFF, 0x10, 0xFF, 0xF0 },
	{ 0x2B, 0x41, 0xDE, 0x97, 0x83, 0xDE, 0xFE, 0x00, 0xEF, 0x00, 0x00 },
	{ 0x2B, 0x3E, 0x21, 0x68, 0x7C, 0x21, 0xFE, 0xFF, 0x10, 0xFF, 0xF0 },
	{ 0x2B, 0x41, 0xDE, 0x97, 0x83, 0xDE, 0xFE, 0x00, 0xEF, 0x00, 0x00 },
	{ 0x2B, 0x3E, 0x21, 0x68, 0x7C, 0x21, 0xFE, 0xFF, 0x10, 0xFF, 0xF0 },
	{ 0x2B, 0x41, 0xDE, 0x97, 0x83, 0xDE, 0xFE, 0x00, 0xEF, 0x00, 0x00 },
	{ 0x2B, 0x3E, 0x21, 0x68, 0x7C, 0x21, 0xFE, 0xFF, 0x10, 0xFF, 0xF0 },
	{ 0x2B, 0x41, 0xDE, 0x97, 0x83, 0xDE, 0xFE, 0x00, 0xEF, 0x00, 0x00 },
	{ 0x2B, 0x3E, 0x21, 0x68, 0x7C, 0x21, 0xFE, 0xFF, 0x10, 0xFF, 0xF0 },
	{ 0x2B, 0x41, 0xDE, 0x97, 0x83, 0xDE, 0xFE, 0x00, 0xEF, 0x00, 0x00 },
	{ 0x2B, 0x3E, 0x21, 0x68, 0x7C, 0x21, 0xFE, 0xFF, 0x10, 0xFF, 0xF0 },
	{ 0x2B, 0x41, 0xDE, 0x97, 0x83, 0xDE, 0xFE, 0x00, 0xEF, 0x00, 0x00 },
	{ 0x2B, 0x3E, 0x21, 0x68, 0x7C, 0x21, 0xFE, 0xFF, 0x10, 0xFF, 0xF0 },
	{ 0x2B, 0x41, 0xDE, 0x97, 0x83, 0xDE, 0xFE, 0x00, 0xEF, 0x00, 0x00 },
	{ 0x2B, 0x3E, 0x21, 0x68, 0x7C, 0x21, 0xFE, 0xFF, 0x10, 0xFF, 0xF0 },
	{ 0x2B, 0x41, 0xDE, 0x97, 0x83, 0xDE, 0xFE, 0x00, 0xEF, 0x00, 0x00 },
	{ 0x2B, 0x3E, 0x21, 0x68, 0x7C, 0x21, 0xFE, 0xFF, 0x10, 0xFF, 0xF0 },
	{ 0x2B, 0x41, 0xDE, 0x97, 0x83, 0xDE, 0xFE, 0x00, 0xEF, 0x00, 0x00 },
	{ 0x2B, 0x3E, 0x21, 0x68, 0x7C, 0x21, 0xFE, 0xFF, 0x10, 0xFF, 0xF0 },
	{ 0x2B, 0x41, 0xDE, 0x97, 0x83, 0xDE, 0xFE, 0x00, 0xEF, 0x00, 0x00 },
	{ 0x2B, 0x3E, 0x21, 0x68, 0x7C, 0x21, 0xFE, 0xFF, 0x10, 0xFF, 0xF0 },
	{ 0x2B, 0x41, 0xDE, 0x97, 0x83, 0xDE, 0xFE, 0x00, 0xEF, 0x00, 0x00 },
	{ 0x2B, 0x3E, 0x21, 0x68, 0x7C, 0x21, 0xFE, 0xFF, 0x10, 0xFF, 0xF0 },
	{ 0x2B, 0x41, 0xDE, 0x97, 0x83, 0xDE, 0xFE, 0x00, 0xEF, 0x00, 0x00 },
	{ 0x2B, 0x3E, 0x21, 0x68, 0x7C, 0x21, 0xFE, 0xFF, 0x10, 0xFF, 0xF0 },
	{ 0x2B, 0x41, 0xDE, 0x97, 0x83, 0xDE, 0xFE, 0x00, 0xEF, 0x00, 0x00 },
	{ 0x2B, 0x3E, 0x21, 0x68, 0x7C, 0x21, 0xFE, 0xFF, 0x10, 0xFF, 0xF0 },
	{ 0x2B, 0x41, 0xDE, 0x97, 0x83, 0xDE, 0xFE, 0x00, 0xEF, 0x00, 0x00 },
	{ 0x2B, 0x3E, 0x21, 0x68, 0x7C, 0x21, 0xFE, 0xFF, 0x10, 0xFF, 0xF0 },
	{ 0x2B, 0x41, 0xDE, 0x97, 0x83, 0xDE, 0xFE, 0x00, 0xEF, 0x00, 0x00 },
	{ 0x2B, 0x3E, 0x21, 0x68, 0x7C, 0x21, 0xFE, 0xFF, 0x10, 0xFF, 0xF0 },
};
static const uint16_t LVL_B_RMS[] = {
	932, 1277, 1273, 1274, 1274, 1274, 1274, 1274, 1273, 1274,
	1273, 1274, 1273, 1273, 1273, 1273, 1273, 1274, 1273, 1274,
	1274, 1274, 1273, 1274, 1273, 1275, 1273, 1277, 1272, 1279,
	1272, 1284, 1335, 1325, 1289, 1271, 1280, 1272, 1277, 1273,
};
#define LVL_FRAMES 40

// the decoder and everything it needs, in one place
typedef struct {
	imbe_dec_state_t st;
	mbe_params_t pr;
	mbe_state_t ms;
	mbe_frame_t prev, cur;
	int32_t buf[MBE_BUF_WORDS];
	imbe_mem_t m;
} rig_t;

static void rig_init(rig_t *r)
{
	memset(r, 0, sizeof *r);
	r->m.s = &r->st;
	r->m.p = &r->pr;
	r->m.mbe.s = &r->ms;
	r->m.mbe.prev = &r->prev;
	r->m.mbe.cur = &r->cur;
	r->m.mbe.buf = r->buf;
	imbe_dec_init(&r->m);
}

static void core_init(rig_t *r)
{
	rig_init(r);
	mbe_synth_init(&r->m.mbe);
}

static double rms_of(const int16_t *x, int n)
{
	double s = 0;

	for (int i = 0; i < n; i++)
		s += (double)x[i] * x[i];
	return sqrt(s / n);
}

static double db(double a, double b) { return 20 * log10((a + 1e-9) / (b + 1e-9)); }

// amplitude and phase fit of one frequency over n samples at 8 kHz: the least-squares sinusoid
static double tone_amp(const int16_t *x, int n, double f, double *resid_rms)
{
	double cc = 0, ss = 0, cs = 0, xc = 0, xs = 0, det, a, b, e = 0;

	for (int i = 0; i < n; i++) {
		double c = cos(2 * M_PI * f * i / 8000), s = sin(2 * M_PI * f * i / 8000);

		cc += c * c; ss += s * s; cs += c * s; xc += x[i] * c; xs += x[i] * s;
	}
	det = cc * ss - cs * cs;
	a = (xc * ss - xs * cs) / det;
	b = (xs * cc - xc * cs) / det;
	for (int i = 0; i < n; i++) {
		double c = cos(2 * M_PI * f * i / 8000), s = sin(2 * M_PI * f * i / 8000), d = x[i] - a * c - b * s;

		e += d * d;
	}
	if (resid_rms)
		*resid_rms = sqrt(e / n);
	return sqrt(a * a + b * b);
}

static double f0_hz(int b0) { return 16000.0 / (b0 + 39.5); }

// a frame with a given pitch code and the other bits from the generator
static void frame_b0(uint8_t f[IMBE_BYTES], int b0, int randomize)
{
	for (int i = 0; i < IMBE_BYTES; i++)
		f[i] = randomize ? (uint8_t)rng() : 0;
	f[0] = (uint8_t)((f[0] & 3) | ((b0 >> 2) << 2));
	f[10] = (uint8_t)((f[10] & ~6) | (((b0 >> 1) & 1) << 2) | ((b0 & 1) << 1));
}

static void test_dequantizer(void)
{
	rig_t *r = malloc(sizeof *r);
	int frame = 0, worst = 0;

	for (int sq = 0; sq < (int)sizeof GOLD_SEQ; sq++) {
		imbe_state_reset(&r->st.q);
		for (; frame < GOLD_FRAMES && (sq + 1 == (int)sizeof GOLD_SEQ || frame < GOLD_SEQ[sq + 1]); frame++) {
			mbe_params_t p;
			int st = imbe_unpack(&r->st.q, GOLD_BITS[frame], &p), err = 0, vm = 0;
			double wrel;

			CHECK(st == IMBE_FRAME_OK, "frame %d: unpack gave %d", frame, st);
			if (st != IMBE_FRAME_OK)
				continue;
			CHECK(p.L == GOLD[frame].L, "frame %d: L %d, expected %d", frame, p.L, GOLD[frame].L);
			if (p.L != GOLD[frame].L)
				continue;
			// w0: the reference rounds it in single precision; allow 1e-6 of it
			wrel = fabs((double)p.w0 - GOLD[frame].w0) / GOLD[frame].w0;
			CHECK(wrel < 1e-6, "frame %d: w0 %u, expected %u", frame, p.w0, GOLD[frame].w0);
			for (int l = 0; l < p.L; l++) {
				int d = abs(p.a[l] - GOLD_A[GOLD[frame].at + l]);

				vm |= (p.v[l] != ((GOLD[frame].v >> l) & 1)) << 0;
				if (d > err)
					err = d;
			}
			CHECK(!vm, "frame %d: voicing differs", frame);
			CHECK(err <= 8, "frame %d: amplitude off by %d / 2048 in log2", frame, err);	// 0.004 in log2, 0.024 dB
			if (err > worst)
				worst = err;
		}
	}
	printf("dequantizer: %d golden frames, worst amplitude error %d / 2048 (log2)\n", GOLD_FRAMES, worst);
	free(r);
}

static void test_pitch_codes(void)
{
	rig_t *r = malloc(sizeof *r);

	for (int b0 = 0; b0 < 256; b0++) {
		uint8_t f[IMBE_BYTES];
		mbe_params_t p;
		int st, L;
		double w0;

		imbe_state_reset(&r->st.q);
		frame_b0(f, b0, 1);
		st = imbe_unpack(&r->st.q, f, &p);
		if (b0 >= 216 && b0 <= 219) {
			CHECK(st == IMBE_FRAME_SILENCE, "b0 %d: %d, expected silence", b0, st);
		} else if (b0 > 207) {
			CHECK(st == IMBE_FRAME_INVALID, "b0 %d: %d, expected invalid", b0, st);
		} else {
			w0 = 4 * M_PI / (b0 + 39.5);
			L = (int)(0.9254 * (int)(M_PI / w0 + 0.25));
			CHECK(st == IMBE_FRAME_OK, "b0 %d: %d", b0, st);
			CHECK(p.L == L && L >= 9 && L <= 56, "b0 %d: L %d, expected %d", b0, p.L, L);
			CHECK(fabs(p.w0 - w0 / (2 * M_PI) * 4294967296.0) < 64, "b0 %d: w0 %u", b0, p.w0);
		}
		if (st != IMBE_FRAME_OK)
			CHECK(r->st.q.L == 30, "b0 %d: the state moved", b0);
	}
	free(r);
}

// one strong voiced harmonic: a tone at the right frequency, at the right level, nothing else
static void test_tone(void)
{
	rig_t *r = malloc(sizeof *r);
	static int16_t pcm[10 * MBE_FRAME_LEN];
	mbe_params_t p;
	const int b0 = 80, h = 5;
	double f = h * f0_hz(b0), amp = 8000, a, resid, best = 0, bestf = 0;

	core_init(r);
	memset(&p, 0, sizeof p);
	p.L = 27;
	p.w0 = (0xFFFFFFFFu / (2 * b0 + 79)) << 2;
	for (int l = 0; l < p.L; l++) {
		p.a[l] = MBE_A_SILENT;
		p.v[l] = 1;
	}
	p.a[h - 1] = (int16_t)lround(log2(amp) * 2048);
	for (int i = 0; i < 20; i++)		// the first frames fade in; measure after them
		mbe_synth_frame(&r->m.mbe, &p, 0, pcm);
	for (int i = 0; i < 10; i++)
		mbe_synth_frame(&r->m.mbe, &p, 0, pcm + i * MBE_FRAME_LEN);
	a = tone_amp(pcm, 10 * MBE_FRAME_LEN, f, &resid);
	CHECK(fabs(db(a, amp)) < 0.5, "tone level %.0f, expected %.0f", a, amp);
	CHECK(db(resid, a) < -35, "the tone is not clean: residual %.1f dB", db(resid, a));
	for (double g = f - 6; g <= f + 6; g += 0.25) {
		double x = tone_amp(pcm, 10 * MBE_FRAME_LEN, g, NULL);

		if (x > best) {
			best = x;
			bestf = g;
		}
	}
	CHECK(fabs(bestf - f) <= 0.5, "tone at %.2f Hz, expected %.2f", bestf, f);
	free(r);
}

// all harmonics unvoiced: noise with the power the model gives its bands and nothing outside them
static void test_noise(void)
{
	rig_t *r = malloc(sizeof *r);
	static int16_t pcm[20 * MBE_FRAME_LEN];
	mbe_params_t p;
	const int b0 = 80;
	double amp = 700, expect, rms, inband = 0, below = 0, above = 0, f0 = f0_hz(b0);

	core_init(r);
	memset(&p, 0, sizeof p);
	p.L = 27;
	p.w0 = (0xFFFFFFFFu / (2 * b0 + 79)) << 2;
	for (int l = 0; l < p.L; l++)
		p.a[l] = (int16_t)lround(log2(amp) * 2048);
	for (int i = 0; i < 20; i++)
		mbe_synth_frame(&r->m.mbe, &p, 0, pcm + i * MBE_FRAME_LEN);
	rms = rms_of(pcm + 4 * MBE_FRAME_LEN, 16 * MBE_FRAME_LEN);
	expect = 1.657 * amp * sqrt(p.L);	// the unvoiced harmonic's rms: 1.657 times its amplitude
	CHECK(fabs(db(rms, expect)) < 1.5, "noise rms %.0f, expected %.0f (%.1f dB)", rms, expect, db(rms, expect));
	// power by region, from a direct DFT of 16 frames
	for (int k = 1; k < 160; k++) {		// up to 4 kHz
		double fr = k * 25.0, re = 0, im = 0;

		for (int n = 0; n < 16 * MBE_FRAME_LEN; n++) {
			double w = 0.5 - 0.5 * cos(2 * M_PI * n / (16 * MBE_FRAME_LEN)), x = pcm[4 * MBE_FRAME_LEN + n] * w;

			re += x * cos(2 * M_PI * fr * n / 8000);
			im += x * sin(2 * M_PI * fr * n / 8000);
		}
		if (fr < 0.35 * f0)
			below += re * re + im * im;
		else if (fr > (p.L + 1.2) * f0)
			above += re * re + im * im;
		else
			inband += re * re + im * im;
	}
	CHECK(10 * log10(below / inband) < -22, "noise below the first band: %.1f dB", 10 * log10(below / inband));
	CHECK(10 * log10(above / inband) < -22, "noise above the last band: %.1f dB", 10 * log10(above / inband));
	free(r);
}

static void test_silence(void)
{
	rig_t *r = malloc(sizeof *r);
	int16_t pcm[MBE_FRAME_LEN];
	mbe_params_t p;
	int worst = 0;

	core_init(r);
	memset(&p, 0, sizeof p);
	for (int i = 0; i < 10; i++) {
		mbe_synth_frame(&r->m.mbe, &p, 0, pcm);
		for (int n = 0; n < MBE_FRAME_LEN; n++)
			worst = abs(pcm[n]) > worst ? abs(pcm[n]) : worst;
	}
	CHECK(worst == 0, "an empty frame is not silent: %d", worst);
	// harmonics marked silent, voiced and unvoiced
	p.L = 30;
	p.w0 = 0x03000000u;
	for (int l = 0; l < p.L; l++) {
		p.a[l] = MBE_A_SILENT;
		p.v[l] = l & 1;
	}
	for (int i = 0; i < 10; i++) {
		mbe_synth_frame(&r->m.mbe, &p, 0, pcm);
		for (int n = 0; n < MBE_FRAME_LEN; n++)
			worst = abs(pcm[n]) > worst ? abs(pcm[n]) : worst;
	}
	CHECK(worst <= 1, "silent harmonics are not silent: %d", worst);
	free(r);
}

// the level against the float reference on steady voiced frames
static void level_run(const uint8_t (*bits)[IMBE_BYTES], const uint16_t *ref, const char *what)
{
	rig_t *r = malloc(sizeof *r);
	double worst = 0, sum = 0;
	int n = 0;

	rig_init(r);
	for (int i = 0; i < LVL_FRAMES; i++) {
		int16_t pcm[MBE_FRAME_LEN];
		double d;

		imbe_dec_frame(&r->m, bits[i], 0, pcm);
		if (i < 3)
			continue;	// the first frames fade in from silence
		d = db(rms_of(pcm, MBE_FRAME_LEN), ref[i]);
		if (fabs(d) > worst)
			worst = fabs(d);
		sum += d;
		n++;
	}
	CHECK(worst < 2.0, "%s: a frame is %.2f dB off the reference", what, worst);
	CHECK(fabs(sum / n) < 0.5, "%s: the mean level is %.2f dB off", what, sum / n);
	printf("level, %s: mean %+.2f dB, worst frame %.2f dB against the reference\n", what, sum / n, worst);
	free(r);
}

static void test_level(void)
{
	level_run(LVL_A_BITS, LVL_A_RMS, "L=27 voiced");
	level_run(LVL_B_BITS, LVL_B_RMS, "L=18 voiced");
}

// concealment: repeat the last good frame up to MBE_REPEAT_MAX times, then silence; a good frame resumes
static void test_conceal(void)
{
	rig_t *r = malloc(sizeof *r);
	int16_t pcm[MBE_FRAME_LEN];
	double good = 0, d[MBE_REPEAT_MAX + 3];
	uint8_t bad_pitch[IMBE_BYTES];

	rig_init(r);
	for (int i = 0; i < LVL_FRAMES; i++) {
		CHECK(imbe_dec_frame(&r->m, LVL_A_BITS[i], 0, pcm) == 0, "good frame %d counted bad", i);
		good = rms_of(pcm, MBE_FRAME_LEN);
	}
	CHECK(good > 300, "no signal before the loss: %.0f", good);
	// the first code word had two errors and the frame eleven: the frame is not used, whatever its bits are
	for (int i = 0; i < MBE_REPEAT_MAX + 3; i++) {
		int run = imbe_dec_frame(&r->m, LVL_A_BITS[i], IMBE_ERRS(11, 2, 0), pcm);

		CHECK(run == (i < MBE_REPEAT_MAX + 1 ? i + 1 : MBE_REPEAT_MAX + 1), "bad frame %d: run %d", i, run);
		d[i] = rms_of(pcm, MBE_FRAME_LEN);
	}
	for (int i = 0; i < MBE_REPEAT_MAX; i++)
		CHECK(fabs(db(d[i], good)) < 1.5, "repeat %d: %.0f against %.0f", i + 1, d[i], good);
	CHECK(d[MBE_REPEAT_MAX] < 0.8 * good, "the frame after the repeats does not fade: %.0f", d[MBE_REPEAT_MAX]);
	CHECK(d[MBE_REPEAT_MAX + 1] < 2 && d[MBE_REPEAT_MAX + 2] < 2, "not muted: %.1f %.1f", d[MBE_REPEAT_MAX + 1],
	      d[MBE_REPEAT_MAX + 2]);
	CHECK(r->st.q.L == 30 && r->st.q.lg[1] == 0, "the prediction was not reset by the mute");
	// a good frame after the mute decodes as the first of a call and plays
	imbe_dec_frame(&r->m, LVL_A_BITS[0], 0, pcm);
	imbe_dec_frame(&r->m, LVL_A_BITS[1], 0, pcm);
	CHECK(rms_of(pcm, MBE_FRAME_LEN) > 100, "no sound after the mute");

	// many corrected bits with the first word sound, or two errors there in a frame with few: still good frames
	rig_init(r);
	CHECK(imbe_dec_frame(&r->m, LVL_A_BITS[0], IMBE_ERRS(12, 1, 1), pcm) == 0, "12 errors, 1 in the first word, counted bad");
	CHECK(imbe_dec_frame(&r->m, LVL_A_BITS[1], IMBE_ERRS(10, 3, 0), pcm) == 0, "10 errors counted bad");
	CHECK(imbe_dec_frame(&r->m, LVL_A_BITS[2], IMBE_ERRS(11, 2, 0), pcm) == 1, "11 errors, 2 in the first word, counted good");
	// a pitch code that is not used is a bad frame
	rig_init(r);
	imbe_dec_frame(&r->m, LVL_A_BITS[0], 0, pcm);
	frame_b0(bad_pitch, 210, 1);
	CHECK(imbe_dec_frame(&r->m, bad_pitch, 0, pcm) == 1, "invalid pitch not counted bad");
	// a loss before any good frame: nothing to repeat
	rig_init(r);
	CHECK(imbe_dec_frame(&r->m, bad_pitch, 0, pcm) == 1, "loss at start");
	CHECK(rms_of(pcm, MBE_FRAME_LEN) < 1, "a loss before any frame is not silent");
	// a silence frame: not a loss, the speech stops and the prediction restarts
	rig_init(r);
	for (int i = 0; i < 20; i++)
		imbe_dec_frame(&r->m, LVL_A_BITS[i], 0, pcm);
	frame_b0(bad_pitch, 217, 1);
	CHECK(imbe_dec_frame(&r->m, bad_pitch, 0, pcm) == 0, "silence frame counted bad");
	CHECK(r->st.q.L == 30, "silence frame did not reset the prediction");
	imbe_dec_frame(&r->m, bad_pitch, 0, pcm);
	CHECK(rms_of(pcm, MBE_FRAME_LEN) < 2, "silence frame is not silent");
	// the named silence frame is one
	CHECK(imbe_unpack(&r->st.q, imbe_silence, &r->pr) == IMBE_FRAME_SILENCE, "imbe_silence is not a silence frame");
	CHECK(imbe_dec_frame(&r->m, imbe_silence, 0, pcm) == 0 && rms_of(pcm, MBE_FRAME_LEN) < 2,
	      "imbe_silence does not decode as silence");
	free(r);
}

// a channel with errors: nothing changes while it is clean, the output is muted while the rate of corrected bits is
// over 8.5 % and comes back when it falls, and a frame's amplitudes are held under errors
static void test_channel_errors(void)
{
	rig_t *a = malloc(sizeof *a), *b = malloc(sizeof *b);
	int16_t pa[MBE_FRAME_LEN], pb[MBE_FRAME_LEN];
	uint8_t loud[IMBE_BYTES];
	int same = 1, muted_at = -1, back_at = -1;
	double clean = 0, held = 0;

	// up to four corrected bits a frame now and then: the same samples as with none
	rig_init(a);
	rig_init(b);
	for (int i = 0; i < LVL_FRAMES; i++) {
		imbe_dec_frame(&a->m, LVL_A_BITS[i], 0, pa);
		imbe_dec_frame(&b->m, LVL_A_BITS[i], i % 8 ? 0 : IMBE_ERRS(4, 1, 1), pb);
		same &= !memcmp(pa, pb, sizeof pa);
	}
	CHECK(same, "a clean channel's frames changed with a few corrected bits");
	CHECK(b->st.err_rate > 0 && b->st.err_rate <= 83886, "error rate %u", (unsigned)b->st.err_rate);
	// 8 bits in every frame settles at 5.7 %: smoothed, still playing
	rig_init(b);
	for (int i = 0; i < 200; i++)
		CHECK(imbe_dec_frame(&b->m, LVL_A_BITS[i % LVL_FRAMES], IMBE_ERRS(8, 1, 0), pb) == 0, "8 bits a frame muted, frame %d", i);
	CHECK(rms_of(pb, MBE_FRAME_LEN) > 100, "8 bits a frame: no sound");
	// 13 bits in every frame settles at 9.3 %: muted, and counted as bad frames
	rig_init(b);
	for (int i = 0; i < 200; i++) {
		int run = imbe_dec_frame(&b->m, LVL_A_BITS[i % LVL_FRAMES], IMBE_ERRS(13, 1, 0), pb);

		if (run && muted_at < 0)
			muted_at = i;
		CHECK(muted_at < 0 || run, "frame %d played after the mute at %d", i, muted_at);
	}
	CHECK(muted_at > 20 && muted_at < 80, "muted at frame %d", muted_at);
	CHECK(rms_of(pb, MBE_FRAME_LEN) < 2, "not silent over the limit: %.1f", rms_of(pb, MBE_FRAME_LEN));
	// clean frames again: the rate falls, the speech comes back within a few frames
	for (int i = 0; i < 40; i++)
		if (imbe_dec_frame(&b->m, LVL_A_BITS[i], 0, pb) == 0 && back_at < 0)
			back_at = i;
	CHECK(back_at >= 1 && back_at < 6, "back at frame %d", back_at);
	CHECK(rms_of(pb, MBE_FRAME_LEN) > 100, "no sound after the mute");
	// frames that never came do not count against the channel
	rig_init(b);
	for (int i = 0; i < 50; i++)
		imbe_dec_frame(&b->m, LVL_A_BITS[0], IMBE_ERRS_LOST, pb);
	CHECK(b->st.err_rate == 0, "lost frames raised the error rate: %u", (unsigned)b->st.err_rate);
	// the largest amplitudes a frame can carry: under errors their sum is held, so the frame is quieter
	rig_init(a);
	rig_init(b);
	memset(loud, 0xFF, sizeof loud);
	frame_b0(loud, 60, 0);
	memset(loud + 1, 0xFF, 3);
	for (int i = 0; i < 12; i++) {
		imbe_dec_frame(&a->m, loud, 0, pa);
		imbe_dec_frame(&b->m, loud, IMBE_ERRS(9, 1, 1), pb);
		clean += rms_of(pa, MBE_FRAME_LEN);
		held += rms_of(pb, MBE_FRAME_LEN);
	}
	CHECK(held <= clean, "amplitudes under errors %.0f, clean %.0f", held / 12, clean / 12);
	free(a);
	free(b);
}

// the largest amplitudes: the output saturates, nothing wraps, nothing overflows (the sanitizers watch)
static void test_extremes(void)
{
	rig_t *r = malloc(sizeof *r);
	int16_t pcm[MBE_FRAME_LEN];
	mbe_params_t p;
	int peak = 0, jump = 0;

	core_init(r);
	memset(&p, 0, sizeof p);
	p.L = MBE_MAXL;
	p.w0 = 0x00D00000u;		// 65 Hz: 56 harmonics under 4 kHz
	for (int l = 0; l < p.L; l++) {
		p.a[l] = 32767;
		p.v[l] = 1;
	}
	for (int i = 0; i < 6; i++) {
		mbe_synth_frame(&r->m.mbe, &p, 0, pcm);
		for (int n = 0; n < MBE_FRAME_LEN; n++)
			peak = abs(pcm[n]) > peak ? abs(pcm[n]) : peak;
	}
	CHECK(peak >= 32000, "loud voiced frame does not reach full scale: %d", peak);
	for (int l = 0; l < p.L; l++)
		p.v[l] = 0;
	peak = 0;
	for (int i = 0; i < 6; i++) {
		mbe_synth_frame(&r->m.mbe, &p, 0, pcm);
		for (int n = 0; n < MBE_FRAME_LEN; n++)
			peak = abs(pcm[n]) > peak ? abs(pcm[n]) : peak;
	}
	CHECK(peak >= 32000, "loud unvoiced frame does not reach full scale: %d", peak);
	// one full-scale tone at 65 Hz: a wrapped sample would be a jump of tens of thousands
	core_init(r);
	for (int l = 0; l < p.L; l++) {
		p.a[l] = l ? MBE_A_SILENT : 32767;
		p.v[l] = 1;
	}
	for (int i = 0; i < 12; i++) {
		mbe_synth_frame(&r->m.mbe, &p, 0, pcm);
		for (int n = 1; n < MBE_FRAME_LEN; n++)
			jump = abs(pcm[n] - pcm[n - 1]) > jump ? abs(pcm[n] - pcm[n - 1]) : jump;
	}
	CHECK(jump < 4000, "a loud tone has a jump of %d between samples", jump);
	// random parameters over the whole range, many frames; random frames through the whole decoder
	core_init(r);
	for (int i = 0; i < 3000; i++) {
		memset(&p, 0, sizeof p);
		p.L = (int16_t)(rng() % 60);		// above MBE_MAXL too
		p.flags = (uint16_t)(rng() & 1);
		p.w0 = rng();
		for (int l = 0; l < MBE_MAXL; l++) {
			p.a[l] = (int16_t)rng();
			p.v[l] = rng() & 1;
		}
		mbe_synth_frame(&r->m.mbe, (rng() & 7) ? &p : NULL, (int)(rng() & 3) == 0, pcm);
	}
	rig_init(r);
	for (int i = 0; i < 5000; i++) {
		uint8_t f[IMBE_BYTES];

		for (int k = 0; k < IMBE_BYTES; k++)
			f[k] = (uint8_t)rng();
		imbe_dec_frame(&r->m, f, (uint8_t)rng(), pcm);
	}
	free(r);
}

// the same input gives the same output, and the memory placement does not matter
static void test_determinism(void)
{
	rig_t *a = malloc(sizeof *a), *b = malloc(sizeof *b);
	int16_t pa[MBE_FRAME_LEN], pb[MBE_FRAME_LEN];
	int same = 1;

	rig_init(a);
	rig_init(b);
	memset(b->buf, 0x5A, sizeof b->buf);
	memset(&b->cur, 0x5A, sizeof b->cur);
	for (int i = 0; i < GOLD_FRAMES; i++) {
		imbe_dec_frame(&a->m, GOLD_BITS[i], 0, pa);
		imbe_dec_frame(&b->m, GOLD_BITS[i], 0, pb);
		same &= !memcmp(pa, pb, sizeof pa);
	}
	CHECK(same, "two decoders gave different output");
	printf("memory: kept %zu + %zu + %zu B (core state, previous frame, prediction), scratch %zu + %zu + %zu B (frame, FFT, params)\n",
	       sizeof(mbe_state_t), sizeof(mbe_frame_t), sizeof(imbe_dec_state_t), sizeof(mbe_frame_t),
	       sizeof(int32_t) * MBE_BUF_WORDS, sizeof(mbe_params_t));
	free(a);
	free(b);
}

int main(void)
{
	test_dequantizer();
	test_pitch_codes();
	test_tone();
	test_noise();
	test_silence();
	test_level();
	test_conceal();
	test_channel_errors();
	test_extremes();
	test_determinism();
	printf("mbeTest: %d checks, %d failed\n", checks, failures);
	return failures != 0;
}
