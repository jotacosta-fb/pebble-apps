// MinMax Weather — PebbleKit JS companion.
// Fetches today's forecast from Open-Meteo (free, no API key) and sends
// only what the watch shows: min/max temp, precipitation probability,
// and the current WMO weather code.

function fetchWeather() {
  navigator.geolocation.getCurrentPosition(
    function (pos) {
      var lat = pos.coords.latitude;
      var lon = pos.coords.longitude;
      var url = 'https://api.open-meteo.com/v1/forecast' +
        '?latitude=' + lat +
        '&longitude=' + lon +
        '&daily=temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset' +
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
          Pebble.sendAppMessage({
            'TEMP_MAX': Math.round(j.daily.temperature_2m_max[0]),
            'TEMP_MIN': Math.round(j.daily.temperature_2m_min[0]),
            'PRECIP': Math.round(precip),
            'CODE': j.current.weather_code,
            'NIGHT': night
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
    },
    function (err) {
      console.log('Location error: ' + err.message);
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
