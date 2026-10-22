const char * ssid = "YourWiFiSSID";
const char * password = "YourWiFiPassword";
// SHA-256 of your device password: 64 hex characters, NOT the password itself. One password
// covers both OTA updates and the web console (if ENABLE_WEB_SOCKET) -- there's only the one.
// Make it with:  python -c "import hashlib,getpass; print(hashlib.sha256(getpass.getpass().encode()).hexdigest())"
// When the Arduino IDE asks for a password during an OTA upload, or the web console asks you
// to log in, type the real password.
const char* otaPasswordHash = "PUT_THE_64_CHARACTER_SHA256_HASH_OF_YOUR_DEVICE_PASSWORD_HERE";