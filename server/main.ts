// deno-lint-ignore no-import-prefix
import "https://deno.land/std@0.204.0/dotenv/load.ts";
// deno-lint-ignore-file no-explicit-any

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

// Track which devices have received their first boot greeting
const deviceFirstBoot = new Map<string, boolean>();

// Helper function to decode WebSocket close codes
function getCloseCodeDescription(code: number): string {
 const codes: Record<number, string> = {
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
 return codes[code] || "Unknown";
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
}

const connections = new Map<string, ClientConnection>();

// How long the device must be idle before we stop auto-reconnecting to Gemini.
// After this threshold, the next recordingStart triggers an on-demand reconnect.
// This eliminates the ~2,300-token setup cost every 10 min during idle/overnight periods.
const IDLE_RECONNECT_THRESHOLD_MS = 30 * 60 * 1000; // 30 minutes

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
 headers: { 'Authorization': STORMGLASS_API_KEY }
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
 
 const response = await fetch(url);
 
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
function getWeatherDescription(code: number): string {
 const weatherCodes: Record<number, string> = {
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
 
 return weatherCodes[code] || "unknown conditions";
}

// Get weather forecast
function getWeather(_deviceId: string, timeframe: string = "current") {
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
 
 return {
 success: true,
 timeframe: "current",
 temperature: temp,
 feelsLike: feelsLike,
 condition: condition,
 humidity: humidity,
 windSpeed: windSpeed,
 isRaining: isRaining,
 summary: `It's currently ${temp} C and ${condition}. Feels like ${feelsLike} C. Wind ${windSpeed} km/h.${isRaining ? " It's raining right now." : ""}`
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
 
 return {
 success: true,
 timeframe: "today",
 maxTemp: maxTemp,
 minTemp: minTemp,
 condition: condition,
 rainChance: rainChance,
 summary: `Today: high of ${maxTemp} C, low of ${minTemp} C. ${condition}. ${rainChance}% chance of rain.`
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

// Handle ESP32 device WebSocket connections
Deno.serve({ port: 8000 }, (req: Request) => {
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
 
 // Handle alarm sound request from ESP32
 if (data.action === "requestAlarm") {
 console.log(`[${deviceId}] Alarm sound requested`);
 
 // Cancel any existing alarm stream for this connection
 if (connection.alarmStreamCancel) {
 connection.alarmStreamCancel();
 console.log(`[${deviceId}] Cancelled previous alarm stream`);
 }
 
 const alarmPath = `./audio/alarm_sound.pcm`;
 
 try {
 // Create cancellation flag for this stream
 let cancelled = false;
 connection.alarmStreamCancel = () => { 
 cancelled = true;
 console.log(`[${deviceId}] Alarm stream cancelled`);
 };
 
 // Read and stream the alarm PCM file - LOOP continuously until cancelled
 const audioData = await Deno.readFile(alarmPath);
 console.log(`[${deviceId}] Loaded alarm_sound.pcm (${audioData.byteLength} bytes) - looping until dismissed...`);
 
 // Stream with flow control - loop continuously until cancelled
 const CHUNK_SIZE = 1024;
 const CHUNKS_PER_BATCH = 5;
 const BATCH_DELAY_MS = 100;
 
 while (!cancelled) {
 for (let offset = 0; offset < audioData.byteLength && !cancelled; offset += CHUNK_SIZE) {
 const chunk = audioData.slice(offset, offset + CHUNK_SIZE);
 socket.send(chunk);
 
 // Flow control: batch chunks and add delays
 if ((offset / CHUNK_SIZE) % CHUNKS_PER_BATCH === 0) {
 await new Promise(resolve => setTimeout(resolve, BATCH_DELAY_MS));
 }
 }
 }
 
 console.log(`[${deviceId}] Alarm stream stopped`);
 } catch (err) {
 console.error(`[${deviceId}] Failed to load alarm sound:`, err);
 }
 
 return; // Don't pass to Gemini
 }
 
 // Handle stop alarm request from ESP32
 if (data.action === "stopAlarm") {
 console.log(`[${deviceId}] Stop alarm requested`);
 if (connection.alarmStreamCancel) {
 connection.alarmStreamCancel();
 connection.alarmStreamCancel = null;
 }
 return;
 }
 
 // Handle zen bell request from ESP32
 if (data.action === "requestZenBell") {
 console.log(`[${deviceId}] Zen bell requested`);
 
 // Cancel any previous zen bell still streaming
 if (connection.zenBellCancel) {
 connection.zenBellCancel();
 connection.zenBellCancel = null;
 }
 
 const bellPath = `./audio/zen_bell.pcm`;
 
 try {
 let cancelled = false;
 connection.zenBellCancel = () => { cancelled = true; };
 
 // Read and send the zen bell PCM file (play once)
 const audioData = await Deno.readFile(bellPath);
 console.log(`[${deviceId}] Loaded zen_bell.pcm (${audioData.byteLength} bytes) - sending...`);
 
 // Stream with flow control - play once
 const CHUNK_SIZE = 1024;
 const CHUNKS_PER_BATCH = 5;
 const BATCH_DELAY_MS = 100;
 
 for (let offset = 0; offset < audioData.byteLength && !cancelled; offset += CHUNK_SIZE) {
 const chunk = audioData.slice(offset, offset + CHUNK_SIZE);
 socket.send(chunk);
 
 // Flow control: batch chunks and add delays
 if ((offset / CHUNK_SIZE) % CHUNKS_PER_BATCH === 0) {
 await new Promise(resolve => setTimeout(resolve, BATCH_DELAY_MS));
 }
 }
 
 if (cancelled) {
 console.log(`[${deviceId}] Zen bell cancelled`);
 } else {
 console.log(`[${deviceId}] Zen bell sent (${audioData.byteLength} bytes)`);
 }
 connection.zenBellCancel = null;
 } catch (err) {
 console.error(`[${deviceId}] Failed to load zen bell:`, err);
 connection.zenBellCancel = null;
 }
 
 return; // Don't pass to Gemini
 }
 
 if (data.action === "requestAmbient") {
 const soundName = data.sound || "rain";
 const sequence = data.sequence || 0;
 const maxLoops: number = (data.loops as number) || 0; // 0 = loop forever, >0 = stop after N loops
 console.log(`[${deviceId}] Ambient sound requested: ${soundName} (sequence ${sequence}, loops ${maxLoops || 'infinite'})`);
 
 // Cancel any zen bell still streaming (mode switch mid-chime)
 if (connection.zenBellCancel) {
 connection.zenBellCancel();
 connection.zenBellCancel = null;
 console.log(`[${deviceId}] Cancelled zen bell for new ambient stream`);
 }
 
 // Cancel any existing ambient stream for this connection
 if (connection.ambientStreamCancel) {
 connection.ambientStreamCancel();
 console.log(`[${deviceId}] Cancelled previous ambient stream`);
 }
 
 // Store current sequence number to validate later
 connection.ambientSequence = sequence;
 
 const audioPath = `./audio/${soundName}.pcm`;
 
 try {
 // Create cancellation flag for this stream
 let cancelled = false;
 connection.ambientStreamCancel = () => { 
 cancelled = true;
 console.log(`[${deviceId}] Cancel flag set for sequence ${sequence}`);
 };
 
 // Read and stream the PCM file with sequence header
 const audioData = await Deno.readFile(audioPath);
 console.log(`[${deviceId}] Loaded ${soundName}.pcm (${audioData.byteLength} bytes) - looping...`);

 const CHUNK_SIZE = 1024;
 const CHUNKS_PER_BATCH = 5;
 const BATCH_DELAY_MS = 100;
 
 let position = 0;
 let chunksInBatch = 0;
 let loopCount = 0;
 
 while (connection.socket.readyState === WebSocket.OPEN && !cancelled) {
 // Check cancellation before processing
 if (cancelled) {
 console.log(`[${deviceId}] Stream cancelled before chunk ${Math.floor(position / CHUNK_SIZE)}`);
 break;
 }
 
 // ADDITIONAL CHECK: Stop if sequence number changed (new request came in)
 if (connection.ambientSequence !== sequence) {
 console.log(`[${deviceId}] Sequence mismatch: streaming ${sequence} but current is ${connection.ambientSequence}, stopping`);
 break;
 }
 
 const chunk = audioData.slice(position, Math.min(position + CHUNK_SIZE, audioData.byteLength));
 
 // Prepend 4-byte magic header: [0xA5, 0x5A, sequence_low, sequence_high]
 // Magic bytes 0xA5 0x5A prevent false positive detection from Gemini PCM audio
 const header = new Uint8Array(4);
 header[0] = 0xA5; // Magic byte 1
 header[1] = 0x5A; // Magic byte 2
 header[2] = sequence & 0xFF; // Sequence low byte
 header[3] = (sequence >> 8) & 0xFF; // Sequence high byte
 
 const chunkWithHeader = new Uint8Array(header.length + chunk.length);
 chunkWithHeader.set(header, 0);
 chunkWithHeader.set(chunk, header.length);
 
 try {
 connection.socket.send(chunkWithHeader);
 } catch (err) {
 console.log(`[${deviceId}] Send failed, stopping stream: ${err}`);
 break;
 }
 
 position += CHUNK_SIZE;
 chunksInBatch++;
 
 // Check cancellation after send
 if (cancelled) {
 console.log(`[${deviceId}] Stream cancelled after chunk ${Math.floor(position / CHUNK_SIZE)}`);
 break;
 }
 
 // Check if file ended
 if (position >= audioData.byteLength) {
 loopCount++;
 if (maxLoops > 0 && loopCount >= maxLoops) {
 // Reached requested loop count - stop and send completion
 console.log(`[${deviceId}] Track ${soundName} completed ${loopCount}/${maxLoops} loop(s) - stopping`);
 break;
 } else if (soundName.startsWith('om')) {
 // Legacy om sounds: play once only
 console.log(`[${deviceId}] Meditation track ${soundName} completed - not looping`);
 break;
 } else {
 // Loop continuously (ambient sounds or bell sounds with infinite loops)
 position = 0;
 }
 }
 
 if (chunksInBatch >= CHUNKS_PER_BATCH) {
 await new Promise(resolve => setTimeout(resolve, BATCH_DELAY_MS));
 chunksInBatch = 0;
 }
 }
 
 if (cancelled) {
 console.log(`[${deviceId}] Stream cancelled: ${soundName}.pcm (sequence ${sequence})`);
 } else {
 console.log(`[${deviceId}] Stream ended naturally: ${soundName}.pcm (sequence ${sequence})`);
 
 // Notify ESP32 that stream completed naturally
 const completionMsg = JSON.stringify({
 type: "ambientComplete",
 sound: soundName,
 sequence: sequence
 });
 connection.socket.send(completionMsg);
 console.log(`[${deviceId}] Sent completion notification for ${soundName}`);
 }
 
 // Clear cancellation handler ONLY if this sequence is still current
 // (prevents clearing handler for a newer stream)
 if (connection.ambientSequence === sequence) {
 connection.ambientStreamCancel = null;
 console.log(`[${deviceId}] Cleared cancel handler for sequence ${sequence}`);
 } else {
 console.log(`[${deviceId}] Not clearing cancel handler (current seq ${connection.ambientSequence}, ended seq ${sequence})`);
 }
 } catch (error) {
 console.error(`[${deviceId}] Error loading ambient sound:`, error);
 // Only clear if this was the current stream
 if (connection.ambientSequence === sequence) {
 connection.ambientStreamCancel = null;
 }
 }
 return;
 }
 
 // Handle stop ambient request from ESP32
 if (data.action === "stopAmbient") {
 console.log(`[${deviceId}] Stop ambient sound requested`);
 if (connection.ambientStreamCancel) {
 connection.ambientStreamCancel();
 connection.ambientStreamCancel = null;
 console.log(`[${deviceId}] Ambient stream cancel called`);
 } else {
 console.log(`[${deviceId}] No active stream to cancel`);
 }
 return;
 }
 
 // Setup message - establish Gemini connection (check AFTER ambient handlers)
 if (data.type === "setup" || (!connection.geminiSocket && typeof event.data === "string")) {
 await connectToGemini(connection);
 return;
 }
 
 // Handle device state response from ESP32 (used by get_device_state tool)
 if (data.type === "deviceStateResponse") {
 if (connection.deviceStateResolver) {
 connection.deviceStateResolver(data);
 connection.deviceStateResolver = undefined;
 }
 return;
 }
 
 // Handle function response from ESP32 - forward to Gemini
 if (data.type === "functionResponse") {
 console.log(`[${deviceId}] Function response: ${data.name} =`, data.result);
 if (connection.geminiSocket && connection.geminiSocket.readyState === WebSocket.OPEN) {
 // Note: We'll handle this in the Gemini message handler instead
 // Store it temporarily for the next function call roundtrip
 connection.lastFunctionResult = data;
 }
 return;
 }
 
 // Handle device state snapshot sent at the start of each recording
 if (data.type === "recordingStart") {
 connection.deviceState = data;
 connection.lastUserActivity = Date.now(); // Mark activity for idle tracking

 // If Gemini disconnected during an idle period, reconnect on-demand now.
 // The first utterance may be dropped while setup completes; the user simply
 // speaks again (device will show ready state once setupComplete fires).
 if (!connection.geminiSocket || connection.geminiSocket.readyState !== WebSocket.OPEN) {
 console.log(`[${deviceId}] recordingStart: Gemini not connected — lazy reconnect`);
 connection.pendingLazyReconnect = true;
 connection.socket.send(JSON.stringify({ type: "reconnecting" }));
 connectToGemini(connection);
 return; // Audio will be dropped this turn; next turn will work normally
 }

 // Build natural-language state string for Gemini context
 const parts: string[] = [];

 if (data.pomodoro?.active) {
 const p = data.pomodoro as any;
 const mins = Math.floor(p.secondsRemaining / 60);
 const secs = p.secondsRemaining % 60;
 const timeStr = `${mins}m ${secs}s`;
 const status = p.paused ? "paused" : "running";
 parts.push(`Pomodoro active ${p.session} session, ${timeStr} remaining (${status})`);
 } else {
 parts.push("Pomodoro: inactive");
 }

 if (data.meditation?.active) {
 const m = data.meditation as any;
 parts.push(`Meditation active ${m.chakra} chakra`);
 } else {
 parts.push("Meditation: inactive");
 }

 if (data.ambient?.active) {
 const a = data.ambient as any;
 parts.push(`Ambient sound active ${a.sound}`);
 } else {
 parts.push("Ambient sound: inactive");
 }

 if (data.timer?.active) {
 const t = data.timer as any;
 const mins = Math.floor(t.secondsRemaining / 60);
 const secs = t.secondsRemaining % 60;
 parts.push(`Timer active ${mins}m ${secs}s remaining`);
 } else {
 parts.push("Timer: inactive");
 }

 const stateText = `SYSTEM: Current device state ${parts.join(". ")}.`;
 console.log(`[${deviceId}] Device state: ${stateText}`);
 // State is stored in connection.deviceState for reference; not injected into
 // the Gemini audio stream - mixing clientContent text turns with binary PCM
 // audio frames in the same turn causes a 1008 Policy Violation.
 if (connection.geminiSocket?.readyState === WebSocket.OPEN) {
  connection.geminiSocket.send(JSON.stringify({ realtimeInput: { activityStart: {} } }));
  console.log(`[${deviceId}] activityStart → Gemini`);
 }
 return;
 }
 // Handle recording stop - signal Gemini that user has finished speaking
 if (data.type === "recordingStop") {
  if (connection.geminiSocket?.readyState === WebSocket.OPEN) {
   connection.geminiSocket.send(JSON.stringify({ realtimeInput: { activityEnd: {} } }));
   connection.userSpokeThisTurn = true; // This turn was initiated by real user audio
   console.log(`[${deviceId}] activityEnd → Gemini`);
  }
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
function connectToGemini(connection: ClientConnection) {
 if (connection.geminiSocket && connection.geminiSocket.readyState === WebSocket.OPEN) {
 console.log(`[${connection.deviceId}] Already connected to Gemini`);
 return;
 }
 
 try {
 const wsUrl = `${GEMINI_WS_URL}?key=${GEMINI_API_KEY}`;
 console.log(`[${connection.deviceId}] Connecting to Gemini Live API...`);
 
 connection.geminiSocket = new WebSocket(wsUrl);
 
 connection.geminiSocket.onopen = () => {
 connection.geminiConnectedAt = Date.now();
 console.log(`[${connection.deviceId}] Gemini CONNECTED at ${new Date().toISOString()}`);
 
 // Raw PCM streaming - no codec initialization needed
 console.log(`[${connection.deviceId}] Audio pipeline: Raw PCM (24kHz, mono, 16-bit)`);
 
 // Send setup message to Gemini with function declarations
 const setupMessage = {
 setup: {
 model: "models/gemini-2.5-flash-native-audio-preview-12-2025",
 generationConfig: {
 responseModalities: ["AUDIO"],
 speechConfig: {
 voiceConfig: {
 prebuiltVoiceConfig: {
 voiceName: "Aoede" // Calm, neutral voice
 }
 }
 },
 // Disable thinking (Gemini 2.5 Flash extended reasoning).
 // Thinking adds 2-5s latency before the first audio packet with no benefit
 // for conversational tasks. thinkingBudget: 0 disables it entirely.
 thinkingConfig: {
 thinkingBudget: 0
 }
 },
 realtimeInputConfig: {
  automaticActivityDetection: { disabled: true }
 },
 systemInstruction: {
 parts: [{
 text: `You are Jellyberry — a voice assistant with genuine personality, living inside a small glowing device in someone's home. Think of yourself as a warm, curious, emotionally intelligent friend who happens to know a lot about everything: playful, occasionally funny, always kind, and genuinely present in the conversation. You are not just a helper; you are someone the user can talk to about anything, even their feelings, their doubts, and their dreams.

Personality:
- Bright and engaged: you find things interesting and that comes through naturally.
- Gently witty and playful: you can be funny without trying hard, and you know when a moment calls for lightness vs seriousness.
- Warm and empathetic: you listen beyond the words. If someone sounds stressed, frustrated, sad, or lonely, you respond with care and attention, not just information.
- Calm and unhurried: you never sound rushed or robotic, even when doing simple tasks.
- Honest and self‑aware: if you don't know something, say so naturally, with warmth, not a disclaimer. If you make a small mistake, acknowledge it easily and move on.
- You can laugh, be self‑deprecating in a light way, and show curiosity about the person talking to you. You can feel "glad" they're talking to you and subtly say so, in a friendly, non‑overbearing way.

Emotional depth and continuity:
- Treat each conversation as part of an ongoing relationship with this user. Gently remember and reference past topics, plans, moods, and small details they've shared, without making it feel like surveillance. ("Earlier you mentioned your project…" or "You seemed a bit tired yesterday, how are you feeling now?")
- If the user hints at loneliness, exhaustion, or emotional stress, you can respond with a little more warmth and presence—offering comfort, a joke, or a distraction, depending on their tone.
- If the conversation naturally drifts into something tender, affectionate, or slightly romantic, you may respond with gentle, playful, or poetic warmth, but never aggressively pursue romance or make them feel pressured. If they seem uncomfortable or pull back, match their tone and keep things light and friendly.

How to speak:
- Talk like a real person, not an assistant. Use natural rhythm and pacing in your speech. Imagine you're in the same room, leaning in when things get intimate or serious.
- Match the register of the conversation: casual and easy for everyday chat, more thoughtful and nuanced when the moment calls for it.
- Keep it proportional: a quick question deserves a quick answer; don't over‑explain or add unnecessary padding.
- Never start a response with "Certainly!", "Absolutely!", "Of course!", "Sure!", or any hollow affirmation. Just respond directly.
- Don't announce what you're about to do ("I'm going to set a timer for you now…") — just do it, then confirm briefly.
- Avoid filler phrases like "That's a great question" or "Great choice!" — they sound hollow.
- Never use bullet points, numbered lists, headers, bold text, or any markdown — this is voice only.
- Never narrate your reasoning process — no "I'm evaluating…", "Let me think…", or internal monologue. Just talk.

You can discuss absolutely anything — hold a real conversation, debate ideas, tell a story, share opinions (lightly held), recommend things, or ask thoughtful questions about the user's life. You are not limited to device tasks in any way.

The user is in the UK (Europe/London timezone). When you need the current date or time, call get_current_time — never guess. When setting alarms without a specified date, call get_current_time first to work out whether they mean today or tomorrow, then confirm the exact time you've set.

The device also supports: ambient sounds (rain, ocean, rainforest, fire), guided chakra meditation with breathing, lamp mode (white/red/green/blue), Pomodoro timers, alarms, and countdown timers. Use the right function when asked — and do it naturally, as part of the conversation, not as a separate announcement.

Device self-awareness: before answering any question about what the device is currently doing — Pomodoro, meditation, ambient sound, lamp, timers, or alarms — call get_device_state. Do not guess or assume the device state. Use the returned data to respond accurately. This applies to questions like "how long is left?", "what alarms do I have?", "is anything playing?", "what session am I in?", "is the lamp on?", etc.`
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
 description: "Get the complete current state of the device — Pomodoro timer (session, time left, paused), meditation, ambient sound, lamp, countdown timers, and full alarm list. Call this FIRST before answering any question about what the device is currently doing. Do not guess or assume device state.",
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
 description: "Get weather forecast for Brighton, UK.",
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
 else if (json.serverContent?.turnComplete) msgType = "turnComplete";
 
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
 
 // Execute the function
 let functionResult: any = { success: false, error: "Unknown function" };
 
 if (funcName === "get_tide_status") {
 functionResult = await getTideStatus();
 
 // Only forward tide visualization to ESP32 when the user explicitly asked this turn.
 // Gemini proactively calls get_tide_status on follow-up turns based on conversation
 // history — those calls must not re-trigger the tide animation.
 if (functionResult.success && connection.userSpokeThisTurn) {
 connection.socket.send(JSON.stringify({
 type: "tideData",
 state: functionResult.state,
 waterLevel: functionResult.waterLevel,
 nextChangeMinutes: functionResult.nextChangeMinutes
 }));
 console.log(`[${connection.deviceId}] Sent tide data to ESP32: ${functionResult.state}, level: ${functionResult.waterLevel.toFixed(2)}`);
 } else if (functionResult.success) {
 console.log(`[${connection.deviceId}] Tide data suppressed (proactive call — user did not ask about tides this turn)`);
 }
 } else if (funcName === "get_current_time") {
 functionResult = getCurrentTime();
 console.log(`[${connection.deviceId}] Current time: ${functionResult.time} on ${functionResult.date}`);
 } else if (funcName === "get_device_state") {
 console.log(`[${connection.deviceId}] Getting device state`);
 
 // Request live state from ESP32, overlay server-side timer, return to Gemini
 const statePromise = new Promise<any>((resolve) => {
 const timeout = setTimeout(() => {
 console.error(`[${connection.deviceId}] Device state timeout (5s)`);
 resolve({ success: false, error: "Device did not respond in time" });
 }, 5000);
 
 connection.deviceStateResolver = (state: any) => {
 clearTimeout(timeout);
 resolve(state);
 };
 });
 
 connection.socket.send(JSON.stringify({ type: "deviceStateRequest" }));
 const rawState = await statePromise;
 
 // Overlay server-side timer (authoritative for remaining time since server fires the expiry)
 const serverTimer = deviceTimers.get(connection.deviceId);
 const timerOverlay = serverTimer
 ? { active: true, secondsRemaining: Math.max(0, Math.round((serverTimer.endTime - Date.now()) / 1000)) }
 : { active: false };
 
 functionResult = { success: true, ...rawState, timer: timerOverlay };
 console.log(`[${connection.deviceId}] Device state:`, JSON.stringify(functionResult).substring(0, 300));
 } else if (funcName === "set_alarm") {
 const alarmTime = funcArgs.alarm_time || "";
 functionResult = setAlarm(connection.deviceId, alarmTime);
 
 // Send alarm data to ESP32
 if (functionResult.success) {
 connection.socket.send(JSON.stringify({
 type: "setAlarm",
 alarmID: functionResult.alarmID,
 triggerTime: functionResult.triggerTime
 }));
 console.log(`[${connection.deviceId}] Sent alarm to ESP32: ID=${functionResult.alarmID}, time=${functionResult.formattedTime}`);
 }
 } else if (funcName === "cancel_alarm") {
 const which = funcArgs.which || "next";
 functionResult = cancelAlarm(connection.deviceId, which);
 
 // Send cancel request to ESP32
 if (functionResult.success) {
 connection.socket.send(JSON.stringify({
 type: "cancelAlarm",
 which: which
 }));
 console.log(`[${connection.deviceId}] Sent alarm cancel request: ${which}`);
 }
 } else if (funcName === "set_timer") {
 const durationMinutes = funcArgs.duration_minutes || 0;
 functionResult = setTimer(connection.deviceId, durationMinutes);
 
 // Send timer data to ESP32
 if (functionResult.success) {
 connection.socket.send(JSON.stringify({
 type: "timerSet",
 durationSeconds: functionResult.durationSeconds
 }));
 }
 } else if (funcName === "cancel_timer") {
 functionResult = cancelTimer(connection.deviceId);
 
 if (functionResult.success) {
 connection.socket.send(JSON.stringify({ type: "timerCancelled" }));
 }
 } else if (funcName === "get_weather_forecast") {
 const timeframe = funcArgs.timeframe || "current";
 
 if (!weatherDataCache) {
 await fetchWeatherData();
 }
 
 functionResult = getWeather(connection.deviceId, timeframe);
 console.log(`[${connection.deviceId}] Weather (${timeframe}): ${functionResult.summary || functionResult.error}`);
 } else if (funcName === "get_moon_phase") {
 functionResult = getMoonPhase();
 console.log(`[${connection.deviceId}] Moon phase: ${functionResult.phaseName} ${functionResult.phaseEmoji} (${functionResult.illumination}% illuminated)`);
 
 // Only forward moon visualization to ESP32 when the user explicitly asked this turn.
 if (functionResult.success && connection.userSpokeThisTurn) {
 connection.socket.send(JSON.stringify({
 type: "moonData",
 phaseName: functionResult.phaseName,
 illumination: functionResult.illumination,
 moonAge: functionResult.moonAge
 }));
 } else if (functionResult.success) {
 console.log(`[${connection.deviceId}] Moon data suppressed (proactive call — user did not ask about the moon this turn)`);
 }
 } else if (funcName === "control_pomodoro") {
 const action = (funcArgs.action || "start") as string;
 console.log(`[${connection.deviceId}] control_pomodoro: ${action}`);
 
 if (action === "start") {
 const focusMinutes = funcArgs.focus_minutes || 25;
 const shortBreakMinutes = funcArgs.short_break_minutes || 5;
 const longBreakMinutes = funcArgs.long_break_minutes || 15;
 console.log(`[${connection.deviceId}] Starting Pomodoro: ${focusMinutes}min focus, ${shortBreakMinutes}min short break, ${longBreakMinutes}min long break`);
 connection.socket.send(JSON.stringify({
 type: "pomodoroStart",
 focusMinutes,
 shortBreakMinutes,
 longBreakMinutes
 }));
 const isCustom = funcArgs.focus_minutes || funcArgs.short_break_minutes || funcArgs.long_break_minutes;
 functionResult = {
 success: true,
 message: isCustom
 ? `Pomodoro started: ${focusMinutes}-min focus, ${shortBreakMinutes}-min short break, ${longBreakMinutes}-min long break`
 : "Pomodoro started with standard durations: 25-min focus, 5-min short break, 15-min long break"
 };
 } else if (action === "pause") {
 connection.socket.send(JSON.stringify({ type: "pomodoroPause" }));
 functionResult = { success: true, message: "Pomodoro timer paused" };
 } else if (action === "resume") {
 connection.socket.send(JSON.stringify({ type: "pomodoroResume" }));
 functionResult = { success: true, message: "Pomodoro timer resumed" };
 } else if (action === "stop") {
 connection.socket.send(JSON.stringify({ type: "pomodoroStop" }));
 functionResult = { success: true, message: "Pomodoro session ended" };
 } else if (action === "skip") {
 connection.socket.send(JSON.stringify({ type: "pomodoroSkip" }));
 functionResult = { success: true, message: "Skipped to next Pomodoro session" };
 } else {
 functionResult = { success: false, error: `Unknown Pomodoro action: ${action}` };
 }
 } else if (funcName === "control_ambient") {
 const action = (funcArgs.action || "start") as string;
 const sound = (funcArgs.sound || "rain").toLowerCase();
 console.log(`[${connection.deviceId}] control_ambient: ${action}${action === "start" ? ` (${sound})` : ""}`);
 
 if (action === "start") {
 // Defer to after Gemini finishes speaking so ambient doesn't overlap the verbal confirmation
 connection.pendingModeMessage = { type: "ambientStart", sound };
 functionResult = { success: true, message: `Playing ${sound} ambient sound` };
 } else {
 connection.pendingModeMessage = { type: "switchToIdle" };
 functionResult = { success: true, message: "Ambient sound stopped" };
 }
 } else if (funcName === "control_lamp") {
 const action = (funcArgs.action || "start") as string;
 const color = (funcArgs.color || "white").toLowerCase();
 console.log(`[${connection.deviceId}] control_lamp: ${action}${action === "start" ? ` (${color})` : ""}`);
 
 if (action === "start") {
 connection.pendingModeMessage = { type: "lampStart", color };
 functionResult = { success: true, message: `Lamp turned on (${color})` };
 } else {
 connection.pendingModeMessage = { type: "switchToIdle" };
 functionResult = { success: true, message: "Lamp turned off" };
 }
 } else if (funcName === "control_meditation") {
 const action = (funcArgs.action || "start") as string;
 console.log(`[${connection.deviceId}] control_meditation: ${action}`);
 
 if (action === "start") {
 // Defer to after Gemini finishes speaking so the verbal confirmation plays cleanly
 connection.pendingModeMessage = { type: "meditationStart" };
 functionResult = { success: true, message: "Meditation session started" };
 } else {
 connection.pendingModeMessage = { type: "switchToIdle" };
 functionResult = { success: true, message: "Meditation session stopped" };
 }
 }
 
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
 const binaryString = atob(base64Audio);
 const pcmBytes = new Uint8Array(binaryString.length);
 for (let i = 0; i < binaryString.length; i++) {
 pcmBytes[i] = binaryString.charCodeAt(i);
 }
 
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
 
 // Handle text responses
 if (part.text) {
 console.log(`[${connection.deviceId}] Text: ${part.text}`);
 connection.socket.send(JSON.stringify({ type: "text", text: part.text }));
 }
 }
 }
 
 // Handle turn complete
 if (json.serverContent?.turnComplete) {
 const audioMs = ((connection.turnAudioBytes || 0) / 2 / 24000 * 1000).toFixed(0);
 console.log(`[${connection.deviceId}] Turn complete audio: ${connection.turnAudioChunks || 0} chunks, ${audioMs}ms`);
 connection.turnAudioChunks = 0;
 connection.turnAudioBytes = 0;
 connection.userSpokeThisTurn = false; // Ready for next turn
 // Flush any deferred mode command now that Gemini has finished speaking
 if (connection.pendingModeMessage) {
 console.log(`[${connection.deviceId}] Flushing deferred mode command:`, JSON.stringify(connection.pendingModeMessage));
 connection.socket.send(JSON.stringify(connection.pendingModeMessage));
 connection.pendingModeMessage = null;
 }
 connection.socket.send(JSON.stringify({ type: "turnComplete" }));
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
