var Clay = require('pebble-clay');
var messageKeys = require('message_keys');
var clayConfig = require('./config');

// autoHandleEvents off: we resolve the location ourselves (GPS or manual)
// and send it as int32 degrees x100 — the watch only deals with integers.
var clay = new Clay(clayConfig, null, { autoHandleEvents: false });

function sendLocation(lat, lon) {
  if (isNaN(lat) || isNaN(lon) || lat < -90 || lat > 90 ||
      lon < -180 || lon > 180) {
    return;
  }
  var dict = {};
  dict[messageKeys.Lat] = Math.round(lat * 100);
  dict[messageKeys.Lon] = Math.round(lon * 100);
  Pebble.sendAppMessage(dict);
}

function fetchGps() {
  if (!navigator.geolocation) {
    console.log('geolocation API not available');
    return;
  }
  navigator.geolocation.getCurrentPosition(function(pos) {
    console.log('geolocation fix: ' + pos.coords.latitude + ',' +
                pos.coords.longitude);
    sendLocation(pos.coords.latitude, pos.coords.longitude);
  }, function(err) {
    console.log('geolocation failed: ' + err.message);
  }, { timeout: 15000, maximumAge: 30 * 60 * 1000 });
}

function settings() {
  try {
    return JSON.parse(localStorage.getItem('clay-settings')) || {};
  } catch (e) {
    return {};
  }
}

Pebble.addEventListener('ready', function() {
  if (settings().UseGps !== false) {
    fetchGps();
  }
});

Pebble.addEventListener('showConfiguration', function() {
  Pebble.openURL(clay.generateUrl());
});

Pebble.addEventListener('webviewclosed', function(e) {
  if (!e || !e.response) {
    return;
  }
  clay.getSettings(e.response);  // persists to localStorage
  var s = settings();
  if (s.UseGps !== false) {
    fetchGps();
  } else {
    sendLocation(parseFloat(s.Lat), parseFloat(s.Lon));
  }
});
