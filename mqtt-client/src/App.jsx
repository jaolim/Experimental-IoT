/*
Initial code written by ChatGPT
A simple React client for listening a specific mqtt broker for a specific topic and posting the latest message
*/
import React, { useEffect, useMemo, useRef, useState } from "react";
import mqtt from "mqtt";
import {
  ResponsiveContainer,
  ComposedChart,
  Bar,
  Line,
  XAxis,
  YAxis,
  CartesianGrid,
  Tooltip,
  Legend,
} from "recharts";

/**
 * Simple MQTT client React app
 * - Connects to a broker over WebSockets
 * - Subscribes to a topic
 * - Displays latest message (attempts to parse JSON)
 *
 */

export function SoundChart({ data }) {
  const chartData = data.map((d) => ({
    ...d,
    timeLabel: new Date(d.time).toLocaleTimeString(),
  }));

  // Compute tight Y domain (zoomed in)
  const allValues = chartData.flatMap((d) => [d.min, d.max, d.average]).filter(Number.isFinite);

  const minY = Math.min(...allValues);
  const maxY = Math.max(...allValues);

  // Add small padding so bars/line aren't stuck to edges
  const padding = 0.3; // <-- adjust (0.2–1.0)
  const domainMin = Math.floor((minY - padding) * 10) / 10; // round to 0.1
  const domainMax = Math.ceil((maxY + padding) * 10) / 10;

  return (
    <ResponsiveContainer width="100%" height={340}>
      <ComposedChart data={chartData}>
        <CartesianGrid strokeDasharray="3 3" />
        <XAxis dataKey="timeLabel" interval="preserveStartEnd" />
        <YAxis
          //domain={[domainMin, domainMax]}
          tickCount={Math.min(20, Math.max(6, Math.round((domainMax - domainMin) * 10) + 1))}
          tickFormatter={(v) => v.toFixed(1)}
         // allowDecimals={true}
        />
        <Tooltip
          formatter={(value) => `${Number(value).toFixed(1)}`}
          labelFormatter={(label) => `Time: ${label}`}
        />
        <Legend />

        {/* Bars */}
        <Bar dataKey="min" name="Min" fill="#3b82f6" />
        <Bar dataKey="max" name="Max" fill="#ef4444" />

        {/* Line */}
        <Line
          type="monotone"
          dataKey="average"
          name="Average"
          stroke="#22c55e"
          strokeWidth={3}
          dot={false}
          isAnimationActive={false}
        />
      </ComposedChart>
    </ResponsiveContainer>
  );
}

export default function App() {
  const [brokerUrl, setBrokerUrl] = useState('ws://test.mosquitto.org:8080');
  const [topic, setTopic] = useState('ExpIoT/Noise');

  const [isConnected, setIsConnected] = useState(false);
  const [status, setStatus] = useState("Idle");

  const [latestRaw, setLatestRaw] = useState("");
  const [latestJson, setLatestJson] = useState(null);
  const [latestTopic, setLatestTopic] = useState("");
  const [latestTime, setLatestTime] = useState(null);

  const [messages, setMessages] = useState([]);
  const [record, setRecord] = useState();

  const clientRef = useRef(null);

  // stable options; change here if you want auth, clean session, etc.
  const options = useMemo(
    () => ({
      // For device testing you usually want clean: true
      clean: true,
      connectTimeout: 10_000,
      reconnectPeriod: 2_000,
      // If you want a predictable clientId, set it here
      clientId: `react-mqtt-${Math.random().toString(16).slice(2)}`,
    }),
    []
  );

  const disconnect = () => {
    const c = clientRef.current;
    if (c) {
      try {
        c.end(true);
      } catch {
        // ignore
      }
      clientRef.current = null;
    }
    setIsConnected(false);
    setStatus("Disconnected");
  };

  const connect = () => {
    disconnect();

    setStatus(`Connecting to ${brokerUrl} ...`);

    const client = mqtt.connect(brokerUrl, options);
    clientRef.current = client;

    client.on("connect", () => {
      setIsConnected(true);
      setStatus("Connected. Subscribing...");

      client.subscribe(topic, { qos: 0 }, (err) => {
        if (err) {
          setStatus(`Subscribe error: ${err.message || String(err)}`);
        } else {
          setStatus(`Subscribed to: ${topic}`);
        }
      });
    });

    client.on("reconnect", () => setStatus("Reconnecting..."));
    client.on("close", () => {
      setIsConnected(false);
      setStatus("Connection closed");
    });
    client.on("offline", () => {
      setIsConnected(false);
      setStatus("Offline");
    });
    client.on("error", (err) => {
      setStatus(`Error: ${err.message || String(err)}`);
    });


    client.on("message", (msgTopic, payload) => {
      const raw = payload?.toString?.() ?? "";

      setLatestTopic(msgTopic);
      setLatestTime(new Date());
      setLatestRaw(raw);
      if (raw.toLowerCase().includes('average')) {
        setMessages(prev => [...prev, parseMessage(raw)]);
      }

    });
  };

  const parseMessage = (message) => {
    const messageValues = message.split(';');
    const messageAverage = Number(messageValues[0].split(',')[1].trim());
    const messageMin = Number(messageValues[1].split(',')[1].trim());
    const messageMax = Number(messageValues[2].split(',')[1].trim());
    const parsedMessage = {
      time: new Date(),
      min: messageMin,
      max: messageMax,
      average: messageAverage
    }
    return parsedMessage;
  }

  // Auto-connect on first render
  useEffect(() => {
    //  connect();
    return () => disconnect();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // Reconnect when broker/topic changes
  useEffect(() => {
    // Only reconnect if we already attempted connection
    if (!brokerUrl || !topic) return;

    const t = setTimeout(() => {
      connect();
    }, 400);

    return () => clearTimeout(t);
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [brokerUrl, topic]);

  return (
    <div style={{ fontFamily: "system-ui, -apple-system, Segoe UI, Roboto, sans-serif", padding: 16, maxWidth: 980 }}>
      <h2 style={{ margin: 0 }}>MQTT Test Client</h2>
      <p style={{ marginTop: 6, color: "#555" }}>
        Simple standalone React MQTT client for IoT testing (WebSocket brokers).
      </p>
      <SoundChart data={messages} />

      <div style={{ display: "grid", gridTemplateColumns: "1fr 1fr", gap: 12, marginTop: 12 }}>
        <div>
          <label style={{ display: "block", fontSize: 12, fontWeight: 600 }}>Broker URL (ws/wss)</label>
          <input
            value={brokerUrl}
            onChange={(e) => setBrokerUrl(e.target.value)}
            placeholder="input broker"
            style={{ width: "100%", padding: 10, borderRadius: 8, border: "1px solid #ddd" }}
          />
        </div>

        <div>
          <label style={{ display: "block", fontSize: 12, fontWeight: 600 }}>Topic</label>
          <input
            value={topic}
            onChange={(e) => setTopic(e.target.value)}
            placeholder="Input topic"
            style={{ width: "100%", padding: 10, borderRadius: 8, border: "1px solid #ddd" }}
          />
        </div>
      </div>

      <div style={{ display: "flex", gap: 10, alignItems: "center", marginTop: 12 }}>
        <button
          onClick={connect}
          style={{
            padding: "10px 14px",
            borderRadius: 10,
            border: "1px solid #ddd",
            cursor: "pointer",
            background: "white",
            fontWeight: 600,
          }}
        >
          Connect / Reconnect
        </button>

        <button
          onClick={disconnect}
          style={{
            padding: "10px 14px",
            borderRadius: 10,
            border: "1px solid #ddd",
            cursor: "pointer",
            background: "white",
            fontWeight: 600,
          }}
        >
          Disconnect
        </button>

        <span
          style={{
            marginLeft: "auto",
            padding: "6px 10px",
            borderRadius: 999,
            fontSize: 12,
            fontWeight: 700,
            border: "1px solid #ddd",
            background: isConnected ? "#e9fff1" : "#fff2f2",
          }}
        >
          {isConnected ? "CONNECTED" : "DISCONNECTED"}
        </span>
      </div>

      <div style={{ marginTop: 14, padding: 12, borderRadius: 12, border: "1px solid #eee", background: "#fafafa" }}>
        <div style={{ fontSize: 12, fontWeight: 700, marginBottom: 6 }}>Status</div>
        <div style={{ fontSize: 13 }}>{status}</div>
      </div>

      <div style={{ marginTop: 14, display: "grid", gridTemplateColumns: "1fr 1fr", gap: 12 }}>
        <div style={{ padding: 12, borderRadius: 12, border: "1px solid #eee" }}>
          <div style={{ fontSize: 12, fontWeight: 700, marginBottom: 6 }}>Latest message (raw)</div>
          <div style={{ fontSize: 12, color: "#666", marginBottom: 6 }}>
            Topic: <b>{latestTopic || "-"}</b>
            {latestTime ? (
              <>
                {" "}· Time: <b>{latestTime.toLocaleString()}</b>
              </>
            ) : null}
          </div>
          <pre style={{ margin: 0, whiteSpace: "pre-wrap", wordBreak: "break-word", fontSize: 12 }}>
            {latestRaw || "(no message yet)"}
          </pre>
        </div>

        <div style={{ padding: 12, borderRadius: 12, border: "1px solid #eee" }}>
          <div style={{ fontSize: 12, fontWeight: 700, marginBottom: 6 }}>Message history (raw)</div>
          <div style={{ fontSize: 12, color: "#666", marginBottom: 6 }}>
            Topic: <b>{latestTopic || "-"}</b>
            {latestTime ? (
              <>
                {" "}· Time: <b>{latestTime.toLocaleString()}</b>
              </>
            ) : null}
          </div>
          <pre style={{ margin: 0, whiteSpace: "pre-wrap", wordBreak: "break-word", fontSize: 12 }}>
            {JSON.stringify(messages) || "(no messages yet)"}
          </pre>
        </div>
      </div>
      <div>
        <button onClick={() => setMessages([...messages, parseMessage("Average, 9.9; Min, 8.3; Max, 11.4;")])}>Test</button>
      </div>

      <div style={{ marginTop: 14, fontSize: 12, color: "#666" }}>
        Tip: many MQTT brokers require WebSockets. Use <b>wss://</b> if you are serving this app over HTTPS.
      </div>
    </div>
  );
}