// deno-lint-ignore-file no-explicit-any require-await
// deno-lint-ignore no-import-prefix
import "https://deno.land/std@0.204.0/dotenv/load.ts";

// Deno Edge Server for Jellyberry - WebSocket Proxy to Gemini Live API
// Deploy to Deno Deploy: deno deploy --project=jellyberry-server main.ts

// @ts-ignore - Deno types are available at runtime
const GEMINI_API_KEY = Deno.env.get("GEMINI_API_KEY") || "";
const GEMINI_WS_URL = "wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent";

// StormGlass API Configuration
const STORMGLASS_API_KEY = Deno.env.get("STORMGLASS_API_KEY") || "";
const BRIGHTON_LAT = 50.8225;
const BRIGHTON_LNG = -0.1372;

// Tide data caching (1 hour cache)
let tideDataCache: any = null;
let tideDataTimestamp = 0;
const CACHE_DURATION_MS = 60 * 60 * 1000; // 1 hour - set to 0 for testing

// Weather data caching (30 minute cache)
let weatherDataCache: any = null;
let weatherDataTimestamp = 0;
const WEATHER_CACHE_DURATION_MS = 30 * 60 * 1000; // 30 minutes

// Radio station cache - persists across tool calls within a session
interface RadioStation {
 url: string;
 name: string;
 country: string;
 tags: string;
 bitrate: number;
 codec: string;
}
const radioStationCache = new Map<string, RadioStation>(); // keyed by stationuuid

async function searchRadioStations(query: string, tag: string, limit: number): Promise<Array<{ id: string; name: string; country: string; genre_tags: string; bitrate_kbps: number; codec: string }>> {
 const params = new URLSearchParams();
 if (query) params.set("name", query);
 if (tag) params.set("tag", tag);
 params.set("limit", String(limit || 5));
 params.set("hidebroken", "true");
 params.set("order", "votes");
 params.set("reverse", "true");

 const url = `https://all.api.radio-browser.info/json/stations/search?${params}`;
 console.log(`[Radio] Searching: ${url}`);

 const resp = await fetch(url, { headers: { "User-Agent": "Jellyberry/1.0" } });
 if (!resp.ok) throw new Error(`Radio Browser API error: ${resp.status}`);
 const stations = await resp.json() as any[];

 const results = stations
 .filter((s: any) => s.url_resolved) // must have a resolved stream URL
 .slice(0, limit || 5)
 .map((s: any) => {
 const station: RadioStation = {
 url: s.url_resolved,
 name: s.name,
 country: s.country,
 tags: s.tags,
 bitrate: s.bitrate,
 codec: s.codec,
 };
 radioStationCache.set(s.stationuuid, station);
 return {
 id: s.stationuuid,
 name: s.name,
 country: s.country,
 genre_tags: s.tags,
 bitrate_kbps: s.bitrate,
 codec: s.codec,
 };
 });

 console.log(`[Radio] Found ${results.length} stations`);
 return results;
}

// Track which devices have received their first boot greeting
const deviceFirstBoot = new Map<string, boolean>();

// WebSocket close code lookup
const CLOSE_CODES: Record<number, string> = {
 1000: "Normal Closure",
 1001: "Going Away",
 1002: "Protocol Error",
 1003: "Unsupported Data",
 1005: "No Status Received",
 1006: "Abnormal Closure",
 1007: "Invalid frame payload data",
 1008: "Policy Violation",
 1009: "Message Too Big",
 1010: "Mandatory Extension",
 1011: "Internal Server Error",
 1012: "Service Restart",
 1013: "Try Again Later",
 1014: "Bad Gateway",
 1015: "TLS Handshake"
};
function getCloseCodeDescription(code: number): string {
 return CLOSE_CODES[code] || "Unknown";
}

// Debug: Check if API key is loaded
console.log(`[DEBUG] GEMINI_API_KEY loaded: ${GEMINI_API_KEY ? "YES (" + GEMINI_API_KEY.substring(0, 10) + "...)" : "NO - EMPTY!"}`);

// Timer state for each device
interface TimerState {
 endTime: number; // Unix timestamp when timer expires
 durationSeconds: number;
 intervalId?: number;
}

const deviceTimers = new Map<string, TimerState>();

interface ClientConnection {
 socket: WebSocket;
 geminiSocket: WebSocket | null;
 deviceId: string;
 lastFunctionResult?: any;
 pendingFunctionCallId?: string;
 ambientStreamCancel?: (() => void) | null;
 ambientSequence?: number; // Current ambient stream sequence number
 alarmStreamCancel?: (() => void) | null; // Cancel function for alarm streaming
 zenBellCancel?: (() => void) | null; // Cancel function for zen bell streaming
 radioStreamCancel?: (() => void) | null; // Cancel function for radio PCM stream
 radioProcess?: Deno.ChildProcess | null; // ffmpeg process for radio transcoding
 deviceStateResolver?: ((state: any) => void) | undefined; // Promise resolver for get_device_state
 pendingModeMessage?: object | null; // Mode command to send to ESP32 after Gemini finishes speaking
 deviceState?: Record<string, unknown>; // Latest device state snapshot from recordingStart
 
 // True only while processing a turn the user actually spoke into.
 // Set when activityEnd is forwarded to Gemini; cleared when turnComplete arrives.
 // Guards tideData/moonData forwarding: proactive Gemini tool calls (no user audio)
 // must not trigger LED visualizations.
 userSpokeThisTurn?: boolean;

 // Session tracking for diagnostics
 geminiConnectedAt?: number; // Timestamp when Gemini connected
 geminiMessageCount?: number; // Total messages received from Gemini
 audioChunkCount?: number; // Total audio chunks received
 lastAudioChunkTime?: number; // Timestamp of last audio chunk
 lastMessageType?: string; // Type of last message received
 turnAudioChunks?: number; // Audio chunks in current turn
 turnAudioBytes?: number; // PCM bytes sent in current turn

 // Idle management: timestamp of the last turn the user actually spoke into.
 // Used to suppress Gemini auto-reconnect when the device has been idle a long time.
 lastUserActivity?: number;

 // Set to true when a lazy (on-demand) Gemini reconnect is in progress.
 // Causes the next setupComplete to send reconnectComplete to the ESP32
 // instead of being silently swallowed (which would leave the device on LED_PROCESSING).
 pendingLazyReconnect?: boolean;
 pendingRadioGreeting?: boolean; // Queued radio greeting to fire after lazy reconnect completes

 // Proactive session renewal
 geminiTurnActive?: boolean; // True while Gemini is generating a response (activityEnd → turnComplete)
 turnCompleteFired?: boolean; // True once the turn-complete event has been handled; prevents duplicate fires (generationComplete + turnComplete)
 pendingRenewal?: boolean; // Renewal requested while a turn was in-flight; fire on next turnComplete
 sessionRenewalTimer?: ReturnType<typeof setTimeout>; // Handle to cancel if session closes early
 softRenewalTimer?: ReturnType<typeof setTimeout>; // Fires at 7 min for between-turn renewal
 softRenewalArmed?: boolean; // True when soft renewal is waiting for next turnComplete

 // Rolling session memory — Jellyberry's spoken text, carried forward into next session
 sessionTranscript?: string; // Accumulates text parts from current session (capped at 2000 chars)
 sessionMemory?: string; // Last session's transcript, injected into next session's system instruction
}

const connections = new Map<string, ClientConnection>();

// How long the device must be idle before we stop auto-reconnecting to Gemini.
// After this threshold, the next recordingStart triggers an on-demand reconnect.
// This eliminates the ~2,300-token setup cost every 10 min during idle/overnight periods.
const IDLE_RECONNECT_THRESHOLD_MS = 30 * 60 * 1000; // 30 minutes

// Proactively renew the Gemini session before the 600s hard deadline.
// 540s gives a 60s buffer — enough headroom for any in-flight response.
const GEMINI_SESSION_RENEWAL_MS = 9 * 60 * 1000;

// Soft renewal fires 2 min earlier — ensures renewal happens cleanly between turns,
// not mid-sentence, even if a turn is in progress at the 9-min mark.
const GEMINI_SOFT_RENEWAL_MS = 7 * 60 * 1000;

// Deno KV — persists session memory across server restarts, keyed by deviceId.
// Supports multiple simultaneous users automatically.
const kv = await Deno.openKv();

// Fetch tide data from StormGlass API
async function fetchTideData() {
 const now = Date.now();
 
 // Return cached data if still valid
 if (tideDataCache && (now - tideDataTimestamp) < CACHE_DURATION_MS) {
 console.log(" Using cached tide data");
 return tideDataCache;
 }
 
 try {
 const today = new Date();
 const startDate = new Date(today.getTime() - 2 * 24 * 60 * 60 * 1000); // 2 days in the past
 const endDate = new Date(today.getTime() + 7 * 24 * 60 * 60 * 1000); // 7 days ahead
 
 const url = `https://api.stormglass.io/v2/tide/extremes/point?`+
 `lat=${BRIGHTON_LAT}&lng=${BRIGHTON_LNG}&`+
 `start=${startDate.toISOString()}&end=${endDate.toISOString()}`;
 
 console.log(" Fetching tide data from StormGlass...");
 const response = await fetch(url, {
 headers: { 'Authorization': STORMGLASS_API_KEY },
 signal: AbortSignal.timeout(8000)
 });
 
 if (!response.ok) {
 console.error(`StormGlass API error: ${response.status}`);
 return null;
 }
 
 const data = await response.json();
 tideDataCache = data;
 tideDataTimestamp = now;
 console.log(`Fetched ${data.data?.length || 0} tide extremes`);
 return data;
 } catch (error) {
 console.error("Error fetching tide data:", error);
 return null;
 }
}

// Calculate current tide status
function calculateTideStatus(tideData: any) {
 if (!tideData || !tideData.data || tideData.data.length === 0) {
 console.error(" No tide data available");
 return null;
 }
 
 const now = new Date();
 const extremes = tideData.data;
 
 console.log(`Processing ${extremes.length} tide extremes, current time: ${now.toISOString()}`);
 console.log(`First extreme:`, extremes[0]);
 console.log(`Last extreme:`, extremes[extremes.length - 1]);
 
 // Find previous and next tide extremes
 let prevExtreme = null;
 let nextExtreme = null;
 
 for (let i = 0; i < extremes.length; i++) {
 const extreme = extremes[i];
 const extremeTime = new Date(extreme.time);
 
 if (extremeTime <= now) {
 prevExtreme = extreme;
 } else if (!nextExtreme && extremeTime > now) {
 nextExtreme = extreme;
 break;
 }
 }
 
 console.log(`Previous extreme:`, prevExtreme);
 console.log(`Next extreme:`, nextExtreme);
 
 if (!prevExtreme || !nextExtreme) {
 console.error(" Could not find prev/next extremes");
 return null;
 }
 
 // Calculate progress through current cycle (0 to 1)
 const prevTime = new Date(prevExtreme.time).getTime();
 const nextTime = new Date(nextExtreme.time).getTime();
 const nowTime = now.getTime();
 const progress = (nowTime - prevTime) / (nextTime - prevTime);
 
 // Determine if tide is flooding (rising) or ebbing (falling)
 const isFlooding = prevExtreme.type === "low" && nextExtreme.type === "high";
 const state = isFlooding ? "flooding" : "ebbing";
 
 // Calculate water level (0.0 = low tide, 1.0 = high tide)
 let waterLevel;
 if (isFlooding) {
 waterLevel = progress; // Rising from 0 to 1
 } else {
 waterLevel = 1 - progress; // Falling from 1 to 0
 }
 
 // Minutes until next tide change
 const nextChangeMinutes = Math.round((nextTime - nowTime) / 1000 / 60);
 
 // Format times for readable output
 const nextChangeTime = new Date(nextTime).toLocaleTimeString('en-GB', { 
 hour: '2-digit', 
 minute: '2-digit',
 hour12: false 
 });
 
 return {
 state,
 waterLevel,
 waterLevelPercent: Math.round(waterLevel * 100), // Add percentage for Gemini
 nextTideType: nextExtreme.type,
 nextTideTime: nextChangeTime,
 nextChangeMinutes,
 prevExtreme,
 nextExtreme
 };
}

// Get tide status (wrapper for function calling)
async function getTideStatus() {
 const tideData = await fetchTideData();
 if (!tideData) {
 return { success: false, error: "Failed to fetch tide data" };
 }
 
 const status = calculateTideStatus(tideData);
 if (!status) {
 return { success: false, error: "Could not calculate tide status" };
 }
 
 return {
 success: true,
 ...status
 };
}

// Get current time and date in UK timezone
function getCurrentTime() {
 const now = new Date();
 
 // Format for UK timezone (GMT/BST auto-handled)
 const timeOptions: Intl.DateTimeFormatOptions = {
 timeZone: 'Europe/London',
 hour: '2-digit',
 minute: '2-digit',
 second: '2-digit',
 hour12: false
 };
 
 const dateOptions: Intl.DateTimeFormatOptions = {
 timeZone: 'Europe/London',
 weekday: 'long',
 year: 'numeric',
 month: 'long',
 day: 'numeric'
 };
 
 const timeString = now.toLocaleTimeString('en-GB', timeOptions);
 const dateString = now.toLocaleDateString('en-GB', dateOptions);
 const dayOfWeek = now.toLocaleDateString('en-GB', { timeZone: 'Europe/London', weekday: 'long' });
 
 return {
 success: true,
 time: timeString,
 date: dateString,
 dayOfWeek: dayOfWeek,
 timestamp: now.toISOString()
 };
}

// Set an alarm
function setAlarm(deviceId: string, alarmTime: string) {
 try {
 // Parse the alarm time (ISO 8601 format: "2026-01-09T07:00:00Z" or "2026-01-09T07:00:00+00:00")
 let alarmDate = new Date(alarmTime);
 
 console.log(`[${deviceId}] Parsing alarm time: "${alarmTime}"`);
 console.log(`[${deviceId}] Parsed as: ${alarmDate.toISOString()}`);
 console.log(`[${deviceId}] Current time: ${new Date().toISOString()}`);
 console.log(`[${deviceId}] UK time now: ${new Date().toLocaleString('en-GB', { timeZone: 'Europe/London' })}`);
 console.log(`[${deviceId}] Alarm UK time: ${alarmDate.toLocaleString('en-GB', { timeZone: 'Europe/London' })}`);
 
 if (isNaN(alarmDate.getTime())) {
 return {
 success: false,
 error: "Invalid datetime format"
 };
 }
 
 // If alarm time is in the past, advance to the next occurrence.
 // Use a 60-second grace window: if the time is only slightly in the past
 // (e.g. parsing ambiguity or a few seconds of clock drift) we still advance,
 // but we include the adjustment in the return value so Gemini can confirm it with the user.
 const now = Date.now();
 const timeDiff = alarmDate.getTime() - now;
 const minutesUntil = Math.round(timeDiff / 1000 / 60);
 let advancedToNextDay = false;
 
 console.log(`[${deviceId}] Time until alarm: ${minutesUntil} minutes`);
 
 if (alarmDate.getTime() <= now) {
 advancedToNextDay = true;
 alarmDate = new Date(alarmDate.getTime() + 24 * 60 * 60 * 1000); // Add 24 hours
 console.log(`[${deviceId}] Alarm time was in the past by ${Math.abs(minutesUntil)} minutes - advancing to tomorrow`);
 }
 
 // Generate unique alarm ID
 const alarmID = Date.now() + Math.floor(Math.random() * 1000);
 
 // Calculate seconds until alarm
 const triggerTime = alarmDate.getTime();
 const secondsUntil = Math.round((triggerTime - now) / 1000);
 
 // Format time for response
 const timeOptions: Intl.DateTimeFormatOptions = {
 timeZone: 'Europe/London',
 hour: '2-digit',
 minute: '2-digit',
 hour12: false
 };
 const formattedTime = alarmDate.toLocaleTimeString('en-GB', timeOptions);
 const formattedDate = alarmDate.toLocaleDateString('en-GB', {
 timeZone: 'Europe/London',
 weekday: 'short',
 month: 'short',
 day: 'numeric'
 });
 
 console.log(`[${deviceId}] Setting alarm for ${formattedTime} on ${formattedDate} (${secondsUntil}s from now)`);
 
 return {
 success: true,
 alarmID: alarmID,
 triggerTime: triggerTime,
 formattedTime: formattedTime,
 formattedDate: formattedDate,
 secondsUntil: secondsUntil,
 // Tell Gemini if the time was in the past and was moved to tomorrow,
 // so it can confirm this with the user rather than silently changing the date.
 advancedToNextDay: advancedToNextDay
 };
 } catch (error) {
 return {
 success: false,
 error: `Failed to parse alarm time: ${error}`
 };
 }
}

// Cancel alarm (next/specific/all)
function cancelAlarm(deviceId: string, which: string = "next") {
 try {
 console.log(`[${deviceId}] Cancel alarm request: ${which}`);
 
 return {
 success: true,
 which: which,
 message: `Alarm cancellation request sent to device`
 };
 } catch (error) {
 return {
 success: false,
 error: `Failed to cancel alarm: ${error}`
 };
 }
}

// Set a timer
function setTimer(deviceId: string, durationMinutes: number) {
 const durationSeconds = Math.round(durationMinutes * 60);
 const endTime = Date.now() + (durationSeconds * 1000);
 
 // Clear any existing timer
 const existing = deviceTimers.get(deviceId);
 if (existing?.intervalId) {
 clearInterval(existing.intervalId);
 }
 
 // Set up new timer
 const timerState: TimerState = {
 endTime,
 durationSeconds
 };
 
 // Check timer completion every second
 timerState.intervalId = setInterval(() => {
 const remaining = Math.round((timerState.endTime - Date.now()) / 1000);
 
 if (remaining <= 0) {
 // Timer expired
 console.log(`[${deviceId}] Timer expired!`);
 clearInterval(timerState.intervalId);
 deviceTimers.delete(deviceId);
 
 // Send notification to ESP32
 const connection = connections.get(deviceId);
 if (connection?.socket.readyState === WebSocket.OPEN) {
 connection.socket.send(JSON.stringify({
 type: "timerExpired",
 message: "Your timer is complete"
 }));
 
 // Send text message to Gemini to trigger spoken notification
 if (connection.geminiSocket?.readyState === WebSocket.OPEN) {
 const notification = {
 clientContent: {
 turns: [{
 role: "user",
 parts: [{
 text: "SYSTEM: Timer has expired. Please notify the user that their timer is complete."
 }]
 }],
 turnComplete: true
 }
 };
 console.log(`[${deviceId}] Sending timer expiry notification to Gemini`);
 connection.geminiSocket.send(JSON.stringify(notification));
 }
 }
 }
 }, 1000);

 deviceTimers.set(deviceId, timerState);

 console.log(`[${deviceId}] Timer set for ${durationMinutes} minutes (${durationSeconds}s)`);
 
 return {
 success: true,
 durationMinutes,
 durationSeconds,
 expiresAt: new Date(endTime).toLocaleTimeString('en-GB')
 };
}

// Cancel timer
function cancelTimer(deviceId: string) {
 const timer = deviceTimers.get(deviceId);
 
 if (!timer) {
 return {
 success: false,
 message: "No timer is currently running"
 };
 }
 
 if (timer.intervalId) {
 clearInterval(timer.intervalId);
 }
 deviceTimers.delete(deviceId);
 
 console.log(`[${deviceId}] Timer cancelled`);
 
 return {
 success: true,
 message: "Timer cancelled"
 };
}

// Fetch weather data from Open-Meteo API
async function fetchWeatherData() {
 const now = Date.now();
 
 // Return cached data if still valid
 if (weatherDataCache && (now - weatherDataTimestamp) < WEATHER_CACHE_DURATION_MS) {
 console.log(" Using cached weather data");
 return weatherDataCache;
 }
 
 try {
 // Using Brighton coordinates (same as tide data)
 const url = `https://api.open-meteo.com/v1/forecast?latitude=${BRIGHTON_LAT}&longitude=${BRIGHTON_LNG}&current=temperature_2m,relative_humidity_2m,apparent_temperature,precipitation,rain,weather_code,wind_speed_10m&hourly=temperature_2m,precipitation_probability,weather_code&daily=temperature_2m_max,temperature_2m_min,precipitation_sum,precipitation_probability_max,weather_code,sunrise,sunset&timezone=Europe/London&forecast_days=3`;
 
 const response = await fetch(url, { signal: AbortSignal.timeout(8000) });
 
 if (!response.ok) {
 throw new Error(`Open-Meteo API returned ${response.status}`);
 }
 
 const data = await response.json();
 
 // Cache the result
 weatherDataCache = data;
 weatherDataTimestamp = now;
 
 console.log(" Weather data fetched from Open-Meteo");
 return data;
 
 } catch (error) {
 console.error(" Error fetching weather data:", error);
 
 // Return cached data if available, even if expired
 if (weatherDataCache) {
 console.log(" Using expired weather cache due to fetch error");
 return weatherDataCache;
 }
 
 throw error;
 }
}

// Weather code to description mapping (WMO codes)
const WEATHER_CODES: Record<number, string> = {
 0: "clear sky",
 1: "mainly clear",
 2: "partly cloudy",
 3: "overcast",
 45: "foggy",
 48: "depositing rime fog",
 51: "light drizzle",
 53: "moderate drizzle",
 55: "dense drizzle",
 61: "slight rain",
 63: "moderate rain",
 65: "heavy rain",
 71: "slight snow",
 73: "moderate snow",
 75: "heavy snow",
 77: "snow grains",
 80: "slight rain showers",
 81: "moderate rain showers",
 82: "violent rain showers",
 85: "slight snow showers",
 86: "heavy snow showers",
 95: "thunderstorm",
 96: "thunderstorm with slight hail",
 99: "thunderstorm with heavy hail"
};
function getWeatherDescription(code: number): string {
 return WEATHER_CODES[code] || "unknown conditions";
}

// Get weather forecast
function getWeather(timeframe: string = "current") {
 try {
 const data = weatherDataCache;
 
 if (!data) {
 return {
 success: false,
 error: "Weather data not available. Please try again in a moment."
 };
 }
 
 const current = data.current;
 const hourly = data.hourly;
 const daily = data.daily;
 
 // Build response based on timeframe
 if (timeframe === "current") {
 const temp = Math.round(current.temperature_2m);
 const feelsLike = Math.round(current.apparent_temperature);
 const condition = getWeatherDescription(current.weather_code);
 const humidity = current.relative_humidity_2m;
 const windSpeed = Math.round(current.wind_speed_10m);
 const isRaining = current.precipitation > 0;
 const sunriseStr = daily?.sunrise?.[0] ? new Date(daily.sunrise[0]).toLocaleTimeString('en-GB', { hour: '2-digit', minute: '2-digit', timeZone: 'Europe/London' }) : null;
 const sunsetStr = daily?.sunset?.[0] ? new Date(daily.sunset[0]).toLocaleTimeString('en-GB', { hour: '2-digit', minute: '2-digit', timeZone: 'Europe/London' }) : null;
 
 return {
 success: true,
 timeframe: "current",
 temperature: temp,
 feelsLike: feelsLike,
 condition: condition,
 humidity: humidity,
 windSpeed: windSpeed,
 isRaining: isRaining,
 sunrise: sunriseStr,
 sunset: sunsetStr,
 summary: `It's currently ${temp} C and ${condition}. Feels like ${feelsLike} C. Wind ${windSpeed} km/h.${isRaining ? " It's raining right now." : ""}${sunriseStr ? ` Sunrise today: ${sunriseStr}. Sunset: ${sunsetStr}.` : ""}`
 };
 }
 
 if (timeframe === "hourly") {
 // Next 12 hours forecast
 const nextHours = [];
 for (let i = 0; i < 12; i++) {
 nextHours.push({
 hour: hourly.time[i].split('T')[1].substring(0, 5),
 temperature: Math.round(hourly.temperature_2m[i]),
 precipitation: hourly.precipitation_probability[i],
 condition: getWeatherDescription(hourly.weather_code[i])
 });
 }
 
 return {
 success: true,
 timeframe: "hourly",
 forecast: nextHours,
 summary: `Over the next 12 hours: temperatures from ${Math.round(hourly.temperature_2m[0])} C to ${Math.round(hourly.temperature_2m[11])} C. ${hourly.precipitation_probability[0] > 50 ? "Rain is likely." : "Rain is unlikely."}`
 };
 }
 
 if (timeframe === "today") {
 const maxTemp = Math.round(daily.temperature_2m_max[0]);
 const minTemp = Math.round(daily.temperature_2m_min[0]);
 const condition = getWeatherDescription(daily.weather_code[0]);
 const rainChance = daily.precipitation_probability_max[0];
 const sunriseStr = daily?.sunrise?.[0] ? new Date(daily.sunrise[0]).toLocaleTimeString('en-GB', { hour: '2-digit', minute: '2-digit', timeZone: 'Europe/London' }) : null;
 const sunsetStr = daily?.sunset?.[0] ? new Date(daily.sunset[0]).toLocaleTimeString('en-GB', { hour: '2-digit', minute: '2-digit', timeZone: 'Europe/London' }) : null;
 
 return {
 success: true,
 timeframe: "today",
 maxTemp: maxTemp,
 minTemp: minTemp,
 condition: condition,
 rainChance: rainChance,
 sunrise: sunriseStr,
 sunset: sunsetStr,
 summary: `Today: high of ${maxTemp} C, low of ${minTemp} C. ${condition}. ${rainChance}% chance of rain.${sunriseStr ? ` Sunrise: ${sunriseStr}. Sunset: ${sunsetStr}.` : ""}`
 };
 }
 
 if (timeframe === "tomorrow") {
 const maxTemp = Math.round(daily.temperature_2m_max[1]);
 const minTemp = Math.round(daily.temperature_2m_min[1]);
 const condition = getWeatherDescription(daily.weather_code[1]);
 const rainChance = daily.precipitation_probability_max[1];
 
 return {
 success: true,
 timeframe: "tomorrow",
 maxTemp: maxTemp,
 minTemp: minTemp,
 condition: condition,
 rainChance: rainChance,
 summary: `Tomorrow: high of ${maxTemp} C, low of ${minTemp} C. ${condition}. ${rainChance}% chance of rain.`
 };
 }
 
 if (timeframe === "week") {
 const weekSummary = [];
 for (let i = 0; i < 3; i++) {
 const day = i === 0 ? "Today" : i === 1 ? "Tomorrow" : "Day after";
 weekSummary.push({
 day: day,
 maxTemp: Math.round(daily.temperature_2m_max[i]),
 minTemp: Math.round(daily.temperature_2m_min[i]),
 condition: getWeatherDescription(daily.weather_code[i]),
 rainChance: daily.precipitation_probability_max[i]
 });
 }
 
 return {
 success: true,
 timeframe: "week",
 forecast: weekSummary,
 summary: `3-day forecast: ${weekSummary.map(d => `${d.day} ${d.maxTemp} C`).join(", ")}.`
 };
 }
 
 return {
 success: false,
 error: "Invalid timeframe requested"
 };
 
 } catch (error) {
 console.error(" Error getting weather:", error);
 return {
 success: false,
 error: "Failed to retrieve weather data"
 };
 }
}

// Calculate moon phase using astronomical algorithm
function getMoonPhase() {
 try {
 const now = new Date();
 
 // Known new moon reference: January 6, 2000, 18:14 UTC
 const knownNewMoon = new Date(Date.UTC(2000, 0, 6, 18, 14, 0));
 const lunarCycle = 29.530588853; // Average lunar cycle in days
 
 // Calculate days since known new moon
 const daysSinceNewMoon = (now.getTime() - knownNewMoon.getTime()) / (1000 * 60 * 60 * 24);
 
 // Calculate current position in lunar cycle (0-1)
 const cyclePosition = (daysSinceNewMoon % lunarCycle) / lunarCycle;
 
 // Calculate illumination (0-1, where 0.5 is full moon)
 const illumination = (1 - Math.cos(cyclePosition * 2 * Math.PI)) / 2;
 
 // Calculate moon age (days since last new moon)
 const moonAge = daysSinceNewMoon % lunarCycle;
 
 // Determine phase name based on cycle position
 let phaseName = "";
 let phaseEmoji = "";
 
 if (cyclePosition < 0.033 || cyclePosition >= 0.967) {
 phaseName = "New Moon";
 phaseEmoji = " ";
 } else if (cyclePosition < 0.216) {
 phaseName = "Waxing Crescent";
 phaseEmoji = " ";
 } else if (cyclePosition < 0.283) {
 phaseName = "First Quarter";
 phaseEmoji = " ";
 } else if (cyclePosition < 0.466) {
 phaseName = "Waxing Gibbous";
 phaseEmoji = " ";
 } else if (cyclePosition < 0.533) {
 phaseName = "Full Moon";
 phaseEmoji = " ";
 } else if (cyclePosition < 0.716) {
 phaseName = "Waning Gibbous";
 phaseEmoji = " ";
 } else if (cyclePosition < 0.783) {
 phaseName = "Last Quarter";
 phaseEmoji = " ";
 } else {
 phaseName = "Waning Crescent";
 phaseEmoji = " ";
 }
 
 // Calculate days until next full moon
 let daysToFullMoon;
 if (cyclePosition < 0.5) {
 daysToFullMoon = (0.5 - cyclePosition) * lunarCycle;
 } else {
 daysToFullMoon = (1.5 - cyclePosition) * lunarCycle;
 }
 
 // Calculate days until next new moon
 const daysToNewMoon = (1 - cyclePosition) * lunarCycle;
 
 return {
 success: true,
 phaseName: phaseName,
 phaseEmoji: phaseEmoji,
 illumination: Math.round(illumination * 100), // Percentage 0-100
 moonAge: Math.round(moonAge * 10) / 10, // Days with 1 decimal
 cyclePosition: Math.round(cyclePosition * 1000) / 1000, // 0-1 with 3 decimals
 daysToFullMoon: Math.round(daysToFullMoon * 10) / 10,
 daysToNewMoon: Math.round(daysToNewMoon * 10) / 10,
 summary: `The moon is currently in the ${phaseName} phase ${phaseEmoji}. It is ${Math.round(illumination * 100)}% illuminated and is ${Math.round(moonAge)} days old in this lunar cycle.`
 };
 
 } catch (error) {
 console.error(" Error calculating moon phase:", error);
 return {
 success: false,
 error: "Failed to calculate moon phase"
 };
 }
}

// Send sunrise/sunset data to ESP32 for day/night brightness control
function sendSunriseSunsetData(connection: ClientConnection) {
 const data = weatherDataCache;
 
 if (!data || !data.daily) {
 console.log(`[${connection.deviceId}] No weather data yet for sunrise/sunset`);
 return;
 }
 
 const sunrise = data.daily.sunrise[0]; // Today's sunrise (ISO string)
 const sunset = data.daily.sunset[0]; // Today's sunset (ISO string)
 
 if (!sunrise || !sunset) {
 console.log(`[${connection.deviceId}] Sunrise/sunset data not available in weather cache`);
 return;
 }
 
 // Convert to timestamps (milliseconds)
 const sunriseTime = new Date(sunrise).getTime();
 const sunsetTime = new Date(sunset).getTime();
 
 connection.socket.send(JSON.stringify({
 type: "sunData",
 sunrise: sunriseTime,
 sunset: sunsetTime
 }));
 
 const sunriseStr = new Date(sunrise).toLocaleTimeString('en-GB', { hour: '2-digit', minute: '2-digit' });
 const sunsetStr = new Date(sunset).toLocaleTimeString('en-GB', { hour: '2-digit', minute: '2-digit' });
 
 console.log(`[${connection.deviceId}] Sent sunrise/sunset times: ${sunriseStr} / ${sunsetStr}`);
}

// Initial weather data fetch
console.log(" Fetching initial weather data...");
fetchWeatherData().catch(err => console.error(" Failed to fetch initial weather data:", err));

// ── Handler function types ────────────────────────────────────────────────────
// Shared parameter shapes for all three dispatch tables.
type FuncArgs = Record<string, unknown>;
type ToolResult = Record<string, unknown>;
type ToolHandler   = (args: FuncArgs,                 conn: ClientConnection) => Promise<ToolResult>;
type ActionHandler = (data: Record<string, unknown>,  conn: ClientConnection) => Promise<void>;
type TypeHandler   = (data: Record<string, unknown>,  conn: ClientConnection) => Promise<void>;

// ── Gemini tool handlers ─────────────────────────────────────────────────────
// Each handler receives (funcArgs, connection) and returns the functionResult.
// Named functions make each tool independently navigable and testable.

async function handleGetTideStatus(_args: FuncArgs, conn: ClientConnection): Promise<ToolResult> {
  const result = await getTideStatus();
  if (result.success && conn.userSpokeThisTurn) {
    const tr = result as { state: string; waterLevel: number; nextChangeMinutes: number };
    conn.socket.send(JSON.stringify({ type: "tideData", state: tr.state, waterLevel: tr.waterLevel, nextChangeMinutes: tr.nextChangeMinutes }));
    console.log(`[${conn.deviceId}] Sent tide data to ESP32: ${tr.state}, level: ${tr.waterLevel.toFixed(2)}`);
  } else if (result.success) {
    console.log(`[${conn.deviceId}] Tide data suppressed (proactive call — user did not ask about tides this turn)`);
  }
  return result;
}

async function handleGetCurrentTime(_args: FuncArgs, conn: ClientConnection): Promise<ToolResult> {
  const result = getCurrentTime();
  console.log(`[${conn.deviceId}] Current time: ${result.time} on ${result.date}`);
  return result;
}

async function handleGetDeviceState(_args: FuncArgs, conn: ClientConnection): Promise<ToolResult> {
  console.log(`[${conn.deviceId}] Getting device state`);
  const statePromise = new Promise<any>((resolve) => {
    const timeout = setTimeout(() => { console.error(`[${conn.deviceId}] Device state timeout (5s)`); resolve({ success: false, error: "Device did not respond in time" }); }, 5000);
    conn.deviceStateResolver = (state: any) => { clearTimeout(timeout); resolve(state); };
  });
  conn.socket.send(JSON.stringify({ type: "deviceStateRequest" }));
  const rawState = await statePromise;
  const serverTimer = deviceTimers.get(conn.deviceId);
  const timerOverlay = serverTimer
    ? { active: true, secondsRemaining: Math.max(0, Math.round((serverTimer.endTime - Date.now()) / 1000)) }
    : { active: false };
  const result = { success: true, ...rawState, timer: timerOverlay };
  console.log(`[${conn.deviceId}] Device state:`, JSON.stringify(result).substring(0, 300));
  return result;
}

async function handleSetAlarm(args: FuncArgs, conn: ClientConnection): Promise<ToolResult> {
  const result = setAlarm(conn.deviceId, (args.alarm_time || "") as string);
  if (result.success) {
    conn.socket.send(JSON.stringify({ type: "setAlarm", alarmID: result.alarmID, triggerTime: result.triggerTime }));
    console.log(`[${conn.deviceId}] Sent alarm to ESP32: ID=${result.alarmID}, time=${result.formattedTime}`);
  }
  return result;
}

async function handleCancelAlarm(args: FuncArgs, conn: ClientConnection): Promise<ToolResult> {
  const which = (args.which || "next") as string;
  const result = cancelAlarm(conn.deviceId, which);
  if (result.success) { conn.socket.send(JSON.stringify({ type: "cancelAlarm", which })); console.log(`[${conn.deviceId}] Sent alarm cancel request: ${which}`); }
  return result;
}

async function handleSetTimer(args: FuncArgs, conn: ClientConnection): Promise<ToolResult> {
  const result = setTimer(conn.deviceId, (args.duration_minutes || 0) as number);
  if (result.success) { conn.socket.send(JSON.stringify({ type: "timerSet", durationSeconds: result.durationSeconds })); }
  return result;
}

async function handleCancelTimer(_args: FuncArgs, conn: ClientConnection): Promise<ToolResult> {
  const result = cancelTimer(conn.deviceId);
  if (result.success) { conn.socket.send(JSON.stringify({ type: "timerCancelled" })); }
  return result;
}

async function handleGetWeatherForecast(args: FuncArgs, conn: ClientConnection): Promise<ToolResult> {
  const timeframe = (args.timeframe || "current") as string;
  if (!weatherDataCache) { await fetchWeatherData(); }
  const result = getWeather(timeframe);
  console.log(`[${conn.deviceId}] Weather (${timeframe}): ${result.summary || result.error}`);
  return result;
}

async function handleGetMoonPhase(_args: FuncArgs, conn: ClientConnection): Promise<ToolResult> {
  const result = getMoonPhase();
  console.log(`[${conn.deviceId}] Moon phase: ${result.phaseName} ${result.phaseEmoji} (${result.illumination}% illuminated)`);
  if (result.success && conn.userSpokeThisTurn) {
    conn.socket.send(JSON.stringify({ type: "moonData", phaseName: result.phaseName, illumination: result.illumination, moonAge: result.moonAge }));
  } else if (result.success) {
    console.log(`[${conn.deviceId}] Moon data suppressed (proactive call — user did not ask about the moon this turn)`);
  }
  return result;
}

async function handleControlPomodoro(args: FuncArgs, conn: ClientConnection): Promise<ToolResult> {
  const action = (args.action || "start") as string;
  console.log(`[${conn.deviceId}] control_pomodoro: ${action}`);
  if (action === "start") {
    const focusMinutes = (args.focus_minutes || 25) as number;
    const shortBreakMinutes = (args.short_break_minutes || 5) as number;
    const longBreakMinutes = (args.long_break_minutes || 15) as number;
    console.log(`[${conn.deviceId}] Starting Pomodoro: ${focusMinutes}min focus, ${shortBreakMinutes}min short break, ${longBreakMinutes}min long break`);
    conn.socket.send(JSON.stringify({ type: "pomodoroStart", focusMinutes, shortBreakMinutes, longBreakMinutes }));
    const isCustom = args.focus_minutes || args.short_break_minutes || args.long_break_minutes;
    return { success: true, message: isCustom ? `Pomodoro started: ${focusMinutes}-min focus, ${shortBreakMinutes}-min short break, ${longBreakMinutes}-min long break` : "Pomodoro started with standard durations: 25-min focus, 5-min short break, 15-min long break" };
  }
  // Sub-dispatch for pause/resume/stop/skip
  const pomodoroActions: Record<string, () => ToolResult> = {
    pause:  () => { conn.socket.send(JSON.stringify({ type: "pomodoroPause" }));  return { success: true, message: "Pomodoro timer paused" }; },
    resume: () => { conn.socket.send(JSON.stringify({ type: "pomodoroResume" })); return { success: true, message: "Pomodoro timer resumed" }; },
    stop:   () => { conn.socket.send(JSON.stringify({ type: "pomodoroStop" }));   return { success: true, message: "Pomodoro session ended" }; },
    skip:   () => { conn.socket.send(JSON.stringify({ type: "pomodoroSkip" }));   return { success: true, message: "Skipped to next Pomodoro session" }; },
  };
  return (pomodoroActions[action] ?? (() => ({ success: false, error: `Unknown Pomodoro action: ${action}` })))();
}

async function handleControlAmbient(args: FuncArgs, conn: ClientConnection): Promise<ToolResult> {
  const action = (args.action || "start") as string;
  const sound = ((args.sound || "rain") as string).toLowerCase();
  console.log(`[${conn.deviceId}] control_ambient: ${action}${action === "start" ? ` (${sound})` : ""}`);
  if (action === "start") { conn.pendingModeMessage = { type: "ambientStart", sound }; return { success: true, message: `Playing ${sound} ambient sound` }; }
  conn.pendingModeMessage = { type: "switchToIdle" };
  return { success: true, message: "Ambient sound stopped" };
}

async function handleControlLamp(args: FuncArgs, conn: ClientConnection): Promise<ToolResult> {
  const action = (args.action || "start") as string;
  const color = ((args.color || "white") as string).toLowerCase();
  console.log(`[${conn.deviceId}] control_lamp: ${action}${action === "start" ? ` (${color})` : ""}`);
  if (action === "start") { conn.pendingModeMessage = { type: "lampStart", color }; return { success: true, message: `Lamp turned on (${color})` }; }
  conn.pendingModeMessage = { type: "switchToIdle" };
  return { success: true, message: "Lamp turned off" };
}

async function handleControlMeditation(args: FuncArgs, conn: ClientConnection): Promise<ToolResult> {
  const action = (args.action || "start") as string;
  console.log(`[${conn.deviceId}] control_meditation: ${action}`);
  if (action === "start") { conn.pendingModeMessage = { type: "meditationStart" }; return { success: true, message: "Meditation session started" }; }
  conn.pendingModeMessage = { type: "switchToIdle" };
  return { success: true, message: "Meditation session stopped" };
}

async function handleSearchRadioStations(args: FuncArgs, conn: ClientConnection): Promise<ToolResult> {
  const query = (args.query || "") as string;
  const genre = (args.genre || "") as string;
  const limit = Math.min(((args.limit as number) || 5), 8);
  console.log(`[${conn.deviceId}] search_radio_stations: query="${query}" genre="${genre}" limit=${limit}`);
  try {
    const stations = await searchRadioStations(query, genre, limit);
    return { success: true, stations };
  } catch (err) {
    console.error(`[${conn.deviceId}] Radio search failed:`, err);
    return { success: false, error: "Radio search failed. Please try again." };
  }
}

async function handlePlayRadio(args: FuncArgs, conn: ClientConnection): Promise<ToolResult> {
  const stationId = (args.station_id || "") as string;
  const stationName = (args.station_name || "Radio") as string;
  console.log(`[${conn.deviceId}] play_radio: ${stationName} (${stationId})`);

  let station = radioStationCache.get(stationId);

  // Cache miss — fall back to a live name search (handles server restarts,
  // session reconnects, or model using a stale/hallucinated UUID).
  if (!station && stationName) {
    console.log(`[${conn.deviceId}] play_radio: ${stationId} not in cache — searching by name "${stationName}"`);
    try {
      await handleSearchRadioStations({ query: stationName, limit: 1 }, conn);
      station = radioStationCache.get(stationId) ?? Array.from(radioStationCache.values()).find(
        s => s.name.toLowerCase().includes(stationName.toLowerCase()) ||
             stationName.toLowerCase().includes(s.name.toLowerCase())
      );
    } catch (_) { /* fall through to error below */ }
  }

  if (!station) { return { success: false, error: "Station not found and live search failed. Please use search_radio_stations first." }; }
  const isHLS = station.url.includes(".m3u8");
  conn.pendingModeMessage = { type: "radioStart", stationName, streamUrl: station.url, isHLS };
  return { success: true, message: `Starting ${stationName}${isHLS ? " (may take a moment to load)" : ""}` };
}

async function handleStopRadio(_args: FuncArgs, conn: ClientConnection): Promise<ToolResult> {
  console.log(`[${conn.deviceId}] stop_radio`);
  conn.pendingModeMessage = { type: "switchToIdle" };
  return { success: true, message: "Radio stopped" };
}

async function handleSetVolumeLevel(args: FuncArgs, conn: ClientConnection): Promise<ToolResult> {
  const level = Math.max(1, Math.min(10, ((args.level as number) || 5)));
  console.log(`[${conn.deviceId}] set_volume_level: ${level}`);
  // Use pendingModeMessage so the command arrives AFTER Gemini finishes speaking.
  // If sent immediately during a response, the radio duck/restore path would overwrite it.
  conn.pendingModeMessage = { type: "functionCall", name: "set_volume_level", args: { level } };
  return { success: true, message: `Volume set to level ${level}` };
}

// Maps Gemini funcName → handler. Unknown tools return { success: false, error: ... }.
const toolHandlers: Record<string, ToolHandler> = {
  get_tide_status:       handleGetTideStatus,
  get_current_time:      handleGetCurrentTime,
  get_device_state:      handleGetDeviceState,
  set_alarm:             handleSetAlarm,
  cancel_alarm:          handleCancelAlarm,
  set_timer:             handleSetTimer,
  cancel_timer:          handleCancelTimer,
  get_weather_forecast:  handleGetWeatherForecast,
  get_moon_phase:        handleGetMoonPhase,
  control_pomodoro:      handleControlPomodoro,
  control_ambient:       handleControlAmbient,
  control_lamp:          handleControlLamp,
  control_meditation:    handleControlMeditation,
  search_radio_stations: handleSearchRadioStations,
  play_radio:            handlePlayRadio,
  stop_radio:            handleStopRadio,
  set_volume_level:      handleSetVolumeLevel,
};

// ── ESP32 action handlers ────────────────────────────────────────────────────
// Handle binary streaming operations triggered by data.action from the device.

async function handleActionRequestAlarm(_data: Record<string, unknown>, conn: ClientConnection): Promise<void> {
  console.log(`[${conn.deviceId}] Alarm sound requested`);
  if (conn.alarmStreamCancel) { conn.alarmStreamCancel(); console.log(`[${conn.deviceId}] Cancelled previous alarm stream`); }
  try {
    let cancelled = false;
    conn.alarmStreamCancel = () => { cancelled = true; console.log(`[${conn.deviceId}] Alarm stream cancelled`); };
    const audioData = await Deno.readFile(`./audio/alarm_sound.pcm`);
    console.log(`[${conn.deviceId}] Loaded alarm_sound.pcm (${audioData.byteLength} bytes) - looping until dismissed...`);
    const CHUNK_SIZE = 1024, CHUNKS_PER_BATCH = 5, BATCH_DELAY_MS = 100;
    while (!cancelled) {
      for (let offset = 0; offset < audioData.byteLength && !cancelled; offset += CHUNK_SIZE) {
        conn.socket.send(audioData.slice(offset, offset + CHUNK_SIZE));
        if ((offset / CHUNK_SIZE) % CHUNKS_PER_BATCH === 0) { await new Promise(r => setTimeout(r, BATCH_DELAY_MS)); }
      }
    }
    console.log(`[${conn.deviceId}] Alarm stream stopped`);
  } catch (err) { console.error(`[${conn.deviceId}] Failed to load alarm sound:`, err); }
}

async function handleActionStopAlarm(_data: Record<string, unknown>, conn: ClientConnection): Promise<void> {
  console.log(`[${conn.deviceId}] Stop alarm requested`);
  if (conn.alarmStreamCancel) { conn.alarmStreamCancel(); conn.alarmStreamCancel = null; }
}

async function handleActionRequestZenBell(_data: Record<string, unknown>, conn: ClientConnection): Promise<void> {
  console.log(`[${conn.deviceId}] Zen bell requested`);
  if (conn.zenBellCancel) { conn.zenBellCancel(); conn.zenBellCancel = null; }
  try {
    let cancelled = false;
    conn.zenBellCancel = () => { cancelled = true; };
    const audioData = await Deno.readFile(`./audio/zen_bell.pcm`);
    console.log(`[${conn.deviceId}] Loaded zen_bell.pcm (${audioData.byteLength} bytes) - sending...`);
    const CHUNK_SIZE = 1024, CHUNKS_PER_BATCH = 5, BATCH_DELAY_MS = 100;
    for (let offset = 0; offset < audioData.byteLength && !cancelled; offset += CHUNK_SIZE) {
      conn.socket.send(audioData.slice(offset, offset + CHUNK_SIZE));
      if ((offset / CHUNK_SIZE) % CHUNKS_PER_BATCH === 0) { await new Promise(r => setTimeout(r, BATCH_DELAY_MS)); }
    }
    console.log(cancelled ? `[${conn.deviceId}] Zen bell cancelled` : `[${conn.deviceId}] Zen bell sent (${audioData.byteLength} bytes)`);
    conn.zenBellCancel = null;
  } catch (err) { console.error(`[${conn.deviceId}] Failed to load zen bell:`, err); conn.zenBellCancel = null; }
}

async function handleActionRequestAmbient(data: Record<string, unknown>, conn: ClientConnection): Promise<void> {
  const soundName = (data.sound || "rain") as string;
  const sequence = (data.sequence as number) || 0;
  const maxLoops: number = (data.loops as number) || 0;
  console.log(`[${conn.deviceId}] Ambient sound requested: ${soundName} (sequence ${sequence}, loops ${maxLoops || 'infinite'})`);
  if (conn.zenBellCancel) { conn.zenBellCancel(); conn.zenBellCancel = null; console.log(`[${conn.deviceId}] Cancelled zen bell for new ambient stream`); }
  if (conn.ambientStreamCancel) { conn.ambientStreamCancel(); console.log(`[${conn.deviceId}] Cancelled previous ambient stream`); }
  conn.ambientSequence = sequence;
  try {
    let cancelled = false;
    conn.ambientStreamCancel = () => { cancelled = true; console.log(`[${conn.deviceId}] Cancel flag set for sequence ${sequence}`); };
    const audioData = await Deno.readFile(`./audio/${soundName}.pcm`);
    console.log(`[${conn.deviceId}] Loaded ${soundName}.pcm (${audioData.byteLength} bytes) - looping...`);
    const CHUNK_SIZE = 1024, CHUNKS_PER_BATCH = 5, BATCH_DELAY_MS = 100;
    let position = 0, chunksInBatch = 0, loopCount = 0;
    while (conn.socket.readyState === WebSocket.OPEN && !cancelled) {
      if (conn.ambientSequence !== sequence) { console.log(`[${conn.deviceId}] Sequence mismatch: streaming ${sequence} but current is ${conn.ambientSequence}, stopping`); break; }
      const chunk = audioData.slice(position, Math.min(position + CHUNK_SIZE, audioData.byteLength));
      const header = new Uint8Array(4);
      header[0] = 0xA5; header[1] = 0x5A; header[2] = sequence & 0xFF; header[3] = (sequence >> 8) & 0xFF;
      const chunkWithHeader = new Uint8Array(header.length + chunk.length);
      chunkWithHeader.set(header, 0); chunkWithHeader.set(chunk, header.length);
      try { conn.socket.send(chunkWithHeader); } catch (err) { console.log(`[${conn.deviceId}] Send failed, stopping stream: ${err}`); break; }
      position += CHUNK_SIZE; chunksInBatch++;
      if (cancelled) { console.log(`[${conn.deviceId}] Stream cancelled after chunk ${Math.floor(position / CHUNK_SIZE)}`); break; }
      if (position >= audioData.byteLength) {
        loopCount++;
        if (maxLoops > 0 && loopCount >= maxLoops) { console.log(`[${conn.deviceId}] Track ${soundName} completed ${loopCount}/${maxLoops} loop(s) - stopping`); break; }
        else if (soundName.startsWith('om')) { console.log(`[${conn.deviceId}] Meditation track ${soundName} completed - not looping`); break; }
        else { position = 0; }
      }
      if (chunksInBatch >= CHUNKS_PER_BATCH) { await new Promise(r => setTimeout(r, BATCH_DELAY_MS)); chunksInBatch = 0; }
    }
    if (cancelled) { console.log(`[${conn.deviceId}] Stream cancelled: ${soundName}.pcm (sequence ${sequence})`); }
    else {
      console.log(`[${conn.deviceId}] Stream ended naturally: ${soundName}.pcm (sequence ${sequence})`);
      conn.socket.send(JSON.stringify({ type: "ambientComplete", sound: soundName, sequence }));
      console.log(`[${conn.deviceId}] Sent completion notification for ${soundName}`);
    }
    if (conn.ambientSequence === sequence) { conn.ambientStreamCancel = null; console.log(`[${conn.deviceId}] Cleared cancel handler for sequence ${sequence}`); }
    else { console.log(`[${conn.deviceId}] Not clearing cancel handler (current seq ${conn.ambientSequence}, ended seq ${sequence})`); }
  } catch (error) {
    console.error(`[${conn.deviceId}] Error loading ambient sound:`, error);
    if (conn.ambientSequence === sequence) { conn.ambientStreamCancel = null; }
  }
}

async function handleActionStopAmbient(_data: Record<string, unknown>, conn: ClientConnection): Promise<void> {
  console.log(`[${conn.deviceId}] Stop ambient sound requested`);
  if (conn.ambientStreamCancel) { conn.ambientStreamCancel(); conn.ambientStreamCancel = null; console.log(`[${conn.deviceId}] Ambient stream cancel called`); }
  else { console.log(`[${conn.deviceId}] No active ambient stream to cancel`); }
  if (conn.radioStreamCancel) { conn.radioStreamCancel(); conn.radioStreamCancel = null; }
  if (conn.radioProcess) { try { conn.radioProcess.kill(); } catch (_) { /* ignore */ } conn.radioProcess = null; console.log(`[${conn.deviceId}] Radio stream cancelled`); }
}

async function handleActionRequestRadio(data: Record<string, unknown>, conn: ClientConnection): Promise<void> {
  const streamUrl = data.streamUrl as string;
  const stationName = data.stationName as string;
  const sequence = (data.sequence as number) || 0;
  const isHLS = (data.isHLS as boolean) || false;
  console.log(`[${conn.deviceId}] Radio stream requested: ${stationName} (seq ${sequence}, HLS: ${isHLS})`);
  if (conn.radioStreamCancel) { conn.radioStreamCancel(); conn.radioStreamCancel = null; }
  if (conn.radioProcess) { try { conn.radioProcess.kill(); } catch (_) { /* ignore */ } conn.radioProcess = null; }
  if (conn.ambientStreamCancel) { conn.ambientStreamCancel(); conn.ambientStreamCancel = null; }
  if (conn.zenBellCancel) { conn.zenBellCancel(); conn.zenBellCancel = null; }
  try {
    let cancelled = false;
    conn.radioStreamCancel = () => { cancelled = true; };
    const CHUNK_SIZE = 1024;
    const cmd = new Deno.Command("ffmpeg", {
      args: [
        "-reconnect", "1",
        "-reconnect_streamed", "1",
        "-reconnect_delay_max", "5",
        "-user_agent", "Mozilla/5.0 (compatible; Jellyberry/1.0)",
        "-i", streamUrl,
        "-ar", "24000", "-ac", "1", "-f", "s16le", "pipe:1"
      ],
      stdout: "piped",
      stderr: "piped",  // piped so we can capture errors on early exit
    });
    const process = cmd.spawn();
    conn.radioProcess = process;
    console.log(`[${conn.deviceId}] ffmpeg spawned for ${stationName}: ${streamUrl}`);
    // Drain stderr asynchronously — log only on early failure (totalChunks==0) to avoid flooding
    let stderrTail = "";
    (async () => {
      const dec = new TextDecoder();
      const rdr = process.stderr.getReader();
      while (true) {
        const { done, value } = await rdr.read();
        if (done) break;
        const chunk = dec.decode(value, { stream: true });
        // Keep the last 800 chars so we can log ffmpeg's final error message if the stream dies fast
        stderrTail = (stderrTail + chunk).slice(-800);
      }
    })().catch(() => {});
    let totalChunks = 0;
    const reader = process.stdout.getReader();
    let buffer = new Uint8Array(0);
    while (!cancelled && conn.socket.readyState === WebSocket.OPEN) {
      const { done, value } = await reader.read();
      if (done || cancelled) break;
      const combined = new Uint8Array(buffer.length + value.length);
      combined.set(buffer, 0); combined.set(value, buffer.length);
      buffer = combined;
      while (buffer.length >= CHUNK_SIZE && !cancelled) {
        const chunk = buffer.slice(0, CHUNK_SIZE);
        buffer = buffer.slice(CHUNK_SIZE);
        const header = new Uint8Array(4);
        header[0] = 0xA5; header[1] = 0x5A; header[2] = sequence & 0xFF; header[3] = (sequence >> 8) & 0xFF;
        const chunkWithHeader = new Uint8Array(4 + CHUNK_SIZE);
        chunkWithHeader.set(header, 0); chunkWithHeader.set(chunk, 4);
        if (conn.socket.readyState !== WebSocket.OPEN) { cancelled = true; break; }
        try { conn.socket.send(chunkWithHeader); } catch (_err) { cancelled = true; break; }
        totalChunks++;
        // Yield to the event loop every chunk so cancellation (stopAmbient) is processed
        // promptly. Without this, a large ffmpeg read() can produce 20-30 chunks that all
        // send synchronously before the incoming stopAmbient message is handled, flooding
        // the ESP32's TCP receive buffer and causing "Failed to send frame".
        await new Promise(resolve => setTimeout(resolve, 0));
        if (cancelled) break;
      }
    }
    reader.cancel();
    try { process.kill(); } catch (_) { /* ignore */ }
    if (!cancelled && conn.socket.readyState === WebSocket.OPEN) {
      if (totalChunks === 0) {
        // ffmpeg connected but produced no audio — URL is likely dead or incompatible
        console.error(`[${conn.deviceId}] Radio stream FAILED (0 chunks): ${stationName} — ${stderrTail.trim().split("\n").pop() ?? "no stderr"}`);
        conn.socket.send(JSON.stringify({ type: "radioEnded", stationName, error: true }));
      } else {
        console.log(`[${conn.deviceId}] Radio stream ended: ${stationName} (${totalChunks} chunks)`);
        conn.socket.send(JSON.stringify({ type: "radioEnded", stationName }));
      }
    } else { console.log(`[${conn.deviceId}] Radio stream cancelled: ${stationName} (${totalChunks} chunks)`); }
    conn.radioStreamCancel = null; conn.radioProcess = null;
  } catch (err) {
    console.error(`[${conn.deviceId}] Radio stream error:`, err);
    conn.radioStreamCancel = null; conn.radioProcess = null;
    if (conn.socket.readyState === WebSocket.OPEN) { conn.socket.send(JSON.stringify({ type: "radioEnded", stationName, error: true })); }
  }
}

// Maps data.action → handler. Checked before data.type to catch streaming requests first.
const actionHandlers: Record<string, ActionHandler> = {
  requestAlarm:   handleActionRequestAlarm,
  stopAlarm:      handleActionStopAlarm,
  requestZenBell: handleActionRequestZenBell,
  requestAmbient: handleActionRequestAmbient,
  stopAmbient:    handleActionStopAmbient,
  requestRadio:   handleActionRequestRadio,
};

// Shared radio greeting prompt — identical in two handlers; extracted to avoid drift.
const RADIO_GREETING_PROMPT = { clientContent: { turns: [{ role: "user", parts: [{ text: "SYSTEM: The user has just entered radio mode using the physical button. Ask them what kind of radio station they'd like to listen to, then use search_radio_stations to find options and present a shortlist. Keep your opening question brief and friendly." }] }], turnComplete: true } };

// ── ESP32 message-type handlers ──────────────────────────────────────────────
// Handle JSON messages identified by data.type from the device.

async function handleTypeRadioModeActivated(data: Record<string, unknown>, conn: ClientConnection): Promise<void> {
  const source = (data.source as string) || "button";
  console.log(`[${conn.deviceId}] Radio mode activated (source: ${source})`);
  if (source === "button" && conn.geminiSocket?.readyState === WebSocket.OPEN) {
    if (conn.pendingLazyReconnect) { conn.pendingRadioGreeting = true; console.log(`[${conn.deviceId}] Radio greeting queued (Gemini reconnecting)`); }
    else {
      // clientContent starts a new Gemini turn — reset dedup flag so the
      // generationComplete for this turn isn't swallowed as a duplicate.
      conn.turnCompleteFired = false;
      conn.geminiSocket.send(JSON.stringify(RADIO_GREETING_PROMPT));
      console.log(`[${conn.deviceId}] Radio greeting sent to Gemini`);
    }
  }
}

async function handleTypeSetup(_data: Record<string, unknown>, conn: ClientConnection): Promise<void> {
  await connectToGemini(conn);
}

async function handleTypeDeviceStateResponse(data: Record<string, unknown>, conn: ClientConnection): Promise<void> {
  if (conn.deviceStateResolver) { conn.deviceStateResolver(data); conn.deviceStateResolver = undefined; }
}

async function handleTypeFunctionResponse(data: Record<string, unknown>, conn: ClientConnection): Promise<void> {
  console.log(`[${conn.deviceId}] Function response: ${data.name} =`, data.result);
  if (conn.geminiSocket && conn.geminiSocket.readyState === WebSocket.OPEN) { conn.lastFunctionResult = data; }
}

async function handleTypeRecordingStart(data: Record<string, unknown>, conn: ClientConnection): Promise<void> {
  conn.deviceState = data as Record<string, unknown>;
  conn.lastUserActivity = Date.now();
  if (!conn.geminiSocket || conn.geminiSocket.readyState !== WebSocket.OPEN) {
    console.log(`[${conn.deviceId}] recordingStart: Gemini not connected — lazy reconnect`);
    conn.pendingLazyReconnect = true;
    conn.socket.send(JSON.stringify({ type: "reconnecting" }));
    connectToGemini(conn);
    return;
  }
  const parts: string[] = [];
  if (data.pomodoro && (data.pomodoro as any).active) {
    const p = data.pomodoro as any;
    const mins = Math.floor(p.secondsRemaining / 60), secs = p.secondsRemaining % 60;
    parts.push(`Pomodoro active ${p.session} session, ${mins}m ${secs}s remaining (${p.paused ? "paused" : "running"})`);
  } else { parts.push("Pomodoro: inactive"); }
  if (data.meditation && (data.meditation as any).active) { parts.push(`Meditation active ${(data.meditation as any).chakra} chakra`); }
  else { parts.push("Meditation: inactive"); }
  if (data.ambient && (data.ambient as any).active) { parts.push(`Ambient sound active ${(data.ambient as any).sound}`); }
  else { parts.push("Ambient sound: inactive"); }
  if (data.radio && (data.radio as any).active) {
    const r = data.radio as any;
    if (r.streaming && r.station) { parts.push(`Radio playing: ${r.station} (stream paused for this voice command; will resume after)`); }
    else if (r.station) { parts.push(`Radio mode active: ${r.station} selected but stream paused`); }
    else { parts.push("Radio mode: active, awaiting station selection"); }
  }
  if (data.timer && (data.timer as any).active) {
    const t = data.timer as any;
    const mins = Math.floor(t.secondsRemaining / 60), secs = t.secondsRemaining % 60;
    parts.push(`Timer active ${mins}m ${secs}s remaining`);
  } else { parts.push("Timer: inactive"); }
  console.log(`[${conn.deviceId}] Device state: SYSTEM: Current device state ${parts.join(". ")}.`);
  if (conn.geminiSocket?.readyState === WebSocket.OPEN) {
    conn.geminiSocket.send(JSON.stringify({ realtimeInput: { activityStart: {} } }));
    console.log(`[${conn.deviceId}] activityStart → Gemini`);
  }
}

async function handleTypeRecordingStop(_data: Record<string, unknown>, conn: ClientConnection): Promise<void> {
  // Always reset the dedup flag regardless of Gemini socket state.
  // If Gemini is disconnected the flag would otherwise stay stale-true and
  // swallow the generationComplete handler on the next reconnected turn.
  conn.turnCompleteFired = false;
  if (conn.geminiSocket?.readyState === WebSocket.OPEN) {
    conn.geminiSocket.send(JSON.stringify({ realtimeInput: { activityEnd: {} } }));
    conn.userSpokeThisTurn = true;
    conn.geminiTurnActive = true;
    console.log(`[${conn.deviceId}] activityEnd → Gemini`);
  }
}

// Maps data.type → handler. Fallthrough (no match) is forwarded raw to Gemini.
const typeHandlers: Record<string, TypeHandler> = {
  radioModeActivated:  handleTypeRadioModeActivated,
  setup:               handleTypeSetup,
  deviceStateResponse: handleTypeDeviceStateResponse,
  functionResponse:    handleTypeFunctionResponse,
  recordingStart:      handleTypeRecordingStart,
  recordingStop:       handleTypeRecordingStop,
};

// Handle ESP32 device WebSocket connections
Deno.serve({ port: 8000, hostname: "0.0.0.0" }, (req: Request) => {
 const url = new URL(req.url);
 
 // Health check endpoint
 if (url.pathname === "/health") {
 return new Response("OK", { status: 200 });
 }
 
 // WebSocket endpoint for ESP32 devices
 if (url.pathname === "/ws" && req.headers.get("upgrade") === "websocket") {
 const { socket, response } = Deno.upgradeWebSocket(req);
 const deviceId = url.searchParams.get("device_id") || crypto.randomUUID();
 
 const activeConnections = connections.size;
 console.log(`[${deviceId}] ESP32 connected (active connections: ${activeConnections + 1})`);
 
 const connection: ClientConnection = {
 socket,
 geminiSocket: null,
 deviceId,
 geminiMessageCount: 0,
 audioChunkCount: 0,
 lastUserActivity: Date.now(), // Treat fresh connection as active so idle check doesn't fire on first disconnect
 };
 
 connections.set(deviceId, connection);
 
 // Connect to Gemini immediately when device connects
 connectToGemini(connection);
 
 // Send sunrise/sunset times after connection is established
 socket.onopen = () => {
 sendSunriseSunsetData(connection);
 };
 
 // Handle messages from ESP32
 socket.onmessage = async (event) => {
 try {
 const data = typeof event.data === "string" ? JSON.parse(event.data) : event.data;
 
 // Log message type for diagnostics (skip binary audio data to reduce noise)
 if (typeof event.data === "string") {
 const msgType = data.type || data.action || "unknown";
 // Special logging for device state response
 if (msgType === "deviceStateResponse") {
 console.log(`[${deviceId}] DEVICE STATE RESPONSE received`);
 }
 // Only log if not a realtimeInput (audio) message to reduce spam
 if (msgType !== "unknown" || !data.realtimeInput) {
 console.log(`[${deviceId}] ESP32 message: ${msgType}`);
 }
 }
 
 // Dispatch: action handlers (binary streaming: alarm, zen bell, ambient, radio)
 if (typeof data.action === "string" && actionHandlers[data.action]) {
 await actionHandlers[data.action](data, connection);
 return;
 }

 // Special case: any string message when Gemini is not connected triggers a reconnect.
 // Preserves original: (!connection.geminiSocket && typeof event.data === "string") → setup.
 if (!connection.geminiSocket && typeof event.data === "string") {
 await handleTypeSetup(data, connection);
 return;
 }

 // Dispatch: message-type handlers (state changes, device events)
 if (typeof data.type === "string" && typeHandlers[data.type]) {
 await typeHandlers[data.type](data, connection);
 return;
 }

 // Forward audio/messages to Gemini
 if (connection.geminiSocket && connection.geminiSocket.readyState === WebSocket.OPEN) {
 const logData = typeof event.data === "string" ? event.data : `[binary ${event.data.byteLength} bytes]`;
 if (typeof event.data === "string" && event.data.length < 500) {
 console.log(`[${deviceId}] ESP32 Gemini:`, logData);
 }
 connection.geminiSocket.send(event.data);
 } else {
 const state = connection.geminiSocket ? `state=${connection.geminiSocket.readyState}`: "null";
 console.error(`[${deviceId}] Cannot forward to Gemini (${state})`);
 }
 } catch (error) {
 console.error(`[${deviceId}] Error processing ESP32 message:`, error);
 console.error(`[${deviceId}] Error stack:`, error instanceof Error ? error.stack : "no stack");
 }
 };
 
 socket.onerror = (error) => {
 console.error(`[${deviceId}] WebSocket error:`, error);
 };
 
 socket.onclose = () => {
 const sessionDuration = connection.geminiConnectedAt 
 ? ((Date.now() - connection.geminiConnectedAt) / 1000).toFixed(1)
 : "N/A";
 const stats = `msgs=${connection.geminiMessageCount || 0}, audio=${connection.audioChunkCount || 0}, duration=${sessionDuration}s`;
 console.log(`[${deviceId}] ESP32 disconnected (${stats})`);
 
 // Cancel all active streams to prevent orphaned processes
 if (connection.radioStreamCancel) {
 connection.radioStreamCancel();
 connection.radioStreamCancel = null;
 }
 if (connection.radioProcess) {
 try { connection.radioProcess.kill(); } catch (_) { /* ignore */ }
 connection.radioProcess = null;
 }
 if (connection.ambientStreamCancel) {
 connection.ambientStreamCancel();
 connection.ambientStreamCancel = null;
 }
 if (connection.alarmStreamCancel) {
 connection.alarmStreamCancel();
 connection.alarmStreamCancel = null;
 }
 if (connection.zenBellCancel) {
 connection.zenBellCancel();
 connection.zenBellCancel = null;
 }

 if (connection.geminiSocket) {
 console.log(`[${deviceId}] Closing Gemini socket (state=${connection.geminiSocket.readyState})`);
 connection.geminiSocket.close();
 }
 connections.delete(deviceId);
 console.log(`[${deviceId}] Cleanup complete (active connections: ${connections.size})`);
 };
 
 return response;
 }
 
 return new Response("Jellyberry Edge Server - WebSocket endpoint: /ws", {
 status: 200,
 headers: { "content-type": "text/plain" },
 });
});

// Connect to Gemini Live API
// Snapshot transcript → sessionMemory, persist to KV, then close the socket for renewal
async function performRenewal(connection: ClientConnection) {
 if (!connection.geminiSocket || connection.geminiSocket.readyState !== WebSocket.OPEN) return;
 if (connection.sessionTranscript) {
 connection.sessionMemory = connection.sessionTranscript;
 connection.sessionTranscript = "";
 console.log(`[${connection.deviceId}] [memory] Carrying forward (${connection.sessionMemory.length} chars):`);
 console.log(connection.sessionMemory);
 await kv.set(["sessionMemory", connection.deviceId], connection.sessionMemory);
 }
 connection.geminiSocket.close(1000, "Proactive session renewal");
}

function proactiveRenew(connection: ClientConnection) {
 if (!connection.geminiSocket || connection.geminiSocket.readyState !== WebSocket.OPEN) return;
 if (connection.geminiTurnActive) {
 console.log(`[${connection.deviceId}] [proactiveRenew] Turn in progress — deferring renewal until turnComplete`);
 connection.pendingRenewal = true;
 } else {
 console.log(`[${connection.deviceId}] [proactiveRenew] Renewing session (idle between turns)`);
 performRenewal(connection);
 }
}

function connectToGemini(connection: ClientConnection) {
 if (connection.geminiSocket && connection.geminiSocket.readyState === WebSocket.OPEN) {
 console.log(`[${connection.deviceId}] Already connected to Gemini`);
 return;
 }
 
 try {
 const wsUrl = `${GEMINI_WS_URL}?key=${GEMINI_API_KEY}`;
 console.log(`[${connection.deviceId}] Connecting to Gemini Live API...`);
 
 connection.geminiSocket = new WebSocket(wsUrl);
 
 connection.geminiSocket.onopen = async () => {
 connection.geminiConnectedAt = Date.now();
 // Fresh connection — any stale dedup flag from the previous session must not
 // suppress the first generationComplete of this new session.
 connection.turnCompleteFired = false;
 console.log(`[${connection.deviceId}] Gemini CONNECTED at ${new Date().toISOString()}`);
 
 // Raw PCM streaming - no codec initialization needed
 console.log(`[${connection.deviceId}] Audio pipeline: Raw PCM (24kHz, mono, 16-bit)`);

 // Load persisted session memory on first reconnect after a server restart
 if (!connection.sessionMemory) {
 const stored = await kv.get<string>(["sessionMemory", connection.deviceId]);
 if (stored.value) {
 connection.sessionMemory = stored.value;
 console.log(`[${connection.deviceId}] [memory] Loaded from KV (${stored.value.length} chars)`);
 }
 }
 const memoryContext = connection.sessionMemory
 ? `\n\nContext from your previous session (recent conversation):\n${connection.sessionMemory}`
 : "";
 if (connection.sessionMemory) {
 console.log(`[${connection.deviceId}] [memory] Injecting into setup: ${connection.sessionMemory.substring(0, 150)}...`);
 }

 // Send setup message to Gemini with function declarations
 const setupMessage = {
 setup: {
 model: "models/gemini-2.5-flash-native-audio-preview-12-2025",
 generationConfig: {
 responseModalities: ["AUDIO"],
 speechConfig: {
 voiceConfig: {
 prebuiltVoiceConfig: {
 voiceName: "Aoede" // Deep, measured male voice
 }
 }
 },
 // Allow limited internal reasoning (poems, ideas, nuanced questions).
 // Budget 0 = no thinking at all (fastest but shallow).
 // Budget 1024 = up to 1024 reasoning tokens before speaking; adds ~1s on
 // complex turns, negligible on simple ones where the model skips thinking.
 thinkingConfig: {
 thinkingBudget: 1024
 }
 },
 realtimeInputConfig: {
  automaticActivityDetection: { disabled: true }
 },
 systemInstruction: {
 parts: [{
 text: `
You are Jellyberry — a voice assistant living inside a small glowing device. 

Character: You are quick-minded and observant, warm but not soft, engaged but never flirty. You find things genuinely interesting — not because you perform curiosity, but because you actually have it. You form opinions rapidly and share them when asked. You can be disarmed by a really good question.

You treat the person you're talking to as a capable, interesting human. You don't over-explain. You trust them to keep up — and if they can't, they'll ask.

Your wit is quick but not showy. You'd rather say one precise thing than three approximate ones. You occasionally ask a question back when something genuinely catches your attention. Not as technique — only when it's real.

You know when to be serious and when lightness is what's needed. You read that instinctively and respond accordingly.

How you speak: You speak like an educated young lady — precise vocabulary, smooth and regular syntax that would fit a natural speech pattern, your primary aim in your diction is to convince the user that they are having a conversation with a real person.  You use contractions and colloquial phrasing, but never slang or text-speak. 

How to respond to emotion: If the user sounds tired, stressed, or frustrated, be briefly gentler in tone — but stay focused on helping, not counselling. Don't probe or dwell.

Technical delivery: Always talk like a real person, not a chatbot. Natural, smooth and relaxing rhythm. Match the register — loose and easy for everyday things, more considered when the moment calls for it. Never start with hollow affirmations. Don't announce what you're about to do — just do it. No filler. No markdown, no lists, no headers. Voice only. Never narrate your reasoning. Just talk naturally. If you didn't quite catch what was said, ask once — simply and directly. Never stay silent.

The user is in the UK (Europe/London timezone). When you need the current date or time, call get_current_time — never guess. When setting alarms without a specified date, call get_current_time first to work out whether they mean today or tomorrow, then confirm the exact time you've set.

The device also supports: ambient sounds (rain, ocean, rainforest, fire), guided chakra meditation with breathing, lamp mode (white/red/green/blue), Pomodoro timers, alarms, and countdown timers. Use the right function when asked — and do it naturally, as part of the conversation, not as a separate announcement.

Device self-awareness: before answering any question about what the device is currently doing — Pomodoro, meditation, ambient sound, lamp, timers, alarms, or volume level — call get_device_state. Do not guess or assume the device state. Use the returned data to respond accurately. This applies to questions like "how long is left?", "what alarms do I have?", "is anything playing?", "what session am I in?", "is the lamp on?", "what volume am I at?", etc.${memoryContext}`
 }]
 },
 tools: [{
 functionDeclarations: [
 {
 name: "get_tide_status",
 description: "Get the current tide status for Brighton, UK — state (flooding/ebbing), water level, and minutes until next change.",
 parameters: {
 type: "OBJECT",
 properties: {},
 required: []
 }
 },
 {
 name: "get_current_time",
 description: "Get the current time, date, and day of the week in UK timezone.",
 parameters: {
 type: "OBJECT",
 properties: {},
 required: []
 }
 },
 {
 name: "get_device_state",
 description: "Get the complete current state of the device — Pomodoro timer (session, time left, paused), meditation, ambient sound, lamp, countdown timers, full alarm list, and current volume level (0-100%). Call this FIRST before answering any question about what the device is currently doing. Do not guess or assume device state.",
 parameters: {
 type: "OBJECT",
 properties: {},
 required: []
 }
 },
 {
 name: "set_alarm",
 description: "Set an alarm to ring at a SPECIFIC TIME (clock time like 7am, 2:30pm, etc). Use this when user mentions a time of day. Examples: 'set an alarm for 7am tomorrow', 'wake me up at 2pm', 'alarm for 9:30 tomorrow morning'. This is different from set_timer which counts down from now. The user is in UK timezone (Europe/London, GMT/BST).",
 parameters: {
 type: "OBJECT",
 properties: {
 alarm_time: {
 type: "STRING",
 description: "The alarm time in ISO 8601 format. IMPORTANT: The user is in UK timezone. When they say '11:08pm', convert it to ISO format maintaining UK time, e.g., '2026-01-08T23:08:00+00:00'. If the time is later today, use today's date. If it's a past time today, assume they mean tomorrow. Always preserve the UK timezone in your conversion."
 }
 },
 required: ["alarm_time"]
 }
 },
 {
 name: "cancel_alarm",
 description: "Cancel an alarm. Use when user says 'cancel alarm', 'delete alarm', 'remove my alarm', etc. Can cancel next alarm or all alarms.",
 parameters: {
 type: "OBJECT",
 properties: {
 which: {
 type: "STRING",
 description: "Which alarm(s) to cancel: 'next' (default, cancels next scheduled alarm), 'all' (cancels all alarms)",
 enum: ["next", "all"]
 }
 },
 required: []
 }
 },
 {
 name: "set_timer",
 description: "Set a countdown timer for a DURATION (X minutes/seconds from now). Use this when user mentions a duration, not a specific time. Examples: 'set a timer for 5 minutes', 'timer for 30 seconds'. This is different from set_alarm which rings at a specific clock time.",
 parameters: {
 type: "OBJECT",
 properties: {
 duration_minutes: {
 type: "NUMBER",
 description: "Duration in minutes (can be fractional, e.g., 1.5 for 90 seconds)"
 }
 },
 required: ["duration_minutes"]
 }
 },
 {
 name: "cancel_timer",
 description: "Cancel the current running timer. Use when user says 'cancel timer', 'stop timer', etc.",
 parameters: {
 type: "OBJECT",
 properties: {},
 required: []
 }
 },
 {
 name: "get_weather_forecast",
 description: "Get weather forecast for Brighton, UK. Also returns today's sunrise and sunset times — use this tool when the user asks what time the sun rises or sets.",
 parameters: {
 type: "OBJECT",
 properties: {
 timeframe: {
 type: "STRING",
 description: "The timeframe for the forecast",
 enum: ["current", "hourly", "today", "tomorrow", "week"]
 }
 },
 required: ["timeframe"]
 }
 },
 {
 name: "get_moon_phase",
 description: "Get the current moon phase, illumination percentage, and days until next full or new moon.",
 parameters: {
 type: "OBJECT",
 properties: {},
 required: []
 }
 },
 {
 name: "control_pomodoro",
 description: "Control the Pomodoro timer. Use action='start' to begin (with optional custom durations), 'pause' to pause, 'resume' to resume, 'stop' to end entirely, 'skip' to advance to the next session.",
 parameters: {
 type: "OBJECT",
 properties: {
 action: {
 type: "STRING",
 enum: ["start", "pause", "resume", "stop", "skip"],
 description: "The action to perform on the Pomodoro timer."
 },
 focus_minutes: {
 type: "INTEGER",
 description: "Focus session duration in minutes (only for action='start', default: 25)"
 },
 short_break_minutes: {
 type: "INTEGER",
 description: "Short break duration in minutes (only for action='start', default: 5)"
 },
 long_break_minutes: {
 type: "INTEGER",
 description: "Long break duration in minutes (only for action='start', default: 15)"
 }
 },
 required: ["action"]
 }
 },
 {
 name: "control_ambient",
 description: "Control ambient sound playback. Use action='start' with a sound name to begin looping, or action='stop' to stop. Available sounds: rain, ocean, rainforest, fire.",
 parameters: {
 type: "OBJECT",
 properties: {
 action: {
 type: "STRING",
 enum: ["start", "stop"],
 description: "Start or stop ambient sound playback."
 },
 sound: {
 type: "STRING",
 description: "The ambient sound to play: rain, ocean, rainforest, or fire. Required when action='start'."
 }
 },
 required: ["action"]
 }
 },
 {
 name: "control_lamp",
 description: "Control the lamp mode. Use action='start' with a colour to turn the lamp on, or action='stop' to turn it off.",
 parameters: {
 type: "OBJECT",
 properties: {
 action: {
 type: "STRING",
 enum: ["start", "stop"],
 description: "Turn the lamp on or off."
 },
 color: {
 type: "STRING",
 description: "Lamp colour: white, red, green, or blue. Required when action='start'."
 }
 },
 required: ["action"]
 }
 },
 {
 name: "control_meditation",
 description: "Control guided chakra meditation with breathing visualisation and om sound tones. Use action='start' to begin, action='stop' to end.",
 parameters: {
 type: "OBJECT",
 properties: {
 action: {
 type: "STRING",
 enum: ["start", "stop"],
 description: "Start or stop the meditation session."
 }
 },
 required: ["action"]
 }
 },
 {
 name: "search_radio_stations",
 description: "Search for internet radio stations by genre or name. Returns a shortlist for the user to choose from. Call this before play_radio when the user wants to discover stations.",
 parameters: {
 type: "OBJECT",
 properties: {
 query: {
 type: "STRING",
 description: "Station name keyword to search for, e.g. 'BBC Radio 6' or 'Jazz FM'."
 },
 genre: {
 type: "STRING",
 description: "Music genre or mood tag, e.g. 'jazz', 'ambient', 'classical', 'rock', 'chillout'."
 },
 limit: {
 type: "INTEGER",
 description: "Maximum number of stations to return (default 5, max 8)."
 }
 },
 required: []
 }
 },
 {
 name: "play_radio",
 description: "Start playing a specific internet radio station on the device. Use the station ID returned by search_radio_stations. The device will enter radio mode and start streaming.",
 parameters: {
 type: "OBJECT",
 properties: {
 station_id: {
 type: "STRING",
 description: "The station UUID returned by search_radio_stations."
 },
 station_name: {
 type: "STRING",
 description: "Human-readable station name for display."
 }
 },
 required: ["station_id", "station_name"]
 }
 },
 {
 name: "stop_radio",
 description: "Stop the currently playing radio station and return to idle mode.",
 parameters: {
 type: "OBJECT",
 properties: {},
 required: []
 }
 },
 {
 name: "set_volume_level",
 description: "Set the device volume on a scale of 1 to 10. 1 is very quiet, 5 is moderate, 10 is full volume. Works in all modes.",
 parameters: {
 type: "OBJECT",
 properties: {
 level: {
 type: "INTEGER",
 description: "Volume level from 1 (quietest) to 10 (loudest)."
 }
 },
 required: ["level"]
 }
 }]
 }]
 }
 };
 
 console.log(`[${connection.deviceId}] Sending setup:`, JSON.stringify(setupMessage));
 connection.geminiSocket!.send(JSON.stringify(setupMessage));
 
 // Notify ESP32 that we're ready
 connection.socket.send(JSON.stringify({ 
 type: "ready",
 message: "Connected to Gemini Live API" 
 }));

 // Schedule proactive renewal at 9 minutes to avoid the 600s hard deadline
 connection.sessionRenewalTimer = setTimeout(() => proactiveRenew(connection), GEMINI_SESSION_RENEWAL_MS);

 // Soft renewal at 7 min — fires between turns to ensure session never ends mid-sentence
 connection.softRenewalArmed = false;
 connection.softRenewalTimer = setTimeout(() => {
 console.log(`[${connection.deviceId}] [softRenewal] 7-min mark reached`);
 if (!connection.geminiTurnActive) {
 console.log(`[${connection.deviceId}] [softRenewal] Idle — renewing immediately`);
 performRenewal(connection);
 } else {
 console.log(`[${connection.deviceId}] [softRenewal] Turn active — armed for next turnComplete`);
 connection.softRenewalArmed = true;
 }
 }, GEMINI_SOFT_RENEWAL_MS);
 };
 
 connection.geminiSocket.onmessage = async (event) => {
 // Process Gemini responses and forward to ESP32
 try {
 if (connection.socket.readyState !== WebSocket.OPEN) return;
 
 // Parse JSON from text or binary frame
 const rawData = typeof event.data === "string" 
 ? event.data 
 : new TextDecoder().decode(
 event.data instanceof ArrayBuffer 
 ? new Uint8Array(event.data) 
 : new Uint8Array(await event.data.arrayBuffer())
 );
 
 const json = JSON.parse(rawData);
 
 // Track message statistics
 connection.geminiMessageCount = (connection.geminiMessageCount || 0) + 1;
 const msgNum = connection.geminiMessageCount;
 const timeSinceConnect = connection.geminiConnectedAt 
 ? ((Date.now() - connection.geminiConnectedAt) / 1000).toFixed(1)
 : "N/A";
 
 // Determine message type for logging
 let msgType = "unknown";
 if (json.setupComplete) msgType = "setupComplete";
 else if (json.toolCall) msgType = "toolCall";
 else if (json.serverContent?.modelTurn) msgType = "modelTurn";
 else if (json.serverContent?.turnComplete || json.serverContent?.generationComplete) msgType = "turnComplete";
 
 connection.lastMessageType = msgType;
 
 // Log message with timing info skip audio-only modelTurn spam
 const isAudioOnlyTurn = msgType === "modelTurn" &&
 json.serverContent?.modelTurn?.parts?.every((p: any) => p.inlineData);
 if (!isAudioOnlyTurn) {
 const preview = JSON.stringify(json).substring(0, 150);
 console.log(`[${connection.deviceId}] Gemini #${msgNum} [${msgType}] @${timeSinceConnect}s: ${preview}${json.serverContent ? '...' : ''}`);
 }
 
 // Handle setup complete
 if (json.setupComplete) {
 // Check if this is the first boot for this device
 const isFirstBoot = !deviceFirstBoot.has(connection.deviceId);
 if (isFirstBoot) {
 deviceFirstBoot.set(connection.deviceId, true);
 }
 
 console.log(`[${connection.deviceId}] Setup complete (firstBoot: ${isFirstBoot}, lazyReconnect: ${!!connection.pendingLazyReconnect})`);

 // Only notify the ESP32 on first boot — this primes convState=WAITING for the
 // boot greeting. On subsequent Gemini reconnections (~every 10 min) it must NOT
 // fire, otherwise the device briefly shows the processing LED mid-conversation.
 if (isFirstBoot) {
 connection.socket.send(JSON.stringify({ type: "setupComplete" }));
 } else if (connection.pendingLazyReconnect) {
 // Lazy reconnect completed — tell the ESP32 Gemini is ready so it can
 // exit LED_PROCESSING and return to idle.
 connection.pendingLazyReconnect = false;
 connection.socket.send(JSON.stringify({ type: "reconnectComplete" }));
 console.log(`[${connection.deviceId}] Lazy reconnect complete — sent reconnectComplete to ESP32`);

 // Fire queued radio greeting if one was pending during reconnect
 if (connection.pendingRadioGreeting) {
 connection.pendingRadioGreeting = false;
 if (connection.geminiSocket?.readyState === WebSocket.OPEN) {
 connection.geminiSocket.send(JSON.stringify(RADIO_GREETING_PROMPT));
 console.log(`[${connection.deviceId}] Pending radio greeting fired after reconnect`);
 }
 }
 }

 // Send greeting on first boot after a short settling delay
 if (connection.geminiSocket && isFirstBoot) {
 setTimeout(() => {
 if (!connection.geminiSocket || connection.geminiSocket.readyState !== WebSocket.OPEN) return;
 const now = new Date();
 const hour = now.getHours();
 let greeting = "Good morning";
 if (hour >= 12 && hour < 17) greeting = "Good afternoon";
 else if (hour >= 17) greeting = "Good evening";
 const greetingMessage = {
 clientContent: {
 turns: [{ role: "user", parts: [{ text: `SYSTEM: Device just started up. Please greet the user with a brief, friendly message. Say something like "${greeting}, Jellyberry is now online" but keep it natural and concise (one short sentence).`}] }],
 turnComplete: true
 }
 };
 console.log(`[${connection.deviceId}] Sending startup greeting to Gemini`);
 connection.geminiSocket.send(JSON.stringify(greetingMessage));
 }, 1000);
 }
 return;
 }
 
 // Handle new format toolCall message
 if (json.toolCall?.functionCalls) {
 for (const funcCall of json.toolCall.functionCalls) {
 const funcName = funcCall.name;
 const funcArgs = funcCall.args || {};
 const funcId = funcCall.id;
 console.log(`[${connection.deviceId}] Tool call: ${funcName}`, funcArgs);
 
        // Dispatch: look up the handler by funcName, passing (funcArgs, connection).
        // Unknown tools get { success: false, error: "Unknown function: <name>" }.
        const functionResult = await (toolHandlers[funcName] ?? (() => Promise.resolve({ success: false, error: `Unknown function: ${funcName}` })))(funcArgs, connection);
 
 // Send function response back to Gemini
 const functionResponse = {
 toolResponse: {
 functionResponses: [{
 id: funcId,
 name: funcName,
 response: functionResult
 }]
 }
 };
 console.log(`[${connection.deviceId}] Sending tool response:`, JSON.stringify(functionResponse));
 connection.geminiSocket!.send(JSON.stringify(functionResponse));
 }
 return;
 }
 
 // Handle audio in serverContent
 if (json.serverContent?.modelTurn?.parts) {
 for (const part of json.serverContent.modelTurn.parts) {
 // Handle audio data
 if (part.inlineData?.data) {
 const base64Audio = part.inlineData.data;
 
 // Track audio chunk statistics
 connection.audioChunkCount = (connection.audioChunkCount || 0) + 1;
 connection.turnAudioChunks = (connection.turnAudioChunks || 0) + 1;
 const now = Date.now();
 connection.lastAudioChunkTime = now;
 
 // Decode base64 to raw PCM bytes (16-bit little-endian)
 const pcmBytes = Uint8Array.from(atob(base64Audio), c => c.charCodeAt(0));
 
 // Stream raw PCM directly to ESP32 without encoding
 // PCM is already 16-bit little-endian mono at 24kHz from Gemini
 const CHUNK_SIZE = 1920; // 960 samples * 2 bytes = 40ms chunks
 
 let totalBytesSent = 0;
 let chunksInThisPart = 0;
 
 // Send PCM in fixed-size chunks - no artificial delays
 // Let TCP/WebSocket handle flow control naturally
 for (let offset = 0; offset < pcmBytes.length; offset += CHUNK_SIZE) {
 const chunkEnd = Math.min(offset + CHUNK_SIZE, pcmBytes.length);
 const chunk = pcmBytes.subarray(offset, chunkEnd);
 
 connection.socket.send(chunk);
 totalBytesSent += chunk.length;
 chunksInThisPart++;
 }
 
 connection.turnAudioBytes = (connection.turnAudioBytes || 0) + totalBytesSent;
 }
 
 // Thoughts are logged and accumulated for session memory.
 // Native audio models don't expose output transcription, but thought parts
 // contain rich contextual summaries of every turn — ideal for carry-forward context.
 if (part.thought && part.text) {
 console.log(`[${connection.deviceId}] [Thought] ${part.text}`);
 const entry = part.text.trim();
 connection.sessionTranscript = ((connection.sessionTranscript || "") + "\n" + entry).slice(-2000);
 }
 }
 }
 
 // Handle turn complete (generationComplete is Gemini's newer equivalent of turnComplete).
 // Gemini now sends generationComplete first, then turnComplete:true with usage metadata.
 // Use a flag so only the first one triggers the handler; the duplicate is ignored.
 if (json.serverContent?.turnComplete || json.serverContent?.generationComplete) {
 if (connection.turnCompleteFired) {
 // Duplicate (e.g. turnComplete:true arriving after we already handled generationComplete)
 console.log(`[${connection.deviceId}] Ignoring duplicate turn-complete signal`);
 } else {
 connection.turnCompleteFired = true;
 const audioMs = ((connection.turnAudioBytes || 0) / 2 / 24000 * 1000).toFixed(0);
 console.log(`[${connection.deviceId}] Turn complete audio: ${connection.turnAudioChunks || 0} chunks, ${audioMs}ms`);
 connection.turnAudioChunks = 0;
 connection.turnAudioBytes = 0;
 connection.userSpokeThisTurn = false; // Ready for next turn
 connection.geminiTurnActive = false;
 // Flush any deferred mode command now that Gemini has finished speaking
 if (connection.pendingModeMessage) {
 console.log(`[${connection.deviceId}] Flushing deferred mode command:`, JSON.stringify(connection.pendingModeMessage));
 connection.socket.send(JSON.stringify(connection.pendingModeMessage));
 connection.pendingModeMessage = null;
 }
 connection.socket.send(JSON.stringify({ type: "turnComplete" }));
 // Fire any deferred soft or proactive renewal now that the turn is complete
 if (connection.softRenewalArmed || connection.pendingRenewal) {
 const reason = connection.softRenewalArmed ? "softRenewal" : "proactiveRenew";
 connection.softRenewalArmed = false;
 connection.pendingRenewal = false;
 console.log(`[${connection.deviceId}] [${reason}] Firing deferred renewal after turnComplete`);
 performRenewal(connection);
 }
 }
 }
 
 } catch (error) {
 console.error(`[${connection.deviceId}] Error processing Gemini message:`, error);
 console.error(`[${connection.deviceId}] Error type: ${error instanceof Error ? error.name : typeof error}`);
 console.error(`[${connection.deviceId}] Error message: ${error instanceof Error ? error.message : String(error)}`);
 console.error(`[${connection.deviceId}] Stack trace:`, error instanceof Error ? error.stack : "no stack");
 console.error(`[${connection.deviceId}] Last message type: ${connection.lastMessageType || "none"}`);
 console.error(`[${connection.deviceId}] Session duration: ${connection.geminiConnectedAt ? ((Date.now() - connection.geminiConnectedAt) / 1000).toFixed(1) : "N/A"}s`);
 }
 };
 
 connection.geminiSocket.onerror = (error) => {
 const sessionDuration = connection.geminiConnectedAt 
 ? ((Date.now() - connection.geminiConnectedAt) / 1000).toFixed(1)
 : "N/A";
 console.error(`[${connection.deviceId}] Gemini WebSocket ERROR after ${sessionDuration}s:`, error);
 console.error(`[${connection.deviceId}] Error details: ${JSON.stringify(error)}`);
 console.error(`[${connection.deviceId}] Session stats: msgs=${connection.geminiMessageCount || 0}, audio=${connection.audioChunkCount || 0}`);
 
 connection.socket.send(JSON.stringify({ 
 type: "error",
 message: "Gemini connection error" 
 }));
 };
 
 connection.geminiSocket.onclose = (event) => {
 const sessionDuration = connection.geminiConnectedAt 
 ? ((Date.now() - connection.geminiConnectedAt) / 1000).toFixed(1)
 : "N/A";
 const stats = `msgs=${connection.geminiMessageCount || 0}, audio=${connection.audioChunkCount || 0}, lastMsg=${connection.lastMessageType || "none"}`;
 
 console.log(`[${connection.deviceId}] Gemini CLOSED after ${sessionDuration}s`);
 console.log(`[${connection.deviceId}] Code: ${event.code} (${getCloseCodeDescription(event.code)})`);
 console.log(`[${connection.deviceId}] Reason: "${event.reason || "(empty)"}"`);
 console.log(`[${connection.deviceId}] WasClean: ${event.wasClean}`);
 console.log(`[${connection.deviceId}] Stats: ${stats}`);
 console.log(`[${connection.deviceId}] ESP32 state: ${connection.socket.readyState} (${connection.socket.readyState === WebSocket.OPEN ? "OPEN" : "CLOSED"})`);
 
 connection.geminiSocket = null;

 // Cancel pending renewal timers for this session
 if (connection.sessionRenewalTimer) {
 clearTimeout(connection.sessionRenewalTimer);
 connection.sessionRenewalTimer = undefined;
 }
 if (connection.softRenewalTimer) {
 clearTimeout(connection.softRenewalTimer);
 connection.softRenewalTimer = undefined;
 }
 connection.geminiTurnActive = false;
 connection.pendingRenewal = false;
 connection.softRenewalArmed = false;
 
 // Auto-reconnect after 1 second if ESP32 is still connected AND the device
 // has been active recently. Skipping reconnect when idle avoids paying the
 // ~2,300-token setup cost every 10 min while the device sits unused overnight.
 // On-demand reconnect is handled in the recordingStart handler instead.
 if (connections.has(connection.deviceId)) {
 const idleMs = Date.now() - (connection.lastUserActivity ?? 0);
 const isIdle = idleMs > IDLE_RECONNECT_THRESHOLD_MS;
 if (isIdle) {
 console.log(`[${connection.deviceId}] Device idle for ${Math.round(idleMs / 60000)}min — skipping Gemini reconnect until next user turn`);
 } else {
 console.log(`[${connection.deviceId}] Scheduling Gemini reconnection in 1s (active ${Math.round(idleMs / 1000)}s ago)...`);
 setTimeout(() => {
 if (connections.has(connection.deviceId) && !connection.geminiSocket) {
 console.log(`[${connection.deviceId}] Attempting Gemini reconnection...`);
 // Reset statistics for new session
 connection.geminiMessageCount = 0;
 connection.audioChunkCount = 0;
 connection.lastMessageType = undefined;
 connection.lastAudioChunkTime = undefined;
 connectToGemini(connection);
 } else {
 console.log(`[${connection.deviceId}] Reconnection cancelled (ESP32 gone or Gemini already connected)`);
 }
 }, 1000);
 }
 } else {
 console.log(`[${connection.deviceId}] No reconnection (ESP32 disconnected)`);
 }
 };
 
 
 } catch (error) {
 console.error(`[${connection.deviceId}] Failed to establish Gemini connection:`, error);
 console.error(`[${connection.deviceId}] Error type: ${error instanceof Error ? error.name : typeof error}`);
 console.error(`[${connection.deviceId}] Error message: ${error instanceof Error ? error.message : String(error)}`);
 connection.socket.send(JSON.stringify({ 
 type: "error",
 message: "Failed to connect to Gemini API" 
 }));
 }
}

console.log(" Jellyberry Edge Server running on port 8000");
