make -s BOARD=dm32 CONFIG_SATELLITE_TLE=y BUILD=build/dm32-satellite
make -s BOARD=dm32 VARIANT=amateur BUILD=build/dm32-amateur

make -s BOARD=mduv380 POWER=5w CONFIG_SATELLITE_TLE=y BUILD=build/rt3s-satellite
make -s BOARD=mduv380 VARIANT=amateur BUILD=build/rt3s-amateur

.venv/bin/python site/build.py
.venv/bin/python -m http.server 8000 --bind 0.0.0.0 --directory build/site