# SPDX-License-Identifier: MIT
# Copyright (c) 2026 The RadioFoundry Authors
"""Checked-in satellite TLE snapshot used when no daily cache is available."""

DISPLAY_NAMES = {
    25544: "ISS (ZARYA)",
    27607: "SO-50",
    43017: "AO-91",
}

FALLBACK_RECORDS = [
    {
        "catalog_id": 25544,
        "name": "ISS (ZARYA)",
        "line1": "1 25544U 98067A   26317.20662175  .00008559  00000+0  16132-3 0  9990",
        "line2": "2 25544  51.6325 282.6584 0007349  20.2588 339.8690 15.49346920594859",
    },
    {
        "catalog_id": 27607,
        "name": "SO-50",
        "line1": "1 27607U 02058C   26316.49281663  .00001582  00000+0  20311-3 0  9999",
        "line2": "2 27607  64.5539  30.0380 0068124 235.0518 124.4178 14.83333417286454",
    },
    {
        "catalog_id": 43017,
        "name": "AO-91",
        "line1": "1 43017U 17073E   26317.60524444  .00008151  00000+0  33970-3 0  9990",
        "line2": "2 43017  97.4443 181.7665 0144887 171.0350 189.3523 15.14807395487992",
    },
]