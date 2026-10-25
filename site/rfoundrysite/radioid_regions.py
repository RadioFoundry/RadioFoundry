# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""The region list of the Codeplug Editor's RadioID source (site/static/js/codeplug/sources/regions.js): the country
and state names RadioID.net uses, so the page can offer them before any user data is loaded and query its API by
them. Only the names are kept, never user records. Rerun when RadioID adds a country:

    curl -o user.csv https://radioid.net/static/user.csv
    python3 site/rfoundrysite/radioid_regions.py user.csv site/static/js/codeplug/sources/regions.js
"""

import collections
import csv
import json
import sys

# A state is listed when it has this many users; below it the names are mostly typos
MIN_STATE = 5

# ISO 3166-1 alpha-2 codes for RadioID's country names (its spelling, including its variants)
ISO = {
	"Aaland Islands": "AX", "Aland Islands": "AX", "Albania": "AL", "Algeria": "DZ", "American Samoa": "AS",
	"Andorra": "AD", "Anguilla": "AI", "Antigua and Barbuda": "AG", "Argentina Republic": "AR", "Armenia": "AM",
	"Aruba": "AW", "Ascension Island": "AC", "Australia": "AU", "Austria": "AT", "Azerbaijan": "AZ", "Bahamas": "BS",
	"Bahrain": "BH", "Bangladesh": "BD", "Barbados": "BB", "Belarus": "BY", "Belgium": "BE", "Belize": "BZ",
	"Bermuda": "BM", "Bolivia": "BO", "Bonaire": "BQ", "Bosnia and Hercegovina": "BA", "Botswana": "BW",
	"Brazil": "BR", "British Virgin Islands": "VG", "Brunei Darussalam": "BN", "Bulgaria": "BG", "Burkina Faso": "BF",
	"Cameroon": "CM", "Canada": "CA", "Cape Verde": "CV", "Cayman Islands": "KY", "Central African Republic": "CF",
	"Chad": "TD", "Chile": "CL", "China": "CN", "Colombia": "CO", "Cook Islands": "CK", "Corsica": "FR",
	"Costa Rica": "CR", "Croatia": "HR", "Cuba": "CU", "Curacao": "CW", "Cyprus": "CY", "Czech Republic": "CZ",
	"Denmark": "DK", "Djibouti": "DJ", "Dominica": "DM", "Dominican Republic": "DO", "Ecuador": "EC", "Egypt": "EG",
	"El Salvador": "SV", "Equatorial Guinea": "GQ", "Estonia": "EE", "Falkland Islands": "FK", "Faroe Islands": "FO",
	"Finland": "FI", "France": "FR", "French Guiana": "GF", "Georgia": "GE", "Germany": "DE", "Ghana": "GH",
	"Gibraltar": "GI", "Greece": "GR", "Greenland": "GL", "Grenada": "GD", "Guadeloupe": "GP", "Guam": "GU",
	"Guatemala": "GT", "Guinea": "GN", "Guyana": "GY", "Haiti": "HT", "Honduras": "HN", "Hong Kong": "HK",
	"Hungary": "HU", "Iceland": "IS", "India": "IN", "Indonesia": "ID", "Iran": "IR", "Iraq": "IQ", "Ireland": "IE",
	"Israel": "IL", "Italy": "IT", "Ivory Coast": "CI", "Jamaica": "JM", "Japan": "JP", "Jordan": "JO",
	"Kazakhstan": "KZ", "Kenya": "KE", "Kiribati": "KI", "Korea Republic of": "KR", "Kosovo": "XK", "Kuwait": "KW",
	"Kyrgyzstan": "KG", "Laos": "LA", "Latvia": "LV", "Lebanon": "LB", "Lesotho": "LS", "Liberia": "LR",
	"Liechtenstein": "LI", "Lithuania": "LT", "Luxemburg": "LU", "Macao": "MO", "Macedonia": "MK",
	"Madagascar": "MG", "Malawi": "MW", "Malaysia": "MY", "Maldives": "MV", "Malta": "MT", "Martinique": "MQ",
	"Mauretania": "MR", "Mauritius": "MU", "Mexico": "MX", "México": "MX", "Micronesia": "FM", "Moldova": "MD",
	"Monaco": "MC", "Mongolia": "MN", "Montenegro": "ME", "Montserrat": "MS", "Morocco": "MA", "Mozambique": "MZ",
	"Namibia": "NA", "Nepal": "NP", "Netherlands": "NL", "New Caledonia": "NC", "New Zealand": "NZ",
	"Nicaragua": "NI", "Nigeria": "NG", "Norway": "NO", "Oman": "OM", "Pakistan": "PK", "Palau": "PW",
	"Panama": "PA", "Papua New Guinea": "PG", "Paraguay": "PY", "Peru": "PE", "Philippines": "PH", "Poland": "PL",
	"Portugal": "PT", "Puerto Rico": "PR", "Qatar": "QA", "Reunion": "RE", "Romania": "RO", "Russia": "RU",
	"Saint Kitts and Nevis": "KN", "Saint Lucia": "LC", "Saint Vincent and the Grenadines": "VC", "Samoa": "WS",
	"San Marino": "SM", "Saudi Arabia": "SA", "Senegal": "SN", "Serbia": "RS", "Seychelles": "SC",
	"Singapore": "SG", "Slovakia": "SK", "Slovenia": "SI", "South Africa": "ZA", "Spain": "ES", "Sri Lanka": "LK",
	"Sudan": "SD", "Suriname": "SR", "Swasiland": "SZ", "Sweden": "SE", "Switzerland": "CH", "Taiwan": "TW",
	"Tanzania": "TZ", "Thailand": "TH", "Togo": "TG", "Tonga": "TO", "Trinidad and Tobago": "TT", "Turkiye": "TR",
	"Türkiye": "TR", "U.S. Virgin Islands": "VI", "Ukraine": "UA", "United Arab Emirates": "AE",
	"United Kingdom": "GB", "United States": "US", "Uruguay": "UY", "Uzbekistan": "UZ", "Vanuatu": "VU",
	"Venezuela": "VE", "Vietnam": "VN", "Yemen": "YE", "Zambia": "ZM", "Zimbabwe": "ZW",
}

# Postal abbreviations where they're in common use on the air
ABBREV = {
	"United States": {
		"Alabama": "AL", "Alaska": "AK", "Arizona": "AZ", "Arkansas": "AR", "California": "CA", "Colorado": "CO",
		"Connecticut": "CT", "Delaware": "DE", "District of Columbia": "DC", "Florida": "FL", "Georgia": "GA",
		"Hawaii": "HI", "Idaho": "ID", "Illinois": "IL", "Indiana": "IN", "Iowa": "IA", "Kansas": "KS",
		"Kentucky": "KY", "Louisiana": "LA", "Maine": "ME", "Maryland": "MD", "Massachusetts": "MA", "Michigan": "MI",
		"Minnesota": "MN", "Mississippi": "MS", "Missouri": "MO", "Montana": "MT", "Nebraska": "NE", "Nevada": "NV",
		"New Hampshire": "NH", "New Jersey": "NJ", "New Mexico": "NM", "New York": "NY", "North Carolina": "NC",
		"North Dakota": "ND", "Ohio": "OH", "Oklahoma": "OK", "Oregon": "OR", "Pennsylvania": "PA",
		"Rhode Island": "RI", "South Carolina": "SC", "South Dakota": "SD", "Tennessee": "TN", "Texas": "TX",
		"Utah": "UT", "Vermont": "VT", "Virginia": "VA", "Washington": "WA", "West Virginia": "WV", "Wisconsin": "WI",
		"Wyoming": "WY",
	},
	"Canada": {
		"Alberta": "AB", "British Columbia": "BC", "Manitoba": "MB", "New Brunswick": "NB", "Newfoundland": "NL",
		"Nova Scotia": "NS", "Northwest Territories": "NT", "Nunavut": "NU", "Ontario": "ON",
		"Prince Edward Island": "PE", "Quebec": "QC", "Saskatchewan": "SK", "Yukon": "YT",
	},
	"Australia": {
		"Australian Capital Territory": "ACT", "New South Wales": "NSW", "Northern Territory": "NT",
		"Queensland": "QLD", "South Australia": "SA", "Tasmania": "TAS", "Victoria": "VIC", "Western Australia": "WA",
	},
}


def regions(path):
	countries = collections.Counter()
	states = collections.defaultdict(collections.Counter)
	with open(path, encoding="utf-8", errors="replace", newline="") as f:
		for r in csv.DictReader(f):
			c = (r.get("COUNTRY") or "").strip()
			if not c:
				continue
			countries[c] += 1
			s = (r.get("STATE") or "").strip()
			if s:
				states[c][s] += 1
	out = []
	for c in sorted(countries, key=lambda x: x.lower()):
		# case variants ("District Of Columbia") fold into the most used spelling
		seen = {}
		for s, n in states[c].most_common():
			if n >= MIN_STATE and s.lower() not in seen:
				seen[s.lower()] = s
		names = sorted(seen.values(), key=lambda x: x.lower()) if len(seen) >= 2 else []
		out.append({"name": c, "cc": ISO.get(c, ""), "states": names})
	missing = [c["name"] for c in out if not c["cc"]]
	if missing:
		print("no ISO code for: " + ", ".join(missing), file=sys.stderr)
	return out


def main():
	src, dst = sys.argv[1:3]
	data = regions(src)
	with open(dst, "w", encoding="utf-8") as f:
		f.write("// SPDX-License-Identifier: MIT\n// Copyright (c) 2026 The RadioFoundry Authors\n")
		f.write("// Generated by site/rfoundrysite/radioid_regions.py: RadioID.net's country and state names (no user data),\n")
		f.write("// ISO 3166 country codes, and the postal abbreviations of US states, Canadian provinces and Australian states.\n\n")
		f.write("export const COUNTRIES = [\n")
		for c in data:
			f.write("\t%s,\n" % json.dumps(c, ensure_ascii=False))
		f.write("];\n\nexport const ABBREV = %s;\n" % json.dumps(ABBREV, ensure_ascii=False, indent="\t").replace("\n", "\n"))
	print("%d countries, %d with states" % (len(data), sum(1 for c in data if c["states"])))


if __name__ == "__main__":
	main()
