// MinMax Weather — PebbleKit JS companion.
// Fetches today's forecast from Open-Meteo (free, no API key) and sends
// only what the watch shows: min/max temp — since v2.7.8 the FEELS-LIKE
// (apparent) pair, not the raw temperature — precipitation probability,
// and the current WMO weather code.

function fetchFor(lat, lon) {
  var url = 'https://api.open-meteo.com/v1/forecast' +
    '?latitude=' + lat +
    '&longitude=' + lon +
    '&daily=apparent_temperature_max,apparent_temperature_min,precipitation_probability_max,sunrise,sunset' +
    '&timeformat=unixtime' +
    '&hourly=precipitation_probability' +
    '&current=weather_code' +
    '&timezone=auto&forecast_days=1';

  var xhr = new XMLHttpRequest();
  xhr.onload = function () {
    try {
      var j = JSON.parse(this.responseText);
      var precip = j.daily.precipitation_probability_max[0];
      if (precip === null || precip === undefined) {
        // Fall back to the max hourly probability for today
        precip = 0;
        var hourly = j.hourly.precipitation_probability || [];
        for (var i = 0; i < hourly.length && i < 24; i++) {
          if (hourly[i] !== null && hourly[i] > precip) precip = hourly[i];
        }
      }
      var now = Math.floor(Date.now() / 1000);
      var night = (now < j.daily.sunrise[0] || now >= j.daily.sunset[0]) ? 1 : 0;
      try { localStorage.setItem('lastFetchAt', String(Date.now())); } catch (e) {}
      Pebble.sendAppMessage({
        'TEMP_MAX': Math.round(j.daily.apparent_temperature_max[0]),
        'TEMP_MIN': Math.round(j.daily.apparent_temperature_min[0]),
        'PRECIP': Math.round(precip),
        'CODE': j.current.weather_code,
        'NIGHT': night,
        'SUNRISE': j.daily.sunrise[0],
        'SUNSET': j.daily.sunset[0]
      }, function () {
        console.log('Weather sent');
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
}

function fetchWeather() {
  // Fetch throttle (v2.7.9, mirrors the watch side): a fetch
  // younger than 30 minutes is reused — the watch is showing
  // it already, so skip the location + network round trip.
  try {
    var last = parseInt(localStorage.getItem('lastFetchAt') || '0', 10);
    if (last && Date.now() - last < 30 * 60 * 1000) {
      console.log('Fetch skipped: last one is still fresh');
      return;
    }
  } catch (e) {}
  navigator.geolocation.getCurrentPosition(
    function (pos) {
      // Remember the last good fix: a later location failure
      // must not silently kill the update (v2.7.5 — the watch
      // sat in night mode all morning on stale sunrise/sunset
      // when a fetch died this way).
      try {
        localStorage.setItem('lastLat', pos.coords.latitude);
        localStorage.setItem('lastLon', pos.coords.longitude);
      } catch (e) {}
      fetchFor(pos.coords.latitude, pos.coords.longitude);
    },
    function (err) {
      console.log('Location error: ' + err.message);
      var lat = null, lon = null;
      try {
        lat = localStorage.getItem('lastLat');
        lon = localStorage.getItem('lastLon');
      } catch (e) {}
      if (lat !== null && lon !== null) {
        fetchFor(parseFloat(lat), parseFloat(lon));
      }
    },
    { timeout: 15000, maximumAge: 60000 }
  );
}

Pebble.addEventListener('ready', function () {
  fetchWeather();
});

Pebble.addEventListener('appmessage', function () {
  fetchWeather();
});
