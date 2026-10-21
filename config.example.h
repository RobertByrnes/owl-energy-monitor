const char * ssid = "YourWiFiSSID";
const char * password = "YourWiFiPassword";
const char* serverName = "http://yourserver.com/api/endpoint";
String apiKeyValue = "YourAPIKeyValue";
// SHA-256 of your OTA password: 64 hex characters, NOT the password itself.
// Make it with:  python -c "import hashlib,getpass; print(hashlib.sha256(getpass.getpass().encode()).hexdigest())"
// When the Arduino IDE asks for a password during an OTA upload, type the real password.
const char* otaPasswordHash = "PUT_THE_64_CHARACTER_SHA256_HASH_OF_YOUR_OTA_PASSWORD_HERE";