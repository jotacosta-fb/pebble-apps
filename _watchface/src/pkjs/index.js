// PebbleKit JS: reicht ausschliesslich die Clay-Einstellungen an die Uhr
// weiter. Die Zeit liest die Uhr selbst -- im laufenden Betrieb gibt es
// deshalb keinerlei Datenverkehr zum Telefon.

var Clay = require('@rebble/clay');
var clayConfig = require('./config');

var clay = new Clay(clayConfig);

// --- _watchface addition: the place for the on-watch sunrise/
// sunset maths. Sent once, throttled to once a day via
// localStorage; the watch persists it and needs no runtime
// traffic (the original Lean design goal). Opening the settings
// page forces a fresh send: that is the natural "sync now"
// moment, and it must not be stuck with a day-old cached fix.
(function () {
  var KEY = 'watchface_loc_sent_at';
  function sendPlace(force) {
    var last = 0;
    try { last = parseInt(localStorage.getItem(KEY) || '0', 10); } catch (e) {}
    if (!force && Date.now() - last < 24 * 3600 * 1000) return;
    if (!navigator.geolocation) return;
    navigator.geolocation.getCurrentPosition(function (pos) {
      var payload = {
        'LAT': Math.round(pos.coords.latitude * 1e6),
        'LON': Math.round(pos.coords.longitude * 1e6)
      };
      Pebble.sendAppMessage(payload, function () {
        try { localStorage.setItem(KEY, '' + Date.now()); } catch (e) {}
        console.log('Place sent for sunrise/sunset');
      }, function (err) {
        console.log('Place send failed: ' + JSON.stringify(err));
      });
    }, function (err) {
      console.log('No location for sunrise/sunset: ' + err.message);
    }, { timeout: 15000, maximumAge: 10 * 60 * 1000 });
  }
  Pebble.addEventListener('ready', function () { sendPlace(false); });
  Pebble.addEventListener('showConfiguration', function () { sendPlace(true); });
})();
