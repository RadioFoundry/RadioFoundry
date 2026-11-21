// SPDX-License-Identifier: MIT
// Copyright (c) 2026 The RadioFoundry Authors
	.caps = CAP_KEYPAD | CAP_ENCODER | (CONFIG_GPS ? CAP_GPS : 0) | (CONFIG_DMR ? CAP_DMR | CAP_BEEP_NOT_ON_AIR : 0) |
		(CONFIG_AM_RX ? CAP_AM_RX : 0) | (CONFIG_BCAST_FM ? CAP_BCAST_FM : 0),
	.bands = bands,
	.band_count = sizeof(bands) / sizeof(bands[0]),
#if CONFIG_RX_EXT
	.rx_ext_count = 3,
	.rx_ext_on = &rx_ext_on,
#endif
	.power_count = sizeof(power_names) / sizeof(power_names[0]),
	.power_names = power_names,
	.power_default = 1,	// low
	.display = &dm32_display,
	.input = &dm32_input,
	.keymap = keymap,
	.keymap_len = sizeof(keymap) / sizeof(keymap[0]),
	.side_keys = side_keys,
	.side_key_names = side_key_names,
	.side_key_count = sizeof(side_keys),
	.home_keys = home_keys,
	.home_key_names = home_key_names,
	.home_key_count = sizeof(home_keys),
	.radio = &dm32_radio,
	.power = &dm32_power,
	.flash = &hrc_flash_spi0,
#if CONFIG_DIAG
	.diag_line = dm32_diag_line,
	.diag_reset = dm32_diag_reset,
#endif
#if CONFIG_DMR
	.dmr = &hrc_dmr,
	.audio = &hrc_audio_pcm,
	.vocoder = &hrc_vocoder,
#if CONFIG_AIR
	.air = &hrc_air,		// the physical layer and the receive copy (modem-hrc7000.md 10)
	.baseband = &hrc_baseband,
#endif
#endif
#if CONFIG_APRS_AFSK
	.afsk = &hrc_afsk,
#endif
#if CONFIG_HOSTLINK
	.host_port = &hrc_host_uart_stream,
#endif
#if CONFIG_NET_PPP
	.netif = &hostlink_ppp_netif,	// IPv4 over PPP on the programming cable's UART (docs/spec/ppp.md)
#endif
#if CONFIG_GPS
	.gps = &hrc_gps,	// CAP_GPS: the module is fitted, but say so only once it has been heard
#endif
	.system = &hrc_system,
	.chip_reg = dm32_chip_reg,
#if CONFIG_BCAST_FM
	.bcast = &rda5807m_hal,
#endif
