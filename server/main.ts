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
  endTime: number;  // Unix timestamp when timer expires
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
  
  // Session tracking for diagnostics
  geminiConnectedAt?: number;  // Timestamp when Gemini connected
  geminiMessageCount?: number;  // Total messages received from Gemini
  audioChunkCount?: number;     // Total audio chunks received
  lastAudioChunkTime?: number;  // Timestamp of last audio chunk
  lastMessageType?: string;     // Type of last message received
}

const connections = new Map<string, ClientConnection>();

// Fetch tide data from StormGlass API
async function fetchTideData() {
  const now = Date.now();
  
  // Return cached data if still valid
  if (tideDataCache && (now - tideDataTimestamp) < CACHE_DURATION_MS) {
    console.log("📊 Using cached tide data");
    return tideDataCache;
  }
  
  try {
    const today = new Date();
    const startDate = new Date(today.getTime() - 2 * 24 * 60 * 60 * 1000); // 2 days in the past
    const endDate = new Date(today.getTime() + 7 * 24 * 60 * 60 * 1000); // 7 days ahead
    
    const url = `https://api.stormglass.io/v2/tide/extremes/point?` +
      `lat=${BRIGHTON_LAT}&lng=${BRIGHTON_LNG}&` +
      `start=${startDate.toISOString()}&end=${endDate.toISOString()}`;
    
    console.log("🌊 Fetching tide data from StormGlass...");
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
    console.log(`✓ Fetched ${data.data?.length || 0} tide extremes`);
    return data;
  } catch (error) {
    console.error("Error fetching tide data:", error);
    return null;
  }
}

// Calculate current tide status
function calculateTideStatus(tideData: any) {
  if (!tideData || !tideData.data || tideData.data.length === 0) {
    console.error("❌ No tide data available");
    return null;
  }
  
  const now = new Date();
  const extremes = tideData.data;
  
  console.log(`📊 Processing ${extremes.length} tide extremes, current time: ${now.toISOString()}`);
  console.log(`📊 First extreme:`, extremes[0]);
  console.log(`📊 Last extreme:`, extremes[extremes.length - 1]);
  
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
  
  console.log(`📊 Previous extreme:`, prevExtreme);
  console.log(`📊 Next extreme:`, nextExtreme);
  
  if (!prevExtreme || !nextExtreme) {
    console.error("❌ Could not find prev/next extremes");
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
      console.log(`[${deviceId}] ⏰ Timer expired!`);
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
    } else if (remaining % 60 === 0 && remaining > 0) {
      // Send progress update every minute
      const connection = connections.get(deviceId);
      if (connection?.socket.readyState === WebSocket.OPEN) {
        connection.socket.send(JSON.stringify({
          type: "timerUpdate",
          secondsRemaining: remaining
        }));
      }
    }
  }, 1000);
  
  deviceTimers.set(deviceId, timerState);
  
  console.log(`[${deviceId}] ⏱️  Timer set for ${durationMinutes} minutes (${durationSeconds}s)`);
  
  return {
    success: true,
    durationMinutes,
    durationSeconds,
    expiresAt: new Date(endTime).toLocaleTimeString('en-GB')
  };
}

// Check timer status
function checkTimer(deviceId: string) {
  const timer = deviceTimers.get(deviceId);
  
  if (!timer) {
    return {
      success: false,
      active: false,
      message: "No timer is currently running"
    };
  }
  
  const remaining = Math.max(0, Math.round((timer.endTime - Date.now()) / 1000));
  const minutes = Math.floor(remaining / 60);
  const seconds = remaining % 60;
  
  return {
    success: true,
    active: true,
    secondsRemaining: remaining,
    minutesRemaining: minutes,
    displayTime: `${minutes}:${seconds.toString().padStart(2, '0')}`
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
  
  console.log(`[${deviceId}] ⏱️  Timer cancelled`);
  
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
    console.log("🌤️  Using cached weather data");
    return weatherDataCache;
  }
  
  try {
    // Using Brighton coordinates (same as tide data)
    const url = `https://api.open-meteo.com/v1/forecast?latitude=${BRIGHTON_LAT}&longitude=${BRIGHTON_LNG}&current=temperature_2m,relative_humidity_2m,apparent_temperature,precipitation,rain,weather_code,wind_speed_10m&hourly=temperature_2m,precipitation_probability,weather_code&daily=temperature_2m_max,temperature_2m_min,precipitation_sum,precipitation_probability_max,weather_code&timezone=Europe/London&forecast_days=3`;
    
    const response = await fetch(url);
    
    if (!response.ok) {
      throw new Error(`Open-Meteo API returned ${response.status}`);
    }
    
    const data = await response.json();
    
    // Cache the result
    weatherDataCache = data;
    weatherDataTimestamp = now;
    
    console.log("🌤️  Weather data fetched from Open-Meteo");
    return data;
    
  } catch (error) {
    console.error("❌ Error fetching weather data:", error);
    
    // Return cached data if available, even if expired
    if (weatherDataCache) {
      console.log("⚠️  Using expired weather cache due to fetch error");
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
function getWeather(deviceId: string, timeframe: string = "current") {
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
        summary: `It's currently ${temp}°C and ${condition}. Feels like ${feelsLike}°C. Wind ${windSpeed} km/h.${isRaining ? " It's raining right now." : ""}`
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
        summary: `Over the next 12 hours: temperatures from ${Math.round(hourly.temperature_2m[0])}°C to ${Math.round(hourly.temperature_2m[11])}°C. ${hourly.precipitation_probability[0] > 50 ? "Rain is likely." : "Rain is unlikely."}`
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
        summary: `Today: high of ${maxTemp}°C, low of ${minTemp}°C. ${condition}. ${rainChance}% chance of rain.`
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
        summary: `Tomorrow: high of ${maxTemp}°C, low of ${minTemp}°C. ${condition}. ${rainChance}% chance of rain.`
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
        summary: `3-day forecast: ${weekSummary.map(d => `${d.day} ${d.maxTemp}°C`).join(", ")}.`
      };
    }
    
    return {
      success: false,
      error: "Invalid timeframe requested"
    };
    
  } catch (error) {
    console.error("❌ Error getting weather:", error);
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
      phaseEmoji = "🌑";
    } else if (cyclePosition < 0.216) {
      phaseName = "Waxing Crescent";
      phaseEmoji = "🌒";
    } else if (cyclePosition < 0.283) {
      phaseName = "First Quarter";
      phaseEmoji = "🌓";
    } else if (cyclePosition < 0.466) {
      phaseName = "Waxing Gibbous";
      phaseEmoji = "🌔";
    } else if (cyclePosition < 0.533) {
      phaseName = "Full Moon";
      phaseEmoji = "🌕";
    } else if (cyclePosition < 0.716) {
      phaseName = "Waning Gibbous";
      phaseEmoji = "🌖";
    } else if (cyclePosition < 0.783) {
      phaseName = "Last Quarter";
      phaseEmoji = "🌗";
    } else {
      phaseName = "Waning Crescent";
      phaseEmoji = "🌘";
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
    console.error("❌ Error calculating moon phase:", error);
    return {
      success: false,
      error: "Failed to calculate moon phase"
    };
  }
}

// Initial weather data fetch
console.log("🌤️  Fetching initial weather data...");
fetchWeatherData().catch(err => console.error("❌ Failed to fetch initial weather data:", err));

// Handle ESP32 device WebSocket connections
Deno.serve({ 
  port: 8000,
  // Increase idle timeout to prevent disconnects during audio streaming
  idleTimeout: 120  // 120 seconds (default is 30-60s)
}, (req) => {
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
    console.log(`[${deviceId}] 🔌 ESP32 connected (active connections: ${activeConnections + 1})`);
    
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
    
    // Handle messages from ESP32
    socket.onmessage = async (event) => {
      try {
        const data = typeof event.data === "string" ? JSON.parse(event.data) : event.data;
        
        // Log message type for diagnostics (skip binary audio data to reduce noise)
        if (typeof event.data === "string") {
          const msgType = data.type || data.action || "unknown";
          // Only log if not a realtimeInput (audio) message to reduce spam
          if (msgType !== "unknown" || !data.realtimeInput) {
            console.log(`[${deviceId}] 📨 ESP32 message: ${msgType}`);
          }
        }
        
        // Setup message - establish Gemini connection
        if (data.type === "setup" || (!connection.geminiSocket && typeof event.data === "string")) {
          await connectToGemini(connection);
          return;
        }
        
        // Handle ambient sound requests from ESP32
        if (data.action === "requestAmbient") {
          const soundName = data.sound || "rain";
          const sequence = data.sequence || 0;
          console.log(`[${deviceId}] Ambient sound requested: ${soundName} (sequence ${sequence})`);
          
          // Cancel any existing ambient stream for this connection
          if (connection.ambientStreamCancel) {
            connection.ambientStreamCancel();
            console.log(`[${deviceId}] ✗ Cancelled previous ambient stream`);
          }
          
          const audioPath = `./audio/${soundName}.pcm`;
          
          try {
            // Create cancellation flag for this stream
            let cancelled = false;
            connection.ambientStreamCancel = () => { cancelled = true; };
            
            // Read and stream the PCM file with sequence header
            const audioData = await Deno.readFile(audioPath);
            console.log(`[${deviceId}] ✓ Loaded ${soundName}.pcm (${audioData.byteLength} bytes)`);
            
            // Stream with flow control
            const CHUNK_SIZE = 1024;
            const CHUNKS_PER_BATCH = 5;
            const BATCH_DELAY_MS = 100;
            
            let position = 0;
            let chunksInBatch = 0;
            
            while (position < audioData.byteLength && connection.socket.readyState === WebSocket.OPEN && !cancelled) {
              const chunk = audioData.slice(position, Math.min(position + CHUNK_SIZE, audioData.byteLength));
              
              // Prepend 4-byte magic header: [0xA5, 0x5A, sequence_low, sequence_high]
              // Magic bytes 0xA5 0x5A prevent false positive detection from Gemini PCM audio
              const header = new Uint8Array(4);
              header[0] = 0xA5;  // Magic byte 1
              header[1] = 0x5A;  // Magic byte 2
              header[2] = sequence & 0xFF;  // Sequence low byte
              header[3] = (sequence >> 8) & 0xFF;  // Sequence high byte
              
              const chunkWithHeader = new Uint8Array(header.length + chunk.length);
              chunkWithHeader.set(header, 0);
              chunkWithHeader.set(chunk, header.length);
              
              connection.socket.send(chunkWithHeader);
              
              position += CHUNK_SIZE;
              chunksInBatch++;
              
              if (chunksInBatch >= CHUNKS_PER_BATCH) {
                await new Promise(resolve => setTimeout(resolve, BATCH_DELAY_MS));
                chunksInBatch = 0;
              }
            }
            
            if (cancelled) {
              console.log(`[${deviceId}] ✗ Stream cancelled: ${soundName}.pcm (sequence ${sequence})`);
            } else {
              console.log(`[${deviceId}] ✓ Streamed ${soundName}.pcm (sequence ${sequence})`);
            }
            
            // Clear cancellation handler if this stream completed
            if (connection.ambientStreamCancel === connection.ambientStreamCancel) {
              connection.ambientStreamCancel = null;
            }
          } catch (error) {
            console.error(`[${deviceId}] ❌ Error loading ambient sound:`, error);
          }
          return;
        }
        
        // Handle stop ambient request from ESP32
        if (data.action === "stopAmbient") {
          console.log(`[${deviceId}] ⏹️  Stop ambient sound requested`);
          if (connection.ambientStreamCancel) {
            connection.ambientStreamCancel();
            connection.ambientStreamCancel = null;
            console.log(`[${deviceId}] ✓ Ambient stream stopped`);
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
        
        // Forward audio/messages to Gemini
        if (connection.geminiSocket && connection.geminiSocket.readyState === WebSocket.OPEN) {
          const logData = typeof event.data === "string" ? event.data : `[binary ${event.data.byteLength} bytes]`;
          if (typeof event.data === "string" && event.data.length < 500) {
            console.log(`[${deviceId}] ESP32 → Gemini:`, logData);
          }
          connection.geminiSocket.send(event.data);
        } else {
          const state = connection.geminiSocket ? `state=${connection.geminiSocket.readyState}` : "null";
          console.error(`[${deviceId}] ⚠️  Cannot forward to Gemini (${state})`);
        }
      } catch (error) {
        console.error(`[${deviceId}] ❌ Error processing ESP32 message:`, error);
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
      console.log(`[${deviceId}] 🔌 ESP32 disconnected (${stats})`);
      
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
async function connectToGemini(connection: ClientConnection) {
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
      console.log(`[${connection.deviceId}] ✅ Gemini CONNECTED at ${new Date().toISOString()}`);
      
      // Raw PCM streaming - no codec initialization needed
      console.log(`[${connection.deviceId}] 🎵 Audio pipeline: Raw PCM (24kHz, mono, 16-bit)`);
      
      // Send setup message to Gemini with function declarations
      const setupMessage = {
        setup: {
          model: "models/gemini-2.0-flash-exp",
          generationConfig: {
            responseModalities: ["AUDIO"],
            speechConfig: {
              voiceConfig: {
                prebuiltVoiceConfig: {
                  voiceName: "Kore"  // Calm, neutral voice
                }
              }
            }
          },
          tools: [{
            functionDeclarations: [
            {
              name: "get_tide_status",
              description: "Get the current tide status for Brighton, UK. ONLY use this when the user specifically asks about tides, the sea, or water levels. Do not call this for general time/date questions.",
              parameters: {
                type: "OBJECT",
                properties: {},
                required: []
              }
            },
            {
              name: "get_current_time",
              description: "Get the current time, date, and day of the week in UK timezone. ONLY use this when the user specifically asks about time, date, or day. Do not call this when asking about tides.",
              parameters: {
                type: "OBJECT",
                properties: {},
                required: []
              }
            },
            {
              name: "set_timer",
              description: "Set a countdown timer for a specified duration. Use when user says 'set a timer', 'timer for X minutes', etc.",
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
              name: "check_timer",
              description: "Check how much time is remaining on the current timer. Use when user asks 'how much time left', 'check timer', etc.",
              parameters: {
                type: "OBJECT",
                properties: {},
                required: []
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
              description: "Get weather forecast information. ONLY use this when the user specifically asks about weather, temperature, rain, or forecast. Do not call this for unrelated questions.",
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
              description: "Get the current moon phase information. ONLY use this when the user specifically asks about the moon, moon phase, or lunar cycle. Do not call this for unrelated questions.",
              parameters: {
                type: "OBJECT",
                properties: {},
                required: []
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
        
        // Log message with timing info
        const preview = JSON.stringify(json).substring(0, 150);
        console.log(`[${connection.deviceId}] 📩 Gemini #${msgNum} [${msgType}] @${timeSinceConnect}s: ${preview}${json.serverContent ? '...' : ''}`);
        
        // Handle setup complete
        if (json.setupComplete) {
          // Check if this is the first boot for this device
          const isFirstBoot = !deviceFirstBoot.has(connection.deviceId);
          if (isFirstBoot) {
            deviceFirstBoot.set(connection.deviceId, true);
          }
          
          console.log(`[${connection.deviceId}] Setup complete (firstBoot: ${isFirstBoot})`);
          connection.socket.send(JSON.stringify({ type: "setupComplete" }));
          
          // Send contextual greeting to Gemini after a short delay
          // Only on first boot, not on reconnections
          if (connection.geminiSocket && isFirstBoot) {
            setTimeout(() => {
              // Check socket is still open
              if (!connection.geminiSocket || connection.geminiSocket.readyState !== WebSocket.OPEN) {
                console.log(`[${connection.deviceId}] Gemini socket closed, skipping greeting`);
                return;
              }
              
              const now = new Date();
              const hour = now.getHours();
              let greeting = "Good morning";
              if (hour >= 12 && hour < 17) {
                greeting = "Good afternoon";
              } else if (hour >= 17) {
                greeting = "Good evening";
              }
              
              const greetingMessage = {
                clientContent: {
                  turns: [{
                    role: "user",
                    parts: [{
                      text: `SYSTEM: Device just started up. Please greet the user with a brief, friendly message. Say something like "${greeting}, Jellyberry is now online" but keep it natural and concise (one short sentence).`
                    }]
                  }],
                  turnComplete: true
                }
              };
              
              console.log(`[${connection.deviceId}] Sending startup greeting request to Gemini`);
              connection.geminiSocket.send(JSON.stringify(greetingMessage));
            }, 2000); // 2 second delay to let device settle
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
              
              // Forward tide data to ESP32 for LED visualization
              if (functionResult.success) {
                connection.socket.send(JSON.stringify({
                  type: "tideData",
                  state: functionResult.state,
                  waterLevel: functionResult.waterLevel,
                  nextChangeMinutes: functionResult.nextChangeMinutes
                }));
                console.log(`[${connection.deviceId}] Sent tide data to ESP32: ${functionResult.state}, level: ${functionResult.waterLevel.toFixed(2)}`);
              }
            } else if (funcName === "get_current_time") {
              functionResult = getCurrentTime();
              console.log(`[${connection.deviceId}] Current time: ${functionResult.time} on ${functionResult.date}`);
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
            } else if (funcName === "check_timer") {
              functionResult = checkTimer(connection.deviceId);
            } else if (funcName === "cancel_timer") {
              functionResult = cancelTimer(connection.deviceId);
              
              // Notify ESP32
              if (functionResult.success) {
                connection.socket.send(JSON.stringify({
                  type: "timerCancelled"
                }));
              }
            } else if (funcName === "get_weather_forecast") {
              const timeframe = funcArgs.timeframe || "current";
              
              // Fetch fresh weather data on first call
              if (!weatherDataCache) {
                await fetchWeatherData();
              }
              
              functionResult = getWeather(connection.deviceId, timeframe);
              console.log(`[${connection.deviceId}] Weather (${timeframe}): ${functionResult.summary || functionResult.error}`);
            } else if (funcName === "get_moon_phase") {
              functionResult = getMoonPhase();
              console.log(`[${connection.deviceId}] Moon phase: ${functionResult.phaseName} ${functionResult.phaseEmoji} (${functionResult.illumination}% illuminated)`);
              
              // Send moon data to ESP32 for LED visualization
              if (functionResult.success) {
                connection.socket.send(JSON.stringify({
                  type: "moonData",
                  phaseName: functionResult.phaseName,
                  illumination: functionResult.illumination,
                  moonAge: functionResult.moonAge
                }));
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
            // Handle function calls
            if (part.functionCall) {
              const funcName = part.functionCall.name;
              const funcArgs = part.functionCall.args;
              console.log(`[${connection.deviceId}] Function call: ${funcName}`, funcArgs);
              
              // Execute the function
              let functionResult: any = { success: false, error: "Unknown function" };
              
              if (funcName === "get_tide_status") {
                functionResult = await getTideStatus();
                
                // Forward tide data to ESP32 for LED visualization
                if (functionResult.success) {
                  connection.socket.send(JSON.stringify({
                    type: "tideData",
                    state: functionResult.state,
                    waterLevel: functionResult.waterLevel,
                    nextChangeMinutes: functionResult.nextChangeMinutes
                  }));
                  console.log(`[${connection.deviceId}] Sent tide data to ESP32: ${functionResult.state}, level: ${functionResult.waterLevel.toFixed(2)}`);
                }
              } else if (funcName === "get_current_time") {
                functionResult = getCurrentTime();
                console.log(`[${connection.deviceId}] Current time: ${functionResult.time} on ${functionResult.date}`);
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
              } else if (funcName === "check_timer") {
                functionResult = checkTimer(connection.deviceId);
              } else if (funcName === "cancel_timer") {
                functionResult = cancelTimer(connection.deviceId);
                
                // Notify ESP32
                if (functionResult.success) {
                  connection.socket.send(JSON.stringify({
                    type: "timerCancelled"
                  }));
                }
              } else if (funcName === "get_weather_forecast") {
                const timeframe = funcArgs.timeframe || "current";
                
                // Fetch fresh weather data on first call
                if (!weatherDataCache) {
                  await fetchWeatherData();
                }
                
                functionResult = getWeather(connection.deviceId, timeframe);
                console.log(`[${connection.deviceId}] Weather (${timeframe}): ${functionResult.summary || functionResult.error}`);
              } else if (funcName === "get_moon_phase") {
                functionResult = getMoonPhase();
                console.log(`[${connection.deviceId}] Moon phase: ${functionResult.phaseName} ${functionResult.phaseEmoji} (${functionResult.illumination}% illuminated)`);
                
                // Send moon data to ESP32 for LED visualization
                if (functionResult.success) {
                  connection.socket.send(JSON.stringify({
                    type: "moonData",
                    phaseName: functionResult.phaseName,
                    illumination: functionResult.illumination,
                    moonAge: functionResult.moonAge
                  }));
                }
              }
              
              // Send function response back to Gemini
              const functionResponse = {
                toolResponse: {
                  functionResponses: [{
                    id: part.functionCall.id || "func_" + Date.now(),
                    name: funcName,
                    response: functionResult
                  }]
                }
              };
              connection.geminiSocket!.send(JSON.stringify(functionResponse));
            }
            
            // Handle audio data
            if (part.inlineData?.data) {
              const base64Audio = part.inlineData.data;
              const mimeType = part.inlineData.mimeType || "unknown";
              
              // Track audio chunk statistics
              connection.audioChunkCount = (connection.audioChunkCount || 0) + 1;
              const now = Date.now();
              const gapMs = connection.lastAudioChunkTime ? (now - connection.lastAudioChunkTime) : 0;
              connection.lastAudioChunkTime = now;
              
              const chunkNum = connection.audioChunkCount;
              console.log(`[${connection.deviceId}] 🔊 Audio chunk #${chunkNum}: ${base64Audio.length} chars base64, gap=${gapMs}ms, mimeType=${mimeType}`);
              
              // Decode base64 to raw PCM bytes (16-bit little-endian)
              const binaryString = atob(base64Audio);
              const pcmBytes = new Uint8Array(binaryString.length);
              for (let i = 0; i < binaryString.length; i++) {
                pcmBytes[i] = binaryString.charCodeAt(i);
              }
              
              // Stream raw PCM directly to ESP32 without encoding
              // PCM is already 16-bit little-endian mono at 24kHz from Gemini
              const CHUNK_SIZE = 1920;  // 960 samples * 2 bytes = 40ms chunks
              
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
              
              console.log(`[${connection.deviceId}] 📤 ESP32 Raw PCM: ${totalBytesSent} bytes in ${chunksInThisPart} chunks (${(totalBytesSent / 2 / 24000 * 1000).toFixed(0)}ms audio)`);
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
          console.log(`[${connection.deviceId}] Turn complete`);
          connection.socket.send(JSON.stringify({ type: "turnComplete" }));
        }
        
      } catch (error) {
        console.error(`[${connection.deviceId}] ❌ Error processing Gemini message:`, error);
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
      console.error(`[${connection.deviceId}] ⚠️  Gemini WebSocket ERROR after ${sessionDuration}s:`, error);
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
      
      console.log(`[${connection.deviceId}] ❌ Gemini CLOSED after ${sessionDuration}s`);
      console.log(`[${connection.deviceId}]    Code: ${event.code} (${getCloseCodeDescription(event.code)})`);
      console.log(`[${connection.deviceId}]    Reason: "${event.reason || "(empty)"}"`);
      console.log(`[${connection.deviceId}]    WasClean: ${event.wasClean}`);
      console.log(`[${connection.deviceId}]    Stats: ${stats}`);
      console.log(`[${connection.deviceId}]    ESP32 state: ${connection.socket.readyState} (${connection.socket.readyState === WebSocket.OPEN ? "OPEN" : "CLOSED"})`);
      
      connection.geminiSocket = null;
      
      // Don't auto-reconnect if ambient sound is playing - we don't need Gemini for that
      // Auto-reconnect after 1 second if ESP32 is still connected
      if (connections.has(connection.deviceId)) {
        console.log(`[${connection.deviceId}] ⏳ Scheduling Gemini reconnection in 1s (ESP32 still connected)...`);
        setTimeout(() => {
          if (connections.has(connection.deviceId) && !connection.geminiSocket) {
            console.log(`[${connection.deviceId}] 🔄 Attempting Gemini reconnection...`);
            // Reset statistics for new session
            connection.geminiMessageCount = 0;
            connection.audioChunkCount = 0;
            connection.lastMessageType = undefined;
            connection.lastAudioChunkTime = undefined;
            connectToGemini(connection);
          } else {
            console.log(`[${connection.deviceId}] ⚠️  Reconnection cancelled (ESP32 gone or Gemini already connected)`);
          }
        }, 1000);
      } else {
        console.log(`[${connection.deviceId}] ℹ️  No reconnection (ESP32 disconnected)`);
      }
    };
    
    
  } catch (error) {
    console.error(`[${connection.deviceId}] ❌ Failed to establish Gemini connection:`, error);
    console.error(`[${connection.deviceId}] Error type: ${error instanceof Error ? error.name : typeof error}`);
    console.error(`[${connection.deviceId}] Error message: ${error instanceof Error ? error.message : String(error)}`);
    connection.socket.send(JSON.stringify({ 
      type: "error",
      message: "Failed to connect to Gemini API" 
    }));
  }
}

console.log("🚀 Jellyberry Edge Server running on port 8000");
