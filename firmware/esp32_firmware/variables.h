int speed;
bool API_photo;
bool send_data;
int min_speed; // Minimum speed to trigger camera at any time. in MPH
int photo_speed;
bool connect_wifi;
bool send_API;
bool photo_finished;
bool speed_collection_complete = false;
bool is_kph;
bool send_photo;
int maxSpeed;

String ssid;
String password;
String camera_id;

int sleep_time;
bool wake_flag;
int sleep_idle_time;

bool ignore_flag;
int ignore_time;

bool sending_data = false;

//Your Domain name with URL path or IP address with path
const char* server_speeding = "https://minispeedcam.com/api/1.1/wf/speeding_capture";
const char* server_non_speeding = "https://minispeedcam.com/api/1.1/wf/non_speeding_capture";
const char* server_local_ip_address = "https://minispeedcam.com/api/1.1/wf/local_ip_address";

// the following variables are unsigned longs because the time, measured in
// milliseconds, will quickly become a bigger number than can be stored in an int.
unsigned long lastTime = 0;

String payload;
int httpsResponseCode;
String httpsRequestData;

String photo_base64;

String photo_filename;
int speed_actual;

bool send_alert;

uint16_t labelWifi;
uint16_t wifi_ssid_text, wifi_pass_text, camera_id_text;
String display_wifi;
String local_ip_address;

const byte DNS_PORT = 53;
IPAddress apIP(192, 168, 4, 1);

String hostname = "Radar";

uint16_t button1;
uint16_t switchOne;
uint16_t status;