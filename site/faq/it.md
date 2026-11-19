<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->
<!-- The FAQ in Italian: as en.md, which says how to add an item -->

## La radio è in un'altra lingua dopo un aggiornamento

Premi Menu, poi 7 (Impostazioni), 2 (Schermo) e Su una volta per arrivare a Lingua, l'ultima riga. Premi OK e scegli Italiano. La scelta Lingua della radio nel programma di installazione web imposta la lingua con cui parte la radio, e un ripristino di fabbrica torna a quella. [Altro](../../docs/user/17-settings.md#display-key-2)

## Il programma di installazione non trova la radio

La radio non è nel bootloader, oppure il cavo USB serve solo a caricare. Spegni la radio, tieni premuti i tasti del bootloader e accendila; lo schermo resta spento. Prova un altro cavo o un'altra porta USB. Su Windows il bootloader può richiedere il suo driver. [Altro](../../docs/user/20-troubleshooting.md#installing)

## Quando programmo la radio, la scelta della porta seriale non si apre o non elenca la mia radio

Chiudi ogni altro programma che possa avere la porta aperta (un CPS, qdmr, un programma per hotspot, un'altra scheda del browser). Verifica che il computer elenchi la porta come porta seriale (su Windows, in Gestione dispositivi). Una radio in modalità rete USB non compare: usa Passa la radio a USB seriale nella pagina, che la mette in modalità seriale fino al riavvio. I cavi con chip FTDI, CP210x e Prolific originali funzionano meglio. I cavi CH340 possono essere inaffidabili per backup e installazioni, e l'installer avvisa quando li trova. Il driver Prolific per Windows disattiva i chip contraffatti, e molti cavi venduti online per queste radio li usano. [Altro](../../docs/user/20-troubleshooting.md#everyday-use)

## Lo schermo resta spento dopo l'installazione

La radio è ancora nel bootloader. Spegnila e riaccendila senza tenere premuto alcun tasto. [Altro](../../docs/user/20-troubleshooting.md#installing)

## La radio resta in modalità aggiornamento

Scegli Radio bloccata in modalità aggiornamento nel programma di installazione web e seleziona la radio. La riavvia o dice cosa fare. Se nulla risponde, togli la batteria per qualche secondo. [Altro](../../docs/user/02-installing.md#if-something-goes-wrong)

## Se RadioFoundry non mi piace, posso tornare indietro?

Sì. Durante l'installazione il programma esegue il backup della radio e del suo codeplug originale, e Ripristina rimette il firmware originale a partire dal file del firmware del produttore. Conserva una copia di quel file. RadioFoundry non cancella mai il bootloader di fabbrica, quindi come ultima risorsa puoi sempre avviarlo a mano e caricare il firmware di fabbrica. [Altro](../../docs/user/02-installing.md#going-back-to-the-stock-firmware)

## Come arrivo al VFO?

In tre modi: Menu, 4 (Zone), dove il VFO è la prima riga; un tasto laterale con la funzione VFO/Canale in Impostazioni > Tasti (o il tasto VFO/MR se la radio lo ha); oppure * e poi 4 (Su VFO) su un canale, che copia il canale nel VFO. [Altro](../../docs/user/07-channels-zones-vfo.md#the-vfo)

## Ho difficoltà a raggiungere il mio hotspot o un ripetitore in particolare in DMR

RadioFoundry usa la calibrazione di frequenza di fabbrica della radio, e gli hotspot sono molto sensibili alla frequenza. Se la tua radio è un po' fuori frequenza, regola Impostazioni > Avanzate > Trim freq., sulle radio che lo hanno. Misura prima l'errore, meglio con un SDR calibrato, un frequenzimetro o il test BER di un hotspot, e inseriscilo con il segno invertito. [Altro](../../docs/user/17-settings.md#advanced-last-row)

## RadioFoundry supporta i modi dati?

Sì. Oltre al packet analogico (APRS e un TNC KISS), RadioFoundry invia dati DMR nativi: messaggi di testo compatibili con le radio DMR commerciali e dati IP con le intestazioni UDP compresse ETSI. Il plug-in gratuito takRF per ATAK ti mette in aria in pochi secondi con il cavo di programmazione e un economico cavo USB OTG per telefono o tablet. [Altro](https://takrf.com/dmr)

## Quali cavi servono per i dati DMR?

Solo il cavo di programmazione della radio. Per collegarlo a un telefono o tablet Android, aggiungi un comune cavo USB OTG. [Altro](https://takrf.com/dmr)

## Il trunking DMR Tier III non mi funziona

È stato provato con veri ripetitori Tier III RadioFoundryGateway, hotspot duplex e simulatori software, con le implementazioni del protocollo confrontate e convalidate su catture di pacchetti di sistemi Tier III commerciali. Apri una segnalazione con i dettagli: il sistema, i suoi canali di controllo e di traffico, le impostazioni del sistema nel codeplug e cosa mostra la radio, con le informazioni di supporto dalla pagina Supporto. [Altro](../../docs/user/09-dmr.md#trunked-systems)

## RadioFoundry supporterà P25 Fase 2?

No. I componenti del ricetrasmettitore RF di queste radio non possono trasmettere né ricevere la modulazione della Fase 2 (H-CPM e H-DQPSK). Lo stesso vale per TETRA. [Altro](../../docs/user/07-channels-zones-vfo.md#p25-trunked-channels)

## Una radio solo FM può fare DMR o gli altri modi digitali?

In generale, no. Non ha i chip necessari. [Altro](../../docs/user/23-radios.md)

## Le chiamate DMR compaiono, ma non c'è audio

Lo schermo dice «Niente codec: voce off»: la radio non ha un codec utilizzabile. Reinstalla con il file del firmware originale della tua radio. [Altro](../../docs/user/02-installing.md#no-codec-no-codec-voice-off)

## Una chiamata mostra «Altro TG: non riprodotto»

Il gruppo di conversazione non è la destinazione del canale né è nella sua lista di ascolto. Premi Monitor, oppure aggiungi il gruppo alla lista dei gruppi RX del canale. [Altro](../../docs/user/20-troubleshooting.md#everyday-use)

## Le impostazioni tornano ai valori predefiniti

La radio ha trovato dati di un altro firmware dove RadioFoundry salva le impostazioni, e non li cancella da sola. [Altro](../../docs/user/20-troubleshooting.md#settings-are-not-kept)

## La radio si è riavviata da sola

Guarda Impostazioni > Avanzate > Log crash e includi le sue righe nella segnalazione insieme alle informazioni di supporto. [Altro](../../docs/user/17-settings.md#crash-log)
