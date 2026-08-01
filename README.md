# AirTime

## ALPR Proximity Map (`alpr-map.html`)

A single-file, offline-first web app that shows nearby ALPR (Flock and
similar) cameras on a street map and alerts you when you approach one.
Camera locations come from the crowdsourced DeFlock tags in OpenStreetMap
(via the Overpass API). The app is passive — it reads public map data and
your own GPS, transmits nothing to any camera, and interferes with nothing.

**Run it:** open `alpr-map.html` in a browser as a local file or hosted
page (it needs `localStorage`, so not inside a sandboxed iframe), grant
location access, and drive. Camera data and settings are cached locally so
it keeps working offline after the first successful load.

- Alert radius, fetch radius, cache age, and alert channels (beep /
  banner / vibration) are configurable in the settings drawer (⚙).
- Markers: grey = known camera, amber = within 2× alert radius,
  red = inside alert radius. Tap one for operator/brand/direction details.
- The dataset is crowdsourced and incomplete — an empty area does **not**
  mean no cameras.
