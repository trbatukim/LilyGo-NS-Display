#ifndef BOARD_HAS_PSRAM
#error "Please enable PSRAM, Arduino IDE -> tools -> PSRAM -> OPI !!!"
#endif

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "epd_driver.h"
#include "utilities.h"
#include "firasans.h"
#include "roboto12.h"
#include "roboto18.h"
#include "roboto32.h"
#include "secrets.h"

#define STATION_CODE "GV"
#define STATION_NAME "Den Haag HS"

#define REFRESH_MS  60000
// Europe/Amsterdam
#define TIMEZONE    "CET-1CEST,M3.5.0,M10.5.0/3"

struct Departure {
    char time[6];            // planned departure, "HH:MM"
    int delay;              // minutes, 0 = on time
    char destination[48];
    char via[96];            // route / remark line, may be ""
    char train[8];           // "IC", "SPR", ...
    char platform[8];
    bool cancelled;
};

// Layout (960 x 540)
#define HEADER_H    72
#define COLUMNS_H   36
#define ROW_H       72
#define MAX_ROWS    ((EPD_HEIGHT - HEADER_H - COLUMNS_H) / ROW_H)

#define MARGIN      24
#define COL_TIME    MARGIN
#define COL_DELAY   200
#define COL_DEST    284
#define COL_TRAIN   740
#define COL_PLAT    848
#define PLAT_W      (EPD_WIDTH - MARGIN - COL_PLAT)
#define DEST_W      (COL_TRAIN - 20 - COL_DEST)

// 4 bit gray levels used for text
#define BLACK       0
#define GRAY        8
#define WHITE       15

uint8_t *framebuffer = NULL;

const char *stationName = STATION_NAME;
char currentTime[6] = "";

Departure departures[MAX_ROWS];
int departureCount = 0;

bool connectWiFi()
{
    if (WiFi.status() == WL_CONNECTED) return true;

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    int tries = 0;
    while (WiFi.status() != WL_CONNECTED && tries < 40) {
        delay(500);
        tries++;
    }
    if (WiFi.status() != WL_CONNECTED) return false;

    configTzTime(TIMEZONE, "pool.ntp.org", "time.nist.gov");
    return true;
}

void updateClock()
{
    struct tm now;
    if (getLocalTime(&now, 5000)) {
        strftime(currentTime, sizeof(currentTime), "%H:%M", &now);
    }
}

bool fetchTrains(JsonDocument& doc)
{
    WiFiClientSecure client;
    client.setInsecure();   // skips cert check

    HTTPClient http;
    http.useHTTP10(true);
    http.setTimeout(8000);

    char url[128];
    snprintf(url, sizeof(url),
             "https://gateway.apiportal.ns.nl/reisinformatie-api/api/v2/departures?station=%s&maxJourneys=%d",
             STATION_CODE, MAX_ROWS);
    if (!http.begin(client, url)) {
        return false;
    }
    http.addHeader("Ocp-Apim-Subscription-Key", NS_API_KEY);

    int code = http.GET();
    if (code != 200) {
        Serial.printf("NS HTTP %d\n", code);
        http.end();
        return false;
    }

    JsonDocument filter;
    JsonObject f = filter["payload"]["departures"][0].to<JsonObject>();
    f["direction"] = true;
    f["plannedDateTime"] = true;
    f["actualDateTime"] = true;
    f["plannedTrack"] = true;
    f["actualTrack"] = true;
    f["cancelled"] = true;
    f["product"]["categoryCode"] = true;
    f["routeStations"][0]["mediumName"] = true;

    DeserializationError err = deserializeJson(doc, http.getStream(),
                                               DeserializationOption::Filter(filter));
    http.end();

    if (err) {
        Serial.printf("NS JSON: %s\n", err.c_str());
        return false;
    }
    return true;
}

int secondsOfDay(const char *iso)
{
    int h, m, s;
    if (sscanf(iso, "%*d-%*d-%*dT%d:%d:%d", &h, &m, &s) != 3) return -1;
    return h * 3600 + m * 60 + s;
}

void loadDepartures(JsonDocument& doc)
{
    departureCount = 0;
    for (JsonObject item : doc["payload"]["departures"].as<JsonArray>()) {
        if (departureCount >= MAX_ROWS) break;
        Departure &d = departures[departureCount++];



        int planned = secondsOfDay(item["plannedDateTime"] | "");
        int actual  = secondsOfDay(item["actualDateTime"] | "");
        if (planned >= 0) {
            snprintf(d.time, sizeof(d.time), "%02d:%02d", planned / 3600, planned / 60 % 60);
        } else {
            strlcpy(d.time, "--:--", sizeof(d.time));
        }

        d.delay = 0;
        if (planned >= 0 && actual >= 0) {
            int diff = actual - planned;
            if (diff < -12 * 3600) diff += 24 * 3600;   // delayed past midnight
            if (diff > 0) d.delay = (diff + 30) / 60;
        }

        strlcpy(d.destination, item["direction"] | "", sizeof(d.destination));
        strlcpy(d.train, item["product"]["categoryCode"] | "", sizeof(d.train));
        strlcpy(d.platform, item["actualTrack"] | (item["plannedTrack"] | ""), sizeof(d.platform));
        d.cancelled = item["cancelled"] | false;

        d.via[0] = '\0';
        for (JsonObject stop : item["routeStations"].as<JsonArray>()) {
            strlcat(d.via, d.via[0] ? ", " : "via ", sizeof(d.via));
            strlcat(d.via, stop["mediumName"] | "", sizeof(d.via));
        }
    }
}

int32_t textWidth(const GFXfont &font, const char *text)
{
    int32_t x = 0, y = 0, x1, y1, w, h;
    get_text_bounds(&font, text, &x, &y, &x1, &y1, &w, &h, NULL);
    return x;
}

void drawText(const GFXfont &font, const char *text, int32_t x, int32_t y, uint8_t fg, uint8_t bg)
{
    FontProperties props = {};
    props.fg_color = fg;
    props.bg_color = bg;
    write_mode(&font, text, &x, &y, framebuffer, BLACK_ON_WHITE, &props);
}

// Draws text, cut off with "..." if it is wider than maxWidth
void drawTextFit(const GFXfont &font, const char *text, int32_t x, int32_t y, int32_t maxWidth, uint8_t fg)
{
    char buf[96];
    strlcpy(buf, text, sizeof(buf) - 3);
    size_t len = strlen(buf);
    while (len > 0 && textWidth(font, buf) > maxWidth) {
        do {
            len--;
        } while (len > 0 && (buf[len] & 0xC0) == 0x80);
        strcpy(buf + len, "...");
    }
    drawText(font, buf, x, y, fg, WHITE);
}

void drawHeader()
{
    epd_fill_rect(0, 0, EPD_WIDTH, HEADER_H, 0x00, framebuffer);
    drawText(FiraSans, stationName, MARGIN, 50, WHITE, BLACK);
    drawText(FiraSans, currentTime, EPD_WIDTH - MARGIN - textWidth(FiraSans, currentTime), 50, WHITE, BLACK);

    int32_t y = HEADER_H + 27;
    drawText(Roboto12, "Time", COL_TIME, y, GRAY, WHITE);
    drawText(Roboto12, "Destination", COL_DEST, y, GRAY, WHITE);
    drawText(Roboto12, "Train", COL_TRAIN, y, GRAY, WHITE);
    drawText(Roboto12, "Platform", EPD_WIDTH - MARGIN - textWidth(Roboto12, "Platform"), y, GRAY, WHITE);
    epd_draw_hline(0, HEADER_H + COLUMNS_H - 1, EPD_WIDTH, 0x00, framebuffer);
}

void drawDeparture(const Departure &d, int32_t top)
{
    uint8_t fg = d.cancelled ? GRAY : BLACK;

    drawText(Roboto32, d.time, COL_TIME, top + 60, fg, WHITE);
    if (d.delay > 0 && !d.cancelled) {
        char delay[8];
        snprintf(delay, sizeof(delay), "+%d", d.delay);
        drawText(Roboto18, delay, COL_DELAY, top + 60, BLACK, WHITE);
    }

    drawTextFit(FiraSans, d.destination, COL_DEST, top + 34, DEST_W, fg);
    if (d.cancelled) {
        int32_t w = min(textWidth(FiraSans, d.destination), (int32_t)DEST_W);
        epd_fill_rect(COL_DEST, top + 23, w, 2, 0x00, framebuffer);
        drawText(Roboto12, "Cancelled", COL_DEST, top + 61, BLACK, WHITE);
    } else {
        drawTextFit(Roboto12, d.via, COL_DEST, top + 61, DEST_W, GRAY);
    }

    drawText(Roboto18, d.train, COL_TRAIN, top + 49, fg, WHITE);

    uint8_t box = d.cancelled ? GRAY : BLACK;
    epd_fill_rect(COL_PLAT, top + 10, PLAT_W, ROW_H - 20, box << 4, framebuffer);
    drawText(Roboto18, d.platform, COL_PLAT + (PLAT_W - textWidth(Roboto18, d.platform)) / 2, top + 49, WHITE, box);

    epd_draw_hline(MARGIN, top + ROW_H - 1, EPD_WIDTH - 2 * MARGIN, 0xC0, framebuffer);
}

// message is shown in place of the rows when there are no departures
void drawBoard(const char *message)
{
    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

    drawHeader();
    for (int i = 0; i < departureCount && i < MAX_ROWS; i++) {
        drawDeparture(departures[i], HEADER_H + COLUMNS_H + i * ROW_H);
    }
    if (departureCount == 0) {
        drawText(FiraSans, message, MARGIN, HEADER_H + COLUMNS_H + 50, BLACK, WHITE);
    }

    epd_poweron();
    epd_clear();
    epd_draw_grayscale_image(epd_full_screen(), framebuffer);
    epd_poweroff();
}

void setup()
{
    Serial.begin(115200);
    delay(1000);
    framebuffer = (uint8_t *)ps_calloc(sizeof(uint8_t), EPD_WIDTH * EPD_HEIGHT / 2);
    if (!framebuffer) {
        Serial.println("alloc memory failed !!!");
        while (1);
    }

    epd_init();
}

void refresh()
{
    static bool hasData = false;
    static const char *shownError = NULL;

    JsonDocument doc;
    const char *error = NULL;
    if (!connectWiFi()) {
        error = "No WiFi connection";
    } else if (!fetchTrains(doc)) {
        error = "Could not reach the NS API";
    }

    if (error) {
        Serial.println(error);
        if (hasData || error == shownError) return;
        departureCount = 0;
    } else {
        loadDepartures(doc);
        hasData = true;
    }
    shownError = error;

    updateClock();
    drawBoard(error ? error : "No departures");
}

void loop()
{
    refresh();
    delay(REFRESH_MS);
}
