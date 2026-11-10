// Sends the phone's location (degrees x100) to the watch so it can
// compute the sun's path locally.
function sendLocation() {
  navigator.geolocation.getCurrentPosition(
    function (pos) {
      Pebble.sendAppMessage({
        LAT: Math.round(pos.coords.latitude * 100),
        LON: Math.round(pos.coords.longitude * 100)
      });
    },
    function (err) {
      console.log('Location error: ' + err.message);
    },
    { timeout: 15000, maximumAge: 60 * 60 * 1000 }
  );
}

Pebble.addEventListener('ready', sendLocation);

Pebble.addEventListener('appmessage', function (e) {
  if (e.payload.REQUEST_LOC !== undefined) {
    sendLocation();
  }
});
