// Deno Edge Server for Jellyberry - WebSocket Proxy to Gemini Live API
// Deploy to Deno Deploy: deno deploy --project=jellyberry-server main.ts

const GEMINI_API_KEY = Deno.env.get("GEMINI_API_KEY") || "";
const GEMINI_WS_URL = "wss://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent";

interface ClientConnection {
  socket: WebSocket;
  geminiSocket: WebSocket | null;
  deviceId: string;
  lastFunctionResult?: any;
  pendingFunctionCallId?: string;
}

const connections = new Map<string, ClientConnection>();

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
          tools: []
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
        
        // Handle setup complete
        if (json.setupComplete) {
          console.log(`[${connection.deviceId}] Setup complete`);
          connection.socket.send(JSON.stringify({ type: "setupComplete" }));
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
              
              // Forward function call to ESP32
              connection.socket.send(JSON.stringify({
                type: "functionCall",
                name: funcName,
                args: funcArgs
              }));
              
              // Send function response back to Gemini
              const functionResponse = {
                toolResponse: {
                  functionResponses: [{
                    id: part.functionCall.id || "func_" + Date.now(),
                    name: funcName,
                    response: { success: true }
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
    
    connection.geminiSocket.onclose = () => {
      console.log(`[${connection.deviceId}] Gemini connection closed - will reconnect on next message`);
      connection.geminiSocket = null;
      // Don't auto-reconnect immediately - wait for next user message
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
