<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->
<!-- The FAQ in Spanish: as en.md, which says how to add an item -->

## La radio está en otro idioma tras una actualización

Pulsa Menú, luego 7 (Ajustes), 2 (Pantalla) y Arriba una vez para llegar a Idioma, la última fila. Pulsa OK y elige Español. La opción Idioma de la radio del instalador web fija el idioma con el que arranca la radio, y un restablecimiento de fábrica vuelve a él. [Más](../../docs/user/17-settings.md#display-key-2)

## El instalador no encuentra mi radio

La radio no está en su gestor de arranque, o el cable USB solo carga. Apaga la radio, mantén pulsadas sus teclas de arranque y enciéndela; la pantalla queda apagada. Prueba otro cable o puerto USB. En Windows el gestor de arranque puede necesitar su controlador. [Más](../../docs/user/20-troubleshooting.md#installing)

## Al programar la radio, el selector de puerto serie no se abre o no muestra mi radio

Cierra cualquier otro programa que pueda tener el puerto abierto (un CPS, qdmr, un programa de hotspot, otra pestaña del navegador). Comprueba que el ordenador muestra el puerto como puerto serie (en Windows, en el Administrador de dispositivos). Una radio en modo de red USB no aparece: usa Cambiar la radio a USB serie en la página, que la pone en modo serie hasta que se reinicie. Los cables con chips FTDI, CP210x y Prolific originales son los que mejor funcionan. Los cables CH340 pueden fallar en copias de seguridad e instalaciones, y el instalador avisa de ellos. El controlador de Prolific para Windows desactiva los chips falsificados, y muchos cables vendidos en internet para estas radios los llevan. [Más](../../docs/user/20-troubleshooting.md#everyday-use)

## La pantalla queda apagada tras instalar

La radio sigue en su gestor de arranque. Apágala y enciéndela sin pulsar ninguna tecla. [Más](../../docs/user/20-troubleshooting.md#installing)

## La radio se queda en modo de actualización

Elige Radio atascada en modo de actualización en el instalador web y selecciona la radio. La reinicia o te dice qué hacer. Si nada responde, quita la batería unos segundos. [Más](../../docs/user/02-installing.md#if-something-goes-wrong)

## Si RadioFoundry no me gusta, ¿puedo volver atrás?

Sí. El instalador hace una copia de seguridad de tu radio y de su codeplug original al instalar, y Restaurar vuelve a poner el firmware original a partir del archivo de firmware del fabricante. Guarda una copia de ese archivo. RadioFoundry nunca borra el gestor de arranque de fábrica, así que como último recurso siempre puedes iniciarlo a mano y cargar el firmware de fábrica. [Más](../../docs/user/02-installing.md#going-back-to-the-stock-firmware)

## ¿Cómo llego al VFO?

De tres formas: Menú, 4 (Zonas), donde el VFO es la primera fila; una tecla lateral con la función VFO/Canal en Ajustes > Teclas (o la tecla VFO/MR si la radio la tiene); o * y luego 4 (A VFO) en un canal, que copia el canal al VFO. [Más](../../docs/user/07-channels-zones-vfo.md#the-vfo)

## Me cuesta acceder a mi hotspot o a un repetidor concreto en DMR

RadioFoundry usa la calibración de frecuencia de fábrica de tu radio, y los hotspots son muy sensibles a la frecuencia. Si tu radio está un poco desviada, ajusta Ajustes > Avanzado > Ajuste frec, en las radios que lo tienen. Mide antes el error, mejor con un SDR calibrado, un frecuencímetro o la prueba BER de un hotspot, e introdúcelo con el signo cambiado. [Más](../../docs/user/17-settings.md#advanced-last-row)

## ¿RadioFoundry hace modos de datos?

Sí. Además de packet analógico (APRS y un TNC KISS), RadioFoundry envía datos DMR nativos: mensajes de texto compatibles con radios DMR comerciales y datos IP con las cabeceras UDP comprimidas de ETSI. El complemento gratuito takRF para ATAK te pone en el aire en segundos con tu cable de programación y un cable USB OTG económico para tu teléfono o tableta. [Más](https://takrf.com/dmr)

## ¿Qué cables necesito para datos DMR?

Solo el cable de programación de tu radio. Para conectarlo a un teléfono o tableta Android, añade un cable USB OTG corriente. [Más](https://takrf.com/dmr)

## El trunking DMR Tier III no me funciona

Se ha probado con repetidores Tier III reales de RadioFoundryGateway, hotspots dúplex y simuladores de software, con las implementaciones del protocolo comparadas y validadas con capturas de paquetes de sistemas Tier III comerciales. Abre una incidencia con los detalles: el sistema, sus canales de control y de tráfico, los ajustes del sistema en tu codeplug y lo que muestra la radio, con la información de soporte de la página Soporte. [Más](../../docs/user/09-dmr.md#trunked-systems)

## ¿RadioFoundry admitirá P25 Fase 2?

No. Los componentes del transceptor de RF de estas radios no pueden emitir ni recibir la modulación de la Fase 2 (H-CPM y H-DQPSK). Lo mismo vale para TETRA. [Más](../../docs/user/07-channels-zones-vfo.md#p25-trunked-channels)

## ¿Puede una radio solo FM hacer DMR u otros modos digitales?

En general, no. No tiene los chips necesarios. [Más](../../docs/user/23-radios.md)

## Se ven llamadas DMR, pero no hay audio

La pantalla dice «Sin códec: voz apagada»: la radio no tiene un códec utilizable. Instala de nuevo con el archivo de firmware original de tu radio. [Más](../../docs/user/02-installing.md#no-codec-no-codec-voice-off)

## Una llamada muestra «Otro TG: no se oye»

El grupo de conversación no es el destino del canal ni está en su lista de escucha. Pulsa Monitor o añade el grupo a la lista de grupos RX del canal. [Más](../../docs/user/20-troubleshooting.md#everyday-use)

## Mis ajustes vuelven a los valores por defecto

La radio encontró datos de otro firmware donde RadioFoundry guarda sus ajustes, y no los borra por su cuenta. [Más](../../docs/user/20-troubleshooting.md#settings-are-not-kept)

## La radio se reinició sola

Mira Ajustes > Avanzado > Log de fallos e incluye sus líneas en tu informe junto con la información de soporte. [Más](../../docs/user/17-settings.md#crash-log)
