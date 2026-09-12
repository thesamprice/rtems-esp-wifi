/* Pull the WiFi blobs into a link and see what is left unresolved. */
extern int esp_wifi_init(const void *config);
extern int esp_wifi_start(void);
int main(void) { return esp_wifi_init(0) + esp_wifi_start(); }
