// MinMax Weather — PebbleKit JS companion.
// Fetches today's forecast from Open-Meteo (free, no API key),
// pinned to the ECMWF IFS model (user, 2026-10-29 — the default
// model blend understated the day's rain chances: 19% vs 37%
// for Toronto on 2026-10-29) and sends
// only what the watch shows: min/max temp (the raw temperature pair;
// v2.7.8–2.7.12 tried the feels-like/apparent pair and the user found
// it read too low, so v2.7.13 reverts), precipitation probability,
// and the current WMO weather code.

// Remaining-hours precipitation (user, v2.7.23): the watch
// shows the highest precipitation probability over the hours
// still left today — current hour through 23:00 local — and
// the hour it happens at. A past peak no longer counts: 90%
// at 11am with 12% at noon and zeros after shows 12% . 12:00,
// not 90%. Ties take the earliest hour. If the hourly series
// is missing entirely, fall back to the whole-day max with
// no hour attached.
function precipRemaining(j, now) {
  var times = (j.hourly && j.hourly.time) || [];
  var probs = (j.hourly && j.hourly.precipitation_probability) || [];
  var hourStart = Math.floor(now / 3600) * 3600;
  var best = -1, bestHour = -1;
  for (var i = 0; i < times.length && i < probs.length; i++) {
    if (times[i] < hourStart) continue;
    var p = probs[i];
    if (p === null || p === undefined) continue;
    if (p > best) {
      best = p;
      bestHour = new Date(times[i] * 1000).getHours();
    }
  }
  if (best < 0) {
    var d = j.daily && j.daily.precipitation_probability_max;
    if (d && d[0] !== null && d[0] !== undefined) {
      return { precip: d[0], hour: -1 };
    }
    return { precip: 0, hour: -1 };
  }
  return { precip: best, hour: bestHour };
}

function fetchFor(lat, lon) {
  var url = 'https://api.open-meteo.com/v1/forecast' +
    '?latitude=' + lat +
    '&longitude=' + lon +
    '&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset' +
    '&timeformat=unixtime' +
    '&hourly=precipitation_probability' +
    '&current=weather_code' +
    '&timezone=auto&forecast_days=1&models=ecmwf_ifs';

  var xhr = new XMLHttpRequest();
  xhr.onload = function () {
    try {
      var j = JSON.parse(this.responseText);
      var now = Math.floor(Date.now() / 1000);
      var rem = precipRemaining(j, now);
      var precip = rem.precip;
      var night = (now < j.daily.sunrise[0] || now >= j.daily.sunset[0]) ? 1 : 0;
      try { localStorage.setItem('lastFetchAt', String(Date.now())); } catch (e) {}
      Pebble.sendAppMessage({
        'TEMP_MAX': Math.round(j.daily.temperature_2m_max[0]),
        'TEMP_MIN': Math.round(j.daily.temperature_2m_min[0]),
        'PRECIP': Math.round(precip),
        'PRECIP_HOUR': rem.hour,
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

function fetchWeather(force) {
  // Fetch throttle (v2.7.9, mirrors the watch side): a fetch
  // younger than 30 minutes is reused — the watch is showing
  // it already, so skip the location + network round trip.
  // A forced fetch (Select on the watch, v2.7.22) bypasses
  // this throttle: the watch's FORCE flag arrives in the
  // appmessage payload, so Select always means a real
  // location + network round trip on the phone too.
  if (!force) {
    try {
      var last = parseInt(localStorage.getItem('lastFetchAt') || '0', 10);
      if (last && Date.now() - last < 30 * 60 * 1000) {
        console.log('Fetch skipped: last one is still fresh');
        return;
      }
    } catch (e) {}
  }
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

Pebble.addEventListener('appmessage', function (e) {
  var force = e.payload && e.payload.FORCE === 1;
  fetchWeather(force);
});
