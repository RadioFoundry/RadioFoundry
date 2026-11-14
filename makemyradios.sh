make -s BOARD=dm32 CONFIG_SATELLITE_TLE=y BUILD=build/dm32-satellite
make -s BOARD=mduv380 CONFIG_SATELLITE_TLE=y BUILD=build/rt3s-satellite

.venv/bin/python site/build.py
.venv/bin/python -m http.server 8000 --bind 0.0.0.0 --directory build/site