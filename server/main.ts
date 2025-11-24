// Deno Edge Server for Jellyberry - WebSocket Proxy to Gemini Live API
// Deploy to Deno Deploy: deno deploy --project=jellyberry-server main.ts

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
Deno.serve({ port: 8000 }, (req) => {
  const url = new URL(req.url);
  
  // Health check endpoint
  if (url.pathname === "/health") {
    return new Response("OK", { status: 200 });
  }
  
  // WebSocket endpoint for ESP32 devices
  if (url.pathname === "/ws" && req.headers.get("upgrade") === "websocket") {
    const { socket, response } = Deno.upgradeWebSocket(req);
    const deviceId = url.searchParams.get("device_id") || crypto.randomUUID();
    
    console.log(`[${deviceId}] Device connected`);
    
    const connection: ClientConnection = {
      socket,
      geminiSocket: null,
      deviceId,
    };
    
    connections.set(deviceId, connection);
    
    // Handle messages from ESP32
    socket.onmessage = async (event) => {
      try {
        const data = typeof event.data === "string" ? JSON.parse(event.data) : event.data;
        
        // Setup message - establish Gemini connection
        if (data.type === "setup" || (!connection.geminiSocket && typeof event.data === "string")) {
          await connectToGemini(connection);
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
          console.error(`[${deviceId}] Gemini not connected, buffering...`);
        }
      } catch (error) {
        console.error(`[${deviceId}] Error processing message:`, error);
      }
    };
    
    socket.onerror = (error) => {
      console.error(`[${deviceId}] WebSocket error:`, error);
    };
    
    socket.onclose = () => {
      console.log(`[${deviceId}] Device disconnected`);
      if (connection.geminiSocket) {
        connection.geminiSocket.close();
      }
      connections.delete(deviceId);
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
      console.log(`[${connection.deviceId}] Connected to Gemini`);
      
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
        
        // Log ALL messages for debugging
        console.log(`[${connection.deviceId}] ← Gemini message:`, JSON.stringify(json).substring(0, 200));
        
        // Handle setup complete
        if (json.setupComplete) {
          console.log(`[${connection.deviceId}] Setup complete`);
          connection.socket.send(JSON.stringify({ type: "setupComplete" }));
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
              console.log(`[${connection.deviceId}] Audio: ${base64Audio.length} chars base64, mimeType: ${mimeType}`);
              
              // Decode base64 to raw PCM bytes
              const binaryString = atob(base64Audio);
              const pcmData = new Uint8Array(binaryString.length);
              for (let i = 0; i < binaryString.length; i++) {
                pcmData[i] = binaryString.charCodeAt(i);
              }
              
              // Send raw PCM in larger chunks (1024 bytes) with no delay
              const chunkSize = 1024;
              const numChunks = Math.ceil(pcmData.length / chunkSize);
              console.log(`[${connection.deviceId}] → ESP32 PCM: ${pcmData.length} bytes in ${numChunks} chunks`);
              
              for (let i = 0; i < pcmData.length; i += chunkSize) {
                const chunk = pcmData.slice(i, Math.min(i + chunkSize, pcmData.length));
                connection.socket.send(chunk);
              }
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
        console.error(`[${connection.deviceId}] Error processing message:`, error);
      }
    };
    
    connection.geminiSocket.onerror = (error) => {
      console.error(`[${connection.deviceId}] Gemini WebSocket error:`, error);
      connection.socket.send(JSON.stringify({ 
        type: "error",
        message: "Gemini connection error" 
      }));
    };
    
    connection.geminiSocket.onclose = (event) => {
      console.log(`[${connection.deviceId}] Gemini connection closed - code: ${event.code}, reason: "${event.reason}", wasClean: ${event.wasClean}`);
      connection.geminiSocket = null;
      
      // Auto-reconnect after 1 second if ESP32 is still connected
      if (connections.has(connection.deviceId)) {
        console.log(`[${connection.deviceId}] Scheduling Gemini reconnection in 1s...`);
        setTimeout(() => {
          if (connections.has(connection.deviceId) && !connection.geminiSocket) {
            console.log(`[${connection.deviceId}] Auto-reconnecting to Gemini...`);
            connectToGemini(connection);
          }
        }, 1000);
      }
    };
    
    connection.geminiSocket.onerror = (error) => {
      console.error(`[${connection.deviceId}] Gemini WebSocket error:`, error);
    };
    
  } catch (error) {
    console.error(`[${connection.deviceId}] Failed to connect to Gemini:`, error);
    connection.socket.send(JSON.stringify({ 
      type: "error",
      message: "Failed to connect to Gemini API" 
    }));
  }
}

console.log("🚀 Jellyberry Edge Server running on port 8000");
