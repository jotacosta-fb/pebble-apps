// Battery Days — JS companion. The watch app needs no battery data
// from the phone; this only reports whether it is currently night,
// computed from Open-Meteo's sunrise/sunset for the phone's
// location (same approach as _weather), for the night-mode face.
function fetchNight() {
  navigator.geolocation.getCurrentPosition(
    function (pos) {
      var url = 'https://api.open-meteo.com/v1/forecast' +
        '?latitude=' + pos.coords.latitude +
        '&longitude=' + pos.coords.longitude +
        '&daily=sunrise,sunset&timeformat=unixtime&forecast_days=1';
      var xhr = new XMLHttpRequest();
      xhr.onload = function () {
        try {
          var j = JSON.parse(this.responseText);
          var now = Math.floor(Date.now() / 1000);
          var night = (now < j.daily.sunrise[0] || now >= j.daily.sunset[0]) ? 1 : 0;
          Pebble.sendAppMessage({ 'NIGHT': night }, function () {
            console.log('Night sent: ' + night);
          }, function (e) {
            console.log('Send failed: ' + JSON.stringify(e));
          });
        } catch (err) {
          console.log('Parse error: ' + err);
        }
      };
      xhr.onerror = function () { console.log('Network error'); };
      xhr.open('GET', url);
      xhr.send();
    },
    function (err) {
      console.log('Location error: ' + err.message);
    },
    { timeout: 15000, maximumAge: 60000 }
  );
}

Pebble.addEventListener('ready', function () {
  fetchNight();
});

Pebble.addEventListener('appmessage', function () {
  fetchNight();
});
