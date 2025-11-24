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

// Debug: Check if API key is loaded
console.log(`[DEBUG] GEMINI_API_KEY loaded: ${GEMINI_API_KEY ? "YES (" + GEMINI_API_KEY.substring(0, 10) + "...)" : "NO - EMPTY!"}`);

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
            functionDeclarations: [{
              name: "get_tide_status",
              description: "Get the current tide status for Brighton, UK including whether the tide is coming in (flooding) or going out (ebbing), the water level, and when the next tide change will occur. Use this when the user asks about tides, tide times, or the sea.",
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
      // Don't auto-reconnect immediately - wait for next user message
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
