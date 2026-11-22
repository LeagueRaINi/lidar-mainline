// SPDX-License-Identifier: GPL-2.0-only
//
// Language of the project page (index.html) and the installer (install.html),
// as in the device's web interface: English is the source and the fallback;
// the markup's texts are read from the page itself (data-i18n: the element's
// HTML; data-i18n-aria, -title, -alt: that attribute), the installer's own
// texts are in STRINGS.en. German has an entry for each. The language was
// chosen before the first paint (the stored choice, else the browser's); the
// EN/DE switch changes it at once and keeps it. Without JavaScript the pages
// stay English. Loaded as a classic script at the end of the body, so the
// markup is translated before the first paint and before installer.js runs.

(function () {
  "use strict";

  const STRINGS = {
    en: {
      "theme": "Theme: {0}", "theme.auto": "auto", "theme.light": "light", "theme.dark": "dark",

      "ins.whatBoot": "an Android boot image", "ins.whatExt4": "a raw ext4 image",
      "ins.available": "available", "ins.missing": "missing", "ins.yes": "yes", "ins.no": "no",
      "ins.needSecure": "Open this page over https, or from http://localhost/ when you run it yourself.",
      "ins.noUsb": "This browser has no WebUSB. Use Chrome, Edge, Brave or Opera on a desktop computer, or follow the install guide (docs/install.md) with the fastboot command-line tool instead.",
      "ins.notChosen": "Not chosen", "ins.checking": "checking", "ins.wrongFile": "wrong file",
      "ins.wrongFileMeta": "{0} is not {1}. Expected {2}.",
      "ins.mismatch": "checksum mismatch", "ins.verified": "verified", "ins.notVerified": "not verified",
      "ins.sumsBad": "{0} does not match {1}: damaged or not that file, it will not be flashed",
      "ins.sumsOk": "{0}, matches {1}", "ins.relOk": "{0}, matches release {1}",
      "ins.relBad": "{0} does not match release {1}: the download is damaged, it will not be flashed. Try again",
      "ins.relOther": ": not the file of release {0} (built yourself, or another version)",
      "ins.release": "release {0}", "ins.sumsFrom": "SHA256SUMS ({0})",
      "ins.progress": "{0}: {1}", "ins.progressOf": "{0}: {1} of {2}",
      "ins.dlFailed": "Download failed ({0}). Try again, or get the files from All releases and pick them below.",
      "ins.allReleases": "All releases", "ins.whyNone": "no release published yet",
      "ins.whyLimit": "GitHub's request limit for this network is used up until {0}", "ins.whyStatus": "GitHub answered {0}",
      "ins.whyOffline": "no connection to GitHub", "ins.whyAsset": "{0} has no usable {1}",
      "ins.dlAsset": "Download {0} ({1})", "ins.notFound": "not found", "ins.unavailable": "unavailable",
      "ins.noRelease": "Could not get the latest release ({0}). Get the images from the releases page, or build them, then pick them below.",
      "ins.getTag": "Download and use {0}",
      "ins.pickBelow": "Download both images, then pick them below. They are checked against the release before anything is flashed.",
      "ins.noSums": "GitHub gave no checksums: compare the SHA-256 shown below with the release's SHA256SUMS yourself.",
      "ins.published": "Published {0}. ", "ins.older": "Older versions and release notes: ",
      "ins.noShaLines": "{0} has no SHA-256 lines.", "ins.sumsLoaded": "{0} loaded: the images are checked against it.",
      "ins.matches": "matches {0}", "ins.notVerifiedLong": "checksum not verified", "ins.describe": "{0} ({1}, {2}) → {3}",
      "ins.noImage": "No image yet: go back to Images and pick {0}.",
      "ins.refusePart": "Refusing to write partition {0}.", "ins.size4096": "The image size is not a multiple of 4096 bytes.",
      "ins.sparse": "This is already a sparse image; pick the raw image instead.",
      "ins.sending": "Sending", "ins.writing": "Writing", "ins.sendingPiece": "Sending piece {0} of {1}",
      "ins.writingPiece": "Writing piece {0} of {1}",
      "ins.refused": "The bootloader refused: {0}.", "ins.noReason": "no reason given",
      "ins.timeout": "No answer from the device. Unplug power, start fastboot again and reconnect.",
      "ins.denied": "Access denied. Close other programs using the Echo, such as fastboot. Windows needs the WinUSB driver, Linux a udev rule (step 1).",
      "ins.noDevice": "No device chosen. An empty list means the Echo is not in fastboot or not seen: check the USB, hold the button while plugging in power, and on Windows the driver (step 1).",
      "ins.lost": "The USB connection was lost.",
      "ins.confirmTitle": "Flash {0}?", "ins.kvPartition": "Partition", "ins.kvImage": "Image", "ins.kvSize": "Size",
      "ins.kvChecksum": "Checksum",
      "ins.p1found": "Kaeru fastboot",
      "ins.p1wrong": "This is U-Boot's fastboot: U-Boot is already on boot_a. Skip this phase (below). For Kaeru's fastboot, unplug power, hold Action and plug in again.",
      "ins.p1warn": "This replaces boot_a. Keep your boot_a dump: it is your way back.",
      "ins.p1recover": "Nothing is lost: Kaeru's fastboot does not depend on boot_a. Unplug power, hold Action, plug in and try again. If the bootloader refuses, the Echo may not be unlocked with Kaeru.",
      "ins.p1done": "U-Boot is on boot_a and the Echo is rebooting. Continue with the System step.",
      "ins.p2found": "U-Boot fastboot",
      "ins.p2wrong": "This is Kaeru's fastboot, not U-Boot's. Either Action was held, or U-Boot is not on boot_a yet (do the U-Boot step first). Unplug power, hold Volume Up (+) and plug in again.",
      "ins.p2warn": "This erases {0} and makes it the slot lidar boots from.",
      "ins.p2recover": "U-Boot's fastboot is still there: unplug power, hold Volume Up (+), plug in and flash again. With an incomplete system, U-Boot opens fastboot by itself. Kaeru's fastboot (Action) also stays available.",
      "ins.p2done": "The system is installed and the Echo is rebooting into lidar.",
      "ins.noPart": "This U-Boot has no {0} partition.", "ins.tooBig": "The image ({0}) is larger than {1} ({2}).",
      "ins.refuseSlot": "Refusing slot {0}.", "ins.selectSlot": "Selecting slot {0}", "ins.rebooting": "Rebooting",
      "ins.flashing": "flashing", "ins.unknown": "unknown device", "ins.wrongMode": "wrong mode", "ins.cannot": "cannot flash",
      "ins.connected": "{0} connected", "ins.notConnected": "not connected", "ins.done": "done", "ins.disconnected": "disconnected",
      "ins.unknownMsg": "Unknown fastboot device: neither Kaeru on an Echo Plus 2nd gen nor lidar's U-Boot. Nothing was written. Unplug other fastboot devices and try again.",
      "ins.nothing": "{0} Nothing was written.", "ins.failed": "Failed: {0}\n{1}", "ins.pct": "{0}, {1} %", "ins.doneProg": "Done",
      "ins.fileProto": "Open this page over https or from a local web server (for example \"python -m http.server\" in this folder, then http://localhost:8000/install.html). It cannot run from a file.",
    },

    de: {
      "theme": "Design: {0}", "theme.auto": "System", "theme.light": "Hell", "theme.dark": "Dunkel",
      "com.language": "Sprache",

      /* shared: top bar and footer */
      "nav.sections": "Abschnitte", "nav.project": "Projekt", "nav.features": "Funktionen", "nav.interface": "Oberfläche",
      "nav.voice": "Sprachsteuerung", "nav.install": "Installieren", "nav.faq": "FAQ",
      "nav.source": "Quellcode auf GitHub", "nav.top": "lidar-mainline, zum Seitenanfang",
      "nav.projectPage": "lidar-mainline, Projektseite", "foot.installer": "Installer", "foot.source": "Quellcode",
      "foot.releases": "Releases", "foot.licence": "Lizenz", "foot.projectPage": "Projektseite",

      /* project page */
      "lp.title": "lidar-mainline: offene Firmware für den Echo Plus (2. Gen.)",
      "lp.skip": "Zum Inhalt springen",
      "lp.tag": "Frühe Testversion v0.0.1",
      "lp.h1": "Dein Echo Plus, <span class=\"grad\">mit offener Firmware.</span>",
      "lp.pitch": "lidar-mainline ersetzt Fire OS auf dem Amazon Echo Plus (2. Generation) durch Mainline-Linux und U-Boot. Ein Bluetooth-Lautsprecher mit eigener Klangabstimmung, ein Lichtring, der mit der Musik mitgeht, ein experimenteller Sprachassistent, der auf dem Gerät selbst zuhört und antwortet, und eine Weboberfläche direkt vom Lautsprecher.",
      "lp.ctaInstall": "Im Browser installieren", "lp.ctaSource": "Quellcode auf GitHub",
      "lp.glance": "Auf einen Blick", "lp.fact1": "<b>Mainline</b>-Linux und U-Boot", "lp.fact2": "<b>Sprachsteuerung</b> auf dem Gerät",
      "lp.fact3": "<b>Bluetooth</b>-Lautsprecher", "lp.fact4": "<b>Open</b> Source",
      "lp.echoTitle": "Ein Echo Plus (2. Generation) von oben: vier Tasten, sieben Mikrofonöffnungen und der Lichtring am Rand, der cyanfarben leuchtet",
      "lp.chip1": "„Hey Jarvis“, direkt auf dem Gerät", "lp.chip2": "Bluetooth mit AAC und SBC", "lp.chip3": "7-Band-Spektrum auf dem Ring",
      "lp.featEyebrow": "Was er kann", "lp.featTitle": "Ein Lautsprecher ohne Blackbox",
      "lp.featSub": "Alles läuft auf dem Echo: kein Konto, keine App, kein Cloud-Dienst dazwischen.",
      "lp.speaker": "Lautsprecher",
      "lp.speakerText": "Spielt als Bluetooth-Lautsprecher (AAC und SBC); zum Koppeln hältst du die Aktionstaste gedrückt, Signaltöne begleiten das. Eine eigene Klangregelung für Bass, Mitten, Höhen und eine Lautstärkeanhebung setzt auf der Original-Abstimmung auf. Lautstärke- und Stummtaste funktionieren wie gewohnt.",
      "lp.ring": "Lichtring",
      "lp.ringText": "Ein Lautstärkebogen, Rot bei Stummschaltung und eine Klanganzeige, die mit der Musik mitgeht, als Pegel oder 7-Band-Spektrum. Der Umgebungslichtsensor dimmt den Ring passend zum Raum.",
      "lp.voice": "Sprachsteuerung", "lp.experimental": "experimentell",
      "lp.voiceText": "„Hey Jarvis“ als Weckwort, sieben Mikrofone mit Echounterdrückung und Beamforming, Spracherkennung auf dem Echo, gesprochene Antworten mit lokalen Piper-Stimmen und ein Assistent für Timer, Wecker, Lautstärke und Musik. Den Rest beantwortet Claude, mit deinem eigenen API-Schlüssel.",
      "lp.webui": "Weboberfläche",
      "lp.webuiText": "Kommt vom Echo selbst: Übersicht, Musikbibliothek, Klang, Sprachsteuerung, Lichtring, WLAN, Bluetooth und Sicherheit. Helles und dunkles Design, auf Deutsch und Englisch, auch fürs Handy gemacht, auf Wunsch mit PIN oder Passwort.",
      "lp.privacy": "Datenschutz",
      "lp.privacyText": "Die Stummtaste trennt die Mikrofone weiterhin per Hardware. Keine Aufnahme verlässt den Echo: Weckwort und Spracherkennung laufen auf ihm. Mit Claude-Schlüssel geht nur der Text von Fragen hinaus, die der Assistent nicht selbst beantworten kann.",
      "lp.open": "Open Source",
      "lp.openText": "Mainline-Linux, U-Boot und die Werkzeuge darüber, größtenteils GPL-2.0 (siehe <a href=\"https://github.com/LeagueRaINi/lidar-mainline/blob/main/LICENSING.md\">Lizenzen</a>). Die Images kannst du selbst bauen; selbst gebaute Images können auch den Zigbee- und Thread-Funk für Home Assistant betreiben.",
      "lp.uiEyebrow": "Weboberfläche", "lp.uiTitle": "Direkt vom Lautsprecher",
      "lp.uiSub": "Öffne den Echo in einem beliebigen Browser in deinem Netzwerk. Keine App nötig; das sind Screenshots vom echten Gerät.",
      "lp.tabOverview": "Übersicht", "lp.tabVoice": "Sprachsteuerung", "lp.tabSound": "Klang", "lp.tabRing": "Lichtring",
      "lp.tabMusic": "Musik", "lp.tabWifi": "WLAN", "lp.tabSecurity": "Sicherheit",
      "lp.altOverview": "Übersicht: Lautstärke, aktuelle Wiedergabe, Statuskacheln für Mikrofone, Klangabstimmung, WLAN und Bluetooth, CPU, Arbeitsspeicher und Temperaturen",
      "lp.altVoice": "Sprachsteuerung: Schalter für Assistent und Mikrofone, Weckwort, Empfindlichkeit, Sprache und eine Live-Ansicht, aus welcher Richtung die Mikrofone hören",
      "lp.altSound": "Klang: Lautstärke, Lautstärkebegrenzung, Klangregelung für Bass, Mitten und Höhen, Lautstärkeanhebung und Status der Klangabstimmung",
      "lp.altRing": "Lichtring: Darstellung der Klanganzeige, Dimmen im Dunkeln, Helligkeit und Lautstärkebogen",
      "lp.altMusic": "Musik: ein Player und eine Bibliothek, in die du Audiodateien auf die Seite ziehst",
      "lp.altBluetooth": "Bluetooth: Funkstatus, gekoppelte Handys, Kopplungsmodus, Lautsprechername, Kopplungsdauer und Töne (Adressen unkenntlich)",
      "lp.altWifi": "WLAN: das verbundene Netzwerk mit Signal und Frequenzband, Suche und Ländereinstellung",
      "lp.altSecurity": "Sicherheit: optionale Anmeldung mit PIN oder Passwort und von wo die Weboberfläche erreichbar ist",
      "lp.capOverview": "Der Lautsprecher auf einen Blick: Lautstärke, Wiedergabe, Mikrofone, Klangabstimmung, Verbindungen und die Hardware.",
      "lp.capVoice": "Weckwort, Empfindlichkeit und Sprache, dazu eine Live-Ansicht des Beamformers: Der Balken, der aufleuchtet, zeigt, wohin der Echo hört.",
      "lp.capSound": "Equalizer für Bass, Mitten und Höhen und eine Lautstärkeanhebung, aufgesetzt auf die Original-Abstimmung des Lautsprechers.",
      "lp.capRing": "Was der Ring zeigt: Lautstärkebogen, Stummschaltung, Pegel oder 7-Band-Spektrum zur Musik, und wie er im Dunkeln dimmt.",
      "lp.capMusic": "Zieh Audiodateien auf die Seite; sie laufen auf dem Echo.",
      "lp.capBluetooth": "Kopplungsmodus, gekoppelte Handys, der Name, den Handys sehen, und die Kopplungstöne.",
      "lp.capWifi": "Das Netzwerk, mit dem sich der Echo verbindet, mit Signal, Frequenzband und Datenrate.",
      "lp.capSecurity": "Eine optionale PIN oder ein Passwort für die ganze Oberfläche, und von wo sie erreichbar ist.",
      "lp.phonesTitle": "Passt in eine Hand",
      "lp.phonesText": "Dieselbe Oberfläche auf dem Handy, mit einer Tab-Leiste unten. Sie folgt dem Systemdesign, hell oder dunkel.",
      "lp.tick1": "Keine App, nur der Browser", "lp.tick2": "Optionale Anmeldung mit PIN oder Passwort",
      "lp.tick3": "Einrichtungsassistent beim ersten Start",
      "lp.altPhoneOverview": "Übersicht auf dem Handy", "lp.altPhoneVoice": "Sprachsteuerung auf dem Handy",
      "lp.voiceEyebrow": "Sprachsteuerung", "lp.voiceTitle": "Vom Weckwort bis zur Antwort, auf dem Echo",
      "lp.voiceSub": "Die Sprachverarbeitung läuft auf dem Lautsprecher. Nur Fragen, die der Assistent nicht selbst beantworten kann, dürfen hinaus – und nur, wenn du das einrichtest.",
      "lp.flow1": "Weckwort", "lp.flow1Text": "„Hey Jarvis“ wird auf dem Gerät erkannt, mit einstellbarer Empfindlichkeit.",
      "lp.flow2": "Richtung", "lp.flow2Text": "Sieben Mikrofone mit Echounterdrückung und Beamforming. Der Ring leuchtet zu der Person hin auf, die spricht.",
      "lp.flow3": "Erkennung", "lp.flow3Text": "Der Echo wandelt Sprache selbst in Text um, auf Englisch oder Deutsch. Das Modell lädt er einmalig herunter.",
      "lp.flow4": "Assistent",
      "lp.flow4Text": "Timer, Wecker, Lautstärke, Musiksteuerung, Uhrzeit und Datum sind eingebaut; die Antwort spricht eine lokale Stimme. Alles andere kann an Claude gehen – mit deinem eigenen API-Schlüssel, wenn du einen einträgst.",
      "lp.altBeam": "Die Karte „Zuhören“ der Weboberfläche: zwölf Balken rund um den Echo, der bei 30 Grad leuchtet, Weckwort 13-mal gehört",
      "lp.beamCap": "Der Beamformer, live in der Weboberfläche.",
      "lp.privTitle": "Datenschutz, angelegt in Hardware und Software",
      "lp.priv1": "Die Stummtaste trennt die Mikrofone per Hardware; der Ring wird rot.",
      "lp.priv2": "Keine Aufnahme verlässt den Echo: Weckwort, Spracherkennung und Sprachausgabe laufen auf ihm.",
      "lp.priv3": "Nur mit Claude-Schlüssel: der Text von Fragen, die der eingebaute Assistent nicht beantworten kann.",
      "lp.routeEcho": "Auf dem Echo", "lp.routeWake": "Weckwort", "lp.routeStt": "Erkennung", "lp.routeAssistant": "Assistent",
      "lp.routeVoice": "Stimme", "lp.routeWire": "nur, was er nicht beantworten kann, mit deinem Schlüssel", "lp.routeOpt": "optional",
      "lp.online": "<b>Online nur, wenn nötig:</b> für die Uhrzeit (NTP), den einmaligen Download des Sprachmodells deiner Sprache und für Claude, wenn du einen Schlüssel einträgst.",
      "lp.instEyebrow": "Installation", "lp.instTitle": "Flashen direkt im Browser",
      "lp.instSub": "Der Installer spricht über USB (WebUSB) direkt aus der Seite mit dem Echo. Er beschreibt zwei Partitionen und sonst nichts.",
      "lp.step1": "Entsperren und sichern",
      "lp.step1Text": "Der Echo muss bereits entsperrt sein (Kaeru-Fastboot), und du brauchst deine eigene Sicherung von <code>boot_a</code>. Diese Seite enthält keinen Code zum Entsperren und keine Exploits; die <a href=\"https://github.com/LeagueRaINi/lidar-mainline/blob/main/docs/install.md#prerequisites\">Installationsanleitung</a> nennt die Voraussetzungen.",
      "lp.step2": "USB anschließen", "lp.step2Text": "Eine USB-Verbindung mit Pogo-Pins zu den Testkontakten auf der Unterseite. Der Installer zeigt die Kontakte mit Fotos.",
      "lp.step3": "Installer öffnen", "lp.step3Text": "In Chrome, Edge, Brave oder Opera auf dem Desktop. Ein Klick lädt die neueste Version herunter und prüft sie.",
      "lp.step4": "Flashen und starten", "lp.step4Text": "Erst U-Boot, dann das System. Der Echo startet lidar, und seine Weboberfläche führt dich durch die Einrichtung.",
      "lp.altJig": "Die Unterseite des Echo mit einem Pogo-Pin-Adapter auf den Testkontakten",
      "lp.need": "Du brauchst", "lp.need1": "Einen Echo Plus (2. Gen.), bereits entsperrt", "lp.need2": "Deine eigene Sicherung von <code>boot_a</code>",
      "lp.need3": "USB mit Pogo-Pins an den Testkontakten", "lp.need4": "Einen Chromium-Browser auf dem Desktop",
      "lp.openInstaller": "Installer öffnen",
      "lp.faqEyebrow": "Stand und Fragen", "lp.faqTitle": "Bevor du flashst",
      "lp.q1": "Wie weit ist es?",
      "lp.a1": "Das sind frühe Testversionen (v0.0.1), und zwischen ihnen ändert sich einiges. Die Sprachsteuerung ist experimentell, und der Installer ist neu: Lies vor dem Start den <a href=\"https://github.com/LeagueRaINi/lidar-mainline/blob/main/docs/status.md\">aktuellen Stand</a>.",
      "lp.q2": "Ist das sicher?",
      "lp.a2": "Der Installer beschreibt nur zwei Partitionen, U-Boot und das System, und fragt vor jeder nach. Fire OS lässt sich aus deiner eigenen Sicherung wiederherstellen. Es bleibt dein Gerät und dein Risiko: Es gibt keine Garantie.",
      "lp.q3": "Kann ich zu Fire OS zurück?",
      "lp.a3": "Ja. Halte die Aktionstaste, während du den Strom einsteckst, um in Kaeru-Fastboot zu kommen, und flashe dann deine Sicherung von <code>boot_a</code>; die <a href=\"https://github.com/LeagueRaINi/lidar-mainline/blob/main/docs/install.md#installing\">Installationsanleitung</a> nennt die Befehle.",
      "lp.q4": "Welche Browser funktionieren?",
      "lp.a4": "Chrome, Edge, Brave und Opera auf einem Desktop-Computer, denn sie haben WebUSB. Firefox und Safari nicht; folge damit der <a href=\"https://github.com/LeagueRaINi/lidar-mainline/blob/main/docs/install.md#installing\">Installationsanleitung</a> und nimm stattdessen das Kommandozeilenwerkzeug <code>fastboot</code>.",
      "lp.q5": "Braucht es ein Konto oder einen Cloud-Dienst?",
      "lp.a5": "Kein Konto. Der Echo geht nur online für die Uhrzeit (NTP), den einmaligen Download des Sprachmodells und für Claude, wenn du deinen eigenen API-Schlüssel einträgst.",
      "lp.q6": "Und Zigbee und Thread?",
      "lp.a6": "Selbst gebaute Images können den Zigbee- und Thread-Funk des Echo für Home Assistant betreiben. Release-Images enthalten diesen Teil nicht: Er braucht Software von Silicon Labs, die mit den Releases nicht weitergegeben wird.",
      "lp.final": "Bereit, wenn dein Echo es ist.",
      "lp.footText": "Offene Firmware für den Echo Plus (2. Gen.). Größtenteils GPL-2.0; siehe <a href=\"https://github.com/LeagueRaINi/lidar-mainline/blob/main/LICENSING.md\">Lizenzen</a>.",
      "lp.legal": "Ein unabhängiges Projekt, weder mit Amazon verbunden noch von Amazon unterstützt oder gesponsert. Echo und zugehörige Marken sind Marken von Amazon.com, Inc. oder seinen verbundenen Unternehmen. Claude ist ein Produkt von Anthropic, das ebenfalls nicht mit diesem Projekt verbunden ist. Keine Garantie; du flashst dein eigenes Gerät auf eigenes Risiko.",

      /* installer */
      "ins.title": "Installer · lidar-mainline", "ins.skip": "Zum Installer springen",
      "ins.early": "frühe Testversion", "ins.h1": "lidar im Browser installieren", "ins.how": "So funktioniert’s",
      "ins.fact1": "Über USB, direkt aus dieser Seite (WebUSB)", "ins.fact2": "Die Images bleiben auf deinem Computer; nichts wird hochgeladen",
      "ins.fact3": "Beschreibt <code>boot_a</code> und <code>system_b</code>, sonst nichts",
      "ins.progressTitle": "Fortschritt", "ins.railTitle": "Echo Plus 2. Gen.", "ins.railSub": "Sechs Schritte, zweimal flashen",
      "ins.steps": "Schritte", "ins.s2": "Bevor es losgeht", "ins.s6": "Fertig",
      "ins.railHelp": "Hängst du fest, oder gibt es kein WebUSB? Die <a href=\"https://github.com/LeagueRaINi/lidar-mainline/blob/main/docs/install.md#installing\" target=\"_blank\" rel=\"noopener\">Installationsanleitung</a> beschreibt den Weg von Hand mit <code>fastboot</code>.",
      "ins.step1of6": "Schritt 1 von 6", "ins.step2of6": "Schritt 2 von 6", "ins.step3of6": "Schritt 3 von 6",
      "ins.step4of6": "Schritt 4 von 6 · Phase 1", "ins.step5of6": "Schritt 5 von 6 · Phase 2", "ins.step6of6": "Schritt 6 von 6",
      "ins.browserCheck": "Browser prüfen", "ins.browserLead": "Diese Seite spricht über USB mit dem Echo. Hochgeladen wird nichts.",
      "ins.webusbMeta": "Chrome, Edge, Brave oder Opera auf dem Desktop. Nicht in Firefox oder Safari.",
      "ins.secure": "Sichere Seite", "ins.secureMeta": "WebUSB braucht https oder localhost.",
      "ins.winSum": "Windows: der WinUSB-Treiber",
      "ins.win1": "Der Browser kann den Echo nur über den Treiber <b>WinUSB</b> öffnen. Funktioniert <code>fastboot</code> mit diesem Echo an diesem PC schon, ist er installiert. Beide Phasen nutzen dieselbe USB-ID, ein Treiber reicht also für beide.",
      "ins.win2": "Fehlt der Echo in der Liste des Browsers, oder kommt „Zugriff verweigert“:",
      "ins.winA": "Starte Kaeru-Fastboot (<b>Aktionstaste</b> halten, Strom einstecken). Der Geräte-Manager zeigt den Echo als <i>Android Bootloader Interface</i>, <i>Android ADB Interface</i> oder unbekanntes Gerät, USB-ID <code>0BB4:0C01</code>.",
      "ins.winB": "Installiere den Treiber, auf eine der beiden Arten:\n            <ul class=\"plain\">\n              <li><a href=\"https://zadig.akeo.ie/\" target=\"_blank\" rel=\"noopener\">Zadig</a>: <code>0BB4 0C01</code> auswählen, <b>WinUSB</b> wählen, „Install Driver“ drücken.</li>\n              <li><a href=\"https://developer.android.com/studio/run/win-usb\" target=\"_blank\" rel=\"noopener\">Google USB Driver</a>: Treiber aktualisieren → Auf meinem Computer nach Treibern suchen → Aus einer Liste verfügbarer Treiber auswählen → Datenträger → <code>android_winusb.inf</code> → <i>Android Bootloader Interface</i>.</li>\n            </ul>",
      "ins.winC": "Steck den Echo neu ein und schließe alles andere, das ihn nutzt, etwa <code>fastboot</code>.",
      "ins.linuxSum": "Linux: USB-Berechtigungen",
      "ins.linux1": "Kommt „Zugriff verweigert“, speichere diese udev-Regel als <code>/etc/udev/rules.d/51-fastboot.rules</code>:",
      "ins.linux2": "Dann <code>sudo udevadm control --reload</code> ausführen und den Echo neu einstecken.",
      "ins.next": "Weiter", "ins.back": "Zurück",
      "ins.beforeLead": "Was du brauchst und was sich am Echo ändert.", "ins.needTitle": "Was du brauchst",
      "ins.need1": "Einen Echo Plus 2. Gen., <b>bereits mit Kaeru-Fastboot entsperrt</b> (Aktionstaste halten, während du den Strom einsteckst). Diese Seite entsperrt nichts: siehe <a id=\"link-prereq\" target=\"_blank\" rel=\"noopener\">Voraussetzungen</a>.",
      "ins.need2": "Deine eigene Sicherung von <code>boot_a</code> aus dem Entsperren. Sie ist dein Weg zurück.",
      "ins.need3": "USB an den Kontakten auf der Unterseite (D+, D−, GND), zum Beispiel mit dem Pogo-Pin-Adapter des Projekts.",
      "ins.need4": "Die beiden Images (nächster Schritt).",
      "ins.altPads": "Die Unterseite des Echo mit aufrechter Etikettenschrift und rechts daneben das Fenster mit sechs goldenen Kontakten. Obere Reihe von links nach rechts: TM45, D+, D−. Untere Reihe: TM35, GND und unten rechts 17 V",
      "ins.padsCap": "Die Kontakte auf der Unterseite, Etikettenschrift aufrecht.",
      "ins.thContact": "Kontakt", "ins.thSignal": "Signal", "ins.thConnect": "Anschließen",
      "ins.pad1": "oben Mitte (TM4)", "ins.pad2": "oben rechts (TM5)", "ins.pad3": "unten Mitte (TM37)", "ins.pad4": "unten rechts (TM33)",
      "ins.pad5": "oben links (TM45)", "ins.pad6": "unten links (TM35)", "ins.ground": "Masse", "ins.hot": "etwa 17 V vom Netzteil",
      "ins.usbId": "USB-ID", "ins.unknownPad": "unbekannt, vermutlich USB-Versorgung aus der Fertigung",
      "ins.padYes": "ja", "ins.padNo": "nein", "ins.padNever": "<b>niemals</b>",
      "ins.altJig": "Der Pogo-Pin-Adapter auf der Unterseite des Echo, sein Kabel führt nach rechts weg",
      "ins.jigCap": "Der Pogo-Pin-Adapter (<code>pogo-jig/</code>): Nur D+, D− und GND sind verdrahtet.",
      "ins.hotCallout": "<b>Der Kontakt unten rechts führt etwa 17 V.</b> Niemals anschließen oder überbrücken.",
      "ins.written": "Was geschrieben wird",
      "ins.bootMeta": "Enthält heute den Kernel von Fire OS. Danach U-Boot, das lidar startet.",
      "ins.systemMeta": "Der Reserve-Update-Slot von Fire OS. Sein Inhalt wird gelöscht.",
      "ins.noFireos": "<b>Fire OS startet danach nicht mehr.</b> Sein System auf <code>system_a</code> bleibt unangetastet (lidar liest daraus die WLAN-Firmware und die Klangabstimmung); deine Sicherung von <code>boot_a</code>, über Kaeru-Fastboot geflasht, bringt es zurück.",
      "ins.nothingElse": "Sonst wird nichts geschrieben; der Installer verweigert jede andere Partition, und Kaeru-Fastboot bleibt zur Wiederherstellung erhalten. Spätere Versionen nutzen <code>system_a</code> auch für Updates: Sie fragen vorher und bieten an, ein vollständiges Fire-OS-Image zu sichern – ohne das gibt es keinen Weg zurück.",
      "ins.slotCallout": "Fire OS läuft normalerweise von <code>system_a</code>. Läuft deins von <code>system_b</code>, hör hier auf: Diese Seite würde es löschen. Installiere dann von Hand mit <code>system_a</code>, wie es die <a href=\"https://github.com/LeagueRaINi/lidar-mainline/blob/main/docs/install.md#installing\" target=\"_blank\" rel=\"noopener\">Installationsanleitung</a> beschreibt.",
      "ins.accept": "Mein Echo ist mit Kaeru-Fastboot entsperrt, und ich habe meine eigene Sicherung von <code>boot_a</code>. Ich nehme hin, dass <code>system_b</code> gelöscht wird und Fire OS erst wieder startet, wenn ich die Sicherung zurückspiele. Auf eigenes Risiko.",
      "ins.imagesLead": "Hol dir die beiden Images der neuesten Version. Sie bleiben auf diesem Computer.",
      "ins.latest": "Neueste Version", "ins.looking": "Suche läuft…", "ins.asking": "GitHub wird nach der neuesten Version gefragt…",
      "ins.get": "Herunterladen und verwenden", "ins.getSub": "Beide Images, geprüft gegen die Version",
      "ins.pick": "Oder wähle vorhandene Dateien", "ins.own": "Selbst gebaute Images",
      "ins.ownText": "Eigene Builds (in <code>out/</code>) erscheinen als „nicht geprüft“. Zum Prüfen schreibst du eine Prüfsummendatei in <code>out/</code> und wählst sie hier:",
      "ins.ubootLead": "<code>lidar-uboot.img</code> nach <code>boot_a</code>, über Kaerus Fastboot.",
      "ins.startKaeru": "Kaeru-Fastboot starten", "ins.unplug": "Strom abziehen. USB bleibt verbunden.",
      "ins.holdAction": "<b>Aktionstaste</b> (der Punkt) halten und Strom einstecken. Ein paar Sekunden weiter halten.",
      "ins.pressConnect": "„Verbinden“ drücken und das Gerät auswählen.",
      "ins.altAction": "Oberseite des Echo: Die Aktionstaste, die mit dem Punkt, ist eingekreist",
      "ins.actionCap": "<b>Aktionstaste</b> (●) halten, dann Strom einstecken.",
      "ins.connect": "Verbinden", "ins.flash": "Flashen", "ins.flashUboot": "U-Boot nach boot_a flashen",
      "ins.skipPhase": "U-Boot schon installiert? Diese Phase überspringen",
      "ins.systemLead": "<code>lidar-test-system.ext4</code> nach <code id=\"p2-part\">system_b</code>, über den Fastboot von U-Boot. Es wird zum Start-Slot.",
      "ins.startUboot": "U-Boot-Fastboot starten",
      "ins.ubootAuto": "Nach Phase 1 öffnet U-Boot seinen Fastboot meist von selbst, weil es noch kein lidar-System zum Starten gibt. Falls nicht:",
      "ins.holdUp": "<b>Lauter-Taste</b> (+) halten und Strom einstecken. Ein paar Sekunden weiter halten.",
      "ins.altUp": "Oberseite des Echo: Die Lauter-Taste, mit + markiert, ist eingekreist",
      "ins.upCap": "<b>Lauter-Taste</b> (+) halten, dann Strom einstecken.", "ins.flashSystem": "System flashen",
      "ins.doneTitle": "Fertig", "ins.doneLead": "Der Echo startet lidar.", "ins.openUi": "Weboberfläche öffnen",
      "ins.ui1": "Lass USB verbunden: Der Echo erscheint als USB-Netzwerkadapter (und als serielle Schnittstelle).",
      "ins.ui2": "Der Echo hat die Adresse <code>169.254.7.2</code>. Gib diesem Adapter eine andere Link-Local-Adresse, zum Beispiel <code>169.254.7.1</code>, Netzmaske <code>255.255.0.0</code>; viele Systeme machen das von selbst.",
      "ins.ui3": "Öffne <a href=\"http://169.254.7.2/\" target=\"_blank\" rel=\"noopener\">http://169.254.7.2/</a> für den Einrichtungsassistenten.",
      "ins.usbNet": "USB-Netzwerk", "ins.diagram": "  PC  169.254.x.y ═══ USB-Netzwerk ═══ 169.254.7.2  Echo      →  http://169.254.7.2/",
      "ins.wrong": "Falls etwas nicht stimmt",
      "ins.fix1": "<b>U-Boot-Fastboot:</b> <b>Lauter-Taste</b> halten, während du den Strom einsteckst, dann den Schritt System wiederholen.",
      "ins.fix2": "<b>Kaeru-Fastboot:</b> <b>Aktionstaste</b> halten, während du den Strom einsteckst; <code>fastboot flash boot_a &lt;deine boot_a-Sicherung&gt;</code> stellt den Originalstart wieder her.",
      "ins.fix3": "Mehr in der <a id=\"link-install\" target=\"_blank\" rel=\"noopener\">Installationsanleitung</a>.",
      "ins.footText": "Offene Firmware für den Echo Plus (2. Gen.). <a id=\"link-repo\" target=\"_blank\" rel=\"noopener\">Quellcode</a> unter GPL-2.0-only; USB-Zugriff über <a href=\"https://github.com/kdrag0n/fastboot.js\" target=\"_blank\" rel=\"noopener\">fastboot.js</a> (MIT).",
      "ins.legal": "Ein unabhängiges Projekt, weder mit Amazon verbunden noch von Amazon unterstützt oder gesponsert. Echo und zugehörige Marken sind Marken von Amazon.com, Inc. oder seinen verbundenen Unternehmen. Keine Garantie; du flashst dein eigenes Gerät auf eigenes Risiko.",
      "ins.cancel": "Abbrechen",

      "ins.whatBoot": "Android-Boot-Image", "ins.whatExt4": "rohes ext4-Image",
      "ins.available": "Vorhanden", "ins.missing": "Fehlt", "ins.yes": "Ja", "ins.no": "Nein",
      "ins.needSecure": "Öffne diese Seite über https oder, wenn du sie selbst bereitstellst, über http://localhost/.",
      "ins.noUsb": "Dieser Browser hat kein WebUSB. Nimm Chrome, Edge, Brave oder Opera auf einem Desktop-Computer, oder folge der Installationsanleitung (docs/install.md) mit dem Kommandozeilenwerkzeug fastboot.",
      "ins.notChosen": "Nicht ausgewählt", "ins.checking": "Wird geprüft", "ins.wrongFile": "Falsche Datei",
      "ins.wrongFileMeta": "{0} ist kein {1}. Erwartet: {2}.",
      "ins.mismatch": "Prüfsumme falsch", "ins.verified": "Geprüft", "ins.notVerified": "Nicht geprüft",
      "ins.sumsBad": "{0} passt nicht zu {1}: beschädigt oder eine andere Datei, sie wird nicht geflasht",
      "ins.sumsOk": "{0}, passt zu {1}", "ins.relOk": "{0}, passt zu Version {1}",
      "ins.relBad": "{0} passt nicht zu Version {1}: Der Download ist beschädigt und wird nicht geflasht. Versuch es noch einmal",
      "ins.relOther": ": nicht die Datei von Version {0} (selbst gebaut oder eine andere Version)",
      "ins.release": "Version {0}", "ins.sumsFrom": "SHA256SUMS ({0})",
      "ins.progress": "{0}: {1}", "ins.progressOf": "{0}: {1} von {2}",
      "ins.dlFailed": "Download fehlgeschlagen ({0}). Versuch es noch einmal, oder hol dir die Dateien unter „Alle Versionen“ und wähle sie unten aus.",
      "ins.allReleases": "Alle Versionen", "ins.whyNone": "noch keine Version veröffentlicht",
      "ins.whyLimit": "das Anfragelimit von GitHub für dieses Netzwerk ist bis {0} aufgebraucht", "ins.whyStatus": "GitHub antwortete mit {0}",
      "ins.whyOffline": "keine Verbindung zu GitHub", "ins.whyAsset": "{0} enthält kein brauchbares {1}",
      "ins.dlAsset": "{0} herunterladen ({1})", "ins.notFound": "Nicht gefunden", "ins.unavailable": "Nicht erreichbar",
      "ins.noRelease": "Die neueste Version ließ sich nicht abrufen ({0}). Hol dir die Images von der Release-Seite oder bau sie selbst, und wähle sie dann unten aus.",
      "ins.getTag": "{0} herunterladen und verwenden",
      "ins.pickBelow": "Lade beide Images herunter und wähle sie dann unten aus. Vor dem Flashen werden sie gegen die Version geprüft.",
      "ins.noSums": "GitHub hat keine Prüfsummen geliefert: Vergleiche die unten angezeigte SHA-256 selbst mit der SHA256SUMS der Version.",
      "ins.published": "Veröffentlicht am {0}. ", "ins.older": "Ältere Versionen und Versionshinweise: ",
      "ins.noShaLines": "{0} enthält keine SHA-256-Zeilen.", "ins.sumsLoaded": "{0} geladen: Die Images werden dagegen geprüft.",
      "ins.matches": "passt zu {0}", "ins.notVerifiedLong": "Prüfsumme nicht geprüft",
      "ins.noImage": "Noch kein Image: Geh zurück zu „Images“ und wähle {0}.",
      "ins.refusePart": "Die Partition {0} wird nicht beschrieben.", "ins.size4096": "Die Größe des Images ist kein Vielfaches von 4096 Bytes.",
      "ins.sparse": "Das ist bereits ein Sparse-Image; wähle stattdessen das rohe Image.",
      "ins.sending": "Senden", "ins.writing": "Schreiben", "ins.sendingPiece": "Senden, Teil {0} von {1}",
      "ins.writingPiece": "Schreiben, Teil {0} von {1}",
      "ins.refused": "Der Bootloader hat abgelehnt: {0}.", "ins.noReason": "ohne Begründung",
      "ins.timeout": "Keine Antwort vom Gerät. Zieh den Strom ab, starte Fastboot neu und verbinde dich erneut.",
      "ins.denied": "Zugriff verweigert. Schließe andere Programme, die den Echo nutzen, etwa fastboot. Windows braucht den WinUSB-Treiber, Linux eine udev-Regel (Schritt 1).",
      "ins.noDevice": "Kein Gerät ausgewählt. Ist die Liste leer, ist der Echo nicht im Fastboot oder wird nicht erkannt: Prüfe die USB-Verbindung und unter Windows den Treiber (Schritt 1), und halte die Taste, während du den Strom einsteckst.",
      "ins.lost": "Die USB-Verbindung ist abgebrochen.",
      "ins.confirmTitle": "{0} flashen?", "ins.kvPartition": "Partition", "ins.kvImage": "Image", "ins.kvSize": "Größe",
      "ins.kvChecksum": "Prüfsumme",
      "ins.p1found": "Kaeru-Fastboot",
      "ins.p1wrong": "Das ist der Fastboot von U-Boot: U-Boot ist schon auf boot_a. Überspring diese Phase (unten). Für Kaerus Fastboot: Strom abziehen, Aktionstaste halten und wieder einstecken.",
      "ins.p1warn": "Das ersetzt boot_a. Behalte deine Sicherung von boot_a: Sie ist dein Weg zurück.",
      "ins.p1recover": "Es ist nichts verloren: Kaerus Fastboot hängt nicht von boot_a ab. Strom abziehen, Aktionstaste halten, einstecken und noch einmal versuchen. Lehnt der Bootloader ab, ist der Echo vielleicht nicht mit Kaeru entsperrt.",
      "ins.p1done": "U-Boot ist auf boot_a, und der Echo startet neu. Weiter mit dem Schritt System.",
      "ins.p2found": "U-Boot-Fastboot",
      "ins.p2wrong": "Das ist Kaerus Fastboot, nicht der von U-Boot. Entweder wurde die Aktionstaste gehalten, oder U-Boot ist noch nicht auf boot_a (erst den Schritt U-Boot erledigen). Strom abziehen, Lauter-Taste (+) halten und wieder einstecken.",
      "ins.p2warn": "Das löscht {0} und macht daraus den Slot, von dem lidar startet.",
      "ins.p2recover": "Der Fastboot von U-Boot ist weiter da: Strom abziehen, Lauter-Taste (+) halten, einstecken und erneut flashen. Bei einem unvollständigen System öffnet U-Boot den Fastboot von selbst. Auch Kaerus Fastboot (Aktionstaste) bleibt verfügbar.",
      "ins.p2done": "Das System ist installiert, und der Echo startet neu in lidar.",
      "ins.noPart": "Dieses U-Boot hat keine Partition {0}.", "ins.tooBig": "Das Image ({0}) ist größer als {1} ({2}).",
      "ins.refuseSlot": "Slot {0} wird nicht verwendet.", "ins.selectSlot": "Slot {0} wird ausgewählt", "ins.rebooting": "Neustart",
      "ins.flashing": "wird geflasht", "ins.unknown": "unbekanntes Gerät", "ins.wrongMode": "falscher Modus", "ins.cannot": "Flashen nicht möglich",
      "ins.connected": "{0} verbunden", "ins.notConnected": "Nicht verbunden", "ins.done": "Fertig", "ins.disconnected": "Getrennt",
      "ins.unknownMsg": "Unbekanntes Fastboot-Gerät: weder Kaeru auf einem Echo Plus 2. Gen. noch das U-Boot von lidar. Es wurde nichts geschrieben. Trenn andere Fastboot-Geräte und versuch es noch einmal.",
      "ins.nothing": "{0} Es wurde nichts geschrieben.", "ins.failed": "Fehlgeschlagen: {0}\n{1}", "ins.pct": "{0}, {1} %", "ins.doneProg": "Fertig",
      "ins.fileProto": "Öffne diese Seite über https oder von einem lokalen Webserver (zum Beispiel „python -m http.server“ in diesem Ordner, dann http://localhost:8000/install.html). Als Datei geöffnet funktioniert sie nicht.",
    },
  };

  let lang = document.documentElement.lang === "de" ? "de" : "en";   // chosen before the first paint
  const hooks = [];
  const ATTRS = [["data-i18n-aria", "aria-label"], ["data-i18n-title", "title"], ["data-i18n-alt", "alt"]];

  /* The text of key in the current language, {0}, {1}, ... filled from args. */
  function t(key, args) {
    const s = STRINGS[lang][key] ?? STRINGS.en[key] ?? key;
    return args == null ? s : s.replace(/\{(\d)\}/g, (m, i) => args[i] ?? m);
  }

  /* A number with d decimals, with a decimal comma in German. */
  function fix(n, d) {
    const s = n.toFixed(d);
    return lang === "de" ? s.replace(".", ",") : s;
  }

  const locale = () => (lang === "de" ? "de-DE" : "en-GB");

  /* The markup in the current language; English comes from the markup itself, the first time. */
  function translate() {
    document.documentElement.lang = lang;
    for (const e of document.querySelectorAll("[data-i18n]")) {
      const k = e.dataset.i18n;
      if (!(k in STRINGS.en)) STRINGS.en[k] = e.innerHTML;
      e.innerHTML = t(k);
    }
    for (const [a, attr] of ATTRS) {
      for (const e of document.querySelectorAll("[" + a + "]")) {
        const k = e.getAttribute(a);
        if (!(k in STRINGS.en)) STRINGS.en[k] = e.getAttribute(attr);
        e.setAttribute(attr, t(k));
      }
    }
    for (const b of document.querySelectorAll(".langsw button")) b.setAttribute("aria-checked", String(b.dataset.lang === lang));
  }

  function setLang(l) {
    if (l === lang) return;
    lang = l;
    try { localStorage.setItem("lang", l); } catch (e) { /* private mode */ }
    translate();
    for (const f of hooks) f();
  }

  translate();
  for (const g of document.querySelectorAll(".langsw")) {
    g.hidden = false;           // without JavaScript there is nothing to switch
    for (const b of g.querySelectorAll("button")) b.addEventListener("click", () => setLang(b.dataset.lang));
  }

  window.i18n = {
    t, fix, locale,
    get lang() { return lang; },
    /* fn runs after every change of language: what the page wrote itself, written again */
    onChange(fn) { hooks.push(fn); },
  };
})();
